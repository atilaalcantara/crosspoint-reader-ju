using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using System.Text.Json;
using Microsoft.Data.Sqlite;
using StudyConverter.Core;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using ZstdSharp;

static void Check(bool condition, string message)
{
    if (!condition) throw new Exception(message);
}

var dir = Path.Combine(Path.GetTempPath(), "study-converter-smoke-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(dir);
try
{
    var database = Path.Combine(dir, "collection.sqlite");
    using (var db = new SqliteConnection($"Data Source={database}"))
    {
        db.Open();
        using var schema = db.CreateCommand();
        schema.CommandText = "CREATE TABLE col(id INTEGER, models TEXT, decks TEXT); CREATE TABLE notes(id INTEGER, mid INTEGER, flds TEXT); CREATE TABLE cards(id INTEGER, nid INTEGER, did INTEGER, ord INTEGER);";
        schema.ExecuteNonQuery();
        var models = new Dictionary<string, object>
        {
            ["10"] = new { id = 10, name = "TypeIn", type = 0,
                flds = new[] { new { name = "Question" }, new { name = "Answer" } },
                tmpls = new[] { new { ord = 0, name = "Card 1", qfmt = "{{Question}}<div>{{type:Answer}}</div>",
                    afmt = "{{FrontSide}}<hr>{{Answer}}<div>Ease: <span>{{info-Ease:}}</span>%</div>" } } },
            ["11"] = new { id = 11, name = "Basic", type = 0,
                flds = new[] { new { name = "Question" }, new { name = "Answer" } },
                tmpls = new[] { new { ord = 0, name = "Card 1", qfmt = "{{Question}}", afmt = "{{Answer}}" } } },
            ["12"] = new { id = 12, name = "Unsupported", type = 0,
                flds = new[] { new { name = "Question" }, new { name = "Answer" } },
                tmpls = new[] { new { ord = 0, name = "Card 1", qfmt = "{{unknown:Answer}}", afmt = "{{Answer}}" } } },
            ["13"] = new { id = 13, name = "TypedBack", type = 0,
                flds = new[] { new { name = "Question" }, new { name = "Answer" } },
                tmpls = new[] { new { ord = 0, name = "Card 1", qfmt = "{{Question}}", afmt = "{{type:Answer}}" } } }
        };
        using var insert = db.CreateCommand();
        insert.CommandText = "INSERT INTO col VALUES (1,$models,$decks)";
        insert.Parameters.AddWithValue("$models", JsonSerializer.Serialize(models));
        insert.Parameters.AddWithValue("$decks", "{\"1\":{\"id\":1,\"name\":\"Test\"}}");
        insert.ExecuteNonQuery();
        using var rows = db.CreateCommand();
        rows.CommandText = "INSERT INTO notes VALUES ($id,$mid,$fields); INSERT INTO cards VALUES ($id,$id,1,0)";
        var idParam = rows.Parameters.Add("$id", SqliteType.Integer);
        var midParam = rows.Parameters.Add("$mid", SqliteType.Integer);
        var fieldsParam = rows.Parameters.Add("$fields", SqliteType.Text);
        for (var i = 1; i <= 45; i++)
        {
            idParam.Value = i;
            midParam.Value = i <= 2 ? 10 : i == 3 ? 11 : i == 45 ? 13 : 12;
            fieldsParam.Value = (i == 2 ? "" : $"Question {i}") + "\u001fSecret answer " + i;
            rows.ExecuteNonQuery();
        }
    }
    var apkg = Path.Combine(dir, "input.apkg");
    using (var zip = ZipFile.Open(apkg, ZipArchiveMode.Create)) zip.CreateEntryFromFile(database, "collection.anki2");
    var output = Path.Combine(dir, "output.cps");
    var converter = new Converter();
    var result = await converter.ConvertAsync(apkg, output);
    Check(result.Converted == 3 && result.Skipped == 42, $"counts: {result.Converted}/{result.Skipped}");
    Check(result.Warnings.Count == 2, "warnings must be grouped by cause");
    Check(result.Warnings.Any(w => w.StartsWith("41 card(s) ignorado(s): filtro de template não suportado: unknown", StringComparison.Ordinal) && w.Contains("IDs: 4, 5, 6, …")), "unsupported filter group");
    Check(result.Warnings.Any(w => w.StartsWith("1 card(s) ignorado(s): frente vazia", StringComparison.Ordinal) && w.Contains("IDs: 2")), "empty type-only front group");
    var data = File.ReadAllBytes(output);
    Check(data.AsSpan(0, 8).SequenceEqual("CPSTUDY\0"u8) && BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(20)) == 3, "Study V1 header");
    var index = (int)BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(28));
    var offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(index));
    Check(BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(offset)) == 1, "first card ID");
    var frontLength = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset + 12));
    var backLength = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset + 14));
    var front = Encoding.UTF8.GetString(data, offset + 16, frontLength);
    var back = Encoding.UTF8.GetString(data, offset + 16 + frontLength, backLength);
    Check(front == "Question 1", $"type widget leaked answer to question: {front}");
    Check(back.Contains("Secret answer 1", StringComparison.Ordinal) && !back.Contains("Ease:", StringComparison.Ordinal) && !back.Contains("Secret answer 1\nSecret answer 1", StringComparison.Ordinal), $"wrong answer: {back}");
    offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(index + 8));
    Check(BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(offset)) == 45, "typed-back card ID");
    frontLength = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset + 12));
    backLength = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset + 14));
    Check(Encoding.UTF8.GetString(data, offset + 16 + frontLength, backLength) == "Secret answer 45", "type-only back must reveal answer");
    Check(!Directory.Exists(Path.Combine(dir, "output.media")), "text-only conversion must not create sidecar");
    using (var db = new SqliteConnection($"Data Source={database}"))
    {
        db.Open();
        using var change = db.CreateCommand();
        change.CommandText = "UPDATE notes SET flds='Picture <img src=\"plate.png\"> <img src=\"plate.png\"> <img src=\"missing.png\">' || char(31) || 'Answer <img src=\"plate.png\">' WHERE id=3";
        change.ExecuteNonQuery();
    }
    byte[] plate;
    using (var image = new Image<Rgba32>(1200, 600, new Rgba32(255, 0, 0, 128)))
    using (var buffer = new MemoryStream()) { image.SaveAsPng(buffer); plate = buffer.ToArray(); }
    var illustrated = Path.Combine(dir, "illustrated.apkg");
    using (var zip = ZipFile.Open(illustrated, ZipArchiveMode.Create))
    {
        zip.CreateEntryFromFile(database, "collection.anki2");
        using (var stream = new StreamWriter(zip.CreateEntry("media").Open())) stream.Write("{\"0\":\"plate.png\"}");
        using var file = zip.CreateEntry("0").Open(); file.Write(plate);
    }
    var images = await converter.ConvertAsync(illustrated, output);
    Check(images.Converted == 3 && images.Warnings.Any(w => w.Contains("mídia ausente: missing.png")), "missing media warning");
    var mediaDir = Path.Combine(dir, "output.media");
    var mediaIndex = File.ReadAllBytes(Path.Combine(mediaDir, "index.bin"));
    Check(mediaIndex.AsSpan(0, 8).SequenceEqual("CPSMEDIA"u8) && BinaryPrimitives.ReadUInt16LittleEndian(mediaIndex.AsSpan(8)) == 1 &&
          BinaryPrimitives.ReadUInt32LittleEndian(mediaIndex.AsSpan(12)) == BinaryPrimitives.ReadUInt32LittleEndian(File.ReadAllBytes(output).AsSpan(36)) &&
          BinaryPrimitives.ReadUInt32LittleEndian(mediaIndex.AsSpan(16)) == 3, "media header and CPS CRC");
    var cursor = 20;
    for (var i = 0; i < 3; i++)
    {
        Check(BinaryPrimitives.ReadUInt64LittleEndian(mediaIndex.AsSpan(cursor)) == 3 && mediaIndex[cursor + 9] == 0, "record ID and reserved");
        Check(mediaIndex[cursor + 8] == (i == 2 ? 1 : 0), "face ordering");
        var length = BinaryPrimitives.ReadUInt16LittleEndian(mediaIndex.AsSpan(cursor + 10));
        Check(length == 36, "filename length");
        var name = Encoding.ASCII.GetString(mediaIndex, cursor + 12, length);
        Check(System.Text.RegularExpressions.Regex.IsMatch(name, "^[0-9a-f]{32}\\.png$") && File.Exists(Path.Combine(mediaDir, name)), "hashed PNG filename");
        cursor += 12 + length;
    }
    Check(cursor == mediaIndex.Length && Directory.GetFiles(mediaDir, "*.png").Length == 1, "deduplicated PNG");
    using (var image = Image.Load<Rgba32>(Directory.GetFiles(mediaDir, "*.png")[0]))
    {
        Check(image.Width == 800 && image.Height == 400, "screen bounds");
        var p = image[0, 0]; Check(p.R == p.G && p.G == p.B && p.A == 255 && p.R > 150, "white composite and grayscale");
    }
    using (var compressor = new Compressor())
    using (var zip = ZipFile.Open(Path.Combine(dir, "modern.apkg"), ZipArchiveMode.Create))
    {
        using (var entry = zip.CreateEntry("meta").Open()) entry.Write(new byte[] { 8, 3 });
        using (var entry = zip.CreateEntry("collection.anki21b").Open()) entry.Write(compressor.Wrap(File.ReadAllBytes(database)).ToArray());
        // MediaEntries { name = "plate.png" } is the first implicit numeric member.
        var proto = new byte[] { 10, 11, 10, 9, (byte)'p', (byte)'l', (byte)'a', (byte)'t', (byte)'e', (byte)'.', (byte)'p', (byte)'n', (byte)'g' };
        using (var entry = zip.CreateEntry("media").Open()) entry.Write(compressor.Wrap(proto).ToArray());
        using (var entry = zip.CreateEntry("0").Open()) entry.Write(plate);
    }
    var modernOutput = Path.Combine(dir, "modern.cps");
    await converter.ConvertAsync(Path.Combine(dir, "modern.apkg"), modernOutput);
    Check(File.ReadAllBytes(modernOutput).SequenceEqual(File.ReadAllBytes(output)) &&
          File.ReadAllBytes(Path.Combine(dir, "modern.media", "index.bin")).SequenceEqual(mediaIndex), "v3 media and CPS parity");
    await converter.ConvertAsync(apkg, output);
    Check(File.ReadAllBytes(output).SequenceEqual(data) && !Directory.Exists(mediaDir), "text-only CPS parity and stale sidecar removal");
    using (var db = new SqliteConnection($"Data Source={database}"))
    {
        db.Open();
        using var change = db.CreateCommand();
        change.CommandText = "UPDATE col SET decks='{\"1\":{\"id\":1,\"name\":\"Test\"},\"2\":{\"id\":2,\"name\":\"Other\"}}'; UPDATE cards SET did=2 WHERE id=3";
        change.ExecuteNonQuery();
    }
    var subset = Path.Combine(dir, "subset.apkg");
    using (var zip = ZipFile.Open(subset, ZipArchiveMode.Create))
    {
        zip.CreateEntryFromFile(database, "collection.anki2");
        using (var entry = new StreamWriter(zip.CreateEntry("media").Open())) entry.Write("{\"0\":\"plate.png\"}");
        using var asset = zip.CreateEntry("0").Open(); asset.Write(plate);
    }
    await converter.ConvertAsync(subset, Path.Combine(dir, "subset.cps"), "Test");
    Check(!Directory.Exists(Path.Combine(dir, "subset.media")), "unselected card media must not be copied");
    var bomb = Path.Combine(dir, "bomb.apkg");
    using (var compressor = new Compressor())
    using (var zip = ZipFile.Open(bomb, ZipArchiveMode.Create))
    {
        using (var entry = zip.CreateEntry("meta").Open()) entry.Write(new byte[] { 8, 3 });
        using (var entry = zip.CreateEntry("collection.anki21b").Open()) entry.Write(compressor.Wrap(File.ReadAllBytes(database)).ToArray());
        using (var entry = zip.CreateEntry("media").Open()) entry.Write(compressor.Wrap(new byte[4 * 1024 * 1024 + 1]).ToArray());
    }
    try { await converter.ConvertAsync(bomb, Path.Combine(dir, "bomb.cps")); throw new Exception("manifest decompression limit not enforced"); }
    catch (StudyConversionException ex) { Check(ex.Message.Contains("excede", StringComparison.Ordinal), "bounded v3 manifest"); }
    Check(!File.Exists(Path.Combine(dir, "bomb.cps")), "rejected media must not publish CPS");
    Console.WriteLine("PASS: type/Ease, empty front, grouped warnings, Study V1 records");
    Console.WriteLine("PASS: legacy/v3 manifests, bounded image conversion, dedup, ordering, CRC, missing media, text-only parity");
}
finally { Directory.Delete(dir, true); }
