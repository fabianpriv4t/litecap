#include "upload/GoogleAuth.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include "upload/CredentialStore.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

using nlohmann::json;

namespace {

constexpr wchar_t kTokenUrl[] = L"https://oauth2.googleapis.com/token";

std::string Base64Url(const uint8_t* data, size_t len) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) v |= data[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    if (i + 1 < len) out += tbl[(v >> 6) & 63];
    if (i + 2 < len) out += tbl[v & 63];
  }
  return out;  // ohne Padding (RFC 7636)
}

std::string RandomToken(size_t bytes) {
  std::string buf(bytes, '\0');
  BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(bytes),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return Base64Url(reinterpret_cast<const uint8_t*>(buf.data()), bytes);
}

std::string Sha256Base64Url(const std::string& s) {
  uint8_t hash[32];
  BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
             reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())), static_cast<ULONG>(s.size()),
             hash, sizeof(hash));
  return Base64Url(hash, sizeof(hash));
}

std::string QueryParam(const std::string& query, const std::string& key) {
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = part.find('=');
    if (eq != std::string::npos && part.substr(0, eq) == key) {
      std::string v = part.substr(eq + 1), out;
      for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) {
          out += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16));
          i += 2;
        } else out += v[i] == '+' ? ' ' : v[i];
      }
      return out;
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return {};
}

struct WsaInit {
  WsaInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WsaInit() { WSACleanup(); }
};

// Wartet auf genau eine Anfrage an http://127.0.0.1:port/ und liefert deren Query-String.
class LoopbackServer {
 public:
  LoopbackServer() {
    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // nur lokal erreichbar
    addr.sin_port = 0;                               // freier Port
    if (sock_ == INVALID_SOCKET || bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
        listen(sock_, 4))
      throw std::runtime_error("Lokaler Anmelde-Server kann nicht gestartet werden");
    int len = sizeof(addr);
    getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
  }
  ~LoopbackServer() {
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
  }
  int Port() const { return port_; }

  std::string WaitForQuery(int timeoutSec) {
    for (;;) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(sock_, &fds);
      timeval tv{timeoutSec, 0};
      if (select(0, &fds, nullptr, nullptr, &tv) <= 0)
        throw std::runtime_error("Zeitüberschreitung bei der Anmeldung (5 Minuten)");
      SOCKET c = accept(sock_, nullptr, nullptr);
      if (c == INVALID_SOCKET) continue;
      char buf[8192];
      int n = recv(c, buf, sizeof(buf) - 1, 0);
      std::string req(buf, n > 0 ? static_cast<size_t>(n) : 0);
      // "GET /?code=…&state=… HTTP/1.1"
      size_t q = req.find("GET /?");
      std::string query;
      if (q == 0) {
        size_t end = req.find(' ', 6);
        query = req.substr(6, end == std::string::npos ? std::string::npos : end - 6);
      }
      bool ok = !QueryParam(query, "code").empty();
      std::string html =
          std::string("<!doctype html><meta charset=utf-8><title>LiteCap</title>"
                      "<body style='font-family:Segoe UI,sans-serif;text-align:center;margin-top:15vh'>"
                      "<h2>") +
          (ok ? "LiteCap ist mit YouTube verbunden ✔" : "Anmeldung nicht abgeschlossen") +
          "</h2><p>Dieses Fenster kann geschlossen werden.</p></body>";
      std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         "Connection: close\r\nContent-Length: " +
                         std::to_string(html.size()) + "\r\n\r\n" + html;
      send(c, resp.data(), static_cast<int>(resp.size()), 0);
      shutdown(c, SD_SEND);
      closesocket(c);
      if (!query.empty()) return query;  // z. B. favicon-Anfragen ignorieren
    }
  }

 private:
  SOCKET sock_ = INVALID_SOCKET;
  int port_ = 0;
};

} // namespace

std::string GoogleAuth::ClientSecret() const {
  auto s = credentials::Read(credentials::kYouTubeClientSecret);
  if (!s || s->empty())
    throw AuthError("Kein OAuth-Client-Secret hinterlegt (Einstellungen → YouTube)");
  return *s;
}

bool GoogleAuth::IsConnected() {
  return credentials::Read(credentials::kYouTubeRefreshToken).has_value();
}

