#include "pipeline/CaptureSession.h"

#include <avrt.h>

#include <algorithm>
#include <format>

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

static GpuVendor PreferredVendor(const std::string& encoder) {
  if (encoder == "nvenc") return GpuVendor::Nvidia;
  if (encoder == "amf") return GpuVendor::Amd;
  if (encoder == "qsv") return GpuVendor::Intel;
  return GpuVendor::Unknown;  // auto → GPU des Monitors
}

std::string CaptureSession::Description() const {
  std::string d = encoder_.Description();
  for (size_t i = 1; i < streams_.size(); ++i) d += (i == 1 ? ", Ton: " : " + ") + streams_[i].title;
  return d;
}

void CaptureSession::Start(const Config& cfg, Callbacks cb) {
  Stop();
  try {
    StartImpl(cfg, std::move(cb));
  } catch (...) {
    Cleanup();
    throw;
  }
}

void CaptureSession::StartImpl(const Config& cfg, Callbacks cb) {
  cb_ = std::move(cb);
  fps_ = cfg.video.fps;

  std::string err;
  auto target = ResolveTarget(cfg.capture, err);
  if (!target) throw std::runtime_error(err);
  target_ = *target;

  gpu_.Create(PreferredVendor(cfg.video.encoder), target_.HostMonitor());

  // Capture starten, um die Quellgröße zu kennen; Frames werden verworfen, bis alles steht.
  capture_.Start(
      gpu_, target_, cfg.capture.captureCursor, fps_,
      [this](const CapturedFrame& f) { OnCapturedFrame(f); },
      [this] {
        if (cb_.onSourceClosed) cb_.onSourceClosed();
      });

  auto src = capture_.SourceSize();
  int outW = cfg.video.width ? cfg.video.width : src.Width;
  int outH = cfg.video.height ? cfg.video.height : src.Height;
  outW &= ~1;
  outH &= ~1;

  VideoEncoderSettings vs;
  vs.width = outW;
  vs.height = outH;
  vs.fps = fps_;
  vs.rateControl = cfg.video.rateControl;
  vs.bitrateKbps = cfg.video.bitrateKbps;
  vs.quality = cfg.video.quality;
  vs.keyframeIntervalSec = cfg.video.keyframeIntervalSec;
  encoder_.Open(gpu_, EncoderCandidates(cfg.video.codec, cfg.video.encoder, gpu_.Vendor()), vs);

  {
    GpuLock lock(gpu_.Multithread());
    converter_.Init(gpu_, static_cast<UINT>(outW), static_cast<UINT>(outH), fps_);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(outW);
    td.Height = static_cast<UINT>(outH);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    winrt::check_hresult(gpu_.Device()->CreateTexture2D(&td, nullptr, latest_.put()));
  }

  videoParams_ = avcodec_parameters_alloc();
  AvCheck(avcodec_parameters_from_context(videoParams_, encoder_.Context()),
          "avcodec_parameters_from_context");
  streams_ = {{videoParams_, encoder_.TimeBase(), "Video"}};

  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  LARGE_INTEGER origin;
  QueryPerformanceCounter(&origin);
  originQpc_ = origin.QuadPart;
  SetupAudio(cfg.audio);
  errorReported_ = false;
  queueClosed_ = false;
  writeClosed_ = false;
  running_ = true;
  writerThread_ = std::thread([this] { WriterLoop(); });
  submitThread_ = std::thread([this] { SubmitLoop(); });
  thread_ = std::thread([this] { EncodeLoop(); });
  log::Info("Session gestartet: {}", Description());
}

void CaptureSession::Stop() {
  if (!running_ && !thread_.joinable()) return;
  Cleanup();
  log::Info("Session beendet");
}

