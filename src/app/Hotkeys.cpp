#include "app/Hotkeys.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

#include "util/Strings.h"

namespace litecap {

static std::optional<UINT> KeyFromName(const std::string& k) {
  if (k.size() == 1) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<UINT>(c);
  }
  if (k.size() >= 2 && k[0] == 'f') {
    int n = std::atoi(k.c_str() + 1);
    if (n >= 1 && n <= 24) return static_cast<UINT>(VK_F1 + n - 1);
  }
  if (k.rfind("num", 0) == 0 && k.size() == 4 && k[3] >= '0' && k[3] <= '9')
    return static_cast<UINT>(VK_NUMPAD0 + (k[3] - '0'));

  static const std::unordered_map<std::string, UINT> named = {
      {"printscreen", VK_SNAPSHOT}, {"print", VK_SNAPSHOT},  {"pause", VK_PAUSE},
      {"scrolllock", VK_SCROLL},    {"insert", VK_INSERT},   {"ins", VK_INSERT},
      {"delete", VK_DELETE},        {"del", VK_DELETE},      {"home", VK_HOME},
      {"end", VK_END},              {"pageup", VK_PRIOR},    {"pagedown", VK_NEXT},
      {"space", VK_SPACE},          {"enter", VK_RETURN},    {"tab", VK_TAB},
      {"up", VK_UP},                {"down", VK_DOWN},       {"left", VK_LEFT},
      {"right", VK_RIGHT},          {"nummultiply", VK_MULTIPLY}, {"numadd", VK_ADD},
      {"numsubtract", VK_SUBTRACT}, {"numdivide", VK_DIVIDE},     {"numdecimal", VK_DECIMAL},
  };
  if (auto it = named.find(k); it != named.end()) return it->second;
  return std::nullopt;
}

std::optional<Hotkey> ParseHotkey(const std::string& text) {
  Hotkey hk;
  std::stringstream ss(ToLower(text));
  std::string part;
  bool haveKey = false;
  while (std::getline(ss, part, '+')) {
    // Leerzeichen entfernen
    std::erase_if(part, [](char c) { return c == ' ' || c == '\t'; });
    if (part.empty()) return std::nullopt;
    if (part == "ctrl" || part == "control" || part == "strg") hk.modifiers |= MOD_CONTROL;
    else if (part == "alt") hk.modifiers |= MOD_ALT;
    else if (part == "shift" || part == "umschalt") hk.modifiers |= MOD_SHIFT;
    else if (part == "win") hk.modifiers |= MOD_WIN;
    else {
      if (haveKey) return std::nullopt;  // zwei Haupttasten
      auto vk = KeyFromName(part);
      if (!vk) return std::nullopt;
      hk.vk = *vk;
      haveKey = true;
    }
  }
  if (!haveKey) return std::nullopt;
  return hk;
}

std::optional<std::string> Hotkeys::Register(HotkeyAction action, const std::string& text) {
  if (text.empty()) return std::nullopt;  // leer = deaktiviert
  auto hk = ParseHotkey(text);
  if (!hk) return "Ungültige Tastenkombination: \"" + text + "\"";
  int id = static_cast<int>(action);
  // MOD_NOREPEAT: gedrückt halten löst nicht mehrfach aus.
  if (!RegisterHotKey(hwnd_, id, hk->modifiers | MOD_NOREPEAT, hk->vk)) {
    DWORD err = GetLastError();
    if (err == ERROR_HOTKEY_ALREADY_REGISTERED)
      return "\"" + text + "\" wird bereits von einem anderen Programm verwendet";
    return "Hotkey \"" + text + "\" konnte nicht registriert werden: " + LastErrorMessage(err);
  }
  registered_.push_back(id);
  return std::nullopt;
}

void Hotkeys::UnregisterAll() {
  for (int id : registered_) UnregisterHotKey(hwnd_, id);
  registered_.clear();
}

} // namespace litecap
