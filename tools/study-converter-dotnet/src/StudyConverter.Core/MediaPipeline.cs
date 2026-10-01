using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using SixLabors.ImageSharp.Processing;
using ZstdSharp;

namespace StudyConverter.Core;

public sealed partial class Converter
{
    private const int MaxManifestBytes = 4 * 1024 * 1024;
    private const int MaxImageBytes = 16 * 1024 * 1024;
    private const long MaxImagePixels = 16_000_000;
    private sealed record ImageReference(ulong CardId, byte Face, string Source);
    private sealed record MediaRecord(ulong CardId, byte Face, string FileName);
    private sealed record PreparedMedia(string Directory, List<MediaRecord> Records);

    private static IEnumerable<ImageReference> FindImages(ulong cardId, byte face, string html)
    {
        foreach (Match tag in HtmlToken.Matches(html))
        {
            if (!Regex.IsMatch(tag.Value, @"^<img\s", RegexOptions.IgnoreCase)) continue;
            var source = Regex.Match(tag.Value, "\\bsrc\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", RegexOptions.IgnoreCase);
            if (!source.Success) continue;
            var raw = source.Groups[1].Success ? source.Groups[1].Value : source.Groups[2].Success ? source.Groups[2].Value : source.Groups[3].Value;
            yield return new ImageReference(cardId, face, System.Net.WebUtility.HtmlDecode(raw));
        }
    }

    private static async Task CopyBoundedAsync(Stream source, Stream destination, int limit, CancellationToken ct)
    {
        var chunk = new byte[65536]; long total = 0; int read;
        while ((read = await source.ReadAsync(chunk, ct)) != 0)
        {
            total += read;
            if (total > limit) throw new StudyConversionException($"Conteúdo descompactado excede {limit} bytes.");
            await destination.WriteAsync(chunk.AsMemory(0, read), ct);
        }
    }

    private static async Task<byte[]> ReadBoundedAsync(Stream source, int limit, CancellationToken ct)
    {
        using var dest = new MemoryStream();
        await CopyBoundedAsync(source, dest, limit, ct);
        return dest.ToArray();
    }

    private static async Task<Dictionary<string, string>> ReadManifestAsync(ZipArchive zip, bool modern, CancellationToken ct)
    {
        var entries = zip.Entries.Where(e => e.FullName == "media").ToArray();
        if (entries.Length == 0) return new(StringComparer.Ordinal);
        if (entries.Length != 1 || entries[0].Length > MaxManifestBytes) throw new StudyConversionException("Manifesto de mídia duplicado ou grande demais.");
        try
        {
            await using var input = entries[0].Open();
            var bytes = modern
                ? await ReadBoundedAsync(new DecompressionStream(input), MaxManifestBytes, ct)
                : await ReadBoundedAsync(input, MaxManifestBytes, ct);
            var result = new Dictionary<string, string>(StringComparer.Ordinal);
            if (modern)
            {
                var ordinal = 0;
                foreach (var field in Proto(bytes))
                {
                    if (field.Number != 1 || field.Wire != 2 || field.Bytes is null) continue;
                    string? name = null;
                    foreach (var inner in Proto(field.Bytes))
                        if (inner.Number == 1 && inner.Wire == 2 && inner.Bytes is not null) name = StrictUtf8.GetString(inner.Bytes);
                    if (string.IsNullOrEmpty(name)) throw new StudyConversionException("Entrada sem nome no manifesto de mídia.");
                    if (!result.TryAdd(name, ordinal.ToString(System.Globalization.CultureInfo.InvariantCulture))) throw new StudyConversionException("Nome duplicado no manifesto de mídia.");
                    ordinal++;
                }
            }
            else
            {
                using var document = JsonDocument.Parse(bytes);
                if (document.RootElement.ValueKind != JsonValueKind.Object) throw new StudyConversionException("Manifesto de mídia deve ser um objeto JSON.");
                foreach (var item in document.RootElement.EnumerateObject())
                {
                    if (item.Value.ValueKind != JsonValueKind.String || string.IsNullOrEmpty(item.Value.GetString()) ||
                        !uint.TryParse(item.Name, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var number) ||
                        !result.TryAdd(item.Value.GetString()!, number.ToString(System.Globalization.CultureInfo.InvariantCulture)))
                        throw new StudyConversionException("Entrada inválida ou duplicada no manifesto de mídia.");
                }
            }
            return result;
        }
        catch (Exception ex) when (ex is JsonException or DecoderFallbackException or ZstdException or InvalidDataException)
        { throw new StudyConversionException("Manifesto de mídia inválido.", ex); }
    }

