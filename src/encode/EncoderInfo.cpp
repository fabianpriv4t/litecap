#include "encode/EncoderInfo.h"

namespace litecap {

const char* CodecName(Codec c) {
  switch (c) {
    case Codec::H264: return "h264";
    case Codec::HEVC: return "hevc";
    case Codec::AV1: return "av1";
  }
  return "?";
}

const char* BackendName(Backend b) {
  switch (b) {
    case Backend::NVENC: return "nvenc";
    case Backend::AMF: return "amf";
    case Backend::QSV: return "qsv";
    case Backend::X264: return "x264";
    case Backend::SVTAV1: return "svtav1";
  }
  return "?";
}

bool IsHardware(Backend b) { return b == Backend::NVENC || b == Backend::AMF || b == Backend::QSV; }

const char* FFmpegEncoderName(Codec c, Backend b) {
  switch (b) {
    case Backend::NVENC:
      return c == Codec::H264 ? "h264_nvenc" : c == Codec::HEVC ? "hevc_nvenc" : "av1_nvenc";
    case Backend::AMF:
      return c == Codec::H264 ? "h264_amf" : c == Codec::HEVC ? "hevc_amf" : "av1_amf";
    case Backend::QSV:
      return c == Codec::H264 ? "h264_qsv" : c == Codec::HEVC ? "hevc_qsv" : "av1_qsv";
    case Backend::X264: return c == Codec::H264 ? "libx264" : nullptr;
    case Backend::SVTAV1: return c == Codec::AV1 ? "libsvtav1" : nullptr;
  }
  return nullptr;
}

static Backend VendorBackend(GpuVendor v) {
  switch (v) {
    case GpuVendor::Amd: return Backend::AMF;
    case GpuVendor::Intel: return Backend::QSV;
    default: return Backend::NVENC;
  }
}

std::vector<EncoderChoice> EncoderCandidates(const std::string& codecSetting,
                                             const std::string& encoderSetting,
                                             GpuVendor captureGpu) {
  std::vector<Backend> hw;
  if (encoderSetting == "nvenc") hw = {Backend::NVENC};
  else if (encoderSetting == "amf") hw = {Backend::AMF};
  else if (encoderSetting == "qsv") hw = {Backend::QSV};
  else if (encoderSetting == "x264") hw = {};
  else {
    // auto: Encoder der Capture-GPU zuerst (Zero-Copy), danach die anderen
    Backend first = VendorBackend(captureGpu);
    hw.push_back(first);
    for (Backend b : {Backend::NVENC, Backend::AMF, Backend::QSV})
      if (b != first) hw.push_back(b);
  }

  std::vector<EncoderChoice> list;
  if (codecSetting == "av1") {
    for (Backend b : hw) list.push_back({Codec::AV1, b, false});
    // Ohne AV1-Hardware: HEVC/H.264 live, danach AV1 per SVT-AV1 im Leerlauf
    for (Backend b : hw) list.push_back({Codec::HEVC, b, true});
    for (Backend b : hw) list.push_back({Codec::H264, b, true});
    list.push_back({Codec::H264, Backend::X264, true});
  } else if (codecSetting == "hevc") {
    for (Backend b : hw) list.push_back({Codec::HEVC, b, false});
    for (Backend b : hw) list.push_back({Codec::H264, b, false});  // Rückfall auf H.264
    list.push_back({Codec::H264, Backend::X264, false});
  } else {
    for (Backend b : hw) list.push_back({Codec::H264, b, false});
    list.push_back({Codec::H264, Backend::X264, false});
  }
  return list;
}

} // namespace litecap
