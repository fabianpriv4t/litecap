#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <string>
#include <vector>

#include "app/App.h"
#include "encode/FFmpeg.h"
#include "output/Transcoder.h"
#include "tools/Commands.h"
#include "tools/SelfTest.h"
#include "util/Log.h"
#include "util/Paths.h"
#include "util/Strings.h"

using namespace litecap;

namespace {

// Kurzlebige Hilfsprozesse (GUI-Befehle, Tests, AV1-Umwandlung). Liefert -1, wenn args kein
// solcher Befehl ist.
int RunCommand(const std::vector<std::wstring>& args) {
  if (args.size() < 2) return -1;
  const std::wstring& cmd = args[1];
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::wstring(); };
  auto initLog = [](const wchar_t* name, log::Level level) {
    log::Init(paths::LogDir() / name, level);
    InstallFFmpegLogger();
  };

  int rc = -1;
  if (cmd == L"--test-capture") {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    initLog(L"selftest.log", log::Level::Debug);
    int s = _wtoi(arg(2).c_str());
    rc = RunCaptureSelfTest(s > 0 ? s : 5);
  } else if (cmd == L"--test-record") {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    initLog(L"selftest.log", log::Level::Debug);
    int s = _wtoi(arg(2).c_str());
    rc = RunRecordSelfTest(s > 0 ? s : 5, WideToUtf8(arg(3)), WideToUtf8(arg(4)));
  } else if (cmd == L"--test-replay") {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    initLog(L"selftest.log", log::Level::Debug);
    int b = _wtoi(arg(2).c_str()), r = _wtoi(arg(3).c_str());
    rc = RunReplaySelfTest(b > 0 ? b : 10, r > 0 ? r : 15);
  } else if (cmd == L"--test-decode" && args.size() >= 3) {
    rc = RunDecodeTest(args[2], args.size() >= 4 ? _wtof(args[3].c_str()) : -1);
  } else if (cmd == L"--test-mf" && args.size() >= 3) {
    rc = RunMfDecodeTest(args[2], args.size() >= 4 && args[3] == L"hw");
  } else if (cmd == L"--transcode-av1" && args.size() >= 4) {
    initLog(L"transcode.log", log::Level::Info);
    rc = RunTranscodeAv1(args[2], args[3]);
  } else if (cmd == L"--list-devices") {
    av_log_set_level(AV_LOG_QUIET);  // Probe-Fehlschläge sind hier normal
    rc = RunListDevices();
  } else if (cmd == L"--youtube-login") {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    initLog(L"youtube.log", log::Level::Info);
    rc = RunYouTubeLogin();
  } else if (cmd == L"--youtube-logout") {
    initLog(L"youtube.log", log::Level::Info);
    rc = RunYouTubeLogout();
  } else {
    return -1;  // z. B. --autostart → normaler Start
  }
  log::Shutdown();
  return rc;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv, argv + argc);
    LocalFree(argv);
    int rc = RunCommand(args);
    if (rc >= 0) return rc;
  }

  // Nur eine Instanz: Ein zweiter Start öffnet stattdessen die Einstellungen der laufenden.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LiteCap.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kCoreWindowClass, nullptr))
      PostMessageW(other, RegisterWindowMessageW(kMsgOpenSettings), 0, 0);
    if (mutex) CloseHandle(mutex);
    return 0;
  }

  // Multithreaded Apartment: Capture (WinRT) und WASAPI laufen in eigenen Threads.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  log::Init(paths::LogDir() / L"litecap.log");
  InstallFFmpegLogger();

  int rc = 1;
  {
    App app(instance);
    if (app.Init()) rc = app.Run();
    else MessageBoxW(nullptr, L"LiteCap konnte nicht gestartet werden. Details im Log:\n"
                              L"%LOCALAPPDATA%\\LiteCap\\logs\\litecap.log",
                     L"LiteCap", MB_ICONERROR);
  }

  log::Info("LiteCap beendet");
  log::Shutdown();
  CoUninitialize();
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  return rc;
}
