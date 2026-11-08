#pragma once

#include <filesystem>
#include <string>
#include <mutex>
#include <vector>

#include "encode/FFmpeg.h"

namespace litecap {

// Schreibt bereits kodierte Pakete in MP4/MKV (kein Neukodieren).
// Thread-sicher: Video- und Audio-Threads dürfen gleichzeitig schreiben.
class Muxer {
 public:
  enum class Mode {
    Clip,       // MP4 mit "faststart" (Index am Anfang → sofort abspielbar/streambar)
    Recording,  // MP4 fragmentiert: bei Absturz/Stromausfall bleibt das Aufgenommene lesbar
  };

  Muxer() = default;
  ~Muxer();
  Muxer(const Muxer&) = delete;
  Muxer& operator=(const Muxer&) = delete;

  // Container wird aus der Dateiendung abgeleitet (.mp4 / .mkv).
  void Open(const std::filesystem::path& file, Mode mode);
  // Muss vor WriteHeader() für jede Spur aufgerufen werden; liefert den Stream-Index.
  int AddStream(const AVCodecParameters* par, AVRational encoderTimeBase,
                const std::string& title = {});
  void WriteHeader();
  // pkt->stream_index = Index aus AddStream; Zeitstempel in der Encoder-Timebase.
  // ptsOffset wird vorher von pts/dts abgezogen (Nullpunkt der Datei).
  // Das Paket wird nicht verändert (intern kopiert).
  void Write(const AVPacket* pkt, int64_t ptsOffset = 0);
  // Finalisiert die Datei. Wirft bei Schreibfehlern (z. B. Datenträger voll).
  void Close();
  void Abort();  // schließt ohne Trailer (Datei bleibt ggf. unvollständig)

  bool IsOpen() const { return fmt_ != nullptr; }
  int64_t BytesWritten() const;
  const std::filesystem::path& File() const { return file_; }

 private:
  std::mutex mutex_;
  std::filesystem::path file_;
  AVFormatContext* fmt_ = nullptr;
  std::vector<AVRational> encoderTb_;
  bool headerWritten_ = false;
  AvPacketPtr tmp_;
  AVDictionary* pendingOpts_ = nullptr;  // Muxer-Optionen bis WriteHeader()
};

} // namespace litecap
