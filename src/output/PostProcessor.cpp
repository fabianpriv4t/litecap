#include "output/PostProcessor.h"

#include <shellapi.h>

#include <format>

#include "util/Log.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace litecap {

namespace fs = std::filesystem;

bool FullscreenAppRunning() {
  QUERY_USER_NOTIFICATION_STATE s;
  if (FAILED(SHQueryUserNotificationState(&s))) return false;
  return s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE;
}

void PostProcessor::Start(YouTubeUploader& uploader, NoticeFn onNotice) {
  Stop();
  uploader_ = &uploader;
  onNotice_ = std::move(onNotice);
  stop_ = false;
  thread_ = std::thread([this] { Run(); });
}

void PostProcessor::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PostProcessor::UpdateConfig(const Config& cfg) {
  std::lock_guard lock(mutex_);
  cfg_ = cfg;
}

void PostProcessor::Submit(const fs::path& file, const std::wstring& source,
                           const std::wstring& type, bool av1) {
  {
    std::lock_guard lock(mutex_);
    jobs_.push_back({file, source, type, av1});
  }
  cv_.notify_all();
}

std::wstring PostProcessor::StatusText() const {
  std::lock_guard lock(mutex_);
  if (transcoding_) return L"AV1-Umwandlung läuft";
  if (!jobs_.empty()) return std::format(L"{} Datei(en) in Nachbearbeitung", jobs_.size());
  return {};
}

bool PostProcessor::StopRequested() const {
  std::lock_guard lock(mutex_);
  return stop_;
}

void PostProcessor::Run() {
  for (;;) {
    Job job;
    Config cfg;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = jobs_.front();
      jobs_.pop_front();
      cfg = cfg_;
    }

    if (job.av1) TranscodeAv1(job.file);
    if (StopRequested()) return;

    const std::string& mode = cfg.output.mode;
    if (mode == "youtube" || mode == "both") {
      // Nur-YouTube: lokale Datei ist bloß Zwischenablage → nach Upload weg
      bool deleteAfter = mode == "youtube" || cfg.output.deleteAfterUpload;
      uploader_->Enqueue(job.file, job.source, job.type, deleteAfter);
    }
  }
}

bool PostProcessor::TranscodeAv1(const fs::path& file) {
  bool onlyWhenIdle;
  {
    std::lock_guard lock(mutex_);
    onlyWhenIdle = cfg_.video.av1TranscodeOnlyWhenIdle;
  }
  fs::path tmp = file;
  tmp.replace_extension(L".av1-tmp" + file.extension().wstring());
  const auto exe = paths::ExeDir() / L"litecap.exe";
  std::error_code ec;
  const auto sizeBefore = fs::file_size(file, ec);

  for (int attempt = 0; attempt < 50; ++attempt) {
    // Warten, solange ein Spiel im Vollbild läuft
    while (onlyWhenIdle && FullscreenAppRunning()) {
      std::unique_lock lock(mutex_);
      if (cv_.wait_for(lock, std::chrono::seconds(15), [this] { return stop_; })) return false;
    }

    std::wstring cmd = L"\"" + exe.wstring() + L"\" --transcode-av1 \"" + file.wstring() +
                       L"\" \"" + tmp.wstring() + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
      log::Error("AV1-Umwandlung kann nicht gestartet werden: {}", LastErrorMessage());
      return false;
    }
    {
      std::lock_guard lock(mutex_);
      transcoding_ = true;
    }
    log::Info("AV1-Umwandlung gestartet: {}", WideToUtf8(file.filename().wstring()));

    bool aborted = false, paused = false;
    for (;;) {
      if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) break;
      if (StopRequested()) aborted = true;
      else if (onlyWhenIdle && FullscreenAppRunning()) paused = true;
      if (aborted || paused) {
        TerminateProcess(pi.hProcess, 2);
        WaitForSingleObject(pi.hProcess, 5000);
        break;
      }
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    {
      std::lock_guard lock(mutex_);
      transcoding_ = false;
    }

    if (aborted || paused) {
      fs::remove(tmp, ec);
      if (aborted) return false;
      log::Info("AV1-Umwandlung pausiert (Vollbildprogramm), wird später neu gestartet");
      continue;
    }
    if (exitCode != 0 || !fs::exists(tmp, ec)) {
      fs::remove(tmp, ec);
      log::Error("AV1-Umwandlung fehlgeschlagen (Code {}) – Datei bleibt im Originalformat",
                 exitCode);
      onNotice_({L"AV1-Umwandlung fehlgeschlagen",
                 file.filename().wstring() + L" bleibt im Originalformat (Details im Log).", true});
      return false;
    }
    const auto sizeAfter = fs::file_size(tmp, ec);
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      log::Error("AV1-Datei kann nicht übernommen werden: {}", LastErrorMessage());
      fs::remove(tmp, ec);
      return false;
    }
    log::Info("AV1-Umwandlung fertig: {:.1f} MB → {:.1f} MB", sizeBefore / 1048576.0,
              sizeAfter / 1048576.0);
    onNotice_({L"AV1-Umwandlung fertig",
               std::format(L"{} ({:.0f} MB → {:.0f} MB)", file.filename().wstring(),
                           sizeBefore / 1048576.0, sizeAfter / 1048576.0),
               false});
    return true;
  }
  return false;
}

} // namespace litecap
