#include "app/CameraDiagnostics.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

namespace {

bool finish(xv::CameraDiagnostics& diagnostics) {
    const HANDLE interval = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (diagnostics.busy() && std::chrono::steady_clock::now() < deadline) {
        diagnostics.poll();
        ::WaitForSingleObject(interval, 20);
    }
    ::CloseHandle(interval);
    return !diagnostics.busy();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "Expected collector script path\n";
        return 1;
    }
    xv::CameraDiagnostics diagnostics;
    xv::CameraDiagnostics::Options options;
    options.script = std::filesystem::absolute(argv[1]);
    const auto tempDirectory = std::filesystem::temp_directory_path() / L"XiaomiViewer long diagnostics test directory";
    std::filesystem::create_directories(tempDirectory);
    std::wstring shortDirectory(32768, L'\0');
    const DWORD length = ::GetShortPathNameW(tempDirectory.c_str(), shortDirectory.data(),
                                            static_cast<DWORD>(shortDirectory.size()));
    if (length == 0 || length >= shortDirectory.size()) {
        std::cerr << "Could not resolve the short temporary directory path\n";
        return 1;
    }
    shortDirectory.resize(length);
    options.outputDirectory = std::filesystem::path(shortDirectory) / L"XiaomiViewer diagnostics \u017E tests";
    options.selfTest = true;
    options.includeStorage = true;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!diagnostics.start(options) || !diagnostics.busy() || !finish(diagnostics)) {
            std::cerr << "Collector launch/poll failed: " << diagnostics.status().error << '\n';
            return 1;
        }
        if (diagnostics.status().state != xv::CameraDiagnostics::State::Complete ||
            !std::filesystem::is_regular_file(diagnostics.status().archive) ||
            !std::filesystem::is_regular_file(diagnostics.status().directory / "owner-report.txt")) {
            std::cerr << "Collector result contract failed: " << diagnostics.status().error << '\n';
            std::filesystem::path latestResult;
            for (const auto& run : std::filesystem::directory_iterator(options.outputDirectory)) {
                const auto result = run.path() / "app-result.json";
                if (std::filesystem::is_regular_file(result) &&
                    (latestResult.empty() || std::filesystem::last_write_time(result) >
                                             std::filesystem::last_write_time(latestResult))) {
                    latestResult = result;
                }
            }
            if (!latestResult.empty()) {
                std::ifstream result(latestResult, std::ios::binary);
                std::cerr << "Collector status: " << result.rdbuf() << '\n';
            }
            return 1;
        }
        std::ifstream input(diagnostics.status().directory / "communication.json");
        const auto events = nlohmann::json::parse(input, nullptr, false);
        if (!events.is_array() || events.size() != 6 ||
            events[4]["request"]["channel"] != 10 || events[4]["success"] != true) {
            std::cerr << "Storage channel 10 was not collected successfully\n";
            return 1;
        }
    }
    if (!diagnostics.start(options)) {
        return 1;
    }
    diagnostics.cancel();
    if (!finish(diagnostics) || diagnostics.status().state != xv::CameraDiagnostics::State::Cancelled) {
        std::cerr << "Cooperative cancellation failed\n";
        return 1;
    }
    options.script = options.outputDirectory / L"missing-collector.ps1";
    if (diagnostics.start(options) || diagnostics.busy() ||
        diagnostics.status().state != xv::CameraDiagnostics::State::Failed) {
        std::cerr << "Missing collector was not reported\n";
        return 1;
    }
    std::cout << "Collector launch, Unicode/space paths, archive result, cancellation and missing-script checks passed\n";
    return 0;
}