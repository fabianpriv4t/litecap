#pragma once

#include <atomic>
#include <mutex>
#include <filesystem>
#include <string>
#include <vector>

#include "output/Muxer.h"
#include "pipeline/CaptureSession.h"

namespace litecap {

// Manuelle Aufnahme: hängt sich an eine laufende CaptureSession und schreibt deren
// Pakete (ab dem nächsten Keyframe) in eine Datei.
class Recorder {
 public:
  Recorder() = default;
  ~Recorder() { Stop(); }
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  void Start(CaptureSession& session, const std::filesystem::path& file);
  // Finalisiert die Datei; wirft bei Schreibfehlern. Liefert den Pfad.
  std::filesystem::path Stop();

  bool Active() const { return session_ != nullptr; }
  // Schreibfehler im Encoder-Thread (z. B. Datenträger voll) → App fragt periodisch ab.
  bool Failed() const { return failed_; }
  std::string FailureReason() const;
  int64_t BytesWritten() const { return muxer_.BytesWritten(); }
  double DurationSec() const;

 private:
  void OnPacket(const AVPacket* pkt);

  CaptureSession* session_ = nullptr;
  int sinkId_ = 0;
  Muxer muxer_;
  std::vector<AVRational> tb_;
  std::atomic<int64_t> startPts_{AV_NOPTS_VALUE};  // Video-Timebase, gemeinsamer Nullpunkt
  std::atomic<int64_t> lastVideoPts_{0};
  std::atomic<bool> failed_{false};
  mutable std::mutex errMutex_;
  std::string error_;
};

} // namespace litecap
