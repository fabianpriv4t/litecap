#pragma once

#include <string>
#include <string_view>
#include <windows.h>

namespace litecap {

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

// Lesbare Fehlermeldung für HRESULT / GetLastError (UTF-8).
std::string HResultMessage(HRESULT hr);
std::string LastErrorMessage(DWORD err = GetLastError());

std::string ToLower(std::string s);

} // namespace litecap
