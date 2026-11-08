#include "audio/AudioTrack.h"

#include <algorithm>

#include "util/Log.h"

namespace litecap {

namespace {
constexpr int kChannels = 2;
// Schutz, falls eine Quelle hängt: mehr als 2 s Vorsprung werden verworfen.
constexpr int64_t kMaxBufferedSamples = 48000 * 2;
} // namespace

AudioTrack::~AudioTrack() = default;

void AudioTrack::Open(int bitrateKbps, int inputs, std::string title, PacketCallback onPacket) {
  std::lock_guard lock(mutex_);
  title_ = std::move(title);
  onPacket_ = std::move(onPacket);
  pos_.assign(static_cast<size_t>(inputs), 0);
  base_ = 0;
  mix_.clear();

  // FFmpegs eigener AAC-Encoder: LGPL, gute Qualität ab ~128 kbit/s, geringe CPU-Last.
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec) throw std::runtime_error("AAC-Encoder fehlt in FFmpeg");
  ctx_.reset(avcodec_alloc_context3(codec));
  AVCodecContext* c = ctx_.get();
  c->sample_rate = 48000;
  c->sample_fmt = AV_SAMPLE_FMT_FLTP;
  c->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
  c->time_base = {1, 48000};
  av_channel_layout_default(&c->ch_layout, kChannels);
  c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  AvCheck(avcodec_open2(c, codec, nullptr), "AAC-Encoder öffnen");

  frame_ = MakeFrame();
  frame_->format = AV_SAMPLE_FMT_FLTP;
  frame_->nb_samples = c->frame_size;  // 1024
  frame_->sample_rate = 48000;
  av_channel_layout_copy(&frame_->ch_layout, &c->ch_layout);
  AvCheck(av_frame_get_buffer(frame_.get(), 0), "Audio-Frame anlegen");
  packet_ = MakePacket();
}

void AudioTrack::Write(int input, const float* data, size_t frames) {
  std::lock_guard lock(mutex_);
  if (!ctx_ || input < 0 || static_cast<size_t>(input) >= pos_.size()) return;
  int64_t& pos = pos_[static_cast<size_t>(input)];

  // Liegt diese Quelle zu weit vor den anderen (eine hängt), ältesten Teil verwerfen
  int64_t minPos = *std::min_element(pos_.begin(), pos_.end());
  if (pos + static_cast<int64_t>(frames) - minPos > kMaxBufferedSamples) {
    for (auto& p : pos_) p = std::max(p, pos + static_cast<int64_t>(frames) - kMaxBufferedSamples);
  }

  const int64_t end = pos + static_cast<int64_t>(frames);
  const size_t needed = static_cast<size_t>(end - base_) * kChannels;
  if (mix_.size() < needed) mix_.resize(needed, 0.0f);
  if (data && pos >= base_) {
    float* dst = mix_.data() + static_cast<size_t>(pos - base_) * kChannels;
    const size_t n = frames * kChannels;
    for (size_t i = 0; i < n; ++i) dst[i] += data[i];  // mischen (Summe)
  }
  pos = end;
  EncodeAvailable(false);
}

void AudioTrack::EncodeAvailable(bool flush) {
  const int frameSize = ctx_->frame_size;
  // Kodierbar ist alles, was ALLE Quellen schon geliefert haben
  int64_t ready = *std::min_element(pos_.begin(), pos_.end());
  if (base_ > ready) base_ = ready;
  // Hat Write() eine hängende Quelle vorgezogen, fehlen dort Samples → als Stille auffüllen
  const size_t needed = static_cast<size_t>(ready - base_) * kChannels;
  if (mix_.size() < needed) mix_.resize(needed, 0.0f);

  auto drain = [this] {
    while (avcodec_receive_packet(ctx_.get(), packet_.get()) == 0) {
      onPacket_(packet_.get());
      av_packet_unref(packet_.get());
    }
  };

  while (ready - base_ >= frameSize || (flush && ready > base_)) {
    int n = static_cast<int>(std::min<int64_t>(frameSize, ready - base_));
    AvCheck(av_frame_make_writable(frame_.get()), "av_frame_make_writable");
    auto* l = reinterpret_cast<float*>(frame_->data[0]);
    auto* r = reinterpret_cast<float*>(frame_->data[1]);
    const float* src = mix_.data();
    for (int i = 0; i < frameSize; ++i) {
      // Interleaved → planar, Übersteuerung beim Mischen begrenzen
      l[i] = i < n ? std::clamp(src[i * 2], -1.0f, 1.0f) : 0.0f;
      r[i] = i < n ? std::clamp(src[i * 2 + 1], -1.0f, 1.0f) : 0.0f;
    }
    frame_->pts = base_;
    AvCheck(avcodec_send_frame(ctx_.get(), frame_.get()), "AAC kodieren");
    drain();
    mix_.erase(mix_.begin(), mix_.begin() + static_cast<ptrdiff_t>(n) * kChannels);
    base_ += n;
  }
  if (flush) {
    avcodec_send_frame(ctx_.get(), nullptr);
    drain();
  }
}

void AudioTrack::Flush() {
  std::lock_guard lock(mutex_);
  if (!ctx_) return;
  try {
    EncodeAvailable(true);
  } catch (const std::exception& e) {
    log::Warn("Audio-Flush ({}): {}", title_, e.what());
  }
}

} // namespace litecap
