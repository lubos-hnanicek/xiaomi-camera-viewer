#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace xv {

class CameraDiagnostics {
public:
    enum class State { Idle, Collecting, Complete, Partial, Cancelled, Failed };

    struct Options {
        std::filesystem::path script;
        std::filesystem::path dll;
        std::filesystem::path outputDirectory;
        std::string deviceId;
        bool includeStorage = false;
        bool includeFirmwareInfo = false;
        bool includeAppLog = false;
        bool selfTest = false;
    };

    struct Status {
        State state = State::Idle;
        std::string step;
        std::string error;
        std::filesystem::path archive;
        std::filesystem::path directory;
    };

    ~CameraDiagnostics();
    CameraDiagnostics() = default;
    CameraDiagnostics(const CameraDiagnostics&) = delete;
    CameraDiagnostics& operator=(const CameraDiagnostics&) = delete;

    bool start(const Options& options);
    void poll();
    void cancel();
    [[nodiscard]] bool busy() const { return process_ != nullptr; }
    [[nodiscard]] const Status& status() const { return status_; }

private:
    void readStatus();

    HANDLE process_ = nullptr;
    std::filesystem::path runDirectory_;
    std::filesystem::path resultPath_;
    std::filesystem::path cancelPath_;
    Status status_;
    std::string resultState_;
    bool cancelRequested_ = false;
};

} // namespace xv