#include "output/FileNamer.h"

#include <windows.h>

#include <format>

#include "util/Strings.h"

namespace litecap {

namespace fs = std::filesystem;

static void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to) {
  for (size_t pos = 0; (pos = s.find(from, pos)) != std::wstring::npos; pos += to.size())
    s.replace(pos, from.size(), to);
}

static std::wstring Expand(const std::wstring& tmpl, const std::wstring& source,
                           const std::wstring& type, bool forFileName) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::wstring s = tmpl;
  ReplaceAll(s, L"{date}", std::format(L"{:04}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay));
  // Doppelpunkte sind in Dateinamen verboten
  ReplaceAll(s, L"{time}", forFileName
                               ? std::format(L"{:02}-{:02}-{:02}", t.wHour, t.wMinute, t.wSecond)
                               : std::format(L"{:02}:{:02}:{:02}", t.wHour, t.wMinute, t.wSecond));
  ReplaceAll(s, L"{source}", source);
  ReplaceAll(s, L"{type}", type);
  return s;
}

static std::wstring SanitizeFileName(std::wstring s) {
  for (wchar_t& c : s)
    if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
  while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
  if (s.size() > 150) s.resize(150);
  return s.empty() ? L"LiteCap" : s;
}

fs::path MakeOutputPath(const Config& cfg, const std::wstring& source, const std::wstring& type) {
  fs::path dir = cfg.OutputFolder();
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) throw std::runtime_error("Zielordner kann nicht angelegt werden: " + WideToUtf8(dir.wstring()));

  std::wstring base =
      SanitizeFileName(Expand(Utf8ToWide(cfg.output.fileNamePattern), source, type, true));
  std::wstring ext = L"." + Utf8ToWide(cfg.output.container);
  fs::path p = dir / (base + ext);
  for (int i = 2; fs::exists(p, ec) && i < 1000; ++i)
    p = dir / std::format(L"{} ({}){}", base, i, ext);
  return p;
}

uint64_t FreeDiskBytes(const fs::path& dir) {
  ULARGE_INTEGER free{};
  fs::path probe = dir;
  std::error_code ec;
  // Ordner existiert evtl. noch nicht → nächsten existierenden Elternordner prüfen
  while (!probe.empty() && !fs::exists(probe, ec) && probe != probe.root_path())
    probe = probe.parent_path();
  if (!GetDiskFreeSpaceExW(probe.c_str(), &free, nullptr, nullptr)) return 0;
  return free.QuadPart;
}

std::string ExpandTemplate(const std::string& tmpl, const std::wstring& source,
                           const std::wstring& type) {
  return WideToUtf8(Expand(Utf8ToWide(tmpl), source, type, false));
}

} // namespace litecap
