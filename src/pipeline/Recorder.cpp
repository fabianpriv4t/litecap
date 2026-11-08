#include "pipeline/Recorder.h"

#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

void Recorder::Start(CaptureSession& session, const std::filesystem::path& file) {
  Stop();
  failed_ = false;
  error_.clear();
  startPts_ = AV_NOPTS_VALUE;
  lastVideoPts_ = 0;

  try {
    muxer_.Open(file, Muxer::Mode::Recording);
    tb_.clear();
    for (const auto& s : session.Streams()) {
      muxer_.AddStream(s.params, s.timeBase, s.title);
      tb_.push_back(s.timeBase);
    }
    muxer_.WriteHeader();
  } catch (...) {
    muxer_.Abort();
    std::error_code ec;
    std::filesystem::remove(file, ec);  // keine leere Datei zurücklassen
    throw;
  }

  session_ = &session;
  sinkId_ = session.AddSink([this](const AVPacket* p) { OnPacket(p); }, /*needKeyframe=*/true);
  log::Info("Aufnahme gestartet: {}", WideToUtf8(file.wstring()));
}

void Recorder::OnPacket(const AVPacket* pkt) {
  if (failed_) return;
  const size_t si = static_cast<size_t>(pkt->stream_index);
  if (si >= tb_.size()) return;

  // Gemeinsamer Nullpunkt = erster Video-Keyframe (Session liefert uns ab diesem)
  if (startPts_ == AV_NOPTS_VALUE) {
    if (si != 0) return;  // Audio vor dem ersten Videobild verwerfen
    startPts_ = pkt->pts;
  }
  const int64_t offset = av_rescale_q(startPts_, tb_[0], tb_[si]);
  if (pkt->pts - offset < 0) return;

  try {
    muxer_.Write(pkt, offset);
    if (si == 0) lastVideoPts_ = pkt->pts - startPts_;
  } catch (const std::exception& e) {
    std::lock_guard lock(errMutex_);
    error_ = e.what();
    failed_ = true;
    log::Error("Aufnahme: {}", e.what());
  }
}

std::filesystem::path Recorder::Stop() {
  if (!session_) return {};
  session_->RemoveSink(sinkId_);  // ab hier keine Pakete mehr
  session_ = nullptr;
  auto file = muxer_.File();
  if (failed_) {
    muxer_.Abort();
    throw std::runtime_error(FailureReason());
  }
  muxer_.Close();  // wirft z. B. bei vollem Datenträger
  log::Info("Aufnahme beendet ({:.1f} s)", DurationSec());
  return file;
}

std::string Recorder::FailureReason() const {
  std::lock_guard lock(errMutex_);
  return error_;
}

double Recorder::DurationSec() const {
  return tb_.empty() ? 0.0 : lastVideoPts_ * av_q2d(tb_[0]);
}

} // namespace litecap
