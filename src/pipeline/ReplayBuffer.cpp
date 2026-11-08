#include "pipeline/ReplayBuffer.h"

#include <algorithm>
#include <format>

#include "output/FileNamer.h"
#include "output/Muxer.h"
#include "util/Log.h"
#include "util/Strings.h"

namespace litecap {

namespace {
constexpr size_t kWriteBuffer = 1 << 20;            // 1 MB
constexpr uint64_t kReserve = 32ull << 20;          // feste Reserve in der Ringdatei
constexpr double kExtraSeconds = 3.0;               // ältester Keyframe vor dem Fenster

void PositionalIo(HANDLE file, uint64_t offset, void* data, DWORD size, bool write) {
  OVERLAPPED ov = {};
  ov.Offset = static_cast<DWORD>(offset);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD done = 0;
  BOOL ok = write ? WriteFile(file, data, size, &done, &ov) : ReadFile(file, data, size, &done, &ov);
  if (!ok || done != size)
    throw std::runtime_error(std::string(write ? "Schreiben" : "Lesen") +
                             " der Replay-Datei fehlgeschlagen: " + LastErrorMessage());
}
} // namespace

void ReplayBuffer::Start(CaptureSession& session, int seconds, int videoKbps, int audioKbpsTotal,
                         const std::filesystem::path& dir) {
  Stop();
  seconds_ = seconds;

  // Größe: Spitzenbitrate (VBR bis 1,5×) × Dauer, +25 % und feste Reserve
  const double bytesPerSec = (videoKbps * 1.5 + audioKbpsTotal) * 1000.0 / 8.0;
  capacity_ = static_cast<uint64_t>(bytesPerSec * (seconds + kExtraSeconds) * 1.25) + kReserve;

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const uint64_t free = FreeDiskBytes(dir);
  if (free < capacity_ + (1ull << 30))
    throw std::runtime_error(std::format("Zu wenig Speicherplatz für den Replay-Puffer ({:.1f} GB "
                                         "nötig + 1 GB Reserve, {:.1f} GB frei)",
                                         capacity_ / 1e9, free / 1e9));

  path_ = dir / std::format(L"replay-{}.ring", GetCurrentProcessId());
  // DELETE_ON_CLOSE: Die Datei verschwindet automatisch, auch nach einem Absturz.
  file_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file_ == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Replay-Datei kann nicht angelegt werden: " + LastErrorMessage());
  // Platz reservieren, damit der Datenträger nicht mitten im Spiel vollläuft
  LARGE_INTEGER size;
  size.QuadPart = static_cast<LONGLONG>(capacity_);
  if (!SetFilePointerEx(file_, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) {
    std::string err = LastErrorMessage();
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    throw std::runtime_error("Replay-Datei kann nicht reserviert werden: " + err);
  }

  {
    std::lock_guard lock(mutex_);
    index_.clear();
    written_ = flushed_ = 0;
    wbuf_.clear();
    wbuf_.reserve(kWriteBuffer + 512 * 1024);
    error_.clear();
    for (auto* p : params_) avcodec_parameters_free(&p);
    params_.clear();
    streams_ = session.Streams();
    for (auto& s : streams_) {
      AVCodecParameters* copy = avcodec_parameters_alloc();
      avcodec_parameters_copy(copy, s.params);
      params_.push_back(copy);
      s.params = copy;
    }
  }

  session_ = &session;
  sinkId_ = session.AddSink([this](const AVPacket* p) { OnPacket(p); }, /*needKeyframe=*/true);
  log::Info("Replay-Puffer aktiv: {} s, Ringdatei {:.0f} MB ({})", seconds, capacity_ / 1048576.0,
            WideToUtf8(path_.wstring()));
}

void ReplayBuffer::Stop() {
  if (session_) {
    session_->RemoveSink(sinkId_);
    session_ = nullptr;
  }
  std::lock_guard lock(mutex_);
  if (file_ != INVALID_HANDLE_VALUE) {
    CloseHandle(file_);  // löscht die Datei (DELETE_ON_CLOSE)
    file_ = INVALID_HANDLE_VALUE;
    log::Info("Replay-Puffer beendet");
  }
  index_.clear();
  wbuf_.clear();
  wbuf_.shrink_to_fit();
  for (auto* p : params_) avcodec_parameters_free(&p);
  params_.clear();
}

void ReplayBuffer::OnPacket(const AVPacket* pkt) {
  std::lock_guard lock(mutex_);
  if (file_ == INVALID_HANDLE_VALUE || !error_.empty()) return;
  if (pkt->size <= 0 || static_cast<uint64_t>(pkt->size) > capacity_ / 4) return;
  try {
    Entry e{written_, static_cast<uint32_t>(pkt->size), pkt->stream_index, pkt->pts,
            pkt->dts == AV_NOPTS_VALUE ? pkt->pts : pkt->dts, pkt->duration, pkt->flags};
    WriteRing(pkt->data, static_cast<size_t>(pkt->size));
    index_.push_back(e);
    TrimLocked();
  } catch (const std::exception& ex) {
    error_ = ex.what();
    log::Error("Replay-Puffer: {}", error_);
  }
}

void ReplayBuffer::WriteRing(const uint8_t* data, size_t size) {
  wbuf_.insert(wbuf_.end(), data, data + size);
  written_ += size;
  if (wbuf_.size() >= kWriteBuffer) FlushWriteBuffer();
}

void ReplayBuffer::FlushWriteBuffer() {
  size_t done = 0;
  while (done < wbuf_.size()) {
    uint64_t phys = flushed_ % capacity_;
    size_t chunk = static_cast<size_t>(std::min<uint64_t>(wbuf_.size() - done, capacity_ - phys));
    PositionalIo(file_, phys, wbuf_.data() + done, static_cast<DWORD>(chunk), true);
    done += chunk;
    flushed_ += chunk;
  }
  wbuf_.clear();
}

void ReplayBuffer::ReadRing(HANDLE file, uint64_t logical, uint8_t* dst, size_t size) const {
  size_t done = 0;
  while (done < size) {
    uint64_t phys = (logical + done) % capacity_;
    size_t chunk = static_cast<size_t>(std::min<uint64_t>(size - done, capacity_ - phys));
    PositionalIo(file, phys, dst + done, static_cast<DWORD>(chunk), false);
    done += chunk;
  }
}

void ReplayBuffer::TrimLocked() {
  // 1) Was die Ringdatei schon überschrieben hat (bzw. gleich überschreibt), muss weg
  while (!index_.empty() && written_ - index_.front().logicalOffset > capacity_ - kWriteBuffer)
    index_.pop_front();
  // 2) Älter als das Replay-Fenster (+ Reserve für den Keyframe davor)
  if (index_.empty() || streams_.empty()) return;
  const AVRational vtb = streams_[0].timeBase;
  int64_t latest = AV_NOPTS_VALUE;
  for (auto it = index_.rbegin(); it != index_.rend(); ++it)
    if (it->stream == 0) {
      latest = it->pts;
      break;
    }
  if (latest == AV_NOPTS_VALUE) return;
  const double cutoff = latest * av_q2d(vtb) - seconds_ - kExtraSeconds;
  while (!index_.empty()) {
    const Entry& e = index_.front();
    double t = e.pts * av_q2d(streams_[static_cast<size_t>(e.stream)].timeBase);
    if (t >= cutoff) break;
    index_.pop_front();
  }
}

double ReplayBuffer::BufferedSeconds() const {
  std::lock_guard lock(mutex_);
  if (index_.empty() || streams_.empty()) return 0;
  int64_t first = AV_NOPTS_VALUE, last = AV_NOPTS_VALUE;
  for (const auto& e : index_)
    if (e.stream == 0) {
      if (first == AV_NOPTS_VALUE) first = e.pts;
      last = e.pts;
    }
  if (first == AV_NOPTS_VALUE) return 0;
  return std::min<double>((last - first) * av_q2d(streams_[0].timeBase), seconds_);
}

double ReplayBuffer::SaveClip(const std::filesystem::path& file) {
  std::vector<Entry> entries;
  std::vector<StreamInfo> streams;
  std::vector<AVCodecParameters*> params;
  {
    std::lock_guard lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Replay-Puffer ist nicht aktiv");
    if (!error_.empty()) throw std::runtime_error(error_);
    FlushWriteBuffer();  // alles bis jetzt muss in der Datei stehen
    entries.assign(index_.begin(), index_.end());
    streams = streams_;
    for (auto* p : params_) {
      AVCodecParameters* copy = avcodec_parameters_alloc();
      avcodec_parameters_copy(copy, p);
      params.push_back(copy);
    }
  }
  auto freeParams = [&] {
    for (auto* p : params) avcodec_parameters_free(&p);
  };

  // Fensterbeginn: erster Video-Keyframe ab (letztes Bild − Replay-Länge);
  // gibt es dort keinen, der letzte Keyframe davor.
  int64_t lastVideo = AV_NOPTS_VALUE;
  for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    if (it->stream == 0) {
      lastVideo = it->pts;
      break;
    }
  if (lastVideo == AV_NOPTS_VALUE) {
    freeParams();
    throw std::runtime_error("Der Replay-Puffer ist noch leer");
  }
  const AVRational vtb = streams[0].timeBase;
  const int64_t windowStart = lastVideo - static_cast<int64_t>(seconds_ / av_q2d(vtb));
  size_t startIdx = entries.size();
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    if (e.stream == 0 && (e.flags & AV_PKT_FLAG_KEY)) {
      if (e.pts <= windowStart || startIdx == entries.size()) startIdx = i;
      if (e.pts >= windowStart) break;
    }
  }
  if (startIdx == entries.size()) {
    freeParams();
    throw std::runtime_error("Kein Keyframe im Replay-Puffer");
  }
  const int64_t startPts = entries[startIdx].pts;

  Muxer mux;
  try {
    mux.Open(file, Muxer::Mode::Clip);
    for (size_t i = 0; i < streams.size(); ++i) mux.AddStream(params[i], streams[i].timeBase, streams[i].title);
    mux.WriteHeader();

    AvPacketPtr pkt = MakePacket();
    for (size_t i = startIdx; i < entries.size(); ++i) {
      const Entry& e = entries[i];
      const AVRational tb = streams[static_cast<size_t>(e.stream)].timeBase;
      const int64_t offset = av_rescale_q(startPts, vtb, tb);
      if (e.pts - offset < 0) continue;  // Ton vor dem ersten Bild

      AvCheck(av_new_packet(pkt.get(), static_cast<int>(e.size)), "av_new_packet");
      ReadRing(file_, e.logicalOffset, pkt->data, e.size);
      {
        // Wurde der Bereich inzwischen überschrieben? (nur bei extrem langem Speichern)
        std::lock_guard lock(mutex_);
        if (written_ - e.logicalOffset > capacity_)
          throw std::runtime_error("Replay-Puffer wurde während des Speicherns überschrieben");
      }
      pkt->stream_index = e.stream;
      pkt->pts = e.pts;
      pkt->dts = e.dts;
      pkt->duration = e.duration;
      pkt->flags = e.flags;
      mux.Write(pkt.get(), offset);
      av_packet_unref(pkt.get());
    }
    mux.Close();
  } catch (...) {
    mux.Abort();
    std::error_code ec;
    std::filesystem::remove(file, ec);
    freeParams();
    throw;
  }
  freeParams();
  double secs = (lastVideo - startPts) * av_q2d(vtb);
  log::Info("Clip gespeichert ({:.1f} s): {}", secs, WideToUtf8(file.wstring()));
  return secs;
}

} // namespace litecap
