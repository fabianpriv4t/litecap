#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/AudioTrack.h"
#include "audio/WasapiSource.h"
#include "capture/CaptureTarget.h"
#include "capture/ColorConverter.h"
#include "capture/GpuDevice.h"
#include "capture/WgcCapture.h"
#include "config/Config.h"
#include "encode/VideoEncoder.h"

namespace litecap {

struct StreamInfo {
  const AVCodecParameters* params;  // gültig solange die Session läuft
  AVRational timeBase;
  std::string title;                // z. B. "System", "Mikrofon" (Spurname in der Datei)
};

// Eine laufende Capture→Encoder-Pipeline. Aufnahme (Recorder) und Replay-Puffer hängen
// sich als "Sinks" an denselben Paketstrom – es wird also nur EINMAL kodiert, egal wie
// viele Ausgaben aktiv sind.
class CaptureSession {
 public:
  // Pakete: stream_index = Index in Streams(), Zeitstempel in deren timeBase.
  using PacketSink = std::function<void(const AVPacket*)>;
  struct Callbacks {
    std::function<void(const std::string&)> onError;  // aus beliebigem Thread
    std::function<void()> onSourceClosed;             // Fenster geschlossen o. Ä.
  };

  CaptureSession() = default;
  ~CaptureSession() { Stop(); }
  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;

  // Wirft std::runtime_error / winrt::hresult_error mit verständlicher Meldung.
  void Start(const Config& cfg, Callbacks cb);
  void Stop();
  bool Running() const { return running_; }

  // needKeyframe: Sink erhält Pakete erst ab dem nächsten Keyframe; der Encoder wird
  // angewiesen, sofort einen zu erzeugen. Liefert eine ID für RemoveSink.
  int AddSink(PacketSink sink, bool needKeyframe);
  void RemoveSink(int id);

  const std::vector<StreamInfo>& Streams() const { return streams_; }
  const VideoEncoder& Video() const { return encoder_; }
  const std::wstring& SourceName() const { return target_.name; }
  std::string Description() const;

 private:
  void StartImpl(const Config& cfg, Callbacks cb);
  void SetupAudio(const AudioConfig& cfg);
  void Cleanup();
  void EncodeLoop();
  void SubmitLoop();
  void WriterLoop();
  void DeliverToSinks(const AVPacket* pkt);
  void LogStats(bool final);
  void ReportError(const std::string& msg);
  void Dispatch(const AVPacket* pkt);
  void OnCapturedFrame(const CapturedFrame& f);

  Callbacks cb_;
  CaptureTarget target_;
  GpuDevice gpu_;
  WgcCapture capture_;
  ColorConverter converter_;
  VideoEncoder encoder_;
  winrt::com_ptr<ID3D11Texture2D> latest_;  // letztes Bild als NV12 (in Ausgabegröße)
  std::atomic<bool> haveFrame_{false};

  AVCodecParameters* videoParams_ = nullptr;
  std::vector<AVCodecParameters*> audioParams_;
  std::vector<std::unique_ptr<AudioTrack>> tracks_;
  std::vector<std::unique_ptr<WasapiSource>> sources_;
  // Gemeinsamer Zeitnullpunkt (QPC) für Video-Frame 0 und Audio-Sample 0 → A/V-Sync
  int64_t originQpc_ = 0;
  std::vector<StreamInfo> streams_;

  struct Sink {
    int id;
    PacketSink fn;
    bool waitingForKey;
  };
  std::mutex sinkMutex_;
  std::vector<Sink> sinks_;
  int nextSinkId_ = 1;
  std::atomic<bool> forceKeyframe_{false};

  // Warteschlange Takt-Thread → Encoder-Thread. 8 Frames ≈ 130 ms bei 60 fps; die Frames
  // liegen im VRAM (je ~3 MB bei 1080p), nicht im RAM.
  static constexpr size_t kMaxQueuedFrames = 8;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<AvFramePtr> queue_;
  bool queueClosed_ = false;
  std::atomic<bool> errorReported_{false};

  // Warteschlange Encoder/Audio → Schreib-Thread. Datei-I/O (Aufnahme, Replay-Ringdatei) darf
  // den Encoder nie aufhalten – eine kurz hängende Festplatte (Virenscanner, Cloud-Sync, HDD)
  // würde sonst Frames kosten.
  std::mutex writeMutex_;
  std::condition_variable writeCv_;
  std::deque<AvPacketPtr> writeQueue_;
  size_t writeQueueBytes_ = 0;
  bool writeClosed_ = false;
  std::thread writerThread_;

  // Statistik (alle 10 s ins Log)
  std::atomic<uint64_t> statPrepared_{0}, statLate_{0}, statQueueDrops_{0}, statWriteDrops_{0};
  std::atomic<uint64_t> statSubmitMaxUs_{0}, statWriteMaxUs_{0}, statWriteQueueMax_{0};

  int fps_ = 60;
  std::thread thread_, submitThread_;
  HANDLE stopEvent_ = nullptr;
  std::atomic<bool> running_{false};
};

} // namespace litecap
