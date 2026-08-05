#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace xv::log {

enum class Level {
    Debug,
    Info,
    Warn,
    Error,
};

// One retained log line, so the in-app log viewer can show recent history
// without re-reading the file.
struct Entry {
    Level level;
    std::string timestamp;
    std::string message;
};

// Opens the log file and starts capturing. Safe to call once at startup.
void init(const std::filesystem::path& file);
void shutdown();

void write(Level level, std::string_view message);

// Returns a copy of the retained tail, newest last.
std::vector<Entry> recent();

const char* levelName(Level level);

template <typename... Args>
void writef(Level level, std::format_string<Args...> fmt, Args&&... args) {
    write(level, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace xv::log

#define XV_DEBUG(...) ::xv::log::writef(::xv::log::Level::Debug, __VA_ARGS__)
#define XV_INFO(...) ::xv::log::writef(::xv::log::Level::Info, __VA_ARGS__)
#define XV_WARN(...) ::xv::log::writef(::xv::log::Level::Warn, __VA_ARGS__)
#define XV_ERROR(...) ::xv::log::writef(::xv::log::Level::Error, __VA_ARGS__)
