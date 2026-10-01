using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Data.Sqlite;
using ZstdSharp;

namespace StudyConverter.Core;

public sealed record DeckSummary(string Name, int Cards);
public sealed record PackageSummary(string FileName, int CardCount, IReadOnlyList<DeckSummary> Decks, IReadOnlyList<string> Warnings);
public sealed record ConversionResult(string OutputPath, int DeckCount, int Converted, int Skipped, IReadOnlyList<string> Warnings);
public sealed class StudyConversionException(string message, Exception? inner = null) : Exception(message, inner);

public sealed partial class Converter
{
    private const int MaxArchiveEntries = 50_000, MaxArchiveBytes = 512 * 1024 * 1024, MaxCollectionBytes = 128 * 1024 * 1024;
    private const int HeaderSize = 40, DeckSize = 108;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static readonly Regex Tags = new("<[^>]*>", RegexOptions.Compiled);
    private static readonly Regex Entities = new("&(?:#(x[0-9a-fA-F]+|[0-9]+)|([a-zA-Z]+));", RegexOptions.Compiled);
    private static readonly Regex ClozeToken = new(@"\{\{c(\d+(?:,\d+)*)::(.*?)\}\}", RegexOptions.Compiled | RegexOptions.Singleline);
    private static readonly Regex Latex = new(@"\[latex\].*?\[/latex\]|\[\$\$\].*?\[/\$\$\]|\[\$\].*?\[/\]", RegexOptions.Compiled | RegexOptions.IgnoreCase | RegexOptions.Singleline);
    private static readonly Regex Sound = new(@"\[sound:[^\]]*\]", RegexOptions.Compiled | RegexOptions.IgnoreCase);
    private static readonly Regex HtmlToken = new(@"<!--[\s\S]*?-->|</?[a-zA-Z][^>]*>", RegexOptions.Compiled);

    public async Task<PackageSummary> InspectAsync(string path, CancellationToken cancellationToken = default)
    {
        using var opened = await OpenCollectionAsync(path, cancellationToken);
        var db = opened.Connection;
        var metadata = ReadMetadata(db);
        var cardsByDeck = new Dictionary<long, int>();
        using (var command = db.CreateCommand())
        {
            command.CommandText = "SELECT did, count(*) FROM cards GROUP BY did";
            using var reader = await command.ExecuteReaderAsync(cancellationToken);
            while (await reader.ReadAsync(cancellationToken)) cardsByDeck[reader.GetInt64(0)] = reader.GetInt32(1);
        }
        var decks = metadata.Decks.OrderBy(d => d.FullName, StringComparer.Ordinal).Select(d => new DeckSummary(d.FullName, cardsByDeck.GetValueOrDefault(d.Id))).ToArray();
        return new PackageSummary(Path.GetFileName(path), cardsByDeck.Values.Sum(), decks, metadata.Warnings);
    }

