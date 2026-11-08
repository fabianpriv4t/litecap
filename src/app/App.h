#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "app/Hotkeys.h"
#include "pipeline/CaptureSession.h"
#include "pipeline/Recorder.h"
#include "pipeline/ReplayBuffer.h"
#include "output/PostProcessor.h"
#include "upload/YouTubeUploader.h"

#include <atomic>
#include <thread>
#include "app/TrayIcon.h"
#include "config/Config.h"

namespace litecap {

// Name der Fensterklasse und registrierte Nachrichten – die Einstellungs-GUI nutzt sie,
// um den laufenden Kern zu finden und zum Neuladen aufzufordern.
inline constexpr wchar_t kCoreWindowClass[] = L"LiteCapCoreWindow";
inline constexpr wchar_t kMsgReloadConfig[] = L"LiteCap.ReloadConfig";
inline constexpr wchar_t kMsgOpenSettings[] = L"LiteCap.OpenSettings";

class App {
 public:
  explicit App(HINSTANCE instance);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  bool Init();
  int Run();  // Message-Loop, kehrt bei "Beenden" zurück

 private:
  static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
  LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

  void LoadConfig(bool initial);
  void ApplyConfig();
  void RegisterHotkeys();
  void ApplyAutostart();
  void StartConfigWatch();

  void ShowTrayMenu();
  void UpdateTray();
  void Notify(const std::wstring& title, const std::wstring& text, bool isError = false);

  // Pipeline
  bool EnsureSession();  // startet die Capture-Session bei Bedarf; false + Meldung bei Fehler
  void StopSessionIfUnused();
  void StartRecording();
  void StopRecording(const std::wstring& reason = {});
  void CheckRecording();  // periodisch: Schreibfehler, freier Speicher, Tooltip
  void Shutdown();        // laufende Aufnahme sauber abschließen
  bool StartReplay(bool notify);
  void StopReplay();
  void OnClipSaved(struct ClipResult* r);

  // Aktionen (Hotkeys / Tray-Menü)
  void ToggleRecording();
  void ToggleReplay();
  void SaveReplay();
  void OpenOutputFolder();
  void OpenSettings();

  HINSTANCE instance_;
  HWND hwnd_ = nullptr;
  UINT msgTaskbarCreated_ = 0;
  UINT msgReloadConfig_ = 0;
  UINT msgOpenSettings_ = 0;
  HANDLE configWatch_ = INVALID_HANDLE_VALUE;

  Config config_;
  TrayIcon tray_;
  std::unique_ptr<Hotkeys> hotkeys_;

  std::unique_ptr<CaptureSession> session_;
  Recorder recorder_;
  ReplayBuffer replay_;
  YouTubeUploader uploader_;
  PostProcessor post_;
  std::thread clipThread_;
  std::atomic<bool> savingClip_{false};
  std::string pipelineKey_;      // um Änderungen an Capture/Video/Audio/Replay zu erkennen
  bool replayWanted_ = false;    // Replay soll laufen (auch wenn es gerade neu startet)
  int replayRestartTries_ = 0;
  bool recording_ = false;
  bool replayActive_ = false;
};

} // namespace litecap
