#include "capture/CaptureTarget.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "util/Strings.h"

namespace litecap {

HMONITOR CaptureTarget::HostMonitor() const {
  if (monitor) return monitor;
  if (window) return MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

std::vector<MonitorInfo> EnumerateMonitors() {
  std::vector<MonitorInfo> list;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi)) {
          reinterpret_cast<std::vector<MonitorInfo>*>(lp)->push_back(
              {m, mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&list));
  std::stable_partition(list.begin(), list.end(), [](const MonitorInfo& m) { return m.primary; });
  return list;
}

static std::wstring ExeNameOf(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return {};
  wchar_t buf[MAX_PATH * 2];
  DWORD n = static_cast<DWORD>(std::size(buf));
  std::wstring name;
  if (QueryFullProcessImageNameW(proc, 0, buf, &n))
    name = std::filesystem::path(std::wstring(buf, n)).filename().wstring();
  CloseHandle(proc);
  return name;
}

static bool IsCapturableWindow(HWND hwnd) {
  if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (ex & WS_EX_TOOLWINDOW) return false;
  if (GetWindowTextLengthW(hwnd) == 0) return false;
  // Unsichtbare UWP-Hüllen ("cloaked") ausblenden
  DWORD cloaked = 0;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  return cloaked == 0;
}

std::vector<WindowInfo> EnumerateWindows() {
  std::vector<WindowInfo> list;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsCapturableWindow(hwnd)) return TRUE;
        wchar_t title[512];
        int n = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        reinterpret_cast<std::vector<WindowInfo>*>(lp)->push_back(
            {hwnd, std::wstring(title, static_cast<size_t>(n)), ExeNameOf(hwnd)});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&list));
  return list;
}

static std::wstring Lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
  return s;
}

std::optional<CaptureTarget> ResolveTarget(const CaptureConfig& cfg, std::string& error) {
  if (cfg.source == "window") {
    std::wstring wantTitle = Lower(Utf8ToWide(cfg.windowTitle));
    std::wstring wantExe = Lower(Utf8ToWide(cfg.windowExe));
    if (wantTitle.empty() && wantExe.empty()) {
      error = "Fenster-Aufnahme gewählt, aber weder Fenstertitel noch Programm angegeben";
      return std::nullopt;
    }
    // EnumWindows liefert in Z-Reihenfolge → das oberste passende Fenster gewinnt.
    for (const auto& w : EnumerateWindows()) {
      bool exeOk = wantExe.empty() || Lower(w.exeName) == wantExe;
      bool titleOk = wantTitle.empty() || Lower(w.title).find(wantTitle) != std::wstring::npos;
      if (exeOk && titleOk) return CaptureTarget{nullptr, w.handle, w.title};
    }
    error = "Kein passendes Fenster gefunden (Titel \"" + cfg.windowTitle + "\", Programm \"" +
            cfg.windowExe + "\")";
    return std::nullopt;
  }

  auto monitors = EnumerateMonitors();
  if (monitors.empty()) {
    error = "Kein Monitor gefunden";
    return std::nullopt;
  }
  size_t idx = static_cast<size_t>(cfg.monitorIndex);
  if (idx >= monitors.size()) idx = 0;  // Monitor abgesteckt → primären verwenden
  return CaptureTarget{monitors[idx].handle, nullptr,
                       L"Monitor" + std::to_wstring(idx + 1)};
}

} // namespace litecap
