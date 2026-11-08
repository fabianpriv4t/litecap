#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"

namespace litecap {

struct CaptureTarget {
  HMONITOR monitor = nullptr;  // gesetzt bei Monitor-Aufnahme
  HWND window = nullptr;       // gesetzt bei Fenster-Aufnahme
  std::wstring name;           // für Dateinamen ({source}) und Log
  // Monitor, auf dem das Ziel liegt (bei Fenstern: der Monitor mit der größten Überlappung)
  HMONITOR HostMonitor() const;
};

struct MonitorInfo {
  HMONITOR handle;
  std::wstring deviceName;  // z. B. \\.\DISPLAY1
  RECT rect;
  bool primary;
};

struct WindowInfo {
  HWND handle;
  std::wstring title;
  std::wstring exeName;  // z. B. game.exe
};

// Primärer Monitor zuerst, danach in Windows-Reihenfolge.
std::vector<MonitorInfo> EnumerateMonitors();
// Sichtbare, aufnehmbare Top-Level-Fenster.
std::vector<WindowInfo> EnumerateWindows();

// Ermittelt das Ziel aus der Konfiguration; nullopt mit Grund in `error`, falls nicht gefunden.
std::optional<CaptureTarget> ResolveTarget(const CaptureConfig& cfg, std::string& error);

} // namespace litecap
