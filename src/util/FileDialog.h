#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace xv {

std::optional<std::filesystem::path> chooseRecordingFile(
    HWND owner, const std::filesystem::path& initialDirectory, std::string& error);

} // namespace xv
