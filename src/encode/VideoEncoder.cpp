#include "encode/VideoEncoder.h"

#include <algorithm>
#include <format>

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

namespace {

void D3DLock(void* ctx) { static_cast<ID3D11Multithread*>(ctx)->Enter(); }
void D3DUnlock(void* ctx) { static_cast<ID3D11Multithread*>(ctx)->Leave(); }

} // namespace

std::string VideoEncoder::Description() const {
  if (!ctx_) return "–";
  return std::format("{} ({}), {}x{} @ {} fps", ctx_->codec->name, IsHardware(choice_.backend)
                         ? "Hardware" : "Software", ctx_->width, ctx_->height, settings_.fps);
}

void VideoEncoder::Open(const GpuDevice& gpu, const std::vector<EncoderChoice>& candidates,
                        const VideoEncoderSettings& settings) {
  Close();
  gpu_ = &gpu;
  settings_ = settings;
  settings_.width &= ~1;
  settings_.height &= ~1;
  packet_ = MakePacket();

  std::string errors;
  for (const auto& c : candidates) {
    std::string err;
    if (TryOpen(c, err)) {
      choice_ = c;
      log::Info("Video-Encoder: {}{}", Description(),
                c.transcodeToAv1 ? " – AV1 wird nachträglich erzeugt" : "");
      return;
    }
    log::Info("Encoder {} nicht verfügbar: {}", FFmpegEncoderName(c.codec, c.backend), err);
    errors += std::string(FFmpegEncoderName(c.codec, c.backend)) + ": " + err + "\n";
  }
  throw std::runtime_error("Kein Video-Encoder verfügbar:\n" + errors);
}

void VideoEncoder::Close() {
  ctx_.reset();
  qsvFrames_.reset();
  qsvDevice_.reset();
  d3dFrames_.reset();
  d3dDevice_.reset();
  staging_ = nullptr;
}

