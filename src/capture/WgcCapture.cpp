#include "capture/WgcCapture.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace {

wgc::GraphicsCaptureItem CreateItem(const CaptureTarget& t) {
  auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();
  wgc::GraphicsCaptureItem item{nullptr};
  if (t.window) {
    winrt::check_hresult(interop->CreateForWindow(
        t.window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
  } else {
    winrt::check_hresult(interop->CreateForMonitor(
        t.monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
  }
  return item;
}

template <class T> winrt::com_ptr<T> SurfaceAs(const wgd::Direct3D11::IDirect3DSurface& s) {
  auto access = s.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  winrt::com_ptr<T> result;
  winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
  return result;
}

constexpr wchar_t kSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";

} // namespace

bool WgcCapture::IsSupported() {
  try {
    return wgc::GraphicsCaptureSession::IsSupported();
  } catch (...) {
    return false;
  }
}

void WgcCapture::Start(const GpuDevice& gpu, const CaptureTarget& target, bool captureCursor,
                       int maxFps, FrameCallback onFrame, ClosedCallback onClosed) {
  Stop();
  if (!IsSupported())
    throw winrt::hresult_error(E_NOTIMPL, L"Windows.Graphics.Capture wird von diesem System "
                                          L"nicht unterstützt (Windows 10 1903 oder neuer nötig)");

  device_ = gpu.WinRtDevice();
  item_ = CreateItem(target);
  sourceSize_ = poolSize_ = item_.Size();
  {
    std::lock_guard lock(callbackMutex_);
    onFrame_ = std::move(onFrame);
    onClosed_ = std::move(onClosed);
  }
  // Etwas Toleranz (75 %), damit bei leicht schwankender Bildrate nicht jeder 2. Frame fällt.
  minInterval100ns_ = maxFps > 0 ? (10'000'000LL / maxFps) * 3 / 4 : 0;
  lastAccepted100ns_ = 0;
  delivered_ = dropped_ = 0;

  // Free-threaded: Frames kommen auf einem Threadpool-Thread, kein DispatcherQueue nötig.
  // 2 Puffer genügen, da wir sofort auf der GPU weiterkopieren.
  pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
      device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize_);
  frameToken_ = pool_.FrameArrived([this](auto& pool, auto&) { OnFrameArrived(pool); });
  closedToken_ = item_.Closed([this](auto&, auto&) {
    log::Info("Capture-Ziel wurde geschlossen");
    ClosedCallback cb;
    {
      std::lock_guard lock(callbackMutex_);
      cb = onClosed_;
    }
    if (cb) cb();
  });

  session_ = pool_.CreateCaptureSession(item_);

  if (ApiInformation::IsPropertyPresent(kSessionClass, L"IsCursorCaptureEnabled"))
    session_.IsCursorCaptureEnabled(captureCursor);

  // Gelben Rahmen abschalten (Windows 11). Erfordert einmalige "Borderless"-Freigabe.
  if (ApiInformation::IsPropertyPresent(kSessionClass, L"IsBorderRequired")) {
    try {
      wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless)
          .get();
      session_.IsBorderRequired(false);
    } catch (const winrt::hresult_error& e) {
      log::Debug("Rahmenlose Aufnahme nicht möglich: {}", WideToUtf8(e.message()));
    }
  }

  // Windows 11 24H2+: Das System liefert gar nicht erst mehr Frames als nötig → spart GPU-Zeit.
  if (maxFps > 0 && ApiInformation::IsPropertyPresent(kSessionClass, L"MinUpdateInterval")) {
    session_.MinUpdateInterval(
        winrt::Windows::Foundation::TimeSpan{10'000'000LL / maxFps});
  }

  session_.StartCapture();
  log::Info("Capture gestartet: {} ({}x{}, max. {} fps)", WideToUtf8(target.name),
            sourceSize_.Width, sourceSize_.Height, maxFps);
}

void WgcCapture::Stop() {
  {
    // Ab hier keine Callbacks mehr an den Aufrufer (ein laufender wird abgewartet).
    std::lock_guard lock(callbackMutex_);
    onFrame_ = nullptr;
    onClosed_ = nullptr;
  }
  if (pool_) {
    pool_.FrameArrived(frameToken_);
  }
  if (item_) item_.Closed(closedToken_);
  if (session_) {
    session_.Close();
    session_ = nullptr;
  }
  if (pool_) {
    pool_.Close();
    pool_ = nullptr;
  }
  item_ = nullptr;
  device_ = nullptr;
  if (delivered_ || dropped_)
    log::Info("Capture gestoppt ({} Frames geliefert, {} verworfen)", delivered_.load(),
              dropped_.load());
}

void WgcCapture::OnFrameArrived(const wgc::Direct3D11CaptureFramePool& pool) {
  wgc::Direct3D11CaptureFrame frame = pool.TryGetNextFrame();
  if (!frame) return;

  const int64_t t = frame.SystemRelativeTime().count();
  const auto size = frame.ContentSize();

  // Größenänderung (Fenster skaliert, Auflösung gewechselt) → Pool anpassen.
  if (size.Width != poolSize_.Width || size.Height != poolSize_.Height) {
    if (size.Width > 0 && size.Height > 0) {
      poolSize_ = size;
      pool.Recreate(device_, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize_);
    }
    frame.Close();
    return;
  }

  if (minInterval100ns_ && lastAccepted100ns_ && t - lastAccepted100ns_ < minInterval100ns_) {
    ++dropped_;
    frame.Close();
    return;
  }
  lastAccepted100ns_ = t;

  {
    std::lock_guard lock(callbackMutex_);
    if (onFrame_) {
      try {
        auto tex = SurfaceAs<ID3D11Texture2D>(frame.Surface());
        onFrame_(CapturedFrame{tex.get(), static_cast<UINT>(size.Width),
                               static_cast<UINT>(size.Height), t});
        ++delivered_;
      } catch (const winrt::hresult_error& e) {
        log::Warn("Frame-Verarbeitung fehlgeschlagen: {}", WideToUtf8(e.message()));
      }
    }
  }
  frame.Close();  // Puffer sofort an den Pool zurückgeben
}

} // namespace litecap
