#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config/Config.h"

namespace litecap {

struct UploadJob {
  std::string id;           // interne ID
  std::string file;         // UTF-8-Pfad
  std::string title, description, privacy;
  std::vector<std::string> tags;
  int categoryId = 20;
  bool deleteAfter = false;
  std::string sessionUri;   // YouTube-Upload-Sitzung (ca. 1 Woche gültig) → Wiederaufnahme
  std::string state = "pending";  // pending | uploading | done | failed
  uint64_t bytesSent = 0, total = 0;
  std::string videoId, error;
  int attempts = 0;
  int64_t nextTry = 0;      // Unix-Zeit
};

// Hintergrund-Uploader mit persistenter Warteschlange (%LOCALAPPDATA%\LiteCap\uploads.json).
// - Resumable Upload (Google-Protokoll): bei Verbindungsabbruch wird beim letzten bestätigten
//   Byte weitergemacht, auch nach einem Neustart von LiteCap oder Windows.
// - Wiederholung mit exponentiellem Backoff; Quota-Fehler → später erneut
// - Thread im Hintergrundmodus (niedrige CPU-/I/O-Priorität) + optionales Bandbreitenlimit
class YouTubeUploader {
 public:
  struct Notice {
    std::wstring title, text;
    bool isError;
  };
  using NoticeFn = std::function<void(const Notice&)>;

  YouTubeUploader() = default;
  ~YouTubeUploader() { Stop(); }
  YouTubeUploader(const YouTubeUploader&) = delete;
  YouTubeUploader& operator=(const YouTubeUploader&) = delete;

  void Start(NoticeFn onNotice);
  void Stop();
  void UpdateConfig(const YouTubeConfig& cfg);  // thread-sicher

  // Datei zur Warteschlange hinzufügen (Titel etc. aus der Config).
  void Enqueue(const std::filesystem::path& file, const std::wstring& source,
               const std::wstring& type, bool deleteAfter);

  // Kurzer Status für Tooltip, z. B. "Upload 42 % (1 weiterer)"; leer wenn nichts ansteht.
  std::wstring StatusText() const;

 private:
  void Run();
  void Process(UploadJob& job);
  void Load();
  void SaveLocked() const;
  void Save() const;

  mutable std::mutex mutex_;
  std::vector<UploadJob> jobs_;
  YouTubeConfig cfg_;
  NoticeFn onNotice_;
  HANDLE wake_ = nullptr;
  HANDLE stop_ = nullptr;
  std::thread thread_;
  std::atomic<int> activePercent_{-1};
};

} // namespace litecap
