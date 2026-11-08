using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace LiteCap.Settings;

/// <summary>Pfade und Hilfsbefehle des C++-Kerns.</summary>
internal static class Core
{
    public static string ConfigFile =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "LiteCap", "config.json");

    public static string LocalDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LiteCap");

    public static string LogFile => Path.Combine(LocalDir, "logs", "litecap.log");
    public static string UploadQueueFile => Path.Combine(LocalDir, "uploads.json");

    public static string DefaultVideoDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "LiteCap");

    /// <summary>litecap.exe liegt im selben Ordner wie diese GUI.</summary>
    public static string Exe => Path.Combine(AppContext.BaseDirectory, "litecap.exe");

    /// <summary>Führt "litecap.exe &lt;args&gt;" aus und liefert (Exitcode, stdout).</summary>
    public static async Task<(int Code, string Output)> RunAsync(string args, CancellationToken ct = default)
    {
        var psi = new ProcessStartInfo(Exe, args)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
        };
        using var p = Process.Start(psi) ?? throw new InvalidOperationException("litecap.exe nicht gefunden");
        string output = await p.StandardOutput.ReadToEndAsync(ct);
        await p.WaitForExitAsync(ct);
        return (p.ExitCode, output);
    }

    public static JsonObject LoadConfig()
    {
        if (!File.Exists(ConfigFile)) return new JsonObject();
        var options = new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true };
        return JsonNode.Parse(File.ReadAllText(ConfigFile), documentOptions: options) as JsonObject ?? new JsonObject();
    }

    /// <summary>Atomar speichern (temp-Datei + Ersetzen), damit der Kern nie eine halbe Datei liest.</summary>
    public static void SaveConfig(JsonObject cfg)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(ConfigFile)!);
        string tmp = ConfigFile + ".tmp";
        File.WriteAllText(tmp, cfg.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        }), new UTF8Encoding(false));
        File.Move(tmp, ConfigFile, overwrite: true);
    }
}

/// <summary>Bequemer Zugriff auf verschachtelte JSON-Werte ("video.fps").</summary>
internal static class JsonExt
{
    public static JsonObject Section(this JsonObject root, string name)
    {
        if (root[name] is JsonObject o) return o;
        var n = new JsonObject();
        root[name] = n;
        return n;
    }

    public static T Get<T>(this JsonObject o, string key, T fallback)
    {
        try
        {
            return o[key] is JsonNode n ? n.GetValue<T>() : fallback;
        }
        catch
        {
            return fallback;
        }
    }
}