void CaptureSession::SetupAudio(const AudioConfig& cfg) {
  struct Plan {
    std::string title;
    std::vector<WasapiSource::Kind> inputs;
  };
  std::vector<Plan> plans;
  using K = WasapiSource::Kind;
  if (cfg.system && cfg.microphone && !cfg.separateTracks) {
    plans.push_back({"System + Mikrofon", {K::Loopback, K::Microphone}});
  } else {
    if (cfg.system) plans.push_back({"System", {K::Loopback}});
    if (cfg.microphone) plans.push_back({"Mikrofon", {K::Microphone}});
  }

  for (auto& plan : plans) {
    const int streamIndex = static_cast<int>(streams_.size());
    auto track = std::make_unique<AudioTrack>();
    track->Open(cfg.bitrateKbps, static_cast<int>(plan.inputs.size()), plan.title,
                [this, streamIndex](AVPacket* p) {
                  p->stream_index = streamIndex;
                  Dispatch(p);
                });
    AVCodecParameters* par = avcodec_parameters_alloc();
    AvCheck(avcodec_parameters_from_context(par, track->Context()), "Audio-Parameter");
    audioParams_.push_back(par);
    streams_.push_back({par, track->TimeBase(), plan.title});

    for (size_t i = 0; i < plan.inputs.size(); ++i) {
      auto src = std::make_unique<WasapiSource>();
      AudioTrack* t = track.get();
      const int input = static_cast<int>(i);
      std::wstring device = plan.inputs[i] == K::Microphone ? Utf8ToWide(cfg.microphoneDeviceId)
                                                            : std::wstring();
      src->Start(plan.inputs[i], device, originQpc_,
                 [this, t, input](const float* data, size_t frames) {
                   try {
                     t->Write(input, data, frames);
                   } catch (const std::exception& e) {
                     ReportError(std::string("Audio: ") + e.what());
                   }
                 });
      sources_.push_back(std::move(src));
    }
    tracks_.push_back(std::move(track));
  }
}

void CaptureSession::Cleanup() {
  capture_.Stop();
  sources_.clear();                    // Audio-Threads beenden
  for (auto& t : tracks_) t->Flush();  // letzte Audio-Pakete ausgeben
  tracks_.clear();
  for (auto*& p : audioParams_) avcodec_parameters_free(&p);
  audioParams_.clear();
  if (stopEvent_) SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();              // schließt die Warteschlange
  if (submitThread_.joinable()) submitThread_.join();  // leert sie und flusht den Encoder
  queue_.clear();
  {
    std::lock_guard lock(writeMutex_);
    writeClosed_ = true;  // Schreib-Thread arbeitet die Warteschlange noch ab
  }
  writeCv_.notify_all();
  if (writerThread_.joinable()) writerThread_.join();
  writeQueue_.clear();
  writeQueueBytes_ = 0;
  LogStats(true);
  running_ = false;
  if (stopEvent_) {
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
  {
    std::lock_guard lock(sinkMutex_);
    sinks_.clear();
  }
  encoder_.Close();
  if (gpu_.Multithread()) {
    GpuLock lock(gpu_.Multithread());
    converter_.Reset();
    latest_ = nullptr;
  }
  haveFrame_ = false;
  streams_.clear();
  avcodec_parameters_free(&videoParams_);
}

void CaptureSession::OnCapturedFrame(const CapturedFrame& f) {
  GpuLock lock(gpu_.Multithread());
  if (!latest_) return;  // Pipeline wird noch aufgebaut
  converter_.Convert(f.texture, f.width, f.height, latest_.get());
  gpu_.Context()->Flush();  // Konvertierung sofort einreihen (siehe VideoEncoder::Encode)
  haveFrame_ = true;
}

int CaptureSession::AddSink(PacketSink sink, bool needKeyframe) {
  std::lock_guard lock(sinkMutex_);
  int id = nextSinkId_++;
  sinks_.push_back({id, std::move(sink), needKeyframe});
  if (needKeyframe) forceKeyframe_ = true;
  return id;
}

void CaptureSession::RemoveSink(int id) {
  std::lock_guard lock(sinkMutex_);
  std::erase_if(sinks_, [id](const Sink& s) { return s.id == id; });
}

// Aus Encoder- und Audio-Threads: nur einreihen (Referenz, keine Kopie der Daten), nie warten.
void CaptureSession::Dispatch(const AVPacket* pkt) {
  constexpr size_t kMaxQueuedBytes = 256u << 20;  // ~100 s bei 20 Mbit/s – Notbremse
  AvPacketPtr copy(av_packet_clone(pkt));
  if (!copy) return;
  {
    std::lock_guard lock(writeMutex_);
    if (writeQueueBytes_ + static_cast<size_t>(pkt->size) > kMaxQueuedBytes) {
      ++statWriteDrops_;  // Datenträger hängt dauerhaft → lieber verwerfen als RAM füllen
      return;
    }
    writeQueueBytes_ += static_cast<size_t>(pkt->size);
    writeQueue_.push_back(std::move(copy));
    uint64_t n = writeQueue_.size();
    if (n > statWriteQueueMax_) statWriteQueueMax_ = n;
  }
  writeCv_.notify_one();
}

void CaptureSession::WriterLoop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  for (;;) {
    AvPacketPtr pkt;
    {
      std::unique_lock lock(writeMutex_);
      writeCv_.wait(lock, [this] { return !writeQueue_.empty() || writeClosed_; });
      if (writeQueue_.empty()) return;
      pkt = std::move(writeQueue_.front());
      writeQueue_.pop_front();
      writeQueueBytes_ -= static_cast<size_t>(pkt->size);
    }
    QueryPerformanceCounter(&t0);
    DeliverToSinks(pkt.get());
    QueryPerformanceCounter(&t1);
    uint64_t us = static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1'000'000 / freq.QuadPart);
    if (us > statWriteMaxUs_) statWriteMaxUs_ = us;
  }
}

