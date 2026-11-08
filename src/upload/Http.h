#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace litecap {

// Netzwerkfehler (keine Verbindung, Timeout …) – im Gegensatz zu HTTP-Fehlercodes.
class NetworkError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct HttpResponse {
  int status = 0;
  std::string body;
  std::map<std::wstring, std::wstring> headers;  // Schlüssel klein geschrieben
  std::wstring Header(const std::wstring& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? L"" : it->second;
  }
};

using HttpHeaders = std::vector<std::pair<std::wstring, std::wstring>>;

// Schlanker synchroner HTTPS-Client auf Basis von WinHTTP (im System enthalten, nutzt die
// Windows-Proxy-Einstellungen und den Windows-Zertifikatsspeicher).
class Http {
 public:
  Http();
  ~Http();
  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  HttpResponse Send(const std::wstring& method, const std::wstring& url, const HttpHeaders& headers,
                    const std::string& body = {});

  // Sendet length Bytes ab offset aus einer Datei als Request-Body.
  // progress(bytesSentInThisRequest) → false bricht ab. maxKbps = 0: unbegrenzt.
  HttpResponse SendFile(const std::wstring& method, const std::wstring& url,
                        const HttpHeaders& headers, HANDLE file, uint64_t offset, uint64_t length,
                        const std::function<bool(uint64_t)>& progress, int maxKbps);

 private:
  HttpResponse Request(const std::wstring& method, const std::wstring& url,
                       const HttpHeaders& headers, uint64_t bodyLength,
                       const std::function<void(void*)>& writeBody);  // void* = HINTERNET
  void* session_ = nullptr;
};

// Hilfen
std::string UrlEncode(const std::string& s);
std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields);

} // namespace litecap
