#include "app/Log.h"

#include <windows.h>

#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>

namespace xv::log {
namespace {

// Enough history for the log pane to be useful after a few reconnect cycles,
// bounded so a long-running session cannot grow without limit.
constexpr size_t kMaxRetained = 2000;

struct State {
    std::mutex mutex;
    std::ofstream file;
    std::deque<Entry> retained;
};

State& state() {
    static State s;
    return s;
}

std::string nowStamp() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%d %H:%M:%OS}", std::chrono::floor<std::chrono::milliseconds>(now));
}

} // namespace

const char* levelName(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "?";
}

void init(const std::filesystem::path& file) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    s.file.open(file, std::ios::out | std::ios::trunc);
}

void shutdown() {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (s.file.is_open()) {
        s.file.flush();
        s.file.close();
    }
}

void write(Level level, std::string_view message) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);

    Entry entry{level, nowStamp(), std::string(message)};
    const std::string line = std::format("{} [{:<5}] {}", entry.timestamp, levelName(level), entry.message);

    if (s.file.is_open()) {
        s.file << line << '\n';
        // Flushing every line costs little at these rates and means a crash
        // still leaves the reason on disk.
        s.file.flush();
    }

    OutputDebugStringA(line.c_str());
    OutputDebugStringA("\n");

    s.retained.push_back(std::move(entry));
    if (s.retained.size() > kMaxRetained) {
        s.retained.pop_front();
    }
}

std::vector<Entry> recent() {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return {s.retained.begin(), s.retained.end()};
}

} // namespace xv::log
