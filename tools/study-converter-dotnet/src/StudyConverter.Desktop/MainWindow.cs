using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Interactivity;
using Avalonia.Input;
using Avalonia.Controls.Primitives;
using System.Diagnostics;

namespace StudyConverter.Desktop;

public sealed class MainWindow : Window
{
    private readonly TextBlock _status;
    private readonly TextBlock _fileName;
    private readonly Button _choose;
    private readonly Button _convert;
    private readonly Button _reveal;
    private readonly Button _detailsButton;
    private readonly StackPanel _details;
    private readonly StackPanel _decks;
    private readonly ComboBox _deckChoice;
    private string? _input;
    private string? _output;
    private bool _busy;

    public MainWindow()
    {
        Title = "Study Converter";
        Width = 680; Height = 700; MinWidth = 480; MinHeight = 540;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        Background = Brush.Parse("#F7F8FA");
        var page = new StackPanel { Spacing = 18, Margin = new Thickness(34), MaxWidth = 760 };
        Content = new ScrollViewer { Content = page, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        page.Children.Add(new TextBlock { Text = "Study Converter", FontSize = 28, FontWeight = FontWeight.Bold, Foreground = Brush.Parse("#17202A") });
        page.Children.Add(new TextBlock { Text = "Transforme seus decks do Anki para o X4 Pro.", FontSize = 15, Foreground = Brush.Parse("#687385") });
        var drop = new Border { BorderBrush = Brush.Parse("#CBD3DE"), BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(14), Background = Brushes.White, Padding = new Thickness(24), MinHeight = 132 };
        var dropStack = new StackPanel { Spacing = 10, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        _fileName = new TextBlock { Text = "Arraste seu arquivo .apkg", FontSize = 17, FontWeight = FontWeight.SemiBold, Foreground = Brush.Parse("#17202A"), HorizontalAlignment = HorizontalAlignment.Center, TextWrapping = TextWrapping.Wrap };
        dropStack.Children.Add(_fileName);
        dropStack.Children.Add(new TextBlock { Text = "ou", Foreground = Brush.Parse("#687385"), HorizontalAlignment = HorizontalAlignment.Center });
        _choose = new Button { Content = "Escolher arquivo", Background = Brush.Parse("#E9EEF5"), Foreground = Brush.Parse("#17202A"), HorizontalAlignment = HorizontalAlignment.Center, Padding = new Thickness(20, 9) };
        _choose.Click += async (_, _) => await ChooseFile();
        dropStack.Children.Add(_choose); drop.Child = dropStack; page.Children.Add(drop);
        _status = new TextBlock { Text = "Seus arquivos permanecem neste computador.", Foreground = Brush.Parse("#687385"), TextWrapping = TextWrapping.Wrap };
        page.Children.Add(_status);
        _detailsButton = new Button { Content = "Ver detalhes", IsVisible = false, HorizontalAlignment = HorizontalAlignment.Left };
        _details = new StackPanel { Spacing = 6, IsVisible = false };
        _detailsButton.Click += (_, _) => _details.IsVisible = !_details.IsVisible;
        page.Children.Add(_detailsButton);
        page.Children.Add(_details);
        _decks = new StackPanel { Spacing = 8, IsVisible = false };
        _deckChoice = new ComboBox { PlaceholderText = "Deck completo", HorizontalAlignment = HorizontalAlignment.Stretch };
        _decks.Children.Add(new TextBlock { Text = "Deck a converter", FontWeight = FontWeight.SemiBold });
        _decks.Children.Add(_deckChoice);
        page.Children.Add(_decks);
        _convert = new Button { Content = "Converter", Background = Brush.Parse("#3157D5"), Foreground = Brushes.White, HorizontalAlignment = HorizontalAlignment.Stretch, Padding = new Thickness(16, 12), IsEnabled = false };
        _deckChoice.SelectionChanged += (_, _) => _convert.IsEnabled = _input is not null;
        _convert.Click += async (_, _) => await Convert(); page.Children.Add(_convert);
        _reveal = new Button { Content = "Mostrar arquivo", IsVisible = false, HorizontalAlignment = HorizontalAlignment.Left };
        _reveal.Click += (_, _) => RevealOutput(); page.Children.Add(_reveal);
        AddHandler(DragDrop.DropEvent, OnDrop);
        DragDrop.SetAllowDrop(this, true);
    }

    private async Task ChooseFile()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { AllowMultiple = false, FileTypeFilter = new[] { new FilePickerFileType("Anki package") { Patterns = new[] { "*.apkg" } } } });
        if (files.Count > 0) await LoadPackage(files[0].Path.LocalPath);
    }

