#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace xv::program_installer {

struct Options {
    std::filesystem::path targetDirectory;
    std::filesystem::path stageDirectory;
    std::filesystem::path workDirectory;
    std::filesystem::path updaterPath;
    DWORD parentProcessId = 0;
    bool waitForParent = true;
    bool restart = true;
    bool cleanUp = true;
};

[[nodiscard]] bool canWriteDirectory(const std::filesystem::path& directory);
[[nodiscard]] std::string install(const Options& options);

} // namespace xv::program_installer
