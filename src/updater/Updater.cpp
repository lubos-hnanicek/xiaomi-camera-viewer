#include <windows.h>

#include <shellapi.h>

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "updater/ProgramInstaller.h"

namespace {

std::unordered_map<std::wstring, std::wstring> parseArguments(int argc, wchar_t** argv) {
    std::unordered_map<std::wstring, std::wstring> arguments;
    for (int index = 1; index < argc; ++index) {
        std::wstring name = argv[index];
        if (!name.starts_with(L"--") || index + 1 >= argc) {
            throw std::runtime_error("Invalid updater command line.");
        }
        arguments.emplace(std::move(name), argv[++index]);
    }
    return arguments;
}

const std::wstring& required(const std::unordered_map<std::wstring, std::wstring>& arguments,
                             const std::wstring& name) {
    const auto match = arguments.find(name);
    if (match == arguments.end() || match->second.empty()) {
        throw std::runtime_error("Missing updater argument.");
    }
    return match->second;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR commandLine, _In_ int) {
    UNREFERENCED_PARAMETER(commandLine);
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 1;
    }
    std::string error;
    try {
        const auto arguments = parseArguments(argc, argv);
        if (arguments.size() != 4) {
            throw std::runtime_error("The updater command line was incomplete.");
        }
        xv::program_installer::Options options;
        options.targetDirectory = required(arguments, L"--target");
        options.stageDirectory = required(arguments, L"--stage");
        options.workDirectory = required(arguments, L"--work");
        options.parentProcessId =
            std::stoul(required(arguments, L"--parent"), nullptr, 10);
        error = xv::program_installer::install(options);
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    ::LocalFree(argv);
    if (!error.empty()) {
        ::MessageBoxA(nullptr, error.c_str(), "Xiaomi Camera Viewer update failed",
                      MB_ICONERROR | MB_OK);
        return 1;
    }
    return 0;
}