    public async Task<ConversionResult> ConvertAsync(string input, string output, string? selectedDeck = null, CancellationToken cancellationToken = default)
    {
        if (!Path.GetExtension(input).Equals(".apkg", StringComparison.OrdinalIgnoreCase)) throw new StudyConversionException("A entrada deve ser um pacote .apkg.");
        if (!Path.GetExtension(output).Equals(".cps", StringComparison.OrdinalIgnoreCase)) throw new StudyConversionException("A saída deve ter extensão .cps.");
        if (Path.GetFullPath(input) == Path.GetFullPath(output)) throw new StudyConversionException("Entrada e saída não podem ser o mesmo arquivo.");
        if (Path.GetFileName(input).Equals("collection.apkg", StringComparison.OrdinalIgnoreCase) && string.IsNullOrWhiteSpace(selectedDeck))
            throw new StudyConversionException("collection.apkg contém a coleção inteira; selecione um deck antes de converter.");
        using var opened = await OpenCollectionAsync(input, cancellationToken);
        var db = opened.Connection;
        var metadata = ReadMetadata(db);
        var chosen = metadata.Decks.Where(d => selectedDeck is null || d.FullName == selectedDeck || d.FullName.StartsWith(selectedDeck + "::", StringComparison.Ordinal)).OrderBy(d => d.FullName, StringComparer.Ordinal).ToList();
        if (selectedDeck is not null && metadata.Decks.All(d => d.FullName != selectedDeck)) throw new StudyConversionException($"Deck não encontrado: {selectedDeck}");
        if (chosen.Count == 0 || chosen.Count > 1024) throw new StudyConversionException("A seleção de decks está vazia ou excede o limite de 1.024.");
        var idMap = chosen.Select((d, i) => (d.Id, NewId: (uint)(i + 1))).ToDictionary(x => x.Id, x => x.NewId);
        long cardsInSelection;
        var hasOriginalDeck = HasColumn(db, "cards", "odid");
        var hasTags = HasColumn(db, "notes", "tags");
        using (var countCommand = db.CreateCommand())
        {
            var parameters = string.Join(",", chosen.Select((_, i) => $"$d{i}"));
            countCommand.CommandText = $"SELECT count(*) FROM cards WHERE did IN ({parameters})" +
                                      (hasOriginalDeck ? $" OR odid IN ({parameters})" : "");
            for (var i = 0; i < chosen.Count; i++) countCommand.Parameters.AddWithValue($"$d{i}", chosen[i].Id);
            cardsInSelection = (long)(countCommand.ExecuteScalar() ?? 0L);
        }
        var byName = chosen.ToDictionary(d => d.FullName, d => d.Id, StringComparer.Ordinal);
        var warningGroups = new Dictionary<string, List<long>>(StringComparer.Ordinal);
        var otherWarnings = 0;
        var cards = new List<Card>();
        var imageReferences = new List<ImageReference>();
        using (var command = db.CreateCommand())
        {
            command.CommandText = $"SELECT c.id,c.did,c.ord,n.mid,n.flds,{(hasTags ? "n.tags" : "''")},{(hasOriginalDeck ? "coalesce(c.odid,0)" : "0")} FROM cards c JOIN notes n ON n.id=c.nid ORDER BY c.id";
            using var reader = await command.ExecuteReaderAsync(cancellationToken);
            while (await reader.ReadAsync(cancellationToken))
            {
                cancellationToken.ThrowIfCancellationRequested();
                var id = reader.GetInt64(0); var deckId = reader.GetInt64(1); var ordinal = reader.GetInt32(2); var modelId = reader.GetInt64(3);
                var originalDeckId = reader.GetInt64(6); var assignedDeck = idMap.ContainsKey(deckId) ? deckId : originalDeckId;
                if (!idMap.TryGetValue(assignedDeck, out var outputDeck)) continue;
                void Skip(string reason)
                {
                    if (!warningGroups.ContainsKey(reason) && warningGroups.Count == 16) { otherWarnings++; return; }
                    if (!warningGroups.TryGetValue(reason, out var group)) warningGroups[reason] = group = [];
                    group.Add(id);
                }
                if (!metadata.Models.TryGetValue(modelId, out var model)) { Skip("modelo ou campos incompatíveis"); continue; }
                var template = model.IsCloze ? model.Templates.FirstOrDefault() : model.Templates.FirstOrDefault(t => t.Ordinal == ordinal);
                if (template is null) { Skip("template não suportado"); continue; }
                var values = reader.GetString(4).Split('\u001f');
                if (values.Length != model.Fields.Count) { Skip("quantidade de campos incompatível"); continue; }
                var fields = model.Fields.Select((name, i) => (name, value: values[i])).ToDictionary(x => x.name, x => x.value, StringComparer.Ordinal);
                var deckName = metadata.Decks.First(d => d.Id == assignedDeck).FullName;
                var specials = new Dictionary<string, string>(fields, StringComparer.Ordinal)
                {
                    ["Tags"] = reader.IsDBNull(5) ? "" : reader.GetString(5), ["Type"] = model.Name,
                    ["Deck"] = deckName, ["Subdeck"] = deckName.Split("::").Last(), ["Card"] = template.Name
                };
                if (model.IsCloze)
                    foreach (var value in values)
                        foreach (Match match in Regex.Matches(value, @"\{\{c(\d+(?:,\d+)*)::"))
                            foreach (var number in match.Groups[1].Value.Split(','))
                                specials["c" + number] = int.TryParse(number, out var n) && n == ordinal + 1 ? "1" : "";
                string front, back; var clozeSeen = new HashSet<int>();
                try
                {
                    front = Render(template.Question, fields, specials, null, model.IsCloze ? ordinal : null, false, clozeSeen);
                    if (model.IsCloze && !clozeSeen.Contains(ordinal)) throw new StudyConversionException("ordinal cloze não corresponde a nenhuma exclusão");
                    back = Render(template.Answer, fields, specials, front, model.IsCloze ? ordinal : null, true, new HashSet<int>());
                }
                catch (StudyConversionException ex) { Skip(ex.Message); continue; }
                var frontHtml = front; var backHtml = back;
                front = HtmlToText(front); back = HtmlToText(back);
                if (front.Length == 0 || StrictUtf8.GetByteCount(front) > 4096 || StrictUtf8.GetByteCount(back) > 4096 || HasForbiddenControl(front) || HasForbiddenControl(back))
                { Skip("frente vazia ou conteúdo fora dos limites Study V1"); continue; }
                cards.Add(new Card((ulong)id, outputDeck, front, back));
                imageReferences.AddRange(FindImages((ulong)id, 0, frontHtml));
                imageReferences.AddRange(FindImages((ulong)id, 1, backHtml));
                if (cards.Count > 100_000) throw new StudyConversionException("A seleção excede 100.000 cards.");
            }
        }
        var warnings = metadata.Warnings.Concat(warningGroups.OrderBy(g => g.Key, StringComparer.Ordinal)
            .Select(g => $"{g.Value.Count} card(s) ignorado(s): {g.Key} (IDs: {string.Join(", ", g.Value.Take(3))}{(g.Value.Count > 3 ? ", …" : "")}).")).ToList();
        if (otherWarnings > 0) warnings.Add($"{otherWarnings} card(s) ignorado(s) por outros motivos.");
        var media = await PrepareMediaAsync(input, output, imageReferences, warnings, cancellationToken);
        try
        {
            await WriteCpsAsync(output, chosen, idMap, byName, cards, cancellationToken);
            await PublishMediaAsync(output, media, cancellationToken);
        }
        finally { if (media is not null && Directory.Exists(media.Directory)) Directory.Delete(media.Directory, true); }
        return new ConversionResult(output, chosen.Count, cards.Count, Math.Max(0, (int)cardsInSelection - cards.Count), warnings);
    }

