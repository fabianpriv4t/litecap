#pragma once

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "config/Config.h"
#include "upload/YouTubeUploader.h"

namespace litecap {

// Nachbearbeitung fertiger Dateien (Aufnahmen und Clips), nacheinander im Hintergrund:
//  1. optional AV1-Umwandlung (Kindprozess mit Leerlauf-Priorität; pausiert bei Vollbildspielen)
//  2. optional Übergabe an den YouTube-Uploader
class PostProcessor {
 public:
  using NoticeFn = YouTubeUploader::NoticeFn;

  PostProcessor() = default;
  ~PostProcessor() { Stop(); }
  PostProcessor(const PostProcessor&) = delete;
  PostProcessor& operator=(const PostProcessor&) = delete;

  void Start(YouTubeUploader& uploader, NoticeFn onNotice);
  void Stop();
  void UpdateConfig(const Config& cfg);

  void Submit(const std::filesystem::path& file, const std::wstring& source,
              const std::wstring& type, bool transcodeToAv1);
  std::wstring StatusText() const;  // z. B. "AV1-Umwandlung läuft"

 private:
  struct Job {
    std::filesystem::path file;
    std::wstring source, type;
    bool av1;
  };
  void Run();
  bool TranscodeAv1(const std::filesystem::path& file);  // false = abgebrochen/fehlgeschlagen
  bool StopRequested() const;

  YouTubeUploader* uploader_ = nullptr;
  NoticeFn onNotice_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  Config cfg_;
  bool stop_ = false;
  bool transcoding_ = false;
  std::thread thread_;
};

// true, wenn gerade ein Vollbildprogramm (Spiel, Präsentation) läuft
bool FullscreenAppRunning();

} // namespace litecap
