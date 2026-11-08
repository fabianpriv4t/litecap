#include "util/Strings.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace litecap {

std::wstring Utf8ToWide(std::string_view s) {
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
  return out;
}

std::string WideToUtf8(std::wstring_view s) {
  if (s.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr,
                      nullptr);
  return out;
}

static std::string SystemMessage(DWORD code) {
  wchar_t* buf = nullptr;
  DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::string msg;
  if (n && buf) {
    msg = WideToUtf8(std::wstring_view(buf, n));
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
      msg.pop_back();
  }
  if (buf) LocalFree(buf);
  return msg;
}

std::string HResultMessage(HRESULT hr) {
  std::string msg = SystemMessage(static_cast<DWORD>(hr));
  return std::format("0x{:08X}{}{}", static_cast<unsigned>(hr), msg.empty() ? "" : " – ", msg);
}

std::string LastErrorMessage(DWORD err) {
  std::string msg = SystemMessage(err);
  return std::format("Fehler {}{}{}", err, msg.empty() ? "" : " – ", msg);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

} // namespace litecap