void CaptureSession::LogStats(bool final) {
  uint64_t prepared = statPrepared_.exchange(0), late = statLate_.exchange(0),
           drops = statQueueDrops_.exchange(0), wdrops = statWriteDrops_.exchange(0),
           submitMax = statSubmitMaxUs_.exchange(0), writeMax = statWriteMaxUs_.exchange(0),
           qmax = statWriteQueueMax_.exchange(0);
  if (!prepared && !final) return;
  auto fmt = [&] {
    return std::format("{} Frames, ausgelassen {} (spät) + {} (Encoder voll), Encoder max {:.1f} ms, "
                       "Schreiben max {:.1f} ms (Warteschlange max {}{})",
                       prepared, late, drops, submitMax / 1000.0, writeMax / 1000.0, qmax,
                       wdrops ? std::format(", {} Pakete VERWORFEN", wdrops) : "");
  };
  if (late + drops + wdrops > 0) log::Warn("Pipeline: {}", fmt());
  else log::Info("Pipeline: {}", fmt());
}

// Schreib-Thread: Pakete an Aufnahme / Replay-Puffer verteilen.
void CaptureSession::DeliverToSinks(const AVPacket* pkt) {
  std::lock_guard lock(sinkMutex_);
  const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0 && pkt->stream_index == 0;
  for (auto& s : sinks_) {
    if (s.waitingForKey) {
      if (!key) continue;
      s.waitingForKey = false;
    }
    s.fn(pkt);
  }
}

void CaptureSession::ReportError(const std::string& msg) {
  bool expected = false;
  if (!errorReported_.compare_exchange_strong(expected, true)) return;  // nur der erste Fehler
  log::Error("Encoder-Fehler: {}", msg);
  SetEvent(stopEvent_);
  if (cb_.onError) cb_.onError(msg);
}

