#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "encode/FFmpeg.h"

namespace litecap {

// Eine Tonspur in der Ausgabedatei: mischt eine oder mehrere Quellen (Sample-genau auf der
// gemeinsamen Zeitachse) und kodiert sie als AAC (48 kHz, Stereo).
// Pakete: Timebase 1/48000, Sample 0 = Session-Beginn.
class AudioTrack {
 public:
  using PacketCallback = std::function<void(AVPacket*)>;

  AudioTrack() = default;
  ~AudioTrack();
  AudioTrack(const AudioTrack&) = delete;
  AudioTrack& operator=(const AudioTrack&) = delete;

  void Open(int bitrateKbps, int inputs, std::string title, PacketCallback onPacket);
  // Thread-sicher. data == nullptr → Stille. Jede Quelle liefert einen lückenlosen Strom.
  void Write(int input, const float* interleavedStereo, size_t frames);
  void Flush();  // Rest kodieren (am Ende der Session)

  const AVCodecContext* Context() const { return ctx_.get(); }
  AVRational TimeBase() const { return {1, 48000}; }
  const std::string& Title() const { return title_; }

 private:
  void EncodeAvailable(bool flush);

  std::mutex mutex_;
  AvCodecCtxPtr ctx_;
  AvFramePtr frame_;
  AvPacketPtr packet_;
  PacketCallback onPacket_;
  std::string title_;
  std::vector<float> mix_;      // interleaved, ab Sample base_
  int64_t base_ = 0;            // Sample-Index von mix_[0]
  std::vector<int64_t> pos_;    // pro Quelle: nächstes Sample
};

} // namespace litecap
