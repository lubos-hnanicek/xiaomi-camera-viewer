#include "updater/ProgramInstaller.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <stdexcept>
#include <set>
#include <vector>

namespace xv::program_installer {
namespace {

std::filesystem::path modulePath() {
    std::wstring module(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, module.data(),
                                               static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size()) {
        throw std::runtime_error("Could not locate the updater executable.");
    }
    module.resize(length);
    return std::filesystem::path(module);
}

bool insideDirectory(const std::filesystem::path& path,
                     const std::filesystem::path& directory) {
    const auto relative = path.lexically_normal().lexically_relative(directory.lexically_normal());
    return !relative.empty() && *relative.begin() != L".." && !relative.is_absolute();
}

bool reparsePoint(const std::filesystem::path& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::wstring commandQuote(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
        backslashes = 0;
        if (character == L'"') {
            result += L'\\';
        }
        result += character;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

std::vector<std::filesystem::path> stagedFiles(const std::filesystem::path& stage) {
    std::vector<std::filesystem::path> files;
    std::set<std::wstring> names;
    for (const auto& entry : std::filesystem::directory_iterator(stage)) {
        if (entry.is_symlink() || !entry.is_regular_file()) {
            throw std::runtime_error("The staged update contains a directory or link.");
        }
        const auto name = entry.path().filename().wstring();
        if (!names.insert(name).second) {
            throw std::runtime_error("The staged update contains duplicate filenames.");
        }
        files.push_back(entry.path());
    }
    if (files.empty() ||
        !std::filesystem::is_regular_file(stage / L"XiaomiViewer.exe") ||
        !std::filesystem::is_regular_file(stage / L"XiaomiViewerUpdater.exe")) {
        throw std::runtime_error("The staged update is incomplete.");
    }
    std::ranges::sort(files);
    return files;
}

void copyFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
        throw std::runtime_error(std::format("Could not copy {}: {}.", destination.filename().string(),
                                             error.message()));
    }
}

void replaceFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
    static unsigned long long sequence = 0;
    const auto temporary = destination.parent_path() /
        (L"XiaomiViewer-update-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(++sequence) + L".tmp");
    copyFile(source, temporary);
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD error = ::GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error(std::format("Could not replace {} (Windows error {}).",
                                             destination.filename().string(), error));
    }
}

void removeReadOnly(const std::filesystem::path& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        ::SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) == 0) {
        throw std::runtime_error(std::format("Could not clear the attributes of {}.",
                                             path.filename().string()));
    }
}

void waitForParent(DWORD processId) {
    if (processId == 0 || processId == ::GetCurrentProcessId()) {
        throw std::runtime_error("The updater received an invalid parent process ID.");
    }
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            return;
        }
        throw std::runtime_error(std::format("Could not open the viewer process (Windows error {}).",
                                             error));
    }
    const DWORD wait = ::WaitForSingleObject(process, 60000);
    ::CloseHandle(process);
    if (wait != WAIT_OBJECT_0) {
        throw std::runtime_error("The viewer did not close in time. No application files were changed.");
    }
}

void startViewer(const std::filesystem::path& directory) {
    const auto executable = directory / L"XiaomiViewer.exe";
    auto command = commandQuote(executable);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                          0, nullptr, directory.c_str(), &startup, &process)) {
        throw std::runtime_error(std::format("Could not start Xiaomi Camera Viewer (Windows error {}).",
                                             ::GetLastError()));
    }
    ::CloseHandle(process.hThread);
    const DWORD wait = ::WaitForSingleObject(process.hProcess, 2000);
    DWORD exitCode = 0;
    const bool exited = wait == WAIT_OBJECT_0 &&
                        ::GetExitCodeProcess(process.hProcess, &exitCode) != 0;
    ::CloseHandle(process.hProcess);
    if (wait == WAIT_FAILED || exited) {
        throw std::runtime_error(std::format("Xiaomi Camera Viewer exited immediately{}.",
                                             exited ? std::format(" (exit code {})", exitCode) : ""));
    }
}

void scheduleCleanup(const std::filesystem::path& target,
                     const std::filesystem::path& work) {
    HANDLE predecessor = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(),
                           ::GetCurrentProcess(), &predecessor, SYNCHRONIZE, TRUE,
                           0)) {
        throw std::runtime_error(std::format(
            "Could not prepare update cleanup (Windows error {}).", ::GetLastError()));
    }
    const auto updater = target / L"XiaomiViewerUpdater.exe";
    auto command = commandQuote(updater) + L" --cleanup true" +
        L" --work " + commandQuote(work) +
        L" --wait-handle " + std::to_wstring(reinterpret_cast<UINT_PTR>(predecessor));
    const auto temporary = std::filesystem::temp_directory_path();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    SIZE_T attributeSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::byte> attributes(attributeSize);
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                             &attributeSize) ||
        !::UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                     PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     &predecessor, sizeof(predecessor), nullptr,
                                     nullptr)) {
        ::DeleteProcThreadAttributeList(startup.lpAttributeList);
        ::CloseHandle(predecessor);
        throw std::runtime_error(std::format(
            "Could not configure update cleanup (Windows error {}).", ::GetLastError()));
    }
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(
        updater.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW |
            EXTENDED_STARTUPINFO_PRESENT, nullptr, temporary.c_str(),
        &startup.StartupInfo, &process);
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    ::CloseHandle(predecessor);
    if (created) {
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    } else {
        throw std::runtime_error(std::format(
            "Could not start update cleanup (Windows error {}).", ::GetLastError()));
    }
}

} // namespace

