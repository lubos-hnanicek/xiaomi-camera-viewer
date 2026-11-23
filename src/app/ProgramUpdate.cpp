#include "app/ProgramUpdate.h"

#include <objbase.h>
#include <shellapi.h>

#include <array>
#include <algorithm>
#include <fstream>
#include <format>

#include <nlohmann/json.hpp>

#include "app/Log.h"
#include "app/Version.h"
#include "updater/ProgramInstaller.h"
#include "util/Encoding.h"

namespace xv {

ProgramUpdate::~ProgramUpdate() {
    if (process_ != nullptr) {
        if (status_.state != State::Installing) {
            cancel();
        }
        ::CloseHandle(process_);
    } else {
        cleanUp();
    }
}

void ProgramUpdate::cleanUp() {
    if (runDirectory_.empty() || status_.state == State::Failed) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(runDirectory_, error);
    if (error) {
        XV_WARN("could not remove update temporary files: {}", error.message());
    }
    runDirectory_.clear();
}

void ProgramUpdate::check() {
    if (busy()) {
        return;
    }
    cleanUp();
    runDirectory_.clear();
    status_ = Status{};
    cancelRequested_ = false;
    try {
        std::wstring module(32768, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (length == 0 || length >= module.size()) {
            throw std::runtime_error("Could not locate the application directory.");
        }
        module.resize(length);
        const auto directory = std::filesystem::path(module).parent_path();
        const auto script = directory / L"update-program.ps1";
        if (!std::filesystem::is_regular_file(script)) {
            throw std::runtime_error("The update helper is missing from this application package.");
        }
        GUID guid{};
        std::array<wchar_t, 40> name{};
        if (FAILED(::CoCreateGuid(&guid)) || ::StringFromGUID2(guid, name.data(), static_cast<int>(name.size())) == 0) {
            throw std::runtime_error("Could not create an update temporary directory.");
        }
        runDirectory_ = std::filesystem::temp_directory_path() /
            (std::wstring(L"XiaomiViewer-update-") + name.data());
        std::filesystem::create_directory(runDirectory_);
        runDirectory_ = std::filesystem::canonical(runDirectory_);
        const auto helper = runDirectory_ / L"update-program.ps1";
        std::filesystem::copy_file(script, helper);

        std::array<wchar_t, MAX_PATH> system{};
        const UINT systemLength = ::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (systemLength == 0 || systemLength >= system.size()) {
            throw std::runtime_error("Could not locate Windows PowerShell.");
        }
        const auto powershell = std::filesystem::path(system.data()) / L"WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring command = encoding::quoteArgument(powershell.wstring()) +
            L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
            encoding::quoteArgument(helper.wstring()) +
            L" -ApplicationDirectory " + encoding::quoteArgument(directory.wstring()) +
            L" -WorkDirectory " + encoding::quoteArgument(runDirectory_.wstring()) +
            L" -CurrentVersion " + encoding::quoteArgument(std::filesystem::path(XV_VERSION).wstring()) +
            L" -Architecture " + encoding::quoteArgument(std::filesystem::path(XV_ARCHITECTURE).wstring()) +
            L" -ParentProcessId " + std::to_wstring(::GetCurrentProcessId());

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, runDirectory_.c_str(), &startup, &process)) {
            throw std::runtime_error(std::format("Could not start the update helper (Windows error {}).", ::GetLastError()));
        }
        ::CloseHandle(process.hThread);
        process_ = process.hProcess;
        status_.state = State::Checking;
        status_.step = "Checking GitHub for a newer stable release";
        nextPoll_ = {};
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

bool ProgramUpdate::signal(const wchar_t* name) {
    std::ofstream marker(runDirectory_ / name);
    if (!marker) {
        fail(std::format("Could not signal the update helper ({}).", std::filesystem::path(name).string()));
        return false;
    }
    marker.close();
    if (!marker) {
        fail("Could not write the update helper signal.");
        return false;
    }
    return true;
}

void ProgramUpdate::confirm() {
    if (!cancelRequested_ && status_.state == State::Available && signal(L"confirm.request")) {
        status_.state = State::Downloading;
        status_.step = "Starting download";
    }
}

bool ProgramUpdate::install() {
    if (cancelRequested_ || status_.state != State::Ready) {
        return false;
    }
    const auto stage = runDirectory_ / L"stage";
    const auto updater = stage / L"XiaomiViewerUpdater.exe";
    std::error_code error;
    const auto canonicalUpdater = std::filesystem::canonical(updater, error);
    if (error || canonicalUpdater.parent_path().lexically_normal() !=
                     stage.lexically_normal()) {
        fail("The update package does not contain a valid standalone updater.");
        return false;
    }

    std::wstring application(32768, L'\0');
    const DWORD applicationLength =
        ::GetModuleFileNameW(nullptr, application.data(), static_cast<DWORD>(application.size()));
    if (applicationLength == 0 || applicationLength >= application.size()) {
        fail("Could not locate the application directory.");
        return false;
    }
    application.resize(applicationLength);
    const auto target = std::filesystem::path(application).parent_path();
    auto arguments = L"--target " + encoding::quoteArgument(target.wstring()) +
        L" --stage " + encoding::quoteArgument(stage.wstring()) +
        L" --work " + encoding::quoteArgument(runDirectory_.wstring()) +
        L" --parent " + std::to_wstring(::GetCurrentProcessId());
    if (program_installer::canWriteDirectory(target)) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(updater.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                              0, nullptr, stage.c_str(), &startup, &process)) {
            fail(std::format("Could not start the standalone updater (Windows error {}).",
                             ::GetLastError()));
            return false;
        }
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    } else {
        SHELLEXECUTEINFOW execute{};
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_NOCLOSEPROCESS;
        execute.lpVerb = L"runas";
        execute.lpFile = updater.c_str();
        execute.lpParameters = arguments.c_str();
        execute.lpDirectory = stage.c_str();
        execute.nShow = SW_SHOWNORMAL;
        if (!::ShellExecuteExW(&execute) || execute.hProcess == nullptr) {
            const DWORD launchError = ::GetLastError();
            if (execute.hProcess != nullptr) {
                ::CloseHandle(execute.hProcess);
            }
            if (launchError == ERROR_CANCELLED) {
                fail("Administrator approval was cancelled. No application files were changed.");
            } else {
                fail(std::format("Could not launch the elevated updater (Windows error {}).",
                                 launchError));
            }
            return false;
        }
        ::CloseHandle(execute.hProcess);
    }
    if (!signal(L"apply.request")) {
        return false;
    }
    status_.state = State::Installing;
    return true;
}

