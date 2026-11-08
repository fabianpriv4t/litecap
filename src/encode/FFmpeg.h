#pragma once

// Zentrale FFmpeg-Includes + kleine RAII-Helfer.
// d3d11.h muss VOR dem extern-"C"-Block stehen (enthält C++-Operatoren).
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <memory>
#include <stdexcept>
#include <string>

namespace litecap {

inline std::string AvError(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf) + " (" + std::to_string(err) + ")";
}

class AvException : public std::runtime_error {
 public:
  AvException(const std::string& what, int err)
      : std::runtime_error(what + ": " + AvError(err)), code_(err) {}
  int code() const { return code_; }

 private:
  int code_;
};

inline void AvCheck(int ret, const char* what) {
  if (ret < 0) throw AvException(what, ret);
}

struct AvFrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct AvPacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct AvCodecCtxDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct AvBufferDeleter { void operator()(AVBufferRef* b) const { av_buffer_unref(&b); } };

using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using AvCodecCtxPtr = std::unique_ptr<AVCodecContext, AvCodecCtxDeleter>;
using AvBufferPtr = std::unique_ptr<AVBufferRef, AvBufferDeleter>;

inline AvFramePtr MakeFrame() { return AvFramePtr(av_frame_alloc()); }
inline AvPacketPtr MakePacket() { return AvPacketPtr(av_packet_alloc()); }

// FFmpeg-Logausgaben ins LiteCap-Log umleiten (nur Warnungen und Fehler).
void InstallFFmpegLogger();

} // namespace litecap
