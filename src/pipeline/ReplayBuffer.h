#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "pipeline/CaptureSession.h"

namespace litecap {

// "Instant Replay": hält die letzten N Sekunden der kodierten Pakete vor.
//
// Speicher: Die Paketdaten liegen in einer Ringdatei auf dem Datenträger, im RAM nur ein
// kleiner Index (~48 Byte pro Paket, bei 10 min/60 fps ≈ 3 MB) plus 1 MB Schreibpuffer.
// Der RAM-Bedarf ist damit praktisch unabhängig von Replay-Länge und Bitrate.
class ReplayBuffer {
 public:
  ReplayBuffer() = default;
  ~ReplayBuffer() { Stop(); }
  ReplayBuffer(const ReplayBuffer&) = delete;
  ReplayBuffer& operator=(const ReplayBuffer&) = delete;

  // Legt die Ringdatei an (Größe aus Bitrate × Dauer + Reserve) und hängt sich an die Session.
  void Start(CaptureSession& session, int seconds, int videoKbps, int audioKbpsTotal,
             const std::filesystem::path& dir);
  void Stop();
  bool Active() const { return session_ != nullptr; }
  int Seconds() const { return seconds_; }

  // Speichert die gepufferten Sekunden als Clip (MP4 mit faststart / MKV).
  // Blockiert nur für das Kopieren des Index; das Schreiben läuft im aufrufenden Thread –
  // also aus einem Hintergrund-Thread aufrufen. Wirft bei Fehlern.
  // Liefert die tatsächliche Clip-Länge in Sekunden.
  double SaveClip(const std::filesystem::path& file);

  // Gepufferte Dauer (für Tooltip/Log)
  double BufferedSeconds() const;
  uint64_t FileCapacity() const { return capacity_; }

 private:
  struct Entry {
    uint64_t logicalOffset;  // fortlaufend; physisch = logicalOffset % capacity_
    uint32_t size;
    int32_t stream;
    int64_t pts, dts, duration;
    int32_t flags;
  };

  void OnPacket(const AVPacket* pkt);
  void WriteRing(const uint8_t* data, size_t size);  // in Schreibpuffer/Datei
  void FlushWriteBuffer();                           // Aufrufer hält mutex_
  void ReadRing(HANDLE file, uint64_t logical, uint8_t* dst, size_t size) const;
  void TrimLocked();

  CaptureSession* session_ = nullptr;
  int sinkId_ = 0;
  int seconds_ = 0;
  std::vector<StreamInfo> streams_;          // Kopie der Stream-Beschreibungen
  std::vector<AVCodecParameters*> params_;   // eigene Kopien (Session kann neu starten)

  mutable std::mutex mutex_;
  std::deque<Entry> index_;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  std::filesystem::path path_;
  uint64_t capacity_ = 0;
  uint64_t written_ = 0;       // logische Schreibposition (Datei + Puffer)
  uint64_t flushed_ = 0;       // bis hierhin liegt alles in der Datei
  std::vector<uint8_t> wbuf_;  // Schreibpuffer (bündelt viele kleine Pakete)
  std::string error_;          // erster Schreibfehler
};

} // namespace litecap
