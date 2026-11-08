#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "config/Config.h"

namespace litecap {

// Erzeugt einen eindeutigen Ausgabepfad aus output.fileNamePattern.
// Platzhalter: {date} = 2026-10-20, {time} = 14-05-33, {source} = Monitor1/Fenstertitel,
//              {type} = Aufnahme/Clip. Ungültige Zeichen werden ersetzt.
// Existiert die Datei schon, wird " (2)", " (3)" … angehängt.
std::filesystem::path MakeOutputPath(const Config& cfg, const std::wstring& source,
                                     const std::wstring& type);

// Freier Speicher auf dem Laufwerk des Pfads (0 bei Fehler).
uint64_t FreeDiskBytes(const std::filesystem::path& dir);

// Ersetzt Platzhalter in Texten wie dem YouTube-Titel.
std::string ExpandTemplate(const std::string& tmpl, const std::wstring& source,
                           const std::wstring& type);

} // namespace litecap
