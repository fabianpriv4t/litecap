#include "config/Config.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "util/Paths.h"
#include "util/Strings.h"

namespace litecap {

using nlohmann::json;
namespace fs = std::filesystem;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GeneralConfig, autostart, startReplayOnLaunch,
                                                logLevel)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CaptureConfig, source, monitorIndex, windowTitle,
                                                windowExe, captureCursor)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(VideoConfig, codec, encoder, fps, width, height,
                                                rateControl, bitrateKbps, quality,
                                                keyframeIntervalSec, av1TranscodeOnlyWhenIdle)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AudioConfig, system, microphone,
                                                microphoneDeviceId, separateTracks, bitrateKbps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ReplayConfig, seconds, bufferDir)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OutputConfig, mode, folder, container,
                                                fileNamePattern, deleteAfterUpload, minFreeSpaceMB)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(YouTubeConfig, clientId, channelName,
                                                titleTemplate, description, tags, privacy,
                                                categoryId, maxUploadKbps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(HotkeyConfig, toggleRecording, saveReplay,
                                                toggleReplay)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NotificationConfig, enabled, sound)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Config, version, general, capture, video, audio,
                                                replay, output, youtube, hotkeys, notifications)

Config Config::Load(const fs::path& file) {
  Config cfg;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    cfg.Save(file);
    return cfg;
  }
  try {
    json j = json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    cfg = j.get<Config>();
  } catch (const json::exception& e) {
    throw std::runtime_error(std::string("config.json ist fehlerhaft: ") + e.what());
  }
  return cfg;
}

void Config::Save(const fs::path& file) const {
  json j = *this;
  fs::path tmp = file;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Konfiguration kann nicht geschrieben werden: " +
                                       WideToUtf8(tmp.wstring()));
    out << j.dump(2);
    if (!out) throw std::runtime_error("Schreibfehler beim Speichern der Konfiguration");
  }
  if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Konfiguration kann nicht ersetzt werden: " + LastErrorMessage());
}

namespace {
template <class T>
void OneOf(std::vector<std::string>& fixes, const char* name, std::string& value,
           std::initializer_list<const char*> allowed, T fallback) {
  std::string v = ToLower(value);
  for (const char* a : allowed)
    if (v == a) {
      value = v;
      return;
    }
  fixes.push_back(std::string(name) + ": ungültiger Wert \"" + value + "\", verwende \"" +
                  fallback + "\"");
  value = fallback;
}

void Clamp(std::vector<std::string>& fixes, const char* name, int& value, int lo, int hi) {
  int c = std::clamp(value, lo, hi);
  if (c != value) {
    fixes.push_back(std::string(name) + ": " + std::to_string(value) + " liegt außerhalb [" +
                    std::to_string(lo) + ", " + std::to_string(hi) + "], verwende " +
                    std::to_string(c));
    value = c;
  }
}
} // namespace

std::vector<std::string> Config::Validate() {
  std::vector<std::string> f;
  OneOf(f, "general.logLevel", general.logLevel, {"debug", "info", "warn", "error"}, "info");
  OneOf(f, "capture.source", capture.source, {"monitor", "window"}, "monitor");
  Clamp(f, "capture.monitorIndex", capture.monitorIndex, 0, 15);
  OneOf(f, "video.codec", video.codec, {"h264", "hevc", "av1"}, "h264");
  OneOf(f, "video.encoder", video.encoder, {"auto", "nvenc", "amf", "qsv", "x264"}, "auto");
  if (video.fps != 30 && video.fps != 60 && video.fps != 120) {
    f.push_back("video.fps: nur 30/60/120 erlaubt, verwende 60");
    video.fps = 60;
  }
  if (video.width != 0 || video.height != 0) {
    Clamp(f, "video.width", video.width, 320, 7680);
    Clamp(f, "video.height", video.height, 240, 4320);
    video.width &= ~1;  // Encoder brauchen gerade Maße
    video.height &= ~1;
  }
  OneOf(f, "video.rateControl", video.rateControl, {"cbr", "vbr", "cqp"}, "vbr");
  Clamp(f, "video.bitrateKbps", video.bitrateKbps, 1000, 150000);
  Clamp(f, "video.quality", video.quality, 0, 51);
  Clamp(f, "video.keyframeIntervalSec", video.keyframeIntervalSec, 1, 10);
  Clamp(f, "audio.bitrateKbps", audio.bitrateKbps, 64, 320);
  Clamp(f, "replay.seconds", replay.seconds, 30, 600);
  OneOf(f, "output.mode", output.mode, {"local", "youtube", "both"}, "local");
  OneOf(f, "output.container", output.container, {"mp4", "mkv"}, "mp4");
  if (output.fileNamePattern.empty()) output.fileNamePattern = "LiteCap_{type}_{date}_{time}";
  Clamp(f, "output.minFreeSpaceMB", output.minFreeSpaceMB, 256, 1 << 20);
  OneOf(f, "youtube.privacy", youtube.privacy, {"private", "unlisted", "public"}, "private");
  Clamp(f, "youtube.maxUploadKbps", youtube.maxUploadKbps, 0, 1 << 20);
  return f;
}

std::string Config::PipelineKey() const {
  return json{{"c", capture}, {"v", video}, {"a", audio}, {"r", replay}}.dump();
}

fs::path Config::OutputFolder() const {
  if (output.folder.empty()) return paths::DefaultVideoDir();
  return paths::ExpandEnv(Utf8ToWide(output.folder));
}

fs::path Config::ReplayBufferDir() const {
  if (replay.bufferDir.empty()) return paths::LocalDataDir() / L"replay";
  return paths::ExpandEnv(Utf8ToWide(replay.bufferDir));
}

} // namespace litecap
