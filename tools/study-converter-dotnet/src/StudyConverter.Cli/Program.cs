using StudyConverter.Core;

if (args.Length is < 2 or > 3)
{
    Console.Error.WriteLine("Uso: StudyConverter.Cli <entrada.apkg> <saida.cps> [deck-subtree]");
    return 2;
}
try
{
    var converter = new Converter();
    if (args.Length == 2 && args[1] == "--inspect")
    {
        var package = await converter.InspectAsync(args[0]);
        Console.WriteLine($"Cards: {package.CardCount}; decks: {package.Decks.Count}");
        foreach (var deck in package.Decks) Console.WriteLine($"{deck.Name}\t{deck.Cards}");
        return 0;
    }
    var result = await converter.ConvertAsync(args[0], args[1], args.Length == 3 ? args[2] : null);
    Console.WriteLine($"Decks: {result.DeckCount}; converted: {result.Converted}; skipped: {result.Skipped}");
    foreach (var warning in result.Warnings) Console.WriteLine($"Warning: {warning}");
    return 0;
}
catch (StudyConversionException ex) { Console.Error.WriteLine(ex.Message); return 1; }