    private static bool HasColumn(SqliteConnection db, string table, string column)
    {
        using var command = db.CreateCommand(); command.CommandText = $"PRAGMA table_info(\"{table}\")";
        using var reader = command.ExecuteReader(); while (reader.Read()) if (reader.GetString(1) == column) return true;
        return false;
    }

    private static async Task<OpenedCollection> OpenCollectionAsync(string path, CancellationToken ct)
    {
        if (!File.Exists(path)) throw new StudyConversionException("Arquivo de entrada não encontrado.");
        try
        {
            using var archive = ZipFile.OpenRead(path);
            if (archive.Entries.Count > MaxArchiveEntries || archive.Entries.Sum(e => (long)e.Length) > MaxArchiveBytes) throw new StudyConversionException("O pacote excede os limites de segurança.");
            var members = archive.Entries.Where(e => e.FullName is "collection.anki2" or "collection.anki21" or "collection.anki21b").ToArray();
            foreach (var name in new[] { "collection.anki2", "collection.anki21", "collection.anki21b" })
                if (members.Count(e => e.FullName == name) > 1) throw new StudyConversionException($"ZIP contém múltiplas entradas {name}.");
            var metas = archive.Entries.Where(e => e.FullName == "meta").ToArray();
            if (metas.Length > 1) throw new StudyConversionException("Metadados de versão duplicados no pacote.");
            var meta = metas.SingleOrDefault(); var version = meta is null ? (members.Any(e => e.FullName == "collection.anki21b") ? 3 : 1) : -1;
            if (meta is not null) { using var s = meta.Open(); var bytes = await ReadBoundedAsync(s, 4096, ct); foreach (var field in Proto(bytes)) if (field.Number == 1 && field.Wire == 0) version = (int)field.Integer; if (version < 0) throw new StudyConversionException("Metadados APKG não contêm versão de pacote."); }
            if (version >= 3 && members.Count(e => e.FullName == "collection.anki21b") != 1)
                throw new StudyConversionException("Pacote Anki v3 sem collection.anki21b única.");
            if (version < 3 && members.Any(e => e.FullName == "collection.anki21b"))
                throw new StudyConversionException("Metadados APKG incompatíveis com collection.anki21b.");
            var member = version >= 3 ? members.Single(e => e.FullName == "collection.anki21b") :
                members.Where(e => e.FullName is "collection.anki2" or "collection.anki21").FirstOrDefault(IsSqliteCollection);
            if (member is null) throw new StudyConversionException("O pacote não contém uma coleção Anki compatível.");
            if (member.Length > MaxCollectionBytes) throw new StudyConversionException("A coleção excede 128 MiB.");
            var temp = Path.Combine(Path.GetTempPath(), $"study-{Guid.NewGuid():N}.sqlite");
            try
            {
                await using (var source = member.Open()) await using (var dest = File.Create(temp))
                {
                    if (member.FullName.EndsWith("anki21b", StringComparison.Ordinal))
                    {
                        using var decompressor = new DecompressionStream(source);
                        await CopyBoundedAsync(decompressor, dest, MaxCollectionBytes, ct);
                    }
                    else await source.CopyToAsync(dest, ct);
                }
                var connection = new SqliteConnection(new SqliteConnectionStringBuilder { DataSource = temp, Mode = SqliteOpenMode.ReadOnly, Pooling = false }.ToString());
                await connection.OpenAsync(ct); connection.CreateCollation("unicase", (a,b) => StringComparer.OrdinalIgnoreCase.Compare(a,b));
                using var check = connection.CreateCommand(); check.CommandText = "PRAGMA query_only=ON; BEGIN"; await check.ExecuteNonQueryAsync(ct);
                return new OpenedCollection(connection, temp);
            }
            catch { File.Delete(temp); throw; }
        }
        catch (StudyConversionException) { throw; }
        catch (InvalidDataException ex) { throw new StudyConversionException("O arquivo não é um APKG/ZIP válido.", ex); }
        catch (Exception ex) when (ex is IOException or SqliteException or InvalidOperationException or ZstdException or ArgumentException)
        { throw new StudyConversionException("Não foi possível abrir a coleção Anki. O arquivo pode estar corrompido ou usar um schema incompatível.", ex); }
    }

