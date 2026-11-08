#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace litecap {

// Alle Strings sind UTF-8. Felder, die in der JSON-Datei fehlen, behalten ihre Standardwerte.

struct GeneralConfig {
  bool autostart = false;
  bool startReplayOnLaunch = true;   // Replay-Puffer beim Start automatisch aktivieren
  std::string logLevel = "info";     // debug | info | warn | error
};

struct CaptureConfig {
  std::string source = "monitor";    // monitor | window
  int monitorIndex = 0;              // 0 = primärer Monitor
  std::string windowTitle;           // Teilstring des Fenstertitels (source = window)
  std::string windowExe;             // alternativ: Prozessname, z. B. "game.exe"
  bool captureCursor = true;
};

struct VideoConfig {
  std::string codec = "h264";        // h264 | hevc | av1 (ohne HW-AV1: HEVC live + AV1 nachträglich)
  std::string encoder = "auto";      // auto | nvenc | amf | qsv | x264
  int fps = 60;                      // 30 | 60 | 120
  int width = 0;                     // 0 = native Auflösung der Quelle
  int height = 0;
  std::string rateControl = "vbr";   // cbr | vbr | cqp
  int bitrateKbps = 20000;           // für cbr / vbr
  int quality = 23;                  // für cqp: 0 (beste) … 51
  int keyframeIntervalSec = 2;
  bool av1TranscodeOnlyWhenIdle = true;  // AV1-Umwandlung erst, wenn kein Vollbildspiel läuft
};

struct AudioConfig {
  bool system = true;
  bool microphone = false;
  std::string microphoneDeviceId;    // leer = Standard-Mikrofon
  bool separateTracks = true;        // System und Mikrofon als getrennte Spuren
  int bitrateKbps = 160;
};

struct ReplayConfig {
  int seconds = 120;                 // 30 … 600
  std::string bufferDir;             // leer = %LOCALAPPDATA%\LiteCap\replay
};

struct OutputConfig {
  std::string mode = "local";        // local | youtube | both
  std::string folder;                // leer = Videos\LiteCap
  std::string container = "mp4";     // mp4 | mkv
  std::string fileNamePattern = "LiteCap_{type}_{date}_{time}";  // {date} {time} {source} {type}
  bool deleteAfterUpload = false;
  int minFreeSpaceMB = 2048;         // Aufnahme stoppt, wenn weniger frei ist
};

struct YouTubeConfig {
  std::string clientId;              // OAuth-Client-ID (Desktop-App) aus der Google Cloud Console
  // Das Client-Secret und die Tokens liegen im Windows Credential Manager, nicht hier.
  std::string channelName;           // nur zur Anzeige
  std::string titleTemplate = "Clip {date} {time}";
  std::string description = "Aufgenommen mit LiteCap";
  std::vector<std::string> tags;
  std::string privacy = "private";   // private | unlisted | public
  int categoryId = 20;               // 20 = Gaming
  int maxUploadKbps = 0;             // 0 = unbegrenzt
};

struct HotkeyConfig {
  std::string toggleRecording = "Ctrl+Alt+F9";  // Alt+F9/F10 belegt die NVIDIA App
  std::string saveReplay = "Ctrl+Alt+F10";
  std::string toggleReplay = "Ctrl+Alt+F11";
};

struct NotificationConfig {
  bool enabled = true;
  bool sound = false;
};

struct Config {
  int version = 1;
  GeneralConfig general;
  CaptureConfig capture;
  VideoConfig video;
  AudioConfig audio;
  ReplayConfig replay;
  OutputConfig output;
  YouTubeConfig youtube;
  HotkeyConfig hotkeys;
  NotificationConfig notifications;

  // Lädt die Datei; existiert sie nicht, wird sie mit Standardwerten angelegt.
  // Bei Syntaxfehlern wird eine Exception geworfen (die Datei bleibt unangetastet).
  static Config Load(const std::filesystem::path& file);
  // Schreibt atomar (temp-Datei + Umbenennen).
  void Save(const std::filesystem::path& file) const;
  // Korrigiert ungültige Werte; liefert eine Liste der Korrekturen.
  std::vector<std::string> Validate();

  // Fingerabdruck aller Einstellungen, die einen Neustart der Capture-Pipeline erfordern
  std::string PipelineKey() const;

  std::filesystem::path OutputFolder() const;
  std::filesystem::path ReplayBufferDir() const;
};

} // namespace litecap
