#include "app/CameraDiagnostics.h"

#include <objbase.h>

#include <array>
#include <fstream>
#include <format>

#include <nlohmann/json.hpp>

namespace xv {
namespace {

std::filesystem::path utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else {
            result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
            backslashes = 0;
            if (character == L'"') {
                result += L'\\';
            }
            result += character;
        }
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

bool insideDirectory(const std::filesystem::path& path, const std::filesystem::path& directory) {
    const auto relative = path.lexically_normal().lexically_relative(directory.lexically_normal());
    return !relative.empty() && *relative.begin() != L".." && !relative.is_absolute();
}

} // namespace

CameraDiagnostics::~CameraDiagnostics() {
    if (process_ != nullptr) {
        cancel();
        ::CloseHandle(process_);
    }
}

bool CameraDiagnostics::start(const Options& options) {
    if (busy()) {
        return false;
    }
    status_ = Status{};
    resultState_.clear();
    cancelRequested_ = false;
    try {
        auto script = options.script;
        if (script.empty()) {
            std::wstring module(32768, L'\0');
            const DWORD length = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
            if (length == 0 || length >= module.size()) {
                throw std::runtime_error("Could not locate the application directory.");
            }
            module.resize(length);
            script = std::filesystem::path(module).parent_path() / L"collect-camera-diagnostics.ps1";
        }
        if (!std::filesystem::is_regular_file(script)) {
            throw std::runtime_error("The camera diagnostics collector is missing from this application package.");
        }
        if (options.deviceId.empty() && !options.selfTest) {
            throw std::runtime_error("Choose a camera from the Xiaomi account first.");
        }
        GUID guid{};
        std::array<wchar_t, 40> name{};
        if (FAILED(::CoCreateGuid(&guid)) || ::StringFromGUID2(guid, name.data(), static_cast<int>(name.size())) == 0) {
            throw std::runtime_error("Could not create a diagnostics output directory.");
        }
        runDirectory_ = std::filesystem::absolute(options.outputDirectory) / name.data();
        std::filesystem::create_directories(runDirectory_);
        resultPath_ = runDirectory_ / L"app-result.json";
        cancelPath_ = runDirectory_ / L"cancel.request";

        std::array<wchar_t, MAX_PATH> system{};
        const UINT length = ::GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (length == 0 || length >= system.size()) {
            throw std::runtime_error("Could not locate Windows PowerShell.");
        }
        const auto powershell = std::filesystem::path(system.data()) / L"WindowsPowerShell/v1.0/powershell.exe";
        std::wstring command = quoteArgument(powershell.wstring()) +
            L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + quoteArgument(script.wstring()) +
            L" -OutputDirectory " + quoteArgument(runDirectory_.wstring()) +
            L" -ResultPath " + quoteArgument(resultPath_.wstring()) +
            L" -CancelPath " + quoteArgument(cancelPath_.wstring());
        if (!options.deviceId.empty()) {
            command += L" -Did " + quoteArgument(utf8Path(options.deviceId).wstring());
        }
        if (!options.dll.empty()) {
            command += L" -DllPath " + quoteArgument(options.dll.wstring());
        }
        if (options.includeStorage) { command += L" -IncludeStorage"; }
        if (options.includeFirmwareInfo) { command += L" -IncludeFirmwareInfo"; }
        if (options.includeAppLog) { command += L" -IncludeAppLog"; }
        if (options.selfTest) { command += L" -SelfTest"; }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, script.parent_path().c_str(), &startup, &process)) {
            throw std::runtime_error(std::format("Could not start the diagnostics collector (Windows error {}).", ::GetLastError()));
        }
        ::CloseHandle(process.hThread);
        process_ = process.hProcess;
        status_.state = State::Collecting;
        status_.step = "Starting collector";
        return true;
    } catch (const std::exception& error) {
        status_.state = State::Failed;
        status_.error = error.what();
        return false;
    }
}

void CameraDiagnostics::readStatus() {
    std::error_code error;
    const auto size = std::filesystem::file_size(resultPath_, error);
    if (error || size > 64 * 1024) {
        return;
    }
    std::ifstream input(resultPath_, std::ios::binary);
    const auto result = nlohmann::json::parse(input, nullptr, false);
    if (!result.is_object()) {
        return;
    }
    try {
        resultState_ = result.value("status", std::string{});
        if (!cancelRequested_) {
            status_.step = result.value("step", std::string{});
        }
        if (result.contains("error") && result["error"].is_string()) {
            status_.error = result["error"].get<std::string>();
        }
        const auto archive = utf8Path(result.value("archive", std::string{}));
        const auto directory = utf8Path(result.value("directory", std::string{}));
        if (insideDirectory(archive, runDirectory_) && insideDirectory(directory, runDirectory_)) {
            status_.archive = archive;
            status_.directory = directory;
        }
    } catch (const std::exception&) {
    }
}

void CameraDiagnostics::poll() {
    if (!busy()) {
        return;
    }
    readStatus();
    const DWORD wait = ::WaitForSingleObject(process_, 0);
    if (wait == WAIT_TIMEOUT) {
        return;
    }
    DWORD exitCode = 1;
    ::GetExitCodeProcess(process_, &exitCode);
    ::CloseHandle(process_);
    process_ = nullptr;
    readStatus();
    std::error_code error;
    const bool archiveExists = !status_.archive.empty() && std::filesystem::is_regular_file(status_.archive, error);
    if (cancelRequested_ || resultState_ == "cancelled") {
        status_.state = State::Cancelled;
    } else if (!archiveExists || exitCode != 0) {
        status_.state = State::Failed;
        if (status_.error.empty()) {
            status_.error = std::format("The collector exited without a completed archive (exit code {}).", exitCode);
        }
    } else {
        status_.state = resultState_ == "collected" || resultState_ == "self-test" ? State::Complete : State::Partial;
    }
    status_.step.clear();
}

void CameraDiagnostics::cancel() {
    if (!busy() || cancelRequested_) {
        return;
    }
    std::ofstream marker(cancelPath_);
    if (!marker) {
        status_.error = "Could not request cancellation.";
        return;
    }
    cancelRequested_ = true;
    status_.step = "Cancellation requested; finishing the current request";
}

} // namespace xv