    private static bool IsSqliteCollection(ZipArchiveEntry entry)
    {
        using var stream = entry.Open(); Span<byte> magic = stackalloc byte[16]; var read = stream.Read(magic);
        return read == magic.Length && magic.SequenceEqual("SQLite format 3\0"u8);
    }

    private static Metadata ReadMetadata(SqliteConnection db)
    {
        using var command = db.CreateCommand(); command.CommandText = "SELECT models,decks FROM col ORDER BY id LIMIT 1";
        using var reader = command.ExecuteReader(); if (!reader.Read()) throw new StudyConversionException("Coleção sem registro de metadados.");
        var modelsJson = reader.IsDBNull(0) ? "{}" : reader.GetString(0); var decksJson = reader.IsDBNull(1) ? "{}" : reader.GetString(1);
        var decks = new List<Deck>(); var models = new Dictionary<long, Model>();
        if (modelsJson.Length == 0 || decksJson.Length == 0) return ReadNormalizedMetadata(db);
        using (var doc = JsonDocument.Parse(decksJson)) foreach (var item in doc.RootElement.EnumerateObject())
        {
            var v = item.Value; var name = v.GetProperty("name").GetString() ?? ""; if (v.TryGetProperty("dyn", out var dyn) && dyn.GetInt32() != 0) continue;
            var id = v.TryGetProperty("id", out var rawId) ? rawId.GetInt64() : long.Parse(item.Name);
            decks.Add(new Deck(id, name));
        }
        using (var doc = JsonDocument.Parse(modelsJson)) foreach (var item in doc.RootElement.EnumerateObject())
        {
            var v = item.Value; var id = v.TryGetProperty("id", out var rawId) ? rawId.GetInt64() : long.Parse(item.Name);
            var fieldNames = v.GetProperty("flds").EnumerateArray().Select(f => f.GetProperty("name").GetString() ?? "").ToList();
            var templates = v.GetProperty("tmpls").EnumerateArray().Select((t, i) => new Template(t.TryGetProperty("ord", out var ord) ? ord.GetInt32() : i, t.GetProperty("qfmt").GetString() ?? "", t.GetProperty("afmt").GetString() ?? "", t.TryGetProperty("name", out var templateName) ? templateName.GetString() ?? "" : "")).ToList();
            models[id] = new Model(fieldNames, templates, v.TryGetProperty("type", out var type) && type.GetInt32() == 1,
                v.TryGetProperty("name", out var modelName) ? modelName.GetString() ?? "" : "");
        }
        using var countCommand = db.CreateCommand(); countCommand.CommandText = "SELECT count(*) FROM cards"; var count = (long)(countCommand.ExecuteScalar() ?? 0L);
        if (decks.Count == 0 || decks.Count > 1024) throw new StudyConversionException("Nenhum deck suportado ou limite de decks excedido.");
        return new Metadata(decks, models, count, []);
    }

