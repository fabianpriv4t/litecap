using System.Diagnostics;
using System.IO;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using Microsoft.Win32;

namespace LiteCap.Settings;

public partial class MainWindow : Window
{
    private JsonObject _cfg = new();
    private JsonObject? _devices;          // Ausgabe von "litecap.exe --list-devices"
    private bool _loading;
    private readonly DispatcherTimer _uploadTimer = new() { Interval = TimeSpan.FromSeconds(3) };

    public MainWindow()
    {
        InitializeComponent();
        _uploadTimer.Tick += (_, _) => RefreshUploads();
    }

    // ------------------------------------------------------------------ Laden

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        try
        {
            _cfg = Core.LoadConfig();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "config.json ist fehlerhaft und wird beim Speichern ersetzt:\n" + ex.Message,
                            "LiteCap", MessageBoxButton.OK, MessageBoxImage.Warning);
            _cfg = new JsonObject();
        }
        ApplyToUi();
        // "--tab youtube" öffnet direkt eine bestimmte Seite (z. B. aus dem Tray-Menü)
        string[] args = Environment.GetCommandLineArgs();
        int t = Array.IndexOf(args, "--tab");
        if (t >= 0 && t + 1 < args.Length)
            foreach (var item in Tabs.Items.OfType<TabItem>())
                if (item.Header?.ToString()?.StartsWith(args[t + 1], StringComparison.OrdinalIgnoreCase) == true)
                    Tabs.SelectedItem = item;
        RefreshUploads();
        _uploadTimer.Start();
        CoreState.Text = CoreWindow.IsRunning
            ? "LiteCap läuft – Änderungen werden sofort übernommen."
            : "LiteCap läuft gerade nicht. Änderungen gelten beim nächsten Start.";

        // Geräte/Encoder im Hintergrund abfragen (Encoder-Test dauert 1–2 s)
        StatusText.Text = "Geräte und Encoder werden geprüft …";
        try
        {
            var (code, output) = await Core.RunAsync("--list-devices");
            if (code == 0) _devices = JsonNode.Parse(output) as JsonObject;
        }
        catch (Exception ex)
        {
            StatusText.Text = "Geräteliste nicht verfügbar: " + ex.Message;
        }
        _loading = true;
        FillDeviceLists();
        _loading = false;
        UpdateEncoderHints();
        UpdateConnectionState();
        if (_devices != null) StatusText.Text = "";
    }

    private void ApplyToUi()
    {
        _loading = true;
        var general = _cfg.Section("general");
        var capture = _cfg.Section("capture");
        var video = _cfg.Section("video");
        var audio = _cfg.Section("audio");
        var replay = _cfg.Section("replay");
        var output = _cfg.Section("output");
        var yt = _cfg.Section("youtube");
        var hk = _cfg.Section("hotkeys");
        var notif = _cfg.Section("notifications");

        // Aufnahme
        bool window = capture.Get("source", "monitor") == "window";
        SrcMonitor.IsChecked = !window;
        SrcWindow.IsChecked = window;
        WindowTitle.Text = capture.Get("windowTitle", "");
        WindowExe.Text = capture.Get("windowExe", "");
        CaptureCursor.IsChecked = capture.Get("captureCursor", true);
        OnSourceChanged(this, new RoutedEventArgs());

        // Video
        SelectTag(Codec, video.Get("codec", "h264"));
        SelectTag(Encoder, video.Get("encoder", "auto"));
        SelectTag(Fps, video.Get("fps", 60).ToString());
        SelectTag(Resolution, $"{video.Get("width", 0)}x{video.Get("height", 0)}");
        if (Resolution.SelectedIndex < 0) Resolution.SelectedIndex = 0;
        SelectTag(RateControl, video.Get("rateControl", "vbr"));
        Bitrate.Value = Math.Round(video.Get("bitrateKbps", 20000) / 1000.0);
        Quality.Value = video.Get("quality", 23);
        Av1Idle.IsChecked = video.Get("av1TranscodeOnlyWhenIdle", true);
        OnRateControlChanged(this, null!);
        OnBitrateChanged(this, null!);
        OnQualityChanged(this, null!);

        // Audio
        AudioSystem.IsChecked = audio.Get("system", true);
        AudioMic.IsChecked = audio.Get("microphone", false);
        SeparateTracks.IsChecked = audio.Get("separateTracks", true);
        SelectTag(AudioBitrate, audio.Get("bitrateKbps", 160).ToString());

        // Replay
        ReplayOnLaunch.IsChecked = general.Get("startReplayOnLaunch", true);
        ReplaySeconds.Value = replay.Get("seconds", 120);
        ReplayDir.Text = replay.Get("bufferDir", "");
        OnReplayChanged(this, null!);

        // Ausgabe
        string mode = output.Get("mode", "local");
        ModeLocal.IsChecked = mode == "local";
        ModeYouTube.IsChecked = mode == "youtube";
        ModeBoth.IsChecked = mode == "both";
        DeleteAfterUpload.IsChecked = output.Get("deleteAfterUpload", false);
        OutputFolder.Text = output.Get("folder", "");
        if (string.IsNullOrEmpty(OutputFolder.Text)) OutputFolder.Text = Core.DefaultVideoDir;
        SelectTag(Container, output.Get("container", "mp4"));
        FileNamePattern.Text = output.Get("fileNamePattern", "LiteCap_{type}_{date}_{time}");
        MinFree.Text = output.Get("minFreeSpaceMB", 2048).ToString();

        // YouTube
        ClientId.Text = yt.Get("clientId", "");
        TitleTemplate.Text = yt.Get("titleTemplate", "Clip {date} {time}");
        Description.Text = yt.Get("description", "Aufgenommen mit LiteCap");
        Tags.Text = yt["tags"] is JsonArray arr ? string.Join(", ", arr.Select(t => t?.GetValue<string>())) : "";
        SelectTag(Privacy, yt.Get("privacy", "private"));
        SelectTag(Category, yt.Get("categoryId", 20).ToString());
        MaxUpload.Text = yt.Get("maxUploadKbps", 0).ToString();
        SecretHint.Text = Credentials.Exists(Credentials.ClientSecret)
            ? "Ein Secret ist sicher im Windows Credential Manager hinterlegt. Feld leer lassen = beibehalten."
            : "Wird verschlüsselt im Windows Credential Manager gespeichert, nicht in der config.json.";

        // Hotkeys
        HkRecord.Text = hk.Get("toggleRecording", "Ctrl+Alt+F9");
        HkSave.Text = hk.Get("saveReplay", "Ctrl+Alt+F10");
        HkReplay.Text = hk.Get("toggleReplay", "Ctrl+Alt+F11");

        // Allgemein
        Autostart.IsChecked = general.Get("autostart", false);
        Notifications.IsChecked = notif.Get("enabled", true);
        NotificationSound.IsChecked = notif.Get("sound", false);
        SelectTag(LogLevel, general.Get("logLevel", "info"));
        _loading = false;
        OnPatternChanged(this, null!);
    }

    private void FillDeviceLists()
    {
        var capture = _cfg.Section("capture");
        var audio = _cfg.Section("audio");

        MonitorList.Items.Clear();
        if (_devices?["monitors"] is JsonArray monitors)
        {
            foreach (var m in monitors.OfType<JsonObject>())
            {
                int idx = m.Get("index", 0);
                string text = $"Monitor {idx + 1} – {m.Get("width", 0)} × {m.Get("height", 0)}" +
                              (m.Get("primary", false) ? " (Hauptbildschirm)" : "");
                MonitorList.Items.Add(new ComboBoxItem { Content = text, Tag = idx });
            }
        }
        if (MonitorList.Items.Count == 0) MonitorList.Items.Add(new ComboBoxItem { Content = "Hauptbildschirm", Tag = 0 });
        int wanted = capture.Get("monitorIndex", 0);
        MonitorList.SelectedIndex = Math.Clamp(wanted, 0, MonitorList.Items.Count - 1);

        string title = WindowTitle.Text;
        WindowTitle.Items.Clear();
        if (_devices?["windows"] is JsonArray windows)
            foreach (var w in windows.OfType<JsonObject>())
                WindowTitle.Items.Add(new ComboBoxItem
                {
                    Content = $"{w.Get("title", "")}  ({w.Get("exe", "")})",
                    Tag = w,
                });
        WindowTitle.Text = title;

        MicList.Items.Clear();
        MicList.Items.Add(new ComboBoxItem { Content = "Standard-Mikrofon von Windows", Tag = "" });
        if (_devices?["microphones"] is JsonArray mics)
            foreach (var m in mics.OfType<JsonObject>())
                MicList.Items.Add(new ComboBoxItem { Content = m.Get("name", "?"), Tag = m.Get("id", "") });
        SelectTag(MicList, audio.Get("microphoneDeviceId", ""));
        if (MicList.SelectedIndex < 0) MicList.SelectedIndex = 0;
    }

    // ------------------------------------------------------------------ Speichern

    private bool CollectFromUi()
    {
        var general = _cfg.Section("general");
        var capture = _cfg.Section("capture");
        var video = _cfg.Section("video");
        var audio = _cfg.Section("audio");
        var replay = _cfg.Section("replay");
        var output = _cfg.Section("output");
        var yt = _cfg.Section("youtube");
        var hk = _cfg.Section("hotkeys");
        var notif = _cfg.Section("notifications");

        if (!int.TryParse(MinFree.Text, out int minFree) || minFree < 256)
            return Fail("„Mindestens frei lassen“ muss eine Zahl ≥ 256 sein.");
        if (!int.TryParse(MaxUpload.Text, out int maxUpload) || maxUpload < 0)
            return Fail("Die Upload-Begrenzung muss eine Zahl ≥ 0 sein.");
        if (SrcWindow.IsChecked == true && string.IsNullOrWhiteSpace(WindowTitle.Text) && string.IsNullOrWhiteSpace(WindowExe.Text))
            return Fail("Für die Fenster-Aufnahme bitte einen Fenstertitel oder ein Programm angeben.");
        var hotkeys = new[] { HkRecord.Text, HkSave.Text, HkReplay.Text }.Where(h => h.Length > 0).ToList();
        if (hotkeys.Count != hotkeys.Distinct().Count())
            return Fail("Zwei Aktionen haben dasselbe Tastenkürzel.");

        general["startReplayOnLaunch"] = ReplayOnLaunch.IsChecked == true;
        general["autostart"] = Autostart.IsChecked == true;
        general["logLevel"] = TagOf(LogLevel, "info");

        capture["source"] = SrcWindow.IsChecked == true ? "window" : "monitor";
        capture["monitorIndex"] = MonitorList.SelectedItem is ComboBoxItem mi ? (int)mi.Tag : 0;
        capture["windowTitle"] = WindowTitle.Text.Trim();
        capture["windowExe"] = WindowExe.Text.Trim();
        capture["captureCursor"] = CaptureCursor.IsChecked == true;

        video["codec"] = TagOf(Codec, "h264");
        video["encoder"] = TagOf(Encoder, "auto");
        video["fps"] = int.Parse(TagOf(Fps, "60"));
        var res = TagOf(Resolution, "0x0").Split('x');
        video["width"] = int.Parse(res[0]);
        video["height"] = int.Parse(res[1]);
        video["rateControl"] = TagOf(RateControl, "vbr");
        video["bitrateKbps"] = (int)Bitrate.Value * 1000;
        video["quality"] = (int)Quality.Value;
        video["av1TranscodeOnlyWhenIdle"] = Av1Idle.IsChecked == true;

        audio["system"] = AudioSystem.IsChecked == true;
        audio["microphone"] = AudioMic.IsChecked == true;
        audio["microphoneDeviceId"] = MicList.SelectedItem is ComboBoxItem mic ? (string)mic.Tag : "";
        audio["separateTracks"] = SeparateTracks.IsChecked == true;
        audio["bitrateKbps"] = int.Parse(TagOf(AudioBitrate, "160"));

        replay["seconds"] = (int)ReplaySeconds.Value;
        replay["bufferDir"] = ReplayDir.Text.Trim();

        output["mode"] = ModeYouTube.IsChecked == true ? "youtube" : ModeBoth.IsChecked == true ? "both" : "local";
        output["deleteAfterUpload"] = DeleteAfterUpload.IsChecked == true;
        string folder = OutputFolder.Text.Trim();
        output["folder"] = string.Equals(folder, Core.DefaultVideoDir, StringComparison.OrdinalIgnoreCase) ? "" : folder;
        output["container"] = TagOf(Container, "mp4");
        output["fileNamePattern"] = FileNamePattern.Text.Trim();
        output["minFreeSpaceMB"] = minFree;

        yt["clientId"] = ClientId.Text.Trim();
        yt["titleTemplate"] = TitleTemplate.Text;
        yt["description"] = Description.Text;
        yt["tags"] = new JsonArray(Tags.Text.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                                             .Select(t => (JsonNode)t!).ToArray());
        yt["privacy"] = TagOf(Privacy, "private");
        yt["categoryId"] = int.Parse(TagOf(Category, "20"));
        yt["maxUploadKbps"] = maxUpload;

        hk["toggleRecording"] = HkRecord.Text;
        hk["saveReplay"] = HkSave.Text;
        hk["toggleReplay"] = HkReplay.Text;

        notif["enabled"] = Notifications.IsChecked == true;
        notif["sound"] = NotificationSound.IsChecked == true;
        _cfg["version"] = 1;
        return true;
    }

    private bool Save()
    {
        if (!CollectFromUi()) return false;
        try
        {
            if (ClientSecret.Password.Length > 0)
            {
                Credentials.Write(Credentials.ClientSecret, ClientSecret.Password.Trim());
                ClientSecret.Clear();
                SecretHint.Text = "Secret gespeichert (Windows Credential Manager).";
            }
            Core.SaveConfig(_cfg);
            CoreWindow.RequestReload();  // der Kern überwacht die Datei ohnehin; so geht's sofort
            StatusText.Text = $"Gespeichert ({DateTime.Now:HH:mm:ss}).";
            return true;
        }
        catch (Exception ex)
        {
            return Fail("Speichern fehlgeschlagen: " + ex.Message);
        }
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        if (Save()) Close();
    }

    private void OnApply(object sender, RoutedEventArgs e) => Save();
    private void OnCancel(object sender, RoutedEventArgs e) => Close();

    // ------------------------------------------------------------------ Reaktionen

    private void OnSourceChanged(object sender, RoutedEventArgs e)
    {
        if (WindowPanel == null) return;
        WindowPanel.IsEnabled = SrcWindow.IsChecked == true;
        MonitorList.IsEnabled = SrcMonitor.IsChecked == true;
    }

    private void OnWindowPicked(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || WindowTitle.SelectedItem is not ComboBoxItem { Tag: JsonObject w }) return;
        Dispatcher.BeginInvoke(() =>
        {
            WindowTitle.Text = w.Get("title", "");
            WindowExe.Text = w.Get("exe", "");
        });
    }

    private void OnCodecChanged(object sender, SelectionChangedEventArgs e) => UpdateEncoderHints();

    private void UpdateEncoderHints()
    {
        if (EncoderHint == null || CodecHint == null) return;
        if (_devices?["encoders"] is not JsonArray encoders)
        {
            CodecHint.Text = "";
            return;
        }
        var list = encoders.OfType<JsonObject>().ToList();
        string codec = TagOf(Codec, "h264");
        bool Has(string c, string? enc = null) =>
            list.Any(x => x.Get("codec", "") == c && x.Get("available", false) && x.Get("encoder", "") != "x264" &&
                          (enc == null || x.Get("encoder", "") == enc));

        var hw = list.Where(x => x.Get("available", false) && x.Get("encoder", "") != "x264")
                     .GroupBy(x => x.Get("encoder", ""))
                     .Select(g => $"{g.Key.ToUpperInvariant()} ({g.First().Get("gpu", "")}): {string.Join(", ", g.Select(x => x.Get("codec", "")))}");
        EncoderHint.Text = "Verfügbar: " + (hw.Any() ? string.Join(" · ", hw) : "kein Hardware-Encoder – es wird x264 verwendet");

        CodecHint.Text = codec switch
        {
            "av1" when !Has("av1") => Has("hevc")
                ? "Deine GPU kann AV1 nicht live kodieren: Aufnahme in HEVC, danach automatische Umwandlung nach AV1 im Hintergrund."
                : "Kein AV1/HEVC-Hardware-Encoder: Aufnahme in H.264, danach Umwandlung nach AV1.",
            "hevc" when !Has("hevc") => "Kein HEVC-Hardware-Encoder gefunden – es wird H.264 verwendet.",
            _ => "",
        };
    }

    private void OnRateControlChanged(object sender, SelectionChangedEventArgs e)
    {
        if (BitratePanel == null) return;
        bool cqp = TagOf(RateControl, "vbr") == "cqp";
        BitratePanel.Visibility = cqp ? Visibility.Collapsed : Visibility.Visible;
        QualityPanel.Visibility = cqp ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnBitrateChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (BitrateLabel != null) BitrateLabel.Text = $"Bitrate: {Bitrate.Value:0} Mbit/s (≈ {Bitrate.Value * 7.5:0} MB pro Minute)";
        OnReplayChanged(sender, null!);
    }

    private void OnQualityChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (QualityLabel != null) QualityLabel.Text = $"Qualitätsstufe: {Quality.Value:0} (niedriger = besser)";
    }

    private void OnReplayChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (ReplayLabel == null || ReplayHint == null) return;
        int s = (int)ReplaySeconds.Value;
        ReplayLabel.Text = $"Länge: {s / 60}:{s % 60:00} min";
        // Gleiche Abschätzung wie der Kern (ReplayBuffer::Start)
        double mbps = TagOf(RateControl, "vbr") == "cqp" ? 50 : Bitrate.Value;
        double gb = ((mbps * 1000 * 1.5 + 320) * 1000 / 8 * (s + 3) * 1.25 + 32 * 1048576) / 1e9;
        ReplayHint.Text = $"Belegt ca. {gb:0.0} GB Festplatte, aber nur wenige MB Arbeitsspeicher.";
    }

    private void OnPatternChanged(object sender, TextChangedEventArgs e)
    {
        if (PatternPreview == null) return;
        var now = DateTime.Now;
        string name = FileNamePattern.Text.Replace("{date}", now.ToString("yyyy-MM-dd"))
                                          .Replace("{time}", now.ToString("HH-mm-ss"))
                                          .Replace("{source}", "Monitor1")
                                          .Replace("{type}", "Clip");
        PatternPreview.Text = $"Beispiel: {name}.{TagOf(Container, "mp4")}   ·   Platzhalter: {{date}} {{time}} {{source}} {{type}}";
    }

    private void OnBrowseOutput(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "Zielordner wählen", InitialDirectory = OutputFolder.Text };
        if (dlg.ShowDialog(this) == true) OutputFolder.Text = dlg.FolderName;
    }

    private void OnBrowseReplayDir(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "Ordner für die Replay-Datei wählen" };
        if (dlg.ShowDialog(this) == true) ReplayDir.Text = dlg.FolderName;
    }

    // ------------------------------------------------------------------ YouTube

    private void UpdateConnectionState()
    {
        bool connected = Credentials.Exists(Credentials.RefreshToken);
        ConnectionState.Text = connected ? "✔ verbunden" : "nicht verbunden";
        DisconnectButton.IsEnabled = connected;
    }

    private async void OnConnect(object sender, RoutedEventArgs e)
    {
        // Client-ID und Secret müssen vorher gespeichert sein: die Anmeldung läuft im Kern
        if (string.IsNullOrWhiteSpace(ClientId.Text))
        {
            Fail("Bitte zuerst die OAuth-Client-ID eintragen.");
            return;
        }
        if (ClientSecret.Password.Length == 0 && !Credentials.Exists(Credentials.ClientSecret))
        {
            Fail("Bitte zuerst das OAuth-Client-Secret eintragen.");
            return;
        }
        if (!Save()) return;
        ConnectButton.IsEnabled = false;
        ConnectionState.Text = "Anmeldung im Browser …";
        try
        {
            var (code, output) = await Core.RunAsync("--youtube-login");
            if (code != 0) Fail(output.Trim().Length > 0 ? output.Trim() : "Anmeldung fehlgeschlagen (Details im Log).");
            else StatusText.Text = "Mit YouTube verbunden.";
        }
        catch (Exception ex)
        {
            Fail(ex.Message);
        }
        ConnectButton.IsEnabled = true;
        UpdateConnectionState();
    }

    private async void OnDisconnect(object sender, RoutedEventArgs e)
    {
        await Core.RunAsync("--youtube-logout");
        UpdateConnectionState();
        StatusText.Text = "YouTube-Verbindung getrennt.";
    }

    private sealed record UploadRow(string File, string State, string Info);

    private void RefreshUploads()
    {
        try
        {
            if (!File.Exists(Core.UploadQueueFile))
            {
                UploadList.ItemsSource = null;
                return;
            }
            using var fs = new FileStream(Core.UploadQueueFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var arr = JsonNode.Parse(fs) as JsonArray;
            var rows = arr?.OfType<JsonObject>().Reverse().Select(j =>
            {
                string state = j.Get("state", "");
                long sent = j.Get("bytesSent", 0L), total = j.Get("total", 0L);
                string stateText = state switch
                {
                    "done" => "Fertig",
                    "failed" => "Fehlgeschlagen",
                    "uploading" => total > 0 ? $"Lädt … {sent * 100 / total} %" : "Lädt …",
                    _ => total > 0 && sent > 0 ? $"Wartet ({sent * 100 / total} %)" : "Wartet",
                };
                string info = state == "done" ? "https://youtu.be/" + j.Get("videoId", "") : j.Get("error", "");
                return new UploadRow(Path.GetFileName(j.Get("file", "")), stateText, info);
            }).ToList();
            UploadList.ItemsSource = rows;
        }
        catch
        {
            // Datei wird gerade geschrieben → beim nächsten Tick erneut
        }
    }

    // ------------------------------------------------------------------ Allgemein

    private void OnOpenLog(object sender, RoutedEventArgs e)
    {
        if (File.Exists(Core.LogFile)) Process.Start(new ProcessStartInfo(Core.LogFile) { UseShellExecute = true });
    }

    private void OnOpenConfigDir(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(Path.GetDirectoryName(Core.ConfigFile)!) { UseShellExecute = true });

    // ------------------------------------------------------------------ Hilfen

    private static void SelectTag(ComboBox box, string tag)
    {
        foreach (var item in box.Items.OfType<ComboBoxItem>())
            if (string.Equals(item.Tag?.ToString(), tag, StringComparison.OrdinalIgnoreCase))
            {
                box.SelectedItem = item;
                return;
            }
        if (box.SelectedIndex < 0 && box.Items.Count > 0) box.SelectedIndex = 0;
    }

    private static string TagOf(ComboBox box, string fallback) =>
        (box.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? fallback;

    private bool Fail(string message)
    {
        MessageBox.Show(this, message, "LiteCap", MessageBoxButton.OK, MessageBoxImage.Warning);
        return false;
    }
}