void ProgramUpdate::cancel() {
    if (process_ == nullptr || cancelRequested_ || status_.state == State::Installing) {
        return;
    }
    // Do not call signal here: a failed cancellation must not recursively retry itself.
    std::ofstream marker(runDirectory_ / L"cancel.request");
    marker.close();
    if (!marker) {
        status_.error = "Could not request update cancellation.";
        XV_ERROR("{}", status_.error);
        return;
    }
    cancelRequested_ = true;
    status_.step = "Cancelling update";
}

void ProgramUpdate::fail(const std::string& error) {
    status_.state = State::Failed;
    status_.error = error;
    XV_ERROR("program update: {}", error);
    cancel();
}

void ProgramUpdate::readStatus() {
    const auto result = runDirectory_ / L"status.json";
    const HANDLE file = ::CreateFileW(result.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            fail(std::format("Could not read update progress (Windows error {}).", error));
        }
        return;
    }
    LARGE_INTEGER length{};
    DWORD read = 0;
    std::string text;
    if (::GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart <= 512 * 1024) {
        text.resize(static_cast<size_t>(length.QuadPart));
        if (!::ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size()) {
            text.clear();
        }
    }
    ::CloseHandle(file);
    try {
        const auto value = nlohmann::json::parse(text, nullptr, false);
        if (value.is_discarded() || !value.is_object() || !value.contains("state")) {
            return;
        }
        const auto state = value.at("state").get<std::string>();
        static constexpr std::array states{
            std::pair{"checking", State::Checking}, std::pair{"available", State::Available},
            std::pair{"up_to_date", State::UpToDate}, std::pair{"downloading", State::Downloading},
            std::pair{"ready", State::Ready}, std::pair{"installing", State::Installing},
            std::pair{"cancelled", State::Cancelled}, std::pair{"failed", State::Failed},
        };
        const auto match = std::find_if(states.begin(), states.end(),
            [&](const auto& candidate) { return candidate.first == state; });
        if (match == states.end()) {
            return;
        }
        const auto version = value.find("version");
        const auto changelog = value.find("changelog");
        const auto downloaded = value.find("downloaded");
        const auto total = value.find("total");
        const auto step = value.find("step");
        const auto helperError = value.find("error");
        if (version == value.end() || !version->is_string() ||
            changelog == value.end() || !changelog->is_string() ||
            downloaded == value.end() || !downloaded->is_number_unsigned() ||
            total == value.end() || !total->is_number_unsigned() ||
            step == value.end() || !step->is_string() ||
            helperError == value.end() || !helperError->is_string()) {
            return;
        }
        status_.version = version->get<std::string>();
        status_.changelog = changelog->get<std::string>();
        status_.downloaded = downloaded->get<unsigned long long>();
        status_.total = total->get<unsigned long long>();
        if (status_.state == State::Failed) {
            return;
        }
        // The helper may not yet have consumed the UI's confirmation or cancellation.
        if (status_.state == State::Downloading && match->second == State::Available) {
            return;
        }
        if (cancelRequested_ && match->second != State::Cancelled && match->second != State::Failed) {
            return;
        }
        status_.state = match->second;
        if (!cancelRequested_) {
            status_.step = step->get<std::string>();
        }
        status_.error = helperError->get<std::string>();
        if (status_.state == State::Failed) {
            XV_ERROR("program update: {}", status_.error);
        }
    } catch (const std::exception&) {
        // status.json is replaced atomically, but a process may still observe
        // the brief interval before the first complete file exists. Retry next
        // frame rather than turning that into a user-visible update failure.
    }
}

void ProgramUpdate::poll() {
    if (process_ == nullptr || std::chrono::steady_clock::now() < nextPoll_) {
        return;
    }
    nextPoll_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    readStatus();
    const DWORD wait = ::WaitForSingleObject(process_, 0);
    if (wait == WAIT_TIMEOUT) {
        return;
    }
    DWORD exitCode = 1;
    if (wait != WAIT_OBJECT_0 || !::GetExitCodeProcess(process_, &exitCode)) {
        fail(std::format("Could not monitor the update helper (Windows error {}).", ::GetLastError()));
        return;
    }
    readStatus();
    ::CloseHandle(process_);
    process_ = nullptr;
    if (status_.state == State::Installing) {
        return;
    }
    if (cancelRequested_ && exitCode == 0 && status_.state != State::Failed) {
        status_.state = State::Cancelled;
        status_.step = "Update cancelled; no application files were changed";
    }
    if (status_.state != State::Failed && status_.state != State::Cancelled &&
        (exitCode != 0 || status_.state != State::UpToDate)) {
        fail(std::format("The update helper exited unexpectedly (exit code {}).", exitCode));
    }
    cleanUp();
}

} // namespace xv