    private static Metadata ReadNormalizedMetadata(SqliteConnection db)
    {
        var fields = new Dictionary<long, List<string>>();
        using (var cmd = db.CreateCommand())
        {
            cmd.CommandText = "SELECT ntid,name FROM fields ORDER BY ntid,ord";
            using var r = cmd.ExecuteReader(); while (r.Read()) { if (!fields.TryGetValue(r.GetInt64(0), out var list)) fields[r.GetInt64(0)] = list = []; list.Add(r.GetString(1)); }
        }
        var templates = new Dictionary<long, List<Template>>();
        using (var cmd = db.CreateCommand())
        {
            cmd.CommandText = "SELECT ntid,ord,config,name FROM templates ORDER BY ntid,ord";
            using var r = cmd.ExecuteReader(); while (r.Read())
            {
                var id = r.GetInt64(0); var question = ""; var answer = ""; var templateName = r.GetString(3);
                foreach (var item in Proto((byte[])r[2])) if (item.Wire == 2 && item.Bytes is not null && item.Number is 1 or 2)
                { var value = StrictUtf8.GetString(item.Bytes); if (item.Number == 1) question = value; else answer = value; }
                if (!templates.TryGetValue(id, out var list)) templates[id] = list = []; list.Add(new Template(r.GetInt32(1), question, answer, templateName));
            }
        }
        var models = new Dictionary<long, Model>();
        using (var cmd = db.CreateCommand())
        {
            cmd.CommandText = "SELECT id,name,config FROM notetypes";
            using var r = cmd.ExecuteReader(); while (r.Read())
            {
                var id = r.GetInt64(0); var type = ReadModernModelType((byte[])r[2]); var modelTemplates = templates.GetValueOrDefault(id, []);
                if (type == 0 && modelTemplates.Any(t => t.Question.Contains("{{cloze:", StringComparison.Ordinal))) type = 1;
                models[id] = new Model(fields.GetValueOrDefault(id, []), modelTemplates, type == 1, r.GetString(1));
            }
        }
        var decks = new List<Deck>();
        using (var cmd = db.CreateCommand())
        {
            cmd.CommandText = "SELECT id,name,kind FROM decks NOT INDEXED";
            using var r = cmd.ExecuteReader(); while (r.Read()) { var dynamic = Proto((byte[])r[2]).Any(x => x.Number == 2 && x.Wire == 2); if (!dynamic) decks.Add(new Deck(r.GetInt64(0), r.GetString(1).Replace("\u001f", "::", StringComparison.Ordinal))); }
        }
        using var countCommand = db.CreateCommand(); countCommand.CommandText = "SELECT count(*) FROM cards"; var count = (long)(countCommand.ExecuteScalar() ?? 0L);
        if (decks.Count == 0 || decks.Count > 1024) throw new StudyConversionException("Nenhum deck suportado ou limite de decks excedido.");
        return new Metadata(decks, models, count, []);
    }

    private static int ReadModernModelType(byte[] config)
    {
        foreach (var item in Proto(config))
        {
            if (item.Number == 1 && item.Wire == 0) return (int)item.Integer;
            if (item.Number == 1 && item.Wire == 2 && item.Bytes is not null)
                foreach (var nested in Proto(item.Bytes)) if (nested.Number == 1 && nested.Wire == 0) return (int)nested.Integer;
        }
        return 0;
    }

