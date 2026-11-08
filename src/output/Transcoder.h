#pragma once

#include <filesystem>

namespace litecap {

// Wandelt das Video einer Datei nach AV1 (SVT-AV1) um; Tonspuren werden unverändert kopiert.
// Läuft im Kindprozess "litecap.exe --transcode-av1 <ein> <aus>" mit Leerlauf-Priorität.
// Liefert 0 bei Erfolg.
int RunTranscodeAv1(const std::filesystem::path& input, const std::filesystem::path& output);

} // namespace litecap
