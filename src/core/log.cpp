#include "core/log.h"

#include <Windows.h>

#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>

namespace bridger::log {
namespace {

constexpr std::size_t kHistory = 512;

std::mutex g_mutex;
std::ofstream g_file;
std::deque<std::string> g_history;
std::uint64_t g_written = 0;

const char* label(Level level) {
    switch (level) {
        case Level::Trace: return "trace";
        case Level::Info:  return "info ";
        case Level::Warn:  return "warn ";
        case Level::Error: return "error";
    }
    return "?????";
}

}

void init(const std::filesystem::path& dir) {
    std::scoped_lock lock(g_mutex);
    if (g_file.is_open()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_file.open(dir / "bridger.log", std::ios::out | std::ios::trunc);
}

void shutdown() {
    std::scoped_lock lock(g_mutex);
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
}

void write(Level level, std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto line = std::format("[{:%H:%M:%OS}] [{}] {}\n",
                                  std::chrono::floor<std::chrono::milliseconds>(now),
                                  label(level), message);

    std::scoped_lock lock(g_mutex);
    OutputDebugStringA(line.c_str());
    if (g_file.is_open()) {
        g_file << line;
        g_file.flush();
    }
    if (!line.empty()) {
        g_history.emplace_back(line.substr(0, line.size() - 1));
        ++g_written;
        while (g_history.size() > kHistory) {
            g_history.pop_front();
        }
    }
}

void snapshot(std::vector<std::string>& out) {
    std::scoped_lock lock(g_mutex);
    out.assign(g_history.begin(), g_history.end());
}

std::uint64_t snapshot_since(std::uint64_t since, std::vector<std::string>& out) {
    std::scoped_lock lock(g_mutex);
    out.clear();
    const std::uint64_t first = g_written - g_history.size();
    for (std::uint64_t i = std::max(since, first); i < g_written; ++i) {
        out.push_back(g_history[static_cast<std::size_t>(i - first)]);
    }
    return g_written;
}

}
