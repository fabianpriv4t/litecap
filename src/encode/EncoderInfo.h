#pragma once

#include <string>
#include <vector>

#include "capture/GpuDevice.h"

namespace litecap {

enum class Codec { H264, HEVC, AV1 };
enum class Backend { NVENC, AMF, QSV, X264, SVTAV1 };

const char* CodecName(Codec c);       // "h264" …
const char* BackendName(Backend b);   // "nvenc" …
bool IsHardware(Backend b);
// FFmpeg-Encodername, z. B. "hevc_nvenc"; nullptr wenn Kombination nicht existiert.
const char* FFmpegEncoderName(Codec c, Backend b);

struct EncoderChoice {
  Codec codec;
  Backend backend;
  // true: Live-Codec ist HEVC/H.264, die Datei wird danach im Leerlauf nach AV1 umgewandelt
  // (GPUs ohne AV1-Hardware-Encoder, z. B. RTX 20/30).
  bool transcodeToAv1 = false;
};

// Kandidaten in sinnvoller Reihenfolge für die gewünschte Einstellung:
// Hersteller der Capture-GPU zuerst, dann andere Hardware-Encoder, x264 zuletzt.
std::vector<EncoderChoice> EncoderCandidates(const std::string& codecSetting,
                                             const std::string& encoderSetting,
                                             GpuVendor captureGpu);

} // namespace litecap
