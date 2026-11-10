#include "app/App.h"

#include <shellapi.h>
#include <shlobj.h>

#include <format>

#include "output/FileNamer.h"
#include "util/Log.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace litecap {

namespace fs = std::filesystem;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_SESSION_ERROR = WM_APP + 2;   // lParam: new std::string (Empfänger löscht)
constexpr UINT WM_SOURCE_CLOSED = WM_APP + 3;
constexpr UINT WM_NOTICE = WM_APP + 4;        // lParam: new YouTubeUploader::Notice
constexpr UINT WM_CLIP_SAVED = WM_APP + 5;    // lParam: new ClipResult
constexpr UINT_PTR kTimerConfigReload = 1;
constexpr UINT_PTR kTimerRecordingWatch = 2;
constexpr UINT_PTR kTimerStatus = 3;          // Tooltip (Replay/Upload) alle 5 s
constexpr UINT_PTR kTimerReplayRestart = 4;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"LiteCap";

enum MenuId : UINT {
  ID_TOGGLE_RECORDING = 100,
  ID_TOGGLE_REPLAY,
  ID_SAVE_REPLAY,
  ID_OPEN_FOLDER,
  ID_SETTINGS,
  ID_AUTOSTART,
  ID_EXIT,
};

log::Level ParseLevel(const std::string& s) {
  if (s == "debug") return log::Level::Debug;
  if (s == "warn") return log::Level::Warn;
  if (s == "error") return log::Level::Error;
  return log::Level::Info;
}

std::wstring HotkeyHint(const std::string& hk) {
  return hk.empty() ? L"" : L"\t" + Utf8ToWide(hk);
}

} // namespace

struct ClipResult {
  fs::path file;
  std::wstring source;
  double seconds = 0;
  bool av1 = false;
  std::string error;
};

App::App(HINSTANCE instance) : instance_(instance) {}

App::~App() {
  Shutdown();
  if (configWatch_ != INVALID_HANDLE_VALUE) FindCloseChangeNotification(configWatch_);
  hotkeys_.reset();
  tray_.Destroy();
  if (hwnd_) DestroyWindow(hwnd_);
}

bool App::Init() {
  msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  msgReloadConfig_ = RegisterWindowMessageW(kMsgReloadConfig);
  msgOpenSettings_ = RegisterWindowMessageW(kMsgOpenSettings);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &App::WndProcThunk;
  wc.hInstance = instance_;
  wc.lpszClassName = kCoreWindowClass;
  if (!RegisterClassExW(&wc)) {
    log::Error("RegisterClassEx: {}", LastErrorMessage());
    return false;
  }
  // Unsichtbares Top-Level-Fenster (kein Message-Only-Fenster, weil diese die
  // Broadcast-Nachricht "TaskbarCreated" nach einem Explorer-Neustart nicht erhalten).
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kCoreWindowClass, L"LiteCap", WS_POPUP, 0, 0, 0, 0,
                          nullptr, nullptr, instance_, this);
  if (!hwnd_) {
    log::Error("CreateWindowEx: {}", LastErrorMessage());
    return false;
  }
  // Die GUI läuft ggf. ohne Admin-Rechte, der Kern evtl. mit – Nachrichten trotzdem zulassen.
  ChangeWindowMessageFilterEx(hwnd_, msgReloadConfig_, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(hwnd_, msgOpenSettings_, MSGFLT_ALLOW, nullptr);

  hotkeys_ = std::make_unique<Hotkeys>(hwnd_);
  // Tray zuerst, damit Fehler beim Laden der Config als Benachrichtigung erscheinen können.
  if (!tray_.Create(hwnd_, WM_TRAY)) return false;
  LoadConfig(/*initial=*/true);
  pipelineKey_ = config_.PipelineKey();

  // Hintergrunddienste: Meldungen kommen aus Worker-Threads → UI-Thread
  auto notice = [hwnd = hwnd_](const YouTubeUploader::Notice& n) {
    PostMessageW(hwnd, WM_NOTICE, 0, reinterpret_cast<LPARAM>(new YouTubeUploader::Notice(n)));
  };
  uploader_.UpdateConfig(config_.youtube);
  uploader_.Start(notice);
  post_.UpdateConfig(config_);
  post_.Start(uploader_, notice);

  ApplyConfig();
  StartConfigWatch();
  SetTimer(hwnd_, kTimerStatus, 5000, nullptr);

  if (config_.general.startReplayOnLaunch) StartReplay(/*notify=*/false);
  UpdateTray();
  log::Info("LiteCap {} gestartet", LITECAP_VERSION);
  return true;
}