    private static string Render(string template, Dictionary<string, string> fields, Dictionary<string, string> specials,
        string? front, int? ordinal, bool revealCloze, HashSet<int> clozeSeen, int depth = 0)
    {
        if (depth > 24) throw new StudyConversionException("condicionais de template aninhadas demais");
        var output = new StringBuilder(); var pos = 0;
        while (pos < template.Length)
        {
            var start = template.IndexOf("{{", pos, StringComparison.Ordinal);
            if (start < 0) { output.Append(template.AsSpan(pos)); break; }
            output.Append(template.AsSpan(pos, start - pos));
            var end = template.IndexOf("}}", start + 2, StringComparison.Ordinal);
            if (end < 0) throw new StudyConversionException("template contém {{ sem fechamento }}");
            var token = template[(start + 2)..end].Trim();
            if (token.StartsWith('#') || token.StartsWith('^'))
            {
                var name = token[1..].Trim();
                if (name.Length == 0 || name.Any(char.IsWhiteSpace)) throw new StudyConversionException($"condicional inválida: {token}");
                var (bodyEnd, sectionEnd) = FindSectionEnd(template, end + 2, name, depth);
                var value = fields.GetValueOrDefault(name, specials.GetValueOrDefault(name, ""));
                var present = !string.IsNullOrWhiteSpace(value);
                if (present != token.StartsWith('^'))
                    output.Append(Render(template[(end + 2)..bodyEnd], fields, specials, front, ordinal, revealCloze, clozeSeen, depth + 1));
                pos = sectionEnd; continue;
            }
            if (token.StartsWith('/')) throw new StudyConversionException($"fechamento condicional inesperado: {token}");
            if (token == "FrontSide")
            {
                if (front is null) throw new StudyConversionException("{{FrontSide}} só pode aparecer no template verso");
                output.Append(front);
            }
            else if (token.StartsWith("cloze:", StringComparison.Ordinal))
            {
                if (ordinal is null) throw new StudyConversionException("filtro cloze usado fora de um modelo Cloze");
                if (!fields.TryGetValue(token[6..], out var value)) throw new StudyConversionException($"campo cloze desconhecido: {token[6..]}");
                var (expanded, found) = ExpandCloze(value, ordinal.Value, revealCloze); output.Append(expanded);
                if (found) clozeSeen.Add(ordinal.Value);
            }
            else if (token.StartsWith("text:", StringComparison.Ordinal))
            {
                if (!fields.TryGetValue(token[5..], out var value)) throw new StudyConversionException($"campo text desconhecido: {token[5..]}");
                output.Append(value);
            }
            else if (token == "info-Ease:")
            {
                // Review statistics are not part of Study V1. Remove the entire display line below.
                output.Append(UnavailableEaseMarker);
            }
            else if (token.StartsWith("type:", StringComparison.Ordinal))
            {
                if (!fields.TryGetValue(token[5..], out var typedAnswer)) throw new StudyConversionException($"campo type desconhecido: {token[5..]}");
                // The typing widget would expose the answer on the question side; the back
                // displays the expected answer, even if it has no other field substitution.
                if (front is not null) output.Append(typedAnswer);
            }
            else if (token.Contains(':')) throw new StudyConversionException($"filtro de template não suportado: {token.Split(':')[0]}");
            else if (fields.TryGetValue(token, out var fieldValue)) output.Append(fieldValue);
            else if (specials.TryGetValue(token, out var specialValue)) output.Append(specialValue);
            else throw new StudyConversionException($"campo/template desconhecido: {token}");
            pos = end + 2;
        }
        return output.ToString();
    }

    private static (int BodyEnd, int SectionEnd) FindSectionEnd(string source, int start, string section, int depth)
    {
        if (depth >= 24) throw new StudyConversionException("condicionais de template aninhadas demais");
        var tokens = Regex.Matches(source[start..], @"\{\{\s*([#^/])\s*([^{}]+?)\s*\}\}");
        var stack = new Stack<string>(); stack.Push(section);
        foreach (Match match in tokens)
        {
            var marker = match.Groups[1].Value[0]; var name = match.Groups[2].Value.Trim();
            if (marker is '#' or '^') { stack.Push(name); if (stack.Count > 24) throw new StudyConversionException("condicionais de template aninhadas demais"); }
            else
            {
                if (stack.Count == 0 || stack.Peek() != name) throw new StudyConversionException($"fechamento condicional inesperado: {name}");
                stack.Pop(); if (stack.Count == 0) return (start + match.Index, start + match.Index + match.Length);
            }
        }
        throw new StudyConversionException($"condicional sem fechamento: {section}");
    }

    private static (string Text, bool FoundOrdinal) ExpandCloze(string value, int ordinal, bool reveal)
    {
        var output = new StringBuilder(); var pos = 0; var found = false;
        foreach (Match match in ClozeToken.Matches(value))
        {
            var body = match.Groups[2].Value;
            if (body.Contains("{{", StringComparison.Ordinal) || body.Contains("}}", StringComparison.Ordinal))
                throw new StudyConversionException("cloze aninhado não é suportado nesta versão");
            output.Append(value.AsSpan(pos, match.Index - pos));
            var parts = body.Split("::", 2, StringSplitOptions.None);
            var selected = match.Groups[1].Value.Split(',').Any(n => n == (ordinal + 1).ToString());
            if (selected) found = true;
            output.Append(selected ? reveal ? parts[0] : $"[{(parts.Length == 2 && parts[1].Length > 0 ? parts[1] : "…")}]" : parts[0]);
            pos = match.Index + match.Length;
        }
        if (value.AsSpan(pos).Contains("{{c", StringComparison.Ordinal)) throw new StudyConversionException("cloze sem fechamento ou sintaxe não suportada");
        output.Append(value.AsSpan(pos)); return (output.ToString(), found);
    }