// Takt-Thread: liefert exakt fps Bilder pro Sekunde (konstante Bildrate). Er kopiert nur das
// aktuelle Bild auf der GPU und reiht es ein – die Übergabe an den Encoder passiert in
// SubmitLoop, damit ein wartender Encoder keinen Takt verpasst.
void CaptureSession::EncodeLoop() {
  // MMCSS: Windows bevorzugt den Thread kurz, wenn er aufwacht → pünktliche Frames auch
  // unter Spielelast, ohne dauerhaft Rechenzeit zu beanspruchen.
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // vor Windows 10 1803

  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  const int64_t start = originQpc_;  // gleicher Nullpunkt wie der Ton
  const double ticksPerFrame = static_cast<double>(freq.QuadPart) / fps_;
  int64_t frameIndex = 0;
  uint64_t lateSkips = 0, queueDrops = 0, prepared = 0;

  HANDLE waits[2] = {stopEvent_, timer};
  try {
    for (;;) {
      // Absoluter Zeitplan (kein Aufsummieren von Fehlern): Frame n fällig bei start + n/fps
      QueryPerformanceCounter(&now);
      int64_t due = start + static_cast<int64_t>(frameIndex * ticksPerFrame);
      int64_t waitTicks = due - now.QuadPart;
      if (waitTicks > 0) {
        LARGE_INTEGER rel;
        rel.QuadPart = -static_cast<int64_t>(waitTicks * 10'000'000.0 / freq.QuadPart);
        SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
      } else if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
        break;
      }

      // Zu spät aufgewacht? Verpasste Takte überspringen statt aufzuholen;
      // die Zeitstempel bleiben trotzdem korrekt.
      QueryPerformanceCounter(&now);
      int64_t behind = static_cast<int64_t>((now.QuadPart - start) / ticksPerFrame);
      if (behind > frameIndex + 1) {
        lateSkips += static_cast<uint64_t>(behind - frameIndex);
        statLate_ += static_cast<uint64_t>(behind - frameIndex);
        frameIndex = behind;
      }
      // Alle 10 s eine Statistikzeile (macht Aussetzer im Log sichtbar)
      if (frameIndex > 0 && frameIndex % (fps_ * 10) == 0) LogStats(false);

      if (haveFrame_) {
        if (gpu_.IsLost())
          throw std::runtime_error("Die GPU wurde zurückgesetzt (Treiber-Neustart)");
        bool full;
        {
          std::lock_guard lock(queueMutex_);
          full = queue_.size() >= kMaxQueuedFrames;
        }
        if (full) {
          ++queueDrops;  // Encoder kommt nicht hinterher → Bild auslassen, nicht blockieren
          ++statQueueDrops_;
        } else {
          AvFramePtr f =
              encoder_.PrepareFrame(latest_.get(), frameIndex, forceKeyframe_.exchange(false));
          {
            std::lock_guard lock(queueMutex_);
            queue_.push_back(std::move(f));
          }
          queueCv_.notify_one();
          ++prepared;
          ++statPrepared_;
        }
      }
      ++frameIndex;
    }
  } catch (const std::exception& e) {
    ReportError(e.what());
  } catch (const winrt::hresult_error& e) {
    ReportError(WideToUtf8(e.message()));
  }

  {
    std::lock_guard lock(queueMutex_);
    queueClosed_ = true;
  }
  queueCv_.notify_one();
  log::Info("Bildtakt: {} Frames erzeugt, ausgelassen: {} (später geweckt), {} (Encoder voll)",
            prepared, lateSkips, queueDrops);
  CloseHandle(timer);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// Encoder-Thread: übergibt die vorbereiteten Frames an den Encoder und verteilt die Pakete.
void CaptureSession::SubmitLoop() {
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
  auto onPacket = [this](AVPacket* p) {
    p->stream_index = 0;
    if (p->duration <= 0) p->duration = 1;  // ein Frame (Timebase 1/fps)
    Dispatch(p);
  };
  double maxMs = 0, sumMs = 0;
  uint64_t count = 0;
  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);

  try {
    for (;;) {
      AvFramePtr f;
      {
        std::unique_lock lock(queueMutex_);
        queueCv_.wait(lock, [this] { return !queue_.empty() || queueClosed_; });
        if (queue_.empty()) break;  // geschlossen und leer
        f = std::move(queue_.front());
        queue_.pop_front();
      }
      if (errorReported_) continue;  // nach einem Fehler nur noch leeren
      QueryPerformanceCounter(&t0);
      encoder_.Submit(f.get(), onPacket);
      QueryPerformanceCounter(&t1);
      double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
      if (uint64_t us = static_cast<uint64_t>(ms * 1000); us > statSubmitMaxUs_) statSubmitMaxUs_ = us;
      sumMs += ms;
      maxMs = std::max(maxMs, ms);
      ++count;
    }
    if (!errorReported_) encoder_.Flush(onPacket);
  } catch (const std::exception& e) {
    ReportError(e.what());
  } catch (const winrt::hresult_error& e) {
    ReportError(WideToUtf8(e.message()));
  }
  if (count)
    log::Info("Encoder: {} Frames übergeben, Ø {:.2f} ms, max {:.1f} ms pro Frame", count,
              sumMs / count, maxMs);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

} // namespace litecap