    private async void OnDrop(object? sender, DragEventArgs args)
    {
        var file = args.DataTransfer.TryGetFiles()?.FirstOrDefault();
        if (file is not null) await LoadPackage(file.Path.LocalPath);
    }

    private async Task LoadPackage(string path)
    {
        if (!path.EndsWith(".apkg", StringComparison.OrdinalIgnoreCase)) { _status.Text = "Escolha um arquivo .apkg."; return; }
        _input = path; _fileName.Text = Path.GetFileName(path); _status.Text = "Inspecionando pacote…"; _convert.IsEnabled = false;
        _detailsButton.IsVisible = _details.IsVisible = false;
        try
        {
            var summary = await Task.Run(() => new StudyConverter.Core.Converter().InspectAsync(path).GetAwaiter().GetResult());
            _decks.IsVisible = true;
            _deckChoice.ItemsSource = new[] { "Todos os decks" }.Concat(summary.Decks.Select(d => $"{d.Name} · {d.Cards} cards")).ToArray();
            _deckChoice.SelectedIndex = 0;
            _status.Text = $"{summary.CardCount:N0} cards · {summary.Decks.Count} decks\n{string.Join("\n", summary.Warnings)}";
            _convert.IsEnabled = true;
        }
        catch (Exception ex) { _status.Text = Friendly(ex); _input = null; }
    }

    private async Task Convert()
    {
        if (_input is null || _busy) return;
        var target = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions { SuggestedFileName = Path.GetFileNameWithoutExtension(_input) + ".cps", DefaultExtension = "cps", FileTypeChoices = new[] { new FilePickerFileType("Study package") { Patterns = new[] { "*.cps" } } } });
        if (target is null) return;
        _busy = true; _convert.IsEnabled = false; _status.Text = "Convertendo…";
        _detailsButton.IsVisible = _details.IsVisible = false;
        try
        {
            var selected = _deckChoice.SelectedIndex > 0 ? _deckChoice.SelectedItem?.ToString()?.Split(" · ")[0] : null;
            var result = await Task.Run(() => new StudyConverter.Core.Converter().ConvertAsync(_input, target.Path.LocalPath, selected).GetAwaiter().GetResult());
            _output = result.OutputPath;
            _status.Text = $"{result.Converted:N0} de {result.Converted + result.Skipped:N0} cards convertidos" +
                (result.Skipped > 0 ? $"\n{result.Skipped:N0} não puderam ser convertidos." : "");
            _details.Children.Clear();
            foreach (var warning in result.Warnings)
            {
                var ids = warning.IndexOf(" (IDs:", StringComparison.Ordinal);
                var summary = ids < 0 ? warning : warning[..ids] + ".";
                if (ids < 0) { _details.Children.Add(new TextBlock { Text = summary, TextWrapping = TextWrapping.Wrap }); continue; }
                var expander = new Expander { Header = summary, Content = new TextBlock { Text = warning[(ids + 2)..], TextWrapping = TextWrapping.Wrap } };
                _details.Children.Add(expander);
            }
            _detailsButton.IsVisible = result.Warnings.Count > 0;
            _reveal.IsVisible = true;
        }
        catch (Exception ex) { _status.Text = Friendly(ex); }
        finally { _busy = false; _convert.IsEnabled = _input is not null; }
    }

    private void RevealOutput()
    {
        if (_output is null) return;
        var start = OperatingSystem.IsMacOS() ? "open" : OperatingSystem.IsWindows() ? "explorer" : "xdg-open";
        var path = OperatingSystem.IsWindows() ? "/select," + _output : Path.GetDirectoryName(_output)!;
        Process.Start(new ProcessStartInfo(start, path) { UseShellExecute = false });
    }

    private static string Friendly(Exception exception) => exception switch
    {
        UnauthorizedAccessException => "Sem permissão para acessar este arquivo ou pasta.",
        IOException => "Não foi possível ler ou gravar o arquivo. Verifique o espaço disponível e tente novamente.",
        InvalidDataException => "O arquivo não é um pacote Anki válido ou está corrompido.",
        _ => exception.Message
    };
}