    private const char UnavailableEaseMarker = '\uE000';

    private static string HtmlToText(string html)
    {
        var output = new StringBuilder(); var pos = 0; var suppressed = 0;
        void AddText(string text)
        {
            if (suppressed != 0 || text.Length == 0) return;
            text = Latex.Replace(text, "[LaTeX omitted]");
            text = Sound.Replace(text, "");
            output.Append(System.Net.WebUtility.HtmlDecode(text));
        }
        void BreakLine() { if (output.Length > 0 && output[^1] != '\n') output.Append('\n'); }
        foreach (Match token in HtmlToken.Matches(html))
        {
            AddText(html[pos..token.Index]); pos = token.Index + token.Length;
            var markup = token.Value;
            if (markup.StartsWith("<!--", StringComparison.Ordinal)) continue;
            var nameMatch = Regex.Match(markup, @"^</?\s*([a-zA-Z0-9]+)"); if (!nameMatch.Success) continue;
            var name = nameMatch.Groups[1].Value.ToLowerInvariant(); var closing = markup.StartsWith("</", StringComparison.Ordinal);
            if (suppressed > 0) { if (name is "audio" or "script" or "style" or "video") suppressed += closing ? -1 : 1; continue; }
            if (!closing && name is "audio" or "script" or "style" or "video") { suppressed = 1; continue; }
            if (!closing && name == "img")
            {
                var alt = Regex.Match(markup, "\\balt\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", RegexOptions.IgnoreCase);
                var text = alt.Success ? System.Net.WebUtility.HtmlDecode(alt.Groups[1].Success ? alt.Groups[1].Value : alt.Groups[2].Success ? alt.Groups[2].Value : alt.Groups[3].Value).Trim() : "";
                output.Append(text.Length > 0 ? $" {text} " : " [image omitted] ");
            }
            else if (name == "br" && !closing) BreakLine();
            else if (name is "address" or "article" or "blockquote" or "div" or "dl" or "fieldset" or "figcaption" or "figure" or "footer" or "h1" or "h2" or "h3" or "h4" or "h5" or "h6" or "header" or "li" or "ol" or "p" or "pre" or "section" or "table" or "tr" or "ul") BreakLine();
        }
        AddText(html[pos..]);
        var normalized = output.ToString().Replace("\r\n", "\n", StringComparison.Ordinal).Replace('\r', '\n');
        var lines = normalized.Split('\n').Select(line => Regex.Replace(line, "[\\t\\f\\v ]+", " ").Trim())
            .Select(line => line.Contains(UnavailableEaseMarker) ? line.Replace(UnavailableEaseMarker.ToString(), "", StringComparison.Ordinal).Trim() : line)
            .Where(line => line is not ("Ease: %" or "Ease:")).ToList();
        while (lines.Count > 0 && lines[0].Length == 0) lines.RemoveAt(0);
        while (lines.Count > 0 && lines[^1].Length == 0) lines.RemoveAt(lines.Count - 1);
        return string.Join('\n', lines);
    }

    private static bool HasForbiddenControl(string text) => text.Any(c => c == '\0' || c == '\r' || char.IsControl(c) && c is not ('\n' or '\t'));
    private static IEnumerable<ProtoField> Proto(byte[] data)
    {
        var i = 0;
        ulong Varint() { ulong value = 0; var shift = 0; while (i < data.Length && shift < 70) { var b = data[i++]; value |= (ulong)(b & 127) << shift; if ((b & 128) == 0) return value; shift += 7; } throw new StudyConversionException("Protobuf Anki inválido."); }
        while (i < data.Length) { var tag = Varint(); var wire = (int)(tag & 7); var field = (int)(tag >> 3); if (field == 0) throw new StudyConversionException("Protobuf Anki inválido."); if (wire == 0) yield return new ProtoField(field, wire, Varint(), null); else if (wire == 2) { var n = (int)Varint(); if (n < 0 || i + n > data.Length) throw new StudyConversionException("Protobuf truncado."); var bytes = data.AsSpan(i, n).ToArray(); i += n; yield return new ProtoField(field, wire, 0, bytes); } else if (wire is 1 or 5) { var n = wire == 1 ? 8 : 4; if (i + n > data.Length) throw new StudyConversionException("Protobuf truncado."); i += n; yield return new ProtoField(field, wire, 0, null); } else throw new StudyConversionException("Wire protobuf não suportado."); }
    }

