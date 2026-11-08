#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace litecap::log {

enum class Level { Debug, Info, Warn, Error };

// Öffnet die Logdatei (rotiert bei > 5 MB nach litecap.log.1).
void Init(const std::filesystem::path& file, Level minLevel = Level::Info);
void SetLevel(Level minLevel);
void Write(Level level, std::string_view msg);
void Shutdown();

template <class... Args> void Debug(std::format_string<Args...> f, Args&&... a) {
  Write(Level::Debug, std::format(f, std::forward<Args>(a)...));
}
template <class... Args> void Info(std::format_string<Args...> f, Args&&... a) {
  Write(Level::Info, std::format(f, std::forward<Args>(a)...));
}
template <class... Args> void Warn(std::format_string<Args...> f, Args&&... a) {
  Write(Level::Warn, std::format(f, std::forward<Args>(a)...));
}
template <class... Args> void Error(std::format_string<Args...> f, Args&&... a) {
  Write(Level::Error, std::format(f, std::forward<Args>(a)...));
}

} // namespace litecap::log
