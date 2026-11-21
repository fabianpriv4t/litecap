#include "util/Log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include "util/Strings.h"

namespace litecap::log {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::atomic<Level> g_minLevel = Level::Info;
constexpr long kMaxSize = 5 * 1024 * 1024;

// Ließ sich die Datei nicht öffnen (z. B. beim Autostart direkt nach der Anmeldung), wird es
// beim Schreiben alle paar Sekunden erneut versucht – statt bis zum Programmende nichts zu loggen.
std::filesystem::path g_path;
DWORD g_openError = 0;
ULONGLONG g_lastTry = 0;
constexpr ULONGLONG kRetryMs = 5000;

// Erwartet g_mutex gesperrt.
bool TryOpen() {
  g_lastTry = GetTickCount64();
  std::error_code ec;
  std::filesystem::create_directories(g_path.parent_path(), ec);
  // Keine Sperre: Mehrere LiteCap-Prozesse (Tray, GUI-Hilfsbefehle, AV1-Umwandlung) und
  // Editoren dürfen die Datei gleichzeitig öffnen.
  g_file = _wfsopen(g_path.c_str(), L"ab", _SH_DENYNO);
  if (!g_file) {
    g_openError = GetLastError();
    // Fallback: eigene Datei pro Prozess, damit nie stillschweigend nichts geloggt wird
    auto alt = g_path;
    alt.replace_filename(g_path.stem().wstring() + L"-" + std::to_wstring(GetCurrentProcessId()) +
                         g_path.extension().wstring());
    g_file = _wfsopen(alt.c_str(), L"ab", _SH_DENYNO);
  }
  return g_file != nullptr;
}

const char* LevelName(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
  }
  return "?";
}
} // namespace

void Init(const std::filesystem::path& file, Level minLevel) {
  std::lock_guard lock(g_mutex);
  g_minLevel = minLevel;
  std::error_code ec;
  if (std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > kMaxSize) {
    auto old = file;
    old += L".1";
    std::filesystem::rename(file, old, ec);
  }
  g_path = file;
  g_openError = 0;
  TryOpen();
}

void SetLevel(Level minLevel) {
  std::lock_guard lock(g_mutex);
  g_minLevel = minLevel;
}

void Write(Level level, std::string_view msg) {
  if (level < g_minLevel) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::string line =
      std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] [{}] {}\n", t.wYear, t.wMonth,
                  t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, LevelName(level),
                  GetCurrentThreadId(), msg);
  std::lock_guard lock(g_mutex);
#ifndef NDEBUG
  OutputDebugStringW(Utf8ToWide(line).c_str());
#endif
  if (!g_file && !g_path.empty() && GetTickCount64() - g_lastTry >= kRetryMs) {
    const DWORD firstError = g_openError;
    if (TryOpen()) {
      std::string note = std::format(
          "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [WARN ] [{}] Log-Datei war bisher nicht "
          "verfügbar (Windows-Fehler {}) – frühere Einträge dieses Programmlaufs fehlen\n",
          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
          GetCurrentThreadId(), firstError);
      fwrite(note.data(), 1, note.size(), g_file);
    }
  }
  if (g_file) {
    fwrite(line.data(), 1, line.size(), g_file);
    // Sofort schreiben: Es sind nur wenige Zeilen pro Minute, und nach einem Absturz oder
    // während der Laufzeit muss das Log vollständig lesbar sein.
    fflush(g_file);
  }
}

void Shutdown() {
  std::lock_guard lock(g_mutex);
  g_path.clear();  // nach dem Beenden nicht erneut öffnen
  if (g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
}

} // namespace litecap::log
