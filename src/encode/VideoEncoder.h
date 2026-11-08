#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <functional>
#include <string>
#include <vector>

#include "capture/GpuDevice.h"
#include "encode/EncoderInfo.h"
#include "encode/FFmpeg.h"

namespace litecap {

struct VideoEncoderSettings {
  int width = 1920, height = 1080, fps = 60;
  std::string rateControl = "vbr";  // cbr | vbr | cqp
  int bitrateKbps = 20000;
  int quality = 23;
  int keyframeIntervalSec = 2;
};

// Kodiert NV12-Texturen. Hardware-Encoder lesen die Textur direkt von der GPU (Zero-Copy);
// nur der Software-Fallback (x264) kopiert das Bild in den RAM.
class VideoEncoder {
 public:
  using PacketCallback = std::function<void(AVPacket*)>;

  VideoEncoder() = default;
  ~VideoEncoder() { Close(); }
  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  // Probiert die Kandidaten der Reihe nach, der erste funktionierende gewinnt.
  // Wirft std::runtime_error, wenn keiner funktioniert. Fehlversuche landen in `log`.
  void Open(const GpuDevice& gpu, const std::vector<EncoderChoice>& candidates,
            const VideoEncoderSettings& settings);
  void Close();

  // Zweistufig, damit ein wartender Encoder den Bildtakt nicht aufhält:
  // 1) PrepareFrame (Takt-Thread): kopiert nv12 auf der GPU in einen Encoder-Frame (< 0,1 ms).
  //    pts in Frame-Einheiten (Timebase 1/fps); forceKeyframe erzwingt einen IDR-Frame.
  //    Aufrufer hält KEINE GpuLock.
  AvFramePtr PrepareFrame(ID3D11Texture2D* nv12, int64_t pts, bool forceKeyframe);
  // 2) Submit (Encoder-Thread): übergibt an den Encoder; kann warten, bis die GPU die Kopie
  //    ausgeführt hat (bei GPU-limitierten Spielen mehrere Millisekunden).
  void Submit(AVFrame* frame, const PacketCallback& onPacket);
  // Restliche Pakete ausgeben (am Ende einer Aufnahme).
  void Flush(const PacketCallback& onPacket);

  const AVCodecContext* Context() const { return ctx_.get(); }
  const EncoderChoice& Choice() const { return choice_; }
  AVRational TimeBase() const { return {1, settings_.fps}; }
  std::string Description() const;

 private:
  bool TryOpen(const EncoderChoice& c, std::string& error);
  void SetupHwFrames(Backend b);
  void ApplyOptions(Backend b, AVDictionary** opts);
  void Drain(const PacketCallback& onPacket);

  const GpuDevice* gpu_ = nullptr;
  VideoEncoderSettings settings_;
  EncoderChoice choice_{};
  AvCodecCtxPtr ctx_;
  AvBufferPtr d3dDevice_, d3dFrames_;  // FFmpeg-Hülle um unser D3D11-Device
  AvBufferPtr qsvDevice_, qsvFrames_;  // nur bei Quick Sync
  AvPacketPtr packet_;
  // x264-Fallback
  winrt::com_ptr<ID3D11Texture2D> staging_;
};

} // namespace litecap
