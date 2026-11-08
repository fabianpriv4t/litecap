using System.Windows;

namespace LiteCap.Settings;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // Nur ein Einstellungsfenster gleichzeitig
        _mutex = new Mutex(true, @"Local\LiteCap.Settings", out bool created);
        if (!created)
        {
            Shutdown();
            return;
        }
        base.OnStartup(e);
    }

    private Mutex? _mutex;
}