void VideoEncoder::SetupHwFrames(Backend b) {
  // 1) FFmpeg-Gerätekontext, der unser vorhandenes D3D11-Device benutzt
  d3dDevice_.reset(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
  if (!d3dDevice_) throw std::bad_alloc();
  auto* hw = reinterpret_cast<AVHWDeviceContext*>(d3dDevice_->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(hw->hwctx);
  d3d->device = gpu_->Device();
  d3d->device->AddRef();  // FFmpeg gibt die Referenzen beim Freigeben wieder frei
  d3d->device_context = gpu_->Context();
  d3d->device_context->AddRef();
  // Gleiche Sperre wie Capture/Konvertierung → sicherer Zugriff aus mehreren Threads
  d3d->lock = D3DLock;
  d3d->unlock = D3DUnlock;
  d3d->lock_ctx = gpu_->Multithread();
  AvCheck(av_hwdevice_ctx_init(d3dDevice_.get()), "av_hwdevice_ctx_init(d3d11va)");

  // 2) Pool von NV12-Texturen, in die wir pro Frame kopieren
  d3dFrames_.reset(av_hwframe_ctx_alloc(d3dDevice_.get()));
  auto* fc = reinterpret_cast<AVHWFramesContext*>(d3dFrames_->data);
  fc->format = AV_PIX_FMT_D3D11;
  fc->sw_format = AV_PIX_FMT_NV12;
  fc->width = settings_.width;
  fc->height = settings_.height;
  // initial_pool_size = 0 → einzelne Texturen statt eines Texture-Arrays. NV12-Arrays lehnt
  // u. a. der NVIDIA-Treiber mit E_INVALIDARG ab; Einzeltexturen werden vom Pool
  // wiederverwendet, es entstehen also nur so viele, wie der Encoder gleichzeitig hält.
  fc->initial_pool_size = 0;
  static_cast<AVD3D11VAFramesContext*>(fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
  AvCheck(av_hwframe_ctx_init(d3dFrames_.get()), "av_hwframe_ctx_init(d3d11)");

  if (b == Backend::QSV) {
    // Quick Sync arbeitet auf eigenen Surfaces, die direkt auf die D3D11-Texturen abgebildet
    // werden (kein Kopieren). Erfordert, dass das Device auf der Intel-GPU liegt.
    AVBufferRef* dev = nullptr;
    AvCheck(av_hwdevice_ctx_create_derived(&dev, AV_HWDEVICE_TYPE_QSV, d3dDevice_.get(), 0),
            "QSV-Gerät ableiten");
    qsvDevice_.reset(dev);
    AVBufferRef* frames = nullptr;
    AvCheck(av_hwframe_ctx_create_derived(&frames, AV_PIX_FMT_QSV, qsvDevice_.get(),
                                          d3dFrames_.get(), AV_HWFRAME_MAP_DIRECT),
            "QSV-Frames ableiten");
    qsvFrames_.reset(frames);
  }
}

void VideoEncoder::ApplyOptions(Backend b, AVDictionary** o) {
  const auto& s = settings_;
  const int64_t bitrate = static_cast<int64_t>(s.bitrateKbps) * 1000;
  const bool cqp = s.rateControl == "cqp";
  const bool cbr = s.rateControl == "cbr";
  AVCodecContext* c = ctx_.get();

  if (!cqp) {
    c->bit_rate = bitrate;
    c->rc_max_rate = cbr ? bitrate : bitrate * 3 / 2;
    c->rc_buffer_size = static_cast<int>(cbr ? bitrate : bitrate * 2);
  }

  switch (b) {
    case Backend::NVENC:
      av_dict_set(o, "preset", "p4", 0);       // gutes Verhältnis Qualität/Encoder-Auslastung
      av_dict_set(o, "tune", "hq", 0);
      av_dict_set(o, "rc", cqp ? "constqp" : cbr ? "cbr" : "vbr", 0);
      if (cqp) av_dict_set_int(o, "qp", s.quality, 0);
      av_dict_set(o, "rc-lookahead", "0", 0);  // kein Lookahead → weniger VRAM, keine Latenz
      // Asynchron: Pakete kommen bis zu 6 Frames verzögert. Mit delay=0 würde jeder Aufruf
      // warten, bis die GPU (die gerade das Spiel rendert) unsere Kopie ausgeführt hat.
      av_dict_set(o, "delay", "6", 0);
      av_dict_set(o, "forced-idr", "1", 0);
      break;
    case Backend::AMF:
      av_dict_set(o, "usage", "lowlatency", 0);
      av_dict_set(o, "quality", "balanced", 0);
      av_dict_set(o, "rc", cqp ? "cqp" : cbr ? "cbr" : "vbr_peak", 0);
      if (cqp) {
        av_dict_set_int(o, "qp_i", s.quality, 0);
        av_dict_set_int(o, "qp_p", s.quality, 0);
      }
      break;
    case Backend::QSV:
      av_dict_set(o, "preset", "veryfast", 0);
      av_dict_set(o, "look_ahead", "0", 0);
      av_dict_set(o, "forced_idr", "1", 0);
      if (cqp) c->global_quality = s.quality;  // ICQ-Modus
      break;
    case Backend::X264:
      av_dict_set(o, "preset", "veryfast", 0);
      av_dict_set(o, "tune", "zerolatency", 0);  // keine Lookahead-Puffer, wenig RAM
      av_dict_set(o, "forced-idr", "1", 0);
      if (cqp) av_dict_set_int(o, "crf", s.quality, 0);
      break;
    case Backend::SVTAV1:
      break;
  }
}

bool VideoEncoder::TryOpen(const EncoderChoice& choice, std::string& error) {
  ctx_.reset();
  qsvFrames_.reset();
  qsvDevice_.reset();
  d3dFrames_.reset();
  d3dDevice_.reset();
  staging_ = nullptr;

  const char* name = FFmpegEncoderName(choice.codec, choice.backend);
  const AVCodec* codec = name ? avcodec_find_encoder_by_name(name) : nullptr;
  if (!codec) {
    error = "nicht in FFmpeg enthalten";
    return false;
  }
  try {
    ctx_.reset(avcodec_alloc_context3(codec));
    AVCodecContext* c = ctx_.get();
    c->width = settings_.width;
    c->height = settings_.height;
    c->time_base = {1, settings_.fps};
    c->framerate = {settings_.fps, 1};
    c->gop_size = settings_.fps * settings_.keyframeIntervalSec;
    c->max_b_frames = 0;  // dts == pts → einfacher Ringpuffer/Schnitt, weniger Latenz
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->sample_aspect_ratio = {1, 1};
    // SPS/PPS in extradata statt in jedem Keyframe → nötig für MP4/MKV
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (IsHardware(choice.backend)) {
      SetupHwFrames(choice.backend);
      if (choice.backend == Backend::QSV) {
        c->pix_fmt = AV_PIX_FMT_QSV;
        c->hw_frames_ctx = av_buffer_ref(qsvFrames_.get());
      } else {
        c->pix_fmt = AV_PIX_FMT_D3D11;
        c->hw_frames_ctx = av_buffer_ref(d3dFrames_.get());
      }
    } else {
      c->pix_fmt = AV_PIX_FMT_NV12;
      // Software-Encoder: wenige Threads, damit das Spiel nicht ausgebremst wird
      c->thread_count = 4;
    }

    AVDictionary* opts = nullptr;
    ApplyOptions(choice.backend, &opts);
    int ret = avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);
    AvCheck(ret, "avcodec_open2");

    if (!IsHardware(choice.backend)) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = static_cast<UINT>(settings_.width);
      td.Height = static_cast<UINT>(settings_.height);
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_NV12;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      winrt::check_hresult(gpu_->Device()->CreateTexture2D(&td, nullptr, staging_.put()));
    }
    return true;
  } catch (const std::exception& e) {
    error = e.what();
  } catch (const winrt::hresult_error& e) {
    error = WideToUtf8(e.message());
  }
  ctx_.reset();
  return false;
}

AvFramePtr VideoEncoder::PrepareFrame(ID3D11Texture2D* nv12, int64_t pts, bool forceKeyframe) {
  AvFramePtr frame = MakeFrame();
  AVFrame* f = frame.get();

  if (IsHardware(choice_.backend)) {
    AvCheck(av_hwframe_get_buffer(d3dFrames_.get(), f, 0), "av_hwframe_get_buffer");
    auto* dst = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    UINT slice = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
    {
      GpuLock lock(gpu_->Multithread());
      // GPU-interne Kopie (≈ 3 MB bei 1080p) – CPU-Aufwand vernachlässigbar
      gpu_->Context()->CopySubresourceRegion(dst, slice, 0, 0, 0, nv12, 0, nullptr);
      // Sofort an die GPU übergeben, statt im Befehlspuffer auf den nächsten Flush zu warten
      gpu_->Context()->Flush();
    }
    if (choice_.backend == Backend::QSV) {
      AvFramePtr q = MakeFrame();
      q->format = AV_PIX_FMT_QSV;
      q->hw_frames_ctx = av_buffer_ref(qsvFrames_.get());
      AvCheck(av_hwframe_map(q.get(), f, AV_HWFRAME_MAP_DIRECT | AV_HWFRAME_MAP_READ),
              "av_hwframe_map(qsv)");
      frame = std::move(q);
      f = frame.get();
    }
  } else {
    // Software: GPU → RAM (einziger Pfad mit CPU-Kopie)
    f->format = AV_PIX_FMT_NV12;
    f->width = settings_.width;
    f->height = settings_.height;
    AvCheck(av_frame_get_buffer(f, 0), "av_frame_get_buffer");
    GpuLock lock(gpu_->Multithread());
    gpu_->Context()->CopyResource(staging_.get(), nv12);
    D3D11_MAPPED_SUBRESOURCE map;
    winrt::check_hresult(gpu_->Context()->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &map));
    auto* src = static_cast<const uint8_t*>(map.pData);
    av_image_copy_plane(f->data[0], f->linesize[0], src, static_cast<int>(map.RowPitch),
                        settings_.width, settings_.height);
    av_image_copy_plane(f->data[1], f->linesize[1],
                        src + static_cast<size_t>(map.RowPitch) * settings_.height,
                        static_cast<int>(map.RowPitch), settings_.width, settings_.height / 2);
    gpu_->Context()->Unmap(staging_.get(), 0);
  }

  f->pts = pts;
  if (forceKeyframe) {
    f->pict_type = AV_PICTURE_TYPE_I;
    f->flags |= AV_FRAME_FLAG_KEY;
  }
  return frame;
}

void VideoEncoder::Submit(AVFrame* frame, const PacketCallback& onPacket) {
  int ret = avcodec_send_frame(ctx_.get(), frame);
  if (ret == AVERROR(EAGAIN)) {
    // Encoder-Warteschlange voll → erst Pakete abholen, Frame verwerfen (nicht blockieren)
    Drain(onPacket);
    return;
  }
  AvCheck(ret, "avcodec_send_frame");
  Drain(onPacket);
}

void VideoEncoder::Drain(const PacketCallback& onPacket) {
  for (;;) {
    int ret = avcodec_receive_packet(ctx_.get(), packet_.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
    AvCheck(ret, "avcodec_receive_packet");
    onPacket(packet_.get());
    av_packet_unref(packet_.get());
  }
}

void VideoEncoder::Flush(const PacketCallback& onPacket) {
  if (!ctx_) return;
  avcodec_send_frame(ctx_.get(), nullptr);
  Drain(onPacket);
}

} // namespace litecap