bool canWriteDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || reparsePoint(directory)) {
        return false;
    }
    const auto probe = directory / (std::wstring(L"XiaomiViewer-write-probe-") +
                                    std::to_wstring(::GetCurrentProcessId()) + L"-" +
                                    std::to_wstring(::GetTickCount64()));
    const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    ::CloseHandle(file);
    std::filesystem::remove(probe, error);
    return !error;
}

std::string install(const Options& options) {
    try {
        std::error_code error;
        const auto target = std::filesystem::canonical(options.targetDirectory, error);
        if (error || !insideDirectory(target, target.parent_path())) {
            throw std::runtime_error("The application directory is invalid.");
        }
        const auto work = std::filesystem::canonical(options.workDirectory, error);
        if (error || work.filename().wstring().starts_with(L"XiaomiViewer-update-") == false) {
            throw std::runtime_error("The update work directory is invalid.");
        }
        const auto stage = std::filesystem::canonical(options.stageDirectory, error);
        if (error || !insideDirectory(stage, work) || stage != work / L"stage") {
            throw std::runtime_error("The staged update directory is invalid.");
        }
        const auto updaterInput =
            options.updaterPath.empty() ? modulePath() : options.updaterPath;
        const auto currentUpdater = std::filesystem::canonical(updaterInput, error);
        if (error || !insideDirectory(currentUpdater, stage)) {
            throw std::runtime_error("The updater is not running from the staged package.");
        }
        if (reparsePoint(target) || !std::filesystem::is_directory(target)) {
            throw std::runtime_error("Linked application directories cannot be updated.");
        }
        const auto files = stagedFiles(stage);
        for (const auto& file : files) {
            const auto destination = target / file.filename();
            const DWORD attributes = ::GetFileAttributesW(destination.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                    throw std::runtime_error(std::format(
                        "Cannot replace {} because it is a directory or link.",
                        file.filename().string()));
                }
            }
        }
        if (options.waitForParent) {
            waitForParent(options.parentProcessId);
        }

        const auto backup = work / L"backup";
        std::filesystem::create_directories(backup);
        std::set<std::filesystem::path> originals;
        for (const auto& file : files) {
            const auto destination = target / file.filename();
            if (std::filesystem::exists(destination)) {
                copyFile(destination, backup / file.filename());
                originals.insert(file.filename());
            }
        }
        std::vector<std::filesystem::path> changed;
        try {
            for (const auto& file : files) {
                const auto destination = target / file.filename();
                if (std::filesystem::exists(destination)) {
                    removeReadOnly(destination);
                }
                replaceFile(file, destination);
                changed.push_back(file.filename());
            }
            if (options.restart) {
                startViewer(target);
            }
        } catch (const std::exception& installError) {
            std::string rollbackError;
            for (auto entry = changed.rbegin(); entry != changed.rend(); ++entry) {
                try {
                    const auto destination = target / *entry;
                    if (originals.contains(*entry)) {
                        if (std::filesystem::exists(destination)) {
                            removeReadOnly(destination);
                        }
                        replaceFile(backup / *entry, destination);
                    } else {
                        std::filesystem::remove(destination);
                    }
                } catch (const std::exception& error) {
                    if (!rollbackError.empty()) {
                        rollbackError += "; ";
                    }
                    rollbackError += error.what();
                }
            }
            if (!rollbackError.empty()) {
                return std::format("Update failed: {}\nRollback was incomplete: {}\nBackups remain in {}.",
                                   installError.what(), rollbackError, backup.string());
            }
            if (options.restart) {
                try {
                    startViewer(target);
                } catch (const std::exception& restartError) {
                    return std::format("Update failed and the original version was restored, but could "
                                       "not restart: {}",
                                       restartError.what());
                }
            }
            return std::format("Update failed: {}\nThe original application files were restored.",
                               installError.what());
        }
        if (options.cleanUp) {
            scheduleCleanup(target, work);
        }
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}

std::string cleanup(const std::filesystem::path& workDirectory, HANDLE predecessor) {
    if (predecessor == nullptr) {
        return "The update cleanup process did not receive its predecessor handle.";
    }
    try {
        if (::WaitForSingleObject(predecessor, INFINITE) != WAIT_OBJECT_0) {
            const DWORD waitError = ::GetLastError();
            ::CloseHandle(predecessor);
            predecessor = nullptr;
            return std::format("Could not wait for the completed updater (Windows error {}).",
                               waitError);
        }
        ::CloseHandle(predecessor);
        predecessor = nullptr;
        std::error_code error;
        const auto work = std::filesystem::canonical(workDirectory, error);
        if (error) {
            return "The update cleanup directory is invalid.";
        }
        const auto temporary =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), error);
        if (error || !work.filename().wstring().starts_with(L"XiaomiViewer-update-") ||
            work.parent_path() != temporary) {
            return "The update cleanup directory is invalid.";
        }
        std::filesystem::remove_all(work, error);
        if (error) {
            return std::format("Could not remove the update files: {}", error.message());
        }
        return {};
    } catch (const std::exception& error) {
        if (predecessor != nullptr) {
            ::CloseHandle(predecessor);
        }
        return error.what();
    }
}

} // namespace xv::program_installer
