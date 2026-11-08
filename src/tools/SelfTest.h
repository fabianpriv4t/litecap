#pragma once

#include <string>

namespace litecap {

// Kommandozeilen-Tests (Ausgabe in die aufrufende Konsole):
//   litecap.exe --test-capture [sekunden]
// Liefert den Prozess-Exitcode.
int RunCaptureSelfTest(int seconds);

//   litecap.exe --test-record [sekunden] [codec] [encoder]
// Nimmt mit den Einstellungen aus der config.json auf (codec/encoder überschreibbar) und
// prüft die entstandene Datei.
int RunRecordSelfTest(int seconds, const std::string& codec, const std::string& encoder);

//   litecap.exe --test-replay [puffersekunden] [laufzeit]
// Füllt den Replay-Puffer, speichert einen Clip und prüft ihn (Länge ≈ Puffersekunden).
int RunReplaySelfTest(int bufferSeconds, int runSeconds);

//   litecap.exe --test-decode <datei>
// Dekodiert alle Videoframes, zählt Decoderfehler, misst Helligkeit/Kontrast und speichert
// das Bild aus der Mitte als BMP (zur Fehlersuche bei grauen/kaputten Videos).
int RunDecodeTest(const std::wstring& file, double snapSec = -1);
// Dasselbe mit dem Windows-Decoder (Media Foundation), wie ihn die Windows-Player nutzen.
int RunMfDecodeTest(const std::wstring& file, bool hardware);

} // namespace litecap
