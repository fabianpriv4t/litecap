#include "encode/FFmpeg.h"

#include <cstdio>

#include "util/Log.h"

namespace litecap {

static void LogCallback(void* avcl, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_WARNING) return;  // Info/Debug von FFmpeg verwerfen
  char line[1024];
  static int printPrefix = 1;
  av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &printPrefix);
  std::string s(line);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  if (s.empty()) return;
  if (level <= AV_LOG_ERROR) log::Error("ffmpeg: {}", s);
  else log::Warn("ffmpeg: {}", s);
}

void InstallFFmpegLogger() {
  av_log_set_level(AV_LOG_WARNING);
  av_log_set_callback(LogCallback);
}

} // namespace litecap
