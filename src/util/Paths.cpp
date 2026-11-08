#include "util/Paths.h"

#include <windows.h>
#include <shlobj.h>

namespace litecap::paths {

namespace fs = std::filesystem;

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) result = p;
  CoTaskMemFree(p);
  return result;
}

static fs::path EnsureDir(fs::path p) {
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

fs::path AppDataDir() { return EnsureDir(KnownFolder(FOLDERID_RoamingAppData) / L"LiteCap"); }
fs::path LocalDataDir() { return EnsureDir(KnownFolder(FOLDERID_LocalAppData) / L"LiteCap"); }
fs::path ConfigFile() { return AppDataDir() / L"config.json"; }
fs::path LogDir() { return EnsureDir(LocalDataDir() / L"logs"); }
fs::path DefaultVideoDir() { return KnownFolder(FOLDERID_Videos) / L"LiteCap"; }

fs::path ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return fs::path(std::wstring(buf, n)).parent_path();
}

std::wstring ExpandEnv(const std::wstring& s) {
  DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
  if (n == 0) return s;
  std::wstring out(n, L'\0');
  ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
  out.resize(n - 1);
  return out;
}

} // namespace litecap::paths
