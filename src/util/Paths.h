#pragma once

#include <filesystem>
#include <string>

namespace litecap::paths {

// %APPDATA%\LiteCap  (Konfiguration, roamt mit dem Benutzerprofil)
std::filesystem::path AppDataDir();
// %LOCALAPPDATA%\LiteCap  (Logs, Replay-Ringdatei, Upload-Warteschlange)
std::filesystem::path LocalDataDir();

std::filesystem::path ConfigFile();
std::filesystem::path LogDir();
// Standard-Zielordner: Videos\LiteCap
std::filesystem::path DefaultVideoDir();
// Verzeichnis der laufenden .exe
std::filesystem::path ExeDir();

// Ersetzt %VARIABLEN% (z. B. %USERPROFILE%).
std::wstring ExpandEnv(const std::wstring& s);

} // namespace litecap::paths
