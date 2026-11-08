#pragma once

#include <d3d11_1.h>
#include <winrt/base.h>

#include <unordered_map>

#include "capture/GpuDevice.h"

namespace litecap {

// BGRA → NV12 (BT.709, Studio-Range) inkl. Skalierung mit Seitenverhältnis (Letterbox),
// komplett auf der GPU über den D3D11 Video Processor (feste Funktionseinheit, kaum Shader-Last).
class ColorConverter {
 public:
  // Wirft winrt::hresult_error, wenn die GPU keinen Video Processor anbietet.
  void Init(const GpuDevice& gpu, UINT outWidth, UINT outHeight, int fps);
  void Reset();

  // Aufrufer muss die GPU-Sperre (GpuLock) halten.
  // dst: NV12-Textur (BIND_RENDER_TARGET), ggf. Texture-Array mit Index dstSlice.
  void Convert(ID3D11Texture2D* src, UINT srcWidth, UINT srcHeight, ID3D11Texture2D* dst,
               UINT dstSlice = 0);

  UINT OutWidth() const { return outW_; }
  UINT OutHeight() const { return outH_; }

 private:
  void CreateProcessor(UINT inW, UINT inH);
  ID3D11VideoProcessorInputView* InputView(ID3D11Texture2D* tex);
  ID3D11VideoProcessorOutputView* OutputView(ID3D11Texture2D* tex, UINT slice);

  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11VideoDevice> videoDevice_;
  winrt::com_ptr<ID3D11VideoContext> videoContext_;
  winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
  winrt::com_ptr<ID3D11VideoProcessor> processor_;
  UINT inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
  int fps_ = 60;

  // WGC verwendet seine Pool-Texturen wieder → Views zwischenspeichern.
  struct InEntry {
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11VideoProcessorInputView> view;
  };
  struct OutEntry {
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> view;
  };
  std::unordered_map<ID3D11Texture2D*, InEntry> inViews_;
  std::unordered_map<uint64_t, OutEntry> outViews_;
};

} // namespace litecap
