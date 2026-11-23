#include "updater/ProgramInstaller.h"

#include <windows.h>

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string message(const std::string& text) {
    return "Program installer test failed: " + text;
}

void require(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(message(text));
    }
}

void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::path tempDirectory() {
    return fs::temp_directory_path() /
        (std::wstring(L"XiaomiViewer installer tests ") + std::to_wstring(::GetTickCount64()));
}

xv::program_installer::Options options(const fs::path& work, const fs::path& target) {
    xv::program_installer::Options result;
    result.workDirectory = work;
    result.stageDirectory = work / "stage";
    result.targetDirectory = target;
    result.updaterPath = result.stageDirectory / "XiaomiViewerUpdater.exe";
    result.waitForParent = false;
    result.restart = false;
    result.cleanUp = false;
    return result;
}

void prepareStage(const fs::path& stage) {
    fs::create_directories(stage);
    write(stage / "XiaomiViewer.exe", "new viewer");
    write(stage / "XiaomiViewerUpdater.exe", "new updater");
    write(stage / "a.dll", "new a");
    write(stage / "b.dll", "new b");
}

} // namespace

int wmain() {
    const fs::path root = tempDirectory();
    try {
        fs::create_directories(root);
        const fs::path work = root / "XiaomiViewer-update-{12345678-1234-1234-1234-123456789abc}";
        const fs::path target = root / "application";
        const fs::path stage = work / "stage";
        fs::create_directories(target);
        fs::create_directories(stage);
        prepareStage(stage);
        write(target / "XiaomiViewer.exe", "old viewer");
        write(target / "XiaomiViewerUpdater.exe", "old updater");
        write(target / "a.dll", "old a");
        write(target / "b.dll", "old b");
        write(target / "unrelated.txt", "keep");

        require(xv::program_installer::install(options(work, target)).empty(),
                "a complete update should succeed");
        require(read(target / "XiaomiViewer.exe") == "new viewer", "viewer was not replaced");
        require(read(target / "a.dll") == "new a", "staged files were not installed");
        require(read(target / "b.dll") == "new b", "staged files were not installed");
        require(read(target / "unrelated.txt") == "keep", "unrelated file was removed");

        fs::remove_all(work);
        fs::create_directories(stage);
        prepareStage(stage);
        write(target / "XiaomiViewer.exe", "old viewer");
        write(target / "a.dll", "old a");
        write(target / "b.dll", "old b");
        const HANDLE locked = ::CreateFileW((target / "b.dll").c_str(),
            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        require(locked != INVALID_HANDLE_VALUE, "could not lock a target file");
        const auto error = xv::program_installer::install(options(work, target));
        ::CloseHandle(locked);
        require(error.find("restored") != std::string::npos,
                ("copy failure did not report rollback: " + error).c_str());
        require(read(target / "XiaomiViewer.exe") == "old viewer",
                "rollback did not restore the viewer");
        require(read(target / "a.dll") == "old a", "rollback did not restore an earlier file");
        require(read(target / "b.dll") == "old b", "rollback changed the locked file");
        require(read(target / "unrelated.txt") == "keep", "rollback changed an unrelated file");

        const fs::path probe = root / "write-probe";
        fs::create_directories(probe);
        require(xv::program_installer::canWriteDirectory(probe),
                "writable application directory was not detected");

        const fs::path cleanupWork = fs::temp_directory_path() /
            (std::wstring(L"XiaomiViewer-update-{cleanup-test-") +
             std::to_wstring(::GetTickCount64()) + L"}");
        fs::create_directories(cleanupWork);
        write(cleanupWork / "leftover.txt", "remove me");
        const HANDLE predecessor = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(predecessor != nullptr, "could not create cleanup predecessor handle");
        std::jthread release([predecessor] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ::SetEvent(predecessor);
        });
        const auto started = std::chrono::steady_clock::now();
        require(xv::program_installer::cleanup(cleanupWork, predecessor).empty(),
                "cleanup helper mode failed");
        require(!fs::exists(cleanupWork), "cleanup helper did not remove the work directory");
        require(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(40),
                "cleanup helper did not wait for the inherited predecessor handle");
        std::error_code cleanup;
        fs::remove_all(root, cleanup);
        std::cout << "Standalone updater installation, rollback, preservation and write-probe checks passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        std::error_code cleanup;
        fs::remove_all(root, cleanup);
        return 1;
    }
}
