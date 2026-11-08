#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace litecap {

// Audioquelle über WASAPI (Shared Mode, ereignisgesteuert).
//  - Loopback: alles, was über das Standard-Wiedergabegerät läuft (System-Ton)
//  - Capture:  Mikrofon (Standardgerät oder per ID)
// Windows liefert direkt 48 kHz / Stereo / float (AUTOCONVERTPCM) → kein eigener Resampler.
//
// Ausgabe ist ein lückenloser Sample-Strom auf einer gemeinsamen Zeitachse (QPC-Ursprung der
// Session): Pausen (bei Loopback kommen ohne Ton gar keine Pakete) werden mit Stille gefüllt,
// Überlappungen abgeschnitten. Dadurch bleibt der Ton synchron zum Bild.
class WasapiSource {
 public:
  static constexpr int kSampleRate = 48000;
  static constexpr int kChannels = 2;

  enum class Kind { Loopback, Microphone };
  // data == nullptr bedeutet Stille. frames = Anzahl Stereo-Samples.
  using DataFn = std::function<void(const float* data, size_t frames)>;

  WasapiSource() = default;
  ~WasapiSource() { Stop(); }
  WasapiSource(const WasapiSource&) = delete;
  WasapiSource& operator=(const WasapiSource&) = delete;

  // originQpc: QueryPerformanceCounter-Wert, der Sample 0 entspricht.
  void Start(Kind kind, const std::wstring& deviceId, int64_t originQpc, DataFn onData);
  void Stop();

  const char* Name() const { return kind_ == Kind::Loopback ? "System-Ton" : "Mikrofon"; }

 private:
  void Run();
  bool CaptureLoop();  // false = Gerät verloren/geändert → neu initialisieren
  void Deliver(const float* data, size_t frames, int64_t startSample);
  void PadSilenceUntilNow();
  int64_t NowSample() const;

  Kind kind_ = Kind::Loopback;
  std::wstring deviceId_;
  int64_t originQpc_ = 0;
  int64_t qpcFreq_ = 1;
  DataFn onData_;
  int64_t position_ = 0;  // nächstes zu lieferndes Sample
  HANDLE stopEvent_ = nullptr;
  HANDLE deviceChanged_ = nullptr;  // Standardgerät gewechselt (z. B. Headset eingesteckt)
  std::thread thread_;
  bool loggedFailure_ = false;
};

} // namespace litecap