int App::Run() {
  MSG msg;
  for (;;) {
    // Wartet ohne CPU-Last auf Fensternachrichten oder Änderungen an der Config-Datei.
    DWORD count = configWatch_ != INVALID_HANDLE_VALUE ? 1 : 0;
    DWORD r = MsgWaitForMultipleObjectsEx(count, &configWatch_, INFINITE, QS_ALLINPUT,
                                          MWMO_INPUTAVAILABLE);
    if (count && r == WAIT_OBJECT_0) {
      FindNextChangeNotification(configWatch_);
      // Entprellen: Editoren schreiben oft mehrfach hintereinander.
      SetTimer(hwnd_, kTimerConfigReload, 400, nullptr);
      continue;
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Konfiguration

void App::LoadConfig(bool initial) {
  const fs::path file = paths::ConfigFile();
  try {
    Config cfg = Config::Load(file);
    for (const auto& fix : cfg.Validate()) log::Warn("Config: {}", fix);
    config_ = std::move(cfg);
    log::SetLevel(ParseLevel(config_.general.logLevel));
    log::Info("Konfiguration geladen: {}", WideToUtf8(file.wstring()));
  } catch (const std::exception& e) {
    log::Error("{}", e.what());
    if (initial) config_ = Config{};  // Standardwerte verwenden, Datei NICHT überschreiben
    Notify(L"Konfigurationsfehler",
           Utf8ToWide(e.what()) + (initial ? L"\nEs werden Standardwerte verwendet."
                                           : L"\nDie bisherigen Einstellungen bleiben aktiv."),
           true);
  }
}

void App::ApplyConfig() {
  uploader_.UpdateConfig(config_.youtube);
  post_.UpdateConfig(config_);
  // Capture-/Encoder-Einstellungen geändert → Replay-Pipeline neu starten (nicht während
  // einer Aufnahme; dann gelten sie ab der nächsten)
  std::string key = config_.PipelineKey();
  if (key != pipelineKey_ && !recording_ && replayActive_) {
    log::Info("Aufnahme-Einstellungen geändert → Replay-Puffer wird neu gestartet");
    StopReplay();
    StartReplay(/*notify=*/false);
  }
  if (!recording_) pipelineKey_ = key;
  RegisterHotkeys();
  ApplyAutostart();
  UpdateTray();
}

void App::RegisterHotkeys() {
  hotkeys_->UnregisterAll();
  std::wstring errors;
  auto reg = [&](HotkeyAction a, const std::string& text) {
    if (auto err = hotkeys_->Register(a, text)) {
      log::Warn("{}", *err);
      errors += Utf8ToWide(*err) + L"\n";
    }
  };
  reg(HotkeyAction::ToggleRecording, config_.hotkeys.toggleRecording);
  reg(HotkeyAction::SaveReplay, config_.hotkeys.saveReplay);
  reg(HotkeyAction::ToggleReplay, config_.hotkeys.toggleReplay);
  if (!errors.empty()) Notify(L"Hotkey-Problem", errors, true);
}

void App::ApplyAutostart() {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
  if (config_.general.autostart) {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --autostart";
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                   static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kRunValue);
  }
  RegCloseKey(key);
}

void App::StartConfigWatch() {
  configWatch_ = FindFirstChangeNotificationW(
      paths::AppDataDir().c_str(), FALSE,
      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
  if (configWatch_ == INVALID_HANDLE_VALUE)
    log::Warn("Config-Überwachung nicht möglich: {}", LastErrorMessage());
}

// ---------------------------------------------------------------------------------------------
// Tray

void App::UpdateTray() {
  TrayState state = recording_ ? TrayState::Recording
                    : replayActive_ ? TrayState::Replay
                                    : TrayState::Idle;
  std::wstring tip = L"LiteCap – ";
  if (recording_) {
    int secs = static_cast<int>(recorder_.DurationSec());
    tip += std::format(L"Aufnahme {}:{:02} ({:.0f} MB)", secs / 60, secs % 60,
                       recorder_.BytesWritten() / 1048576.0);
  }
  else if (replayActive_)
    tip += std::format(L"Replay {:.0f}/{} s", replay_.BufferedSeconds(), replay_.Seconds());
  else tip += L"Bereit";
  if (std::wstring up = post_.StatusText(); !up.empty()) tip += L"\n" + up;
  if (std::wstring up = uploader_.StatusText(); !up.empty()) tip += L"\n" + up;
  tray_.SetState(state, tip);
}

void App::Notify(const std::wstring& title, const std::wstring& text, bool isError) {
  if (!config_.notifications.enabled && !isError) return;
  tray_.Notify(title, text, isError, config_.notifications.sound);
}

void App::ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  const auto& hk = config_.hotkeys;

  std::wstring status = recording_ ? L"● Aufnahme läuft"
                        : replayActive_ ? L"● Replay-Puffer aktiv"
                                        : L"○ Bereit";
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_TOGGLE_RECORDING,
              ((recording_ ? L"Aufnahme stoppen" : L"Aufnahme starten") +
               HotkeyHint(hk.toggleRecording)).c_str());
  AppendMenuW(menu, MF_STRING, ID_TOGGLE_REPLAY,
              ((replayActive_ ? L"Replay-Puffer deaktivieren" : L"Replay-Puffer aktivieren") +
               HotkeyHint(hk.toggleReplay)).c_str());
  AppendMenuW(menu, MF_STRING | (replayActive_ ? 0 : MF_GRAYED), ID_SAVE_REPLAY,
              (L"Clip speichern" + HotkeyHint(hk.saveReplay)).c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_OPEN_FOLDER, L"Aufnahmeordner öffnen");
  AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Einstellungen…");
  AppendMenuW(menu, MF_STRING | (config_.general.autostart ? MF_CHECKED : 0), ID_AUTOSTART,
              L"Mit Windows starten");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_EXIT, L"Beenden");
  SetMenuDefaultItem(menu, ID_SETTINGS, FALSE);

  POINT pt;
  GetCursorPos(&pt);
  // Ohne SetForegroundWindow schließt sich das Menü nicht beim Klick daneben.
  SetForegroundWindow(hwnd_);
  UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y,
                              hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);

  switch (cmd) {
    case ID_TOGGLE_RECORDING: ToggleRecording(); break;
    case ID_TOGGLE_REPLAY: ToggleReplay(); break;
    case ID_SAVE_REPLAY: SaveReplay(); break;
    case ID_OPEN_FOLDER: OpenOutputFolder(); break;
    case ID_SETTINGS: OpenSettings(); break;
    case ID_AUTOSTART:
      config_.general.autostart = !config_.general.autostart;
      ApplyAutostart();
      try {
        config_.Save(paths::ConfigFile());
      } catch (const std::exception& e) {
        Notify(L"Fehler", Utf8ToWide(e.what()), true);
      }
      break;
    case ID_EXIT:
      Shutdown();
      PostQuitMessage(0);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------
// Pipeline

bool App::EnsureSession() {
  if (session_ && session_->Running()) return true;
  session_ = std::make_unique<CaptureSession>();
  CaptureSession::Callbacks cb;
  // Callbacks kommen aus Worker-Threads → in den UI-Thread umleiten
  cb.onError = [hwnd = hwnd_](const std::string& msg) {
    PostMessageW(hwnd, WM_SESSION_ERROR, 0, reinterpret_cast<LPARAM>(new std::string(msg)));
  };
  cb.onSourceClosed = [hwnd = hwnd_] { PostMessageW(hwnd, WM_SOURCE_CLOSED, 0, 0); };
  try {
    session_->Start(config_, std::move(cb));
  } catch (const winrt::hresult_error& e) {
    session_.reset();
    log::Error("Session-Start: {}", WideToUtf8(e.message()));
    Notify(L"Aufnahme nicht möglich", std::wstring(e.message()), true);
    return false;
  } catch (const std::exception& e) {
    session_.reset();
    log::Error("Session-Start: {}", e.what());
    Notify(L"Aufnahme nicht möglich", Utf8ToWide(e.what()), true);
    return false;
  }
  if (!IsHardware(session_->Video().Choice().backend))
    Notify(L"Software-Encoding aktiv",
           L"Kein Hardware-Encoder verfügbar – die CPU-Last ist dadurch höher.", true);
  return true;
}

void App::StopSessionIfUnused() {
  // Der Replay-Puffer braucht die Session dauerhaft, Aufnahmen nur solange sie laufen.
  if (!recording_ && !replayActive_ && session_) {
    session_->Stop();
    session_.reset();
  }
}

void App::StartRecording() {
  const fs::path dir = config_.OutputFolder();
  const uint64_t minFree = static_cast<uint64_t>(config_.output.minFreeSpaceMB) * 1024 * 1024;
  if (FreeDiskBytes(dir) < minFree) {
    Notify(L"Zu wenig Speicherplatz",
           std::format(L"Auf dem Ziellaufwerk sind weniger als {} MB frei.",
                       config_.output.minFreeSpaceMB), true);
    return;
  }
  if (!EnsureSession()) return;
  try {
    fs::path file = MakeOutputPath(config_, session_->SourceName(), L"Aufnahme");
    recorder_.Start(*session_, file);
  } catch (const std::exception& e) {
    log::Error("Aufnahme-Start: {}", e.what());
    Notify(L"Aufnahme nicht möglich", Utf8ToWide(e.what()), true);
    StopSessionIfUnused();
    return;
  }
  recording_ = true;
  pipelineKey_ = config_.PipelineKey();
  SetTimer(hwnd_, kTimerRecordingWatch, 2000, nullptr);
  Notify(L"Aufnahme gestartet", Utf8ToWide(session_->Description()));
  UpdateTray();
}

void App::StopRecording(const std::wstring& reason) {
  if (!recording_) return;
  KillTimer(hwnd_, kTimerRecordingWatch);
  recording_ = false;
  try {
    double secs = recorder_.DurationSec();
    fs::path file = recorder_.Stop();
    std::wstring text = std::format(L"{} ({}:{:02})", file.filename().wstring(),
                                    static_cast<int>(secs) / 60, static_cast<int>(secs) % 60);
    if (!reason.empty()) text = reason + L"\n" + text;
    Notify(L"Aufnahme gespeichert", text, !reason.empty());
    const bool av1 = session_ && session_->Video().Choice().transcodeToAv1;
    post_.Submit(file, session_ ? session_->SourceName() : L"", L"Aufnahme", av1);
  } catch (const std::exception& e) {
    log::Error("Aufnahme-Ende: {}", e.what());
    Notify(L"Aufnahme fehlerhaft", (reason.empty() ? L"" : reason + L"\n") + Utf8ToWide(e.what()),
           true);
  }
  StopSessionIfUnused();
  // Während der Aufnahme geänderte Einstellungen jetzt übernehmen
  if (config_.PipelineKey() != pipelineKey_) ApplyConfig();
  UpdateTray();
}

void App::CheckRecording() {
  if (!recording_) return;
  if (recorder_.Failed()) {
    StopRecording(L"Schreibfehler – Aufnahme abgebrochen.");
    return;
  }
  const uint64_t minFree = static_cast<uint64_t>(config_.output.minFreeSpaceMB) * 1024 * 1024;
  if (FreeDiskBytes(config_.OutputFolder()) < minFree / 2) {
    StopRecording(L"Speicherplatz fast voll – Aufnahme beendet.");
    return;
  }
  UpdateTray();
}

void App::Shutdown() {
  if (clipThread_.joinable()) clipThread_.join();  // laufendes Clip-Speichern abwarten
  if (recording_) StopRecording();
  replay_.Stop();
  replayActive_ = false;
  if (session_) {
    session_->Stop();
    session_.reset();
  }
  post_.Stop();      // bricht eine laufende AV1-Umwandlung ab (Original bleibt erhalten)
  uploader_.Stop();  // Upload wird beim nächsten Start fortgesetzt
}

bool App::StartReplay(bool notify) {
  replayWanted_ = true;
  if (replayActive_) return true;
  if (!EnsureSession()) {
    UpdateTray();
    return false;
  }
  const auto& v = config_.video;
  const int videoKbps = v.rateControl == "cqp" ? 50000 : v.bitrateKbps;  // CQP: Annahme
  const int audioKbps = config_.audio.bitrateKbps * 2;
  try {
    replay_.Start(*session_, config_.replay.seconds, videoKbps, audioKbps,
                  config_.ReplayBufferDir());
  } catch (const std::exception& e) {
    log::Error("Replay-Start: {}", e.what());
    Notify(L"Replay-Puffer nicht möglich", Utf8ToWide(e.what()), true);
    replayWanted_ = false;
    StopSessionIfUnused();
    UpdateTray();
    return false;
  }
  replayActive_ = true;
  replayRestartTries_ = 0;
  pipelineKey_ = config_.PipelineKey();
  if (notify)
    Notify(L"Replay-Puffer aktiv",
           std::format(L"Die letzten {} s werden mitgeschnitten. {} speichert einen Clip.",
                       config_.replay.seconds, Utf8ToWide(config_.hotkeys.saveReplay)));
  UpdateTray();
  return true;
}

void App::StopReplay() {
  if (clipThread_.joinable()) clipThread_.join();
  replay_.Stop();
  replayActive_ = false;
  StopSessionIfUnused();
  UpdateTray();
}

void App::OnClipSaved(ClipResult* r) {
  std::unique_ptr<ClipResult> res(r);
  if (clipThread_.joinable()) clipThread_.join();
  if (!res->error.empty()) {
    Notify(L"Clip nicht gespeichert", Utf8ToWide(res->error), true);
    return;
  }
  Notify(L"Clip gespeichert",
         std::format(L"{} ({:.0f} s)", res->file.filename().wstring(), res->seconds));
  post_.Submit(res->file, res->source, L"Clip", res->av1);
}

// ---------------------------------------------------------------------------------------------
// Aktionen

void App::ToggleRecording() {
  if (recording_) StopRecording();
  else StartRecording();
}

void App::ToggleReplay() {
  if (replayActive_) {
    replayWanted_ = false;
    StopReplay();
    Notify(L"Replay-Puffer aus", L"Es wird nichts mehr mitgeschnitten.");
  } else {
    StartReplay(/*notify=*/true);
  }
}

void App::SaveReplay() {
  if (!replayActive_) {
    Notify(L"LiteCap", L"Der Replay-Puffer ist nicht aktiv.", true);
    return;
  }
  if (savingClip_) {
    Notify(L"LiteCap", L"Der vorige Clip wird noch gespeichert.", true);
    return;
  }
  const uint64_t minFree = static_cast<uint64_t>(config_.output.minFreeSpaceMB) * 1024 * 1024;
  if (FreeDiskBytes(config_.OutputFolder()) < minFree) {
    Notify(L"Zu wenig Speicherplatz", L"Der Clip kann nicht gespeichert werden.", true);
    return;
  }
  fs::path file;
  try {
    file = MakeOutputPath(config_, session_->SourceName(), L"Clip");
  } catch (const std::exception& e) {
    Notify(L"Clip nicht gespeichert", Utf8ToWide(e.what()), true);
    return;
  }
  if (clipThread_.joinable()) clipThread_.join();
  savingClip_ = true;
  auto* result = new ClipResult{file, session_->SourceName(), 0,
                                session_->Video().Choice().transcodeToAv1, {}};
  // Schreiben im Hintergrund: Hotkey/Tray bleiben bedienbar, das Spiel merkt nichts
  clipThread_ = std::thread([this, result, hwnd = hwnd_] {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    try {
      result->seconds = replay_.SaveClip(result->file);
    } catch (const std::exception& e) {
      result->error = e.what();
      log::Error("Clip: {}", e.what());
    }
    savingClip_ = false;
    PostMessageW(hwnd, WM_CLIP_SAVED, 0, reinterpret_cast<LPARAM>(result));
  });
}

void App::OpenOutputFolder() {
  fs::path dir = config_.OutputFolder();
  std::error_code ec;
  fs::create_directories(dir, ec);
  ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::OpenSettings() {
  // Neben litecap.exe (eigener Build) oder im Unterordner "settings" (Release-ZIP: GUI mit
  // eigenem .NET als normale Dateien statt selbstentpackender Einzeldatei)
  std::error_code ec;
  fs::path gui = paths::ExeDir() / L"litecap-settings.exe";
  if (!fs::exists(gui, ec)) gui = paths::ExeDir() / L"settings" / L"litecap-settings.exe";
  if (fs::exists(gui, ec)) {
    // Die GUI ist ein eigener Prozess: Sie belegt nur Ressourcen, solange sie offen ist.
    ShellExecuteW(nullptr, L"open", gui.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else {
    // Bis die GUI gebaut ist: config.json im Standard-Editor öffnen (wird live neu geladen).
    fs::path cfg = paths::ConfigFile();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", cfg.c_str(), nullptr, nullptr,
                                                SW_SHOWNORMAL)) <= 32)
      ShellExecuteW(nullptr, L"open", L"notepad.exe", cfg.c_str(), nullptr, SW_SHOWNORMAL);
  }
}

// ---------------------------------------------------------------------------------------------
// Fensterprozedur

LRESULT CALLBACK App::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* self = nullptr;
  if (msg == WM_NCCREATE) {
    self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  return self ? self->WndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == msgTaskbarCreated_ && msg != 0) {
    tray_.Recreate();
    UpdateTray();
    return 0;
  }
  if (msg == msgReloadConfig_ && msg != 0) {
    LoadConfig(false);
    ApplyConfig();
    return 0;
  }
  if (msg == msgOpenSettings_ && msg != 0) {
    OpenSettings();
    return 0;
  }

  switch (msg) {
    case WM_HOTKEY:
      switch (static_cast<HotkeyAction>(wp)) {
        case HotkeyAction::ToggleRecording: ToggleRecording(); break;
        case HotkeyAction::SaveReplay: SaveReplay(); break;
        case HotkeyAction::ToggleReplay: ToggleReplay(); break;
      }
      return 0;

    case WM_TRAY:
      // NOTIFYICON_VERSION_4: Ereignis in LOWORD(lParam)
      switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: ShowTrayMenu(); break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK: OpenSettings(); break;
        default: break;
      }
      return 0;

    case WM_SESSION_ERROR: {
      std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
      // Encoder-Thread ist beendet → Aufnahme abschließen, Session verwerfen
      if (recording_) StopRecording(L"Fehler: " + Utf8ToWide(*text));
      else Notify(L"LiteCap-Fehler", Utf8ToWide(*text), true);
      const bool wanted = replayWanted_;
      StopReplay();
      if (session_) {
        session_->Stop();
        session_.reset();
      }
      // z. B. nach einem Treiber-Reset: Replay automatisch neu starten (max. 3 Versuche)
      replayWanted_ = wanted;
      if (wanted && replayRestartTries_++ < 3) SetTimer(hwnd, kTimerReplayRestart, 3000, nullptr);
      UpdateTray();
      return 0;
    }

    case WM_SOURCE_CLOSED:
      if (recording_) StopRecording(L"Das aufgenommene Fenster wurde geschlossen.");
      if (replayActive_) {
        StopReplay();
        Notify(L"Replay-Puffer beendet", L"Das aufgenommene Fenster wurde geschlossen.", true);
      }
      if (session_) {
        session_->Stop();
        session_.reset();
      }
      UpdateTray();
      return 0;

    case WM_NOTICE: {
      std::unique_ptr<YouTubeUploader::Notice> n(reinterpret_cast<YouTubeUploader::Notice*>(lp));
      Notify(n->title, n->text, n->isError);
      UpdateTray();
      return 0;
    }

    case WM_CLIP_SAVED:
      OnClipSaved(reinterpret_cast<ClipResult*>(lp));
      return 0;

    case WM_TIMER:
      if (wp == kTimerRecordingWatch) {
        CheckRecording();
        return 0;
      }
      if (wp == kTimerStatus) {
        UpdateTray();
        return 0;
      }
      if (wp == kTimerReplayRestart) {
        KillTimer(hwnd, kTimerReplayRestart);
        if (replayWanted_ && !replayActive_) StartReplay(/*notify=*/true);
        return 0;
      }
      if (wp == kTimerConfigReload) {
        KillTimer(hwnd, kTimerConfigReload);
        LoadConfig(false);
        ApplyConfig();
      }
      return 0;

    case WM_QUERYENDSESSION:
      return TRUE;

    case WM_ENDSESSION:
      // Windows fährt herunter: Aufnahme noch finalisieren, sonst ist die Datei unvollständig
      if (wp) {
        Shutdown();
        PostQuitMessage(0);
      }
      return 0;

    case WM_CLOSE:  // z. B. von einem Installer/Updater: sauber beenden
      Shutdown();
      PostQuitMessage(0);
      return 0;

    default: break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace litecap
