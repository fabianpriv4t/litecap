#pragma once

#include <d3d11.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#include "capture/CaptureTarget.h"
#include "capture/GpuDevice.h"

namespace litecap {

struct CapturedFrame {
  ID3D11Texture2D* texture;  // BGRA, nur während des Callbacks gültig
  UINT width, height;        // tatsächlicher Bildinhalt (Textur kann größer sein)
  int64_t time100ns;         // SystemRelativeTime (QPC-basiert, 100-ns-Einheiten)
};

// Bildschirm-/Fensteraufnahme über Windows.Graphics.Capture.
// Die Frames bleiben als Textur auf der GPU; es wird nichts in den RAM kopiert.
class WgcCapture {
 public:
  using FrameCallback = std::function<void(const CapturedFrame&)>;
  using ClosedCallback = std::function<void()>;

  WgcCapture() = default;
  ~WgcCapture() { Stop(); }
  WgcCapture(const WgcCapture&) = delete;
  WgcCapture& operator=(const WgcCapture&) = delete;

  static bool IsSupported();

  // onFrame läuft auf einem Threadpool-Thread und sollte nur kurz GPU-Befehle absetzen.
  // maxFps begrenzt die Rate schon an der Quelle (überzählige Frames werden verworfen).
  void Start(const GpuDevice& gpu, const CaptureTarget& target, bool captureCursor, int maxFps,
             FrameCallback onFrame, ClosedCallback onClosed);
  void Stop();

  bool Running() const { return session_ != nullptr; }
  // Größe der Quelle beim Start (für die Encoder-Auflösung)
  winrt::Windows::Graphics::SizeInt32 SourceSize() const { return sourceSize_; }
  uint64_t FramesDelivered() const { return delivered_; }
  uint64_t FramesDropped() const { return dropped_; }

 private:
  void OnFrameArrived(const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& pool);

  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
  winrt::event_token frameToken_{}, closedToken_{};

  std::mutex callbackMutex_;  // schützt onFrame_ gegen Stop() während eines Callbacks
  FrameCallback onFrame_;
  ClosedCallback onClosed_;
  winrt::Windows::Graphics::SizeInt32 sourceSize_{};
  winrt::Windows::Graphics::SizeInt32 poolSize_{};
  int64_t minInterval100ns_ = 0;
  int64_t lastAccepted100ns_ = 0;
  std::atomic<uint64_t> delivered_{0}, dropped_{0};
};

} // namespace litecap
