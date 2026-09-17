#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace bridger::log {

enum class Level { Trace, Info, Warn, Error };

void init(const std::filesystem::path& dir);
void shutdown();
void write(Level level, std::string_view message);
void snapshot(std::vector<std::string>& out);
std::uint64_t snapshot_since(std::uint64_t since, std::vector<std::string>& out);

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}
