#include "capture/ColorConverter.h"

#include <algorithm>

#include "util/Log.h"

namespace litecap {

void ColorConverter::Init(const GpuDevice& gpu, UINT outWidth, UINT outHeight, int fps) {
  Reset();
  device_.copy_from(gpu.Device());
  videoDevice_ = device_.as<ID3D11VideoDevice>();
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  ctx.copy_from(gpu.Context());
  videoContext_ = ctx.as<ID3D11VideoContext>();
  outW_ = outWidth & ~1u;
  outH_ = outHeight & ~1u;
  fps_ = fps;
}

void ColorConverter::Reset() {
  inViews_.clear();
  outViews_.clear();
  processor_ = nullptr;
  enumerator_ = nullptr;
  videoContext_ = nullptr;
  videoDevice_ = nullptr;
  device_ = nullptr;
  inW_ = inH_ = 0;
}

void ColorConverter::CreateProcessor(UINT inW, UINT inH) {
  inViews_.clear();  // Input-Views hängen am Enumerator
  outViews_.clear();
  processor_ = nullptr;
  enumerator_ = nullptr;

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
  desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  desc.InputWidth = inW;
  desc.InputHeight = inH;
  desc.OutputWidth = outW_;
  desc.OutputHeight = outH_;
  desc.InputFrameRate = {static_cast<UINT>(fps_), 1};
  desc.OutputFrameRate = {static_cast<UINT>(fps_), 1};
  desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
  winrt::check_hresult(videoDevice_->CreateVideoProcessorEnumerator(&desc, enumerator_.put()));

  UINT support = 0;
  winrt::check_hresult(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support));
  if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
    throw winrt::hresult_error(E_NOTIMPL, L"GPU kann nicht nach NV12 konvertieren");

  winrt::check_hresult(videoDevice_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()));

  // Farbräume: Desktop = sRGB/Full-Range, Video = BT.709/Studio-Range (Standard für H.264/HEVC)
  if (auto vc1 = videoContext_.try_as<ID3D11VideoContext1>()) {
    vc1->VideoProcessorSetStreamColorSpace1(processor_.get(), 0,
                                            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vc1->VideoProcessorSetOutputColorSpace1(processor_.get(),
                                            DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  } else {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {}, out = {};
    in.RGB_Range = 0;       // 0-255
    out.RGB_Range = 1;      // 16-235
    out.YCbCr_Matrix = 1;   // BT.709
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &in);
    videoContext_->VideoProcessorSetOutputColorSpace(processor_.get(), &out);
  }

  // Seitenverhältnis erhalten, Rand schwarz füllen
  D3D11_VIDEO_COLOR black = {};
  black.YCbCr = {16.0f / 255.0f, 0.5f, 0.5f, 1.0f};
  videoContext_->VideoProcessorSetOutputBackgroundColor(processor_.get(), TRUE, &black);

  RECT srcRect = {0, 0, static_cast<LONG>(inW), static_cast<LONG>(inH)};
  double scale = std::min(static_cast<double>(outW_) / inW, static_cast<double>(outH_) / inH);
  LONG dw = static_cast<LONG>(inW * scale + 0.5) & ~1L, dh = static_cast<LONG>(inH * scale + 0.5) & ~1L;
  LONG dx = ((static_cast<LONG>(outW_) - dw) / 2) & ~1L, dy = ((static_cast<LONG>(outH_) - dh) / 2) & ~1L;
  RECT dstRect = {dx, dy, dx + dw, dy + dh};
  RECT outRect = {0, 0, static_cast<LONG>(outW_), static_cast<LONG>(outH_)};
  videoContext_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &srcRect);
  videoContext_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &dstRect);
  videoContext_->VideoProcessorSetOutputTargetRect(processor_.get(), TRUE, &outRect);
  videoContext_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0,
                                                    D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  // Kein automatisches Nachbearbeiten (Rauschfilter etc.) – kostet nur Zeit.
  videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);

  inW_ = inW;
  inH_ = inH;
  log::Info("Farbkonvertierung: {}x{} BGRA → {}x{} NV12", inW, inH, outW_, outH_);
}

ID3D11VideoProcessorInputView* ColorConverter::InputView(ID3D11Texture2D* tex) {
  if (auto it = inViews_.find(tex); it != inViews_.end()) return it->second.view.get();
  if (inViews_.size() > 8) inViews_.clear();  // Pool wurde neu angelegt → alte Views verwerfen

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd = {};
  vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  InEntry e;
  e.tex.copy_from(tex);
  winrt::check_hresult(
      videoDevice_->CreateVideoProcessorInputView(tex, enumerator_.get(), &vd, e.view.put()));
  auto* view = e.view.get();
  inViews_.emplace(tex, std::move(e));
  return view;
}

ID3D11VideoProcessorOutputView* ColorConverter::OutputView(ID3D11Texture2D* tex, UINT slice) {
  uint64_t key = (reinterpret_cast<uint64_t>(tex) << 8) ^ slice;
  if (auto it = outViews_.find(key); it != outViews_.end()) return it->second.view.get();

  D3D11_TEXTURE2D_DESC td;
  tex->GetDesc(&td);
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC vd = {};
  if (td.ArraySize > 1) {
    vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.FirstArraySlice = slice;
    vd.Texture2DArray.ArraySize = 1;
  } else {
    vd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  }
  OutEntry e;
  e.tex.copy_from(tex);
  winrt::check_hresult(
      videoDevice_->CreateVideoProcessorOutputView(tex, enumerator_.get(), &vd, e.view.put()));
  auto* view = e.view.get();
  outViews_.emplace(key, std::move(e));
  return view;
}

void ColorConverter::Convert(ID3D11Texture2D* src, UINT srcW, UINT srcH, ID3D11Texture2D* dst,
                             UINT dstSlice) {
  if (!videoDevice_) throw winrt::hresult_error(E_UNEXPECTED, L"ColorConverter nicht initialisiert");
  if (srcW != inW_ || srcH != inH_ || !processor_) CreateProcessor(srcW, srcH);

  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = InputView(src);
  winrt::check_hresult(videoContext_->VideoProcessorBlt(processor_.get(),
                                                        OutputView(dst, dstSlice), 0, 1, &stream));
}

} // namespace litecap
