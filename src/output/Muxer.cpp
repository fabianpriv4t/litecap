#include "output/Muxer.h"

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

Muxer::~Muxer() { Abort(); }

void Muxer::Open(const std::filesystem::path& file, Mode mode) {
  std::lock_guard lock(mutex_);
  file_ = file;
  std::string path = WideToUtf8(file.wstring());  // FFmpeg erwartet UTF-8-Pfade unter Windows
  AvCheck(avformat_alloc_output_context2(&fmt_, nullptr, nullptr, path.c_str()),
          "Ausgabeformat bestimmen");
  tmp_ = MakePacket();

  AVDictionary* opts = nullptr;
  if (std::string(fmt_->oformat->name) == "mp4" || std::string(fmt_->oformat->name) == "mov") {
    av_dict_set(&opts, "movflags",
                mode == Mode::Clip ? "+faststart" : "+frag_keyframe+empty_moov+default_base_moof",
                0);
  }
  // Optionen werden bei avformat_write_header() übergeben
  av_dict_set(&fmt_->metadata, "encoder", "LiteCap", 0);
  pendingOpts_ = opts;

  if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
    AvCheck(avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE), "Datei öffnen");
  }
}

int Muxer::AddStream(const AVCodecParameters* par, AVRational encoderTimeBase,
                     const std::string& title) {
  std::lock_guard lock(mutex_);
  AVStream* st = avformat_new_stream(fmt_, nullptr);
  if (!st) throw std::bad_alloc();
  AvCheck(avcodec_parameters_copy(st->codecpar, par), "avcodec_parameters_copy");
  st->codecpar->codec_tag = 0;  // Container wählt passenden Tag (z. B. hvc1 für HEVC in MP4)
  st->time_base = encoderTimeBase;
  if (!title.empty()) {
    // MP4: Spurname über handler_name ("title" erzeugt ein udta-Atom, das manche Leser,
    // auch FFmpeg selbst, bemängeln). MKV: "title".
    const bool mp4 = std::string(fmt_->oformat->name) == "mp4";
    av_dict_set(&st->metadata, mp4 ? "handler_name" : "title", title.c_str(), 0);
  }
  // Erste Tonspur als Standard markieren (Player spielen sonst evtl. die falsche ab)
  if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
    bool firstAudio = true;
    for (unsigned i = 0; i + 1 < fmt_->nb_streams; ++i)
      if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) firstAudio = false;
    st->disposition = firstAudio ? AV_DISPOSITION_DEFAULT : 0;
  }
  encoderTb_.push_back(encoderTimeBase);
  return st->index;
}

void Muxer::WriteHeader() {
  std::lock_guard lock(mutex_);
  AVDictionary* opts = pendingOpts_;
  pendingOpts_ = nullptr;
  // HEVC in MP4: "hvc1"-Tag, damit auch Apple-Geräte/Browser die Datei abspielen
  for (unsigned i = 0; i < fmt_->nb_streams; ++i)
    if (fmt_->streams[i]->codecpar->codec_id == AV_CODEC_ID_HEVC &&
        std::string(fmt_->oformat->name) == "mp4")
      fmt_->streams[i]->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
  int ret = avformat_write_header(fmt_, &opts);
  av_dict_free(&opts);
  AvCheck(ret, "Header schreiben");
  headerWritten_ = true;
}

void Muxer::Write(const AVPacket* pkt, int64_t ptsOffset) {
  std::lock_guard lock(mutex_);
  if (!fmt_ || !headerWritten_) return;
  AvCheck(av_packet_ref(tmp_.get(), pkt), "av_packet_ref");
  if (tmp_->pts != AV_NOPTS_VALUE) tmp_->pts -= ptsOffset;
  tmp_->dts = tmp_->dts == AV_NOPTS_VALUE ? tmp_->pts : tmp_->dts - ptsOffset;
  AVStream* st = fmt_->streams[pkt->stream_index];
  av_packet_rescale_ts(tmp_.get(), encoderTb_[static_cast<size_t>(pkt->stream_index)],
                       st->time_base);
  int ret = av_interleaved_write_frame(fmt_, tmp_.get());  // übernimmt/entleert tmp_
  av_packet_unref(tmp_.get());
  AvCheck(ret, "Paket schreiben");
}

int64_t Muxer::BytesWritten() const {
  return fmt_ && fmt_->pb ? avio_tell(fmt_->pb) : 0;
}

void Muxer::Close() {
  std::lock_guard lock(mutex_);
  if (!fmt_) return;
  int ret = 0;
  if (headerWritten_) ret = av_write_trailer(fmt_);
  if (fmt_->pb) {
    avio_flush(fmt_->pb);
    if (ret >= 0 && fmt_->pb->error < 0) ret = fmt_->pb->error;
    avio_closep(&fmt_->pb);
  }
  avformat_free_context(fmt_);
  fmt_ = nullptr;
  headerWritten_ = false;
  encoderTb_.clear();
  AvCheck(ret, "Datei abschließen");
  log::Info("Datei geschrieben: {}", WideToUtf8(file_.wstring()));
}

void Muxer::Abort() {
  std::lock_guard lock(mutex_);
  if (!fmt_) return;
  if (headerWritten_) av_write_trailer(fmt_);  // best effort
  if (fmt_->pb) avio_closep(&fmt_->pb);
  avformat_free_context(fmt_);
  fmt_ = nullptr;
  av_dict_free(&pendingOpts_);
  headerWritten_ = false;
  encoderTb_.clear();
}

} // namespace litecap
