#include "upload/Http.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "util/Strings.h"

namespace litecap {

namespace {

[[noreturn]] void ThrowNet(const char* what) {
  DWORD err = GetLastError();
  std::string msg = std::string(what) + ": ";
  switch (err) {
    case ERROR_WINHTTP_TIMEOUT: msg += "Zeitüberschreitung"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: msg += "Server nicht gefunden (keine Internetverbindung?)"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: msg += "Verbindung fehlgeschlagen"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: msg += "Verbindung abgebrochen"; break;
    case ERROR_WINHTTP_OPERATION_CANCELLED: msg += "abgebrochen"; break;
    default: msg += "WinHTTP-Fehler " + std::to_string(err);
  }
  throw NetworkError(msg);
}

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

} // namespace

Http::Http() {
  session_ = WinHttpOpen(L"LiteCap/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_)  // vor Windows 8.1: Standard-Proxy
    session_ = WinHttpOpen(L"LiteCap/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_) ThrowNet("WinHttpOpen");
  // Auflösen/Verbinden 30 s, Senden/Empfangen 120 s
  WinHttpSetTimeouts(session_, 30'000, 30'000, 120'000, 120'000);
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // ältere Windows-Versionen ohne TLS 1.3
    WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
}

Http::~Http() {
  if (session_) WinHttpCloseHandle(session_);
}

HttpResponse Http::Request(const std::wstring& method, const std::wstring& url,
                           const HttpHeaders& headers, uint64_t bodyLength,
                           const std::function<void(void*)>& writeBody) {
  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256], path[4096];
  uc.lpszHostName = host;
  uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  uc.dwExtraInfoLength = 1;  // Query an den Pfad anhängen lassen
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) ThrowNet("Ungültige URL");
  std::wstring fullPath(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.lpszExtraInfo) fullPath.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

  Handle conn{WinHttpConnect(session_, host, uc.nPort, 0)};
  if (!conn.h) ThrowNet("Verbinden");
  Handle req{WinHttpOpenRequest(conn.h, method.c_str(), fullPath.c_str(), nullptr,
                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
  if (!req.h) ThrowNet("Anfrage anlegen");
  // Keine automatischen Weiterleitungen: Googles resumable Upload antwortet mit 308 "Resume
  // Incomplete" (ohne Location-Header). WinHTTP hält das sonst für "308 Permanent Redirect",
  // versucht zu folgen und scheitert mit ERROR_WINHTTP_REDIRECT_FAILED (12156) – der Aufrufer
  // muss den Status 308 selbst sehen.
  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                        sizeof(redirectPolicy)))
    ThrowNet("Weiterleitungen abschalten");

  std::wstring hdr;
  for (const auto& [k, v] : headers) hdr += k + L": " + v + L"\r\n";
  // Content-Length immer explizit (auch 0), sonst lehnt Google PUT-Anfragen ab
  if (!WinHttpSendRequest(req.h, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
                          hdr.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0,
                          bodyLength > 0xFFFFFFFFull ? WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH
                                                     : static_cast<DWORD>(bodyLength),
                          0))
    ThrowNet("Senden");
  if (writeBody) writeBody(req.h);
  if (!WinHttpReceiveResponse(req.h, nullptr)) ThrowNet("Antwort empfangen");

  HttpResponse resp;
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  resp.status = static_cast<int>(status);

  // Alle Header als Text holen und zerlegen
  DWORD len = 0;
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                      &len, WINHTTP_NO_HEADER_INDEX);
  if (len) {
    std::wstring raw(len / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            raw.data(), &len, WINHTTP_NO_HEADER_INDEX)) {
      size_t pos = 0;
      while (pos < raw.size()) {
        size_t end = raw.find(L"\r\n", pos);
        if (end == std::wstring::npos) end = raw.size();
        std::wstring line = raw.substr(pos, end - pos);
        size_t colon = line.find(L':');
        if (colon != std::wstring::npos) {
          std::wstring key = line.substr(0, colon);
          std::transform(key.begin(), key.end(), key.begin(), ::towlower);
          size_t vstart = line.find_first_not_of(L' ', colon + 1);
          resp.headers[key] = vstart == std::wstring::npos ? L"" : line.substr(vstart);
        }
        pos = end + 2;
      }
    }
  }

  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req.h, &avail)) ThrowNet("Antwort lesen");
    if (avail == 0) break;
    size_t old = resp.body.size();
    resp.body.resize(old + avail);
    DWORD read = 0;
    if (!WinHttpReadData(req.h, resp.body.data() + old, avail, &read)) ThrowNet("Antwort lesen");
    resp.body.resize(old + read);
  }
  return resp;
}

HttpResponse Http::Send(const std::wstring& method, const std::wstring& url,
                        const HttpHeaders& headers, const std::string& body) {
  return Request(method, url, headers, body.size(), [&](HINTERNET req) {
    size_t done = 0;
    while (done < body.size()) {
      DWORD written = 0;
      if (!WinHttpWriteData(req, body.data() + done, static_cast<DWORD>(body.size() - done),
                            &written))
        ThrowNet("Daten senden");
      done += written;
    }
  });
}

HttpResponse Http::SendFile(const std::wstring& method, const std::wstring& url,
                            const HttpHeaders& headers, HANDLE file, uint64_t offset,
                            uint64_t length, const std::function<bool(uint64_t)>& progress,
                            int maxKbps) {
  return Request(method, url, headers, length, [&](HINTERNET req) {
    constexpr DWORD kPiece = 256 * 1024;
    std::vector<char> buf(kPiece);
    uint64_t sent = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (sent < length) {
      DWORD want = static_cast<DWORD>(std::min<uint64_t>(kPiece, length - sent));
      OVERLAPPED ov = {};
      ov.Offset = static_cast<DWORD>(offset + sent);
      ov.OffsetHigh = static_cast<DWORD>((offset + sent) >> 32);
      DWORD got = 0;
      if (!ReadFile(file, buf.data(), want, &got, &ov) || got != want)
        throw std::runtime_error("Videodatei kann nicht gelesen werden: " + LastErrorMessage());
      DWORD written = 0;
      if (!WinHttpWriteData(req, buf.data(), want, &written) || written != want)
        ThrowNet("Hochladen");
      sent += want;
      if (progress && !progress(sent)) {
        SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED);
        ThrowNet("Upload");
      }
      if (maxKbps > 0) {
        // Bandbreitenbegrenzung: nicht schneller als maxKbps senden
        double should = sent * 8.0 / (maxKbps * 1000.0);
        double is = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (should > is)
          std::this_thread::sleep_for(std::chrono::duration<double>(should - is));
      }
    }
  });
}

std::string UrlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (const auto& [k, v] : fields) {
    if (!out.empty()) out += '&';
    out += UrlEncode(k) + "=" + UrlEncode(v);
  }
  return out;
}

} // namespace litecap
