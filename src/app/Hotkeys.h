#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace litecap {

struct Hotkey {
  UINT modifiers = 0;  // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
  UINT vk = 0;
};

// Parst z. B. "Ctrl+Alt+F9", "Alt+Shift+R", "Win+PrintScreen". Groß-/Kleinschreibung egal.
std::optional<Hotkey> ParseHotkey(const std::string& text);

enum class HotkeyAction : int { ToggleRecording = 1, SaveReplay = 2, ToggleReplay = 3 };

// Registriert globale Hotkeys (RegisterHotKey) für ein Fenster. Ereignisse kommen als WM_HOTKEY
// mit wParam = HotkeyAction. Kein Keyboard-Hook → keine Last pro Tastendruck.
class Hotkeys {
 public:
  explicit Hotkeys(HWND hwnd) : hwnd_(hwnd) {}
  ~Hotkeys() { UnregisterAll(); }
  Hotkeys(const Hotkeys&) = delete;
  Hotkeys& operator=(const Hotkeys&) = delete;

  // Liefert eine Fehlermeldung, wenn die Kombination ungültig oder schon belegt ist.
  std::optional<std::string> Register(HotkeyAction action, const std::string& text);
  void UnregisterAll();

 private:
  HWND hwnd_;
  std::vector<int> registered_;
};

} // namespace litecap
