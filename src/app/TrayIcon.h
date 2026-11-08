#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace litecap {

enum class TrayState { Idle, Replay, Recording, Error };

// Tray-Icon mit zur Laufzeit gezeichneten Status-Icons (keine Bilddateien nötig).
// Benachrichtigungen laufen über NIF_INFO → unter Windows 10/11 als kleine Toast-Meldung.
class TrayIcon {
 public:
  TrayIcon() = default;
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  // callbackMsg erhält in lParam die Maus-/Tastaturereignisse (NOTIFYICON_VERSION_4).
  bool Create(HWND hwnd, UINT callbackMsg);
  // Nach Explorer-Neustart (Nachricht "TaskbarCreated") erneut anlegen.
  void Recreate();
  void Destroy();

  void SetState(TrayState state, const std::wstring& tooltip);
  void Notify(const std::wstring& title, const std::wstring& text, bool isError = false,
              bool sound = false);

 private:
  void LoadIcons();
  NOTIFYICONDATAW BaseData() const;

  HWND hwnd_ = nullptr;
  UINT callbackMsg_ = 0;
  bool added_ = false;
  TrayState state_ = TrayState::Idle;
  std::wstring tooltip_ = L"LiteCap";
  HICON icons_[4] = {};
};

} // namespace litecap
