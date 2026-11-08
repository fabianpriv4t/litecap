#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include "upload/Http.h"

namespace litecap {

// Anmeldung muss (neu) erfolgen: kein/abgelaufenes/widerrufenes Refresh-Token, falsches Secret.
class AuthError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// OAuth 2.0 für Desktop-Apps (Google "installed app" Flow):
//  - Browser öffnet die Google-Anmeldung, Rückleitung an http://127.0.0.1:<zufälliger Port>
//  - PKCE (S256) schützt den Code gegen Abfangen
//  - Das Refresh-Token landet im Windows Credential Manager; Access-Tokens nur im RAM
class GoogleAuth {
 public:
  static constexpr char kScope[] = "https://www.googleapis.com/auth/youtube.upload";

  explicit GoogleAuth(std::string clientId) : clientId_(std::move(clientId)) {}

  // Interaktive Anmeldung (blockiert bis zu 5 Minuten). Wirft bei Fehlern.
  void Login(Http& http);
  // Meldet ab: Token bei Google widerrufen und lokal löschen.
  static void Logout(Http& http);
  static bool IsConnected();

  // Gültiges Access-Token (erneuert es bei Bedarf). Wirft AuthError / NetworkError.
  std::string AccessToken(Http& http);
  void Invalidate() { access_.clear(); }

 private:
  std::string ClientSecret() const;

  std::string clientId_;
  std::string access_;
  std::chrono::steady_clock::time_point expires_{};
};

} // namespace litecap
