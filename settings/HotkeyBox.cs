using System.Windows.Controls;
using System.Windows.Input;

namespace LiteCap.Settings;

/// <summary>
/// Textfeld, das die gedrückte Tastenkombination erfasst und im Format des Kerns anzeigt
/// (z. B. "Ctrl+Alt+F9"). Entf/Rücktaste ohne Modifizierer = Hotkey löschen.
/// </summary>
public sealed class HotkeyBox : TextBox
{
    public HotkeyBox()
    {
        IsReadOnly = true;
        IsReadOnlyCaretVisible = false;
        Cursor = Cursors.Hand;
        ToolTip = "Klicken und die gewünschte Tastenkombination drücken. Entf löscht.";
    }

    protected override void OnPreviewKeyDown(KeyEventArgs e)
    {
        e.Handled = true;
        Key key = e.Key == Key.System ? e.SystemKey : e.Key;
        var mods = Keyboard.Modifiers;

        if (mods == ModifierKeys.None && key is Key.Delete or Key.Back)
        {
            Text = "";
            return;
        }
        // Reine Modifizierer abwarten
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt or Key.LeftShift
            or Key.RightShift or Key.LWin or Key.RWin)
            return;

        string? name = KeyName(key);
        if (name is null) return;

        var parts = new List<string>();
        if (mods.HasFlag(ModifierKeys.Control)) parts.Add("Ctrl");
        if (mods.HasFlag(ModifierKeys.Alt)) parts.Add("Alt");
        if (mods.HasFlag(ModifierKeys.Shift)) parts.Add("Shift");
        if (mods.HasFlag(ModifierKeys.Windows)) parts.Add("Win");
        // Ohne Modifizierer nur F-Tasten/Sondertasten erlauben (sonst kann man nicht mehr tippen)
        if (parts.Count == 0 && !(name.StartsWith('F') && name.Length > 1) && name is not ("PrintScreen" or "Pause" or "ScrollLock"))
            return;
        parts.Add(name);
        Text = string.Join("+", parts);
    }

    /// <summary>Tastennamen exakt wie im C++-Parser (app/Hotkeys.cpp).</summary>
    private static string? KeyName(Key k)
    {
        if (k >= Key.A && k <= Key.Z) return k.ToString();
        if (k >= Key.D0 && k <= Key.D9) return ((char)('0' + (k - Key.D0))).ToString();
        if (k >= Key.F1 && k <= Key.F24) return "F" + (k - Key.F1 + 1);
        if (k >= Key.NumPad0 && k <= Key.NumPad9) return "Num" + (k - Key.NumPad0);
        return k switch
        {
            Key.Snapshot => "PrintScreen",
            Key.Pause => "Pause",
            Key.Scroll => "ScrollLock",
            Key.Insert => "Insert",
            Key.Delete => "Delete",
            Key.Home => "Home",
            Key.End => "End",
            Key.PageUp => "PageUp",
            Key.PageDown => "PageDown",
            Key.Space => "Space",
            Key.Enter => "Enter",
            Key.Tab => "Tab",
            Key.Up => "Up",
            Key.Down => "Down",
            Key.Left => "Left",
            Key.Right => "Right",
            Key.Multiply => "NumMultiply",
            Key.Add => "NumAdd",
            Key.Subtract => "NumSubtract",
            Key.Divide => "NumDivide",
            Key.Decimal => "NumDecimal",
            _ => null,
        };
    }
}