    private static async Task<PreparedMedia?> PrepareMediaAsync(string input, string output, List<ImageReference> references, List<string> warnings, CancellationToken ct)
    {
        if (references.Count == 0) return null;
        using var zip = ZipFile.OpenRead(input);
        var modern = zip.Entries.Any(e => e.FullName == "collection.anki21b");
        var manifest = await ReadManifestAsync(zip, modern, ct);
        var members = zip.Entries.GroupBy(e => e.FullName, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.ToArray(), StringComparer.Ordinal);
        var folder = Path.GetDirectoryName(Path.GetFullPath(output))!;
        Directory.CreateDirectory(folder);
        var temp = Path.Combine(folder, $".{Path.GetFileNameWithoutExtension(output)}.media.{Guid.NewGuid():N}.tmp");
        var records = new List<MediaRecord>();
        var issues = new Dictionary<string, (int Count, List<string> Examples)>(StringComparer.Ordinal);
        void Warn(string reason, ulong id, string source)
        {
            var group = issues.GetValueOrDefault(reason, (Count: 0, Examples: new List<string>()));
            group.Count++;
            if (group.Examples.Count < 3) group.Examples.Add($"{source} (card {id})");
            issues[reason] = group;
        }
        try
        {
            var counts = new Dictionary<(ulong, byte), int>();
            var processed = new Dictionary<string, string?>(StringComparer.Ordinal);
            foreach (var reference in references)
            {
                ct.ThrowIfCancellationRequested();
                var key = (reference.CardId, reference.Face);
                if (counts.GetValueOrDefault(key) == 4) { Warn("limite de 4 imagens por face", reference.CardId, reference.Source); continue; }
                if (!manifest.TryGetValue(reference.Source, out var member) || !members.TryGetValue(member, out var matches) || matches.Length != 1)
                { Warn("mídia ausente", reference.CardId, reference.Source); continue; }
                if (!Regex.IsMatch(reference.Source, @"\.(png|jpe?g|gif|webp|bmp)$", RegexOptions.IgnoreCase))
                { Warn("imagem não suportada", reference.CardId, reference.Source); continue; }
                if (!processed.TryGetValue(member, out var fileName))
                {
                    fileName = null;
                    try
                    {
                        if (matches[0].Length > MaxImageBytes) throw new InvalidDataException("imagem grande demais");
                        await using var stream = matches[0].Open();
                        var bytes = await ReadBoundedAsync(stream, MaxImageBytes, ct);
                        using var probe = new MemoryStream(bytes);
                        var info = Image.Identify(probe);
                        if (info is null || (long)info.Width * info.Height > MaxImagePixels) throw new InvalidDataException("dimensões de imagem fora do limite");
                        using var image = Image.Load<Rgba32>(bytes);
                        if (image.Width > 800 || image.Height > 480)
                            image.Mutate(x => x.Resize(new ResizeOptions { Mode = ResizeMode.Max, Size = new Size(800, 480) }));
                        image.ProcessPixelRows(accessor =>
                        {
                            for (var y = 0; y < accessor.Height; y++)
                            {
                                var row = accessor.GetRowSpan(y);
                                for (var x = 0; x < row.Length; x++)
                                {
                                    var p = row[x];
                                    var r = (p.R * p.A + 255 * (255 - p.A) + 127) / 255;
                                    var g = (p.G * p.A + 255 * (255 - p.A) + 127) / 255;
                                    var b = (p.B * p.A + 255 * (255 - p.A) + 127) / 255;
                                    var gray = (byte)((299 * r + 587 * g + 114 * b + 500) / 1000);
                                    row[x] = new Rgba32(gray, gray, gray, 255);
                                }
                            }
                        });
                        using var grayscale = image.CloneAs<L8>();
                        using var png = new MemoryStream(); grayscale.SaveAsPng(png);
                        var result = png.ToArray();
                        fileName = Convert.ToHexStringLower(SHA256.HashData(result))[..32] + ".png";
                        Directory.CreateDirectory(temp);
                        var destination = Path.Combine(temp, fileName);
                        if (!File.Exists(destination)) await File.WriteAllBytesAsync(destination, result, ct);
                    }
                    catch (Exception ex) when (ex is UnknownImageFormatException or InvalidImageContentException or InvalidDataException or NotSupportedException)
                    { fileName = null; }
                    processed[member] = fileName;
                }
                if (fileName is null) { Warn("imagem inválida", reference.CardId, reference.Source); continue; }
                records.Add(new MediaRecord(reference.CardId, reference.Face, fileName));
                counts[key] = counts.GetValueOrDefault(key) + 1;
            }
            foreach (var (reason, group) in issues.OrderBy(x => x.Key, StringComparer.Ordinal))
                warnings.Add($"{group.Count} imagem(ns) ignorada(s): {reason}: {string.Join(", ", group.Examples)}{(group.Count > 3 ? ", …" : "")}.");
            if (records.Count == 0) { if (Directory.Exists(temp)) Directory.Delete(temp, true); return null; }
            return new PreparedMedia(temp, records);
        }
        catch { if (Directory.Exists(temp)) Directory.Delete(temp, true); throw; }
    }

    private static async Task PublishMediaAsync(string output, PreparedMedia? media, CancellationToken ct)
    {
        var target = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(output))!, Path.GetFileNameWithoutExtension(output) + ".media");
        if (media is null) { if (Directory.Exists(target)) Directory.Delete(target, true); return; }
        var header = new byte[20]; "CPSMEDIA"u8.CopyTo(header);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(8), 1);
        var cpsHeader = new byte[40];
        await using (var cps = File.OpenRead(output)) await cps.ReadExactlyAsync(cpsHeader, ct);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12), BinaryPrimitives.ReadUInt32LittleEndian(cpsHeader.AsSpan(36)));
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(16), (uint)media.Records.Count);
        await using (var stream = File.Create(Path.Combine(media.Directory, "index.bin")))
        {
            await stream.WriteAsync(header, ct);
            foreach (var record in media.Records.OrderBy(r => r.CardId).ThenBy(r => r.Face))
            {
                var name = Encoding.ASCII.GetBytes(record.FileName);
                var prefix = new byte[12];
                BinaryPrimitives.WriteUInt64LittleEndian(prefix, record.CardId);
                prefix[8] = record.Face;
                BinaryPrimitives.WriteUInt16LittleEndian(prefix.AsSpan(10), (ushort)name.Length);
                await stream.WriteAsync(prefix, ct); await stream.WriteAsync(name, ct);
            }
        }
        var backup = target + "." + Guid.NewGuid().ToString("N") + ".bak";
        if (Directory.Exists(target)) Directory.Move(target, backup);
        try { Directory.Move(media.Directory, target); if (Directory.Exists(backup)) Directory.Delete(backup, true); }
        catch { if (Directory.Exists(backup) && !Directory.Exists(target)) Directory.Move(backup, target); throw; }
    }
}
