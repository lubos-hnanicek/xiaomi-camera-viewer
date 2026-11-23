#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace xv {

class ProgramUpdate {
public:
    enum class State {
        Idle, Checking, Available, UpToDate, Downloading, Ready, Installing, Cancelled, Failed,
    };

    struct Status {
        State state = State::Idle;
        std::string version;
        std::string changelog;
        std::string step;
        std::string error;
        unsigned long long downloaded = 0;
        unsigned long long total = 0;
    };

    ~ProgramUpdate();
    ProgramUpdate() = default;
    ProgramUpdate(const ProgramUpdate&) = delete;
    ProgramUpdate& operator=(const ProgramUpdate&) = delete;

    void check();
    void confirm();
    bool install();
    void cancel();
    void fail(const std::string& error);
    void poll();
    [[nodiscard]] bool busy() const { return process_ != nullptr; }
    [[nodiscard]] const Status& status() const { return status_; }

private:
    bool signal(const wchar_t* name);
    void readStatus();
    void cleanUp();

    Status status_;
    HANDLE process_ = nullptr;
    std::filesystem::path runDirectory_;
    std::chrono::steady_clock::time_point nextPoll_{};
    bool cancelRequested_ = false;
};

} // namespace xv