    private static async Task WriteCpsAsync(string path, List<Deck> decks, Dictionary<long,uint> ids, Dictionary<string,long> names, List<Card> cards, CancellationToken ct)
    {
        var folder = Path.GetDirectoryName(Path.GetFullPath(path))!; Directory.CreateDirectory(folder); var temp = Path.Combine(folder, $".{Path.GetFileName(path)}.{Guid.NewGuid():N}.tmp");
        var indexOffset = HeaderSize + decks.Count * DeckSize; var dataOffset = indexOffset + cards.Count * 4; uint offset = (uint)dataOffset;
        try
        {
            await using var stream = new FileStream(temp, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None, 65536, FileOptions.Asynchronous | FileOptions.WriteThrough);
            stream.SetLength(dataOffset); stream.Position = HeaderSize;
            foreach (var deck in decks)
            {
                var name = StrictUtf8.GetBytes(deck.FullName.Split("::").Last()); if (name.Length is 0 or > 96) throw new StudyConversionException("Nome de deck fora do limite Study V1.");
                var rec = new byte[DeckSize]; BinaryPrimitives.WriteUInt32LittleEndian(rec, ids[deck.Id]); var parentName = deck.FullName.Contains("::") ? deck.FullName[..deck.FullName.LastIndexOf("::", StringComparison.Ordinal)] : "";
                BinaryPrimitives.WriteUInt32LittleEndian(rec.AsSpan(4), names.TryGetValue(parentName, out var p) ? ids[p] : 0); BinaryPrimitives.WriteUInt16LittleEndian(rec.AsSpan(8), (ushort)name.Length); name.CopyTo(rec.AsSpan(12)); await stream.WriteAsync(rec, ct);
            }
            foreach (var card in cards) { var b = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, offset); await stream.WriteAsync(b, ct); offset += (uint)(16 + StrictUtf8.GetByteCount(card.Front) + StrictUtf8.GetByteCount(card.Back)); }
            stream.Position = dataOffset;
            foreach (var card in cards)
            {
                var front = StrictUtf8.GetBytes(card.Front); var back = StrictUtf8.GetBytes(card.Back); var prefix = new byte[16]; BinaryPrimitives.WriteUInt64LittleEndian(prefix, card.Id); BinaryPrimitives.WriteUInt32LittleEndian(prefix.AsSpan(8), card.Deck); BinaryPrimitives.WriteUInt16LittleEndian(prefix.AsSpan(12), (ushort)front.Length); BinaryPrimitives.WriteUInt16LittleEndian(prefix.AsSpan(14), (ushort)back.Length); await stream.WriteAsync(prefix, ct); await stream.WriteAsync(front, ct); await stream.WriteAsync(back, ct);
            }
            var size = checked((uint)stream.Length); var crc = 0xffffffffu; stream.Position = HeaderSize; var chunk = new byte[65536]; int read; while ((read = await stream.ReadAsync(chunk, ct)) > 0) for (var i = 0; i < read; i++) { crc ^= chunk[i]; for (var bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (uint)-(int)(crc & 1)); }
            var header = new byte[HeaderSize]; "CPSTUDY\0"u8.CopyTo(header); BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(8), 1); BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(10), HeaderSize); BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12), size); BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(16), (uint)decks.Count); BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(20), (uint)cards.Count); BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(24), HeaderSize); BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(28), (uint)indexOffset); BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(32), (uint)dataOffset); BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(36), ~crc); stream.Position = 0; await stream.WriteAsync(header, ct); await stream.FlushAsync(ct); stream.Flush(true);
            File.Move(temp, path, true);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }

    private sealed record Deck(long Id, string FullName);
    private sealed record Model(List<string> Fields, List<Template> Templates, bool IsCloze, string Name);
    private sealed record Template(int Ordinal, string Question, string Answer, string Name);
    private sealed record ProtoField(int Number, int Wire, ulong Integer, byte[]? Bytes);
    private sealed record Card(ulong Id, uint Deck, string Front, string Back);
    private sealed record Metadata(List<Deck> Decks, Dictionary<long,Model> Models, long TotalCards, List<string> Warnings);
    private sealed class OpenedCollection(SqliteConnection connection, string path) : IDisposable
    {
        public SqliteConnection Connection { get; } = connection;
        public void Dispose() { Connection.Dispose(); try { File.Delete(path); } catch (IOException) { } }
    }
}