void GoogleAuth::Login(Http& http) {
  if (clientId_.empty()) throw AuthError("Keine OAuth-Client-ID eingetragen (Einstellungen → YouTube)");
  const std::string secret = ClientSecret();
  WsaInit wsa;
  LoopbackServer server;
  const std::string redirect = "http://127.0.0.1:" + std::to_string(server.Port());
  const std::string verifier = RandomToken(48);
  const std::string state = RandomToken(16);

  std::string url = "https://accounts.google.com/o/oauth2/v2/auth?" +
                    FormEncode({{"client_id", clientId_},
                                {"redirect_uri", redirect},
                                {"response_type", "code"},
                                {"scope", kScope},
                                {"code_challenge", Sha256Base64Url(verifier)},
                                {"code_challenge_method", "S256"},
                                {"state", state},
                                {"access_type", "offline"},  // liefert ein Refresh-Token
                                {"prompt", "consent"}});
  ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  log::Info("YouTube-Anmeldung im Browser gestartet (Rückleitung an {})", redirect);

  std::string query = server.WaitForQuery(300);
  if (QueryParam(query, "state") != state) throw AuthError("Ungültige Antwort (state stimmt nicht)");
  if (std::string err = QueryParam(query, "error"); !err.empty())
    throw AuthError("Anmeldung abgelehnt: " + err);
  const std::string code = QueryParam(query, "code");

  HttpResponse r = http.Send(L"POST", kTokenUrl,
                             {{L"Content-Type", L"application/x-www-form-urlencoded"}},
                             FormEncode({{"code", code},
                                         {"client_id", clientId_},
                                         {"client_secret", secret},
                                         {"redirect_uri", redirect},
                                         {"grant_type", "authorization_code"},
                                         {"code_verifier", verifier}}));
  json j = json::parse(r.body, nullptr, false);
  if (r.status != 200 || !j.is_object() || !j.contains("refresh_token"))
    throw AuthError("Token-Austausch fehlgeschlagen (HTTP " + std::to_string(r.status) +
                    "): " + (j.is_object() ? j.value("error_description", j.value("error", "")) : r.body));
  if (!credentials::Write(credentials::kYouTubeRefreshToken, j["refresh_token"].get<std::string>()))
    throw std::runtime_error("Refresh-Token konnte nicht im Credential Manager gespeichert werden");
  access_ = j.value("access_token", "");
  expires_ = std::chrono::steady_clock::now() + std::chrono::seconds(j.value("expires_in", 3600) - 60);
  log::Info("YouTube verbunden");
}

void GoogleAuth::Logout(Http& http) {
  if (auto token = credentials::Read(credentials::kYouTubeRefreshToken)) {
    try {
      http.Send(L"POST", L"https://oauth2.googleapis.com/revoke",
                {{L"Content-Type", L"application/x-www-form-urlencoded"}},
                FormEncode({{"token", *token}}));
    } catch (const std::exception& e) {
      log::Warn("Token-Widerruf fehlgeschlagen (wird trotzdem lokal gelöscht): {}", e.what());
    }
  }
  credentials::Remove(credentials::kYouTubeRefreshToken);
  log::Info("YouTube abgemeldet");
}

std::string GoogleAuth::AccessToken(Http& http) {
  if (!access_.empty() && std::chrono::steady_clock::now() < expires_) return access_;
  auto refresh = credentials::Read(credentials::kYouTubeRefreshToken);
  if (!refresh) throw AuthError("Nicht mit YouTube verbunden");
  HttpResponse r = http.Send(L"POST", kTokenUrl,
                             {{L"Content-Type", L"application/x-www-form-urlencoded"}},
                             FormEncode({{"client_id", clientId_},
                                         {"client_secret", ClientSecret()},
                                         {"refresh_token", *refresh},
                                         {"grant_type", "refresh_token"}}));
  json j = json::parse(r.body, nullptr, false);
  if (r.status == 400 || r.status == 401) {
    // invalid_grant: Token widerrufen/abgelaufen (bei "Testing"-Apps nach 7 Tagen!)
    throw AuthError("YouTube-Anmeldung abgelaufen oder widerrufen – bitte neu verbinden (" +
                    (j.is_object() ? j.value("error", std::string("?")) : std::string("?")) + ")");
  }
  if (r.status != 200 || !j.is_object() || !j.contains("access_token"))
    throw NetworkError("Token-Erneuerung fehlgeschlagen (HTTP " + std::to_string(r.status) + ")");
  access_ = j["access_token"].get<std::string>();
  expires_ = std::chrono::steady_clock::now() + std::chrono::seconds(j.value("expires_in", 3600) - 60);
  return access_;
}

} // namespace litecap
