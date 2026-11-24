#include "app/ProgramUpdate.h"

#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "util/Encoding.h"

namespace {

void require(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

} // namespace

int wmain() {
    try {
        const auto command = xv::encoding::createProcessCommand(
            LR"(C:\Program Files\XiaomiViewerUpdater.exe)",
            L"--target \"C:\\Program Files\\Xiaomi Viewer\" --stage \"C:\\Temp\\stage\"");
        int argc = 0;
        wchar_t** argv = ::CommandLineToArgvW(command.c_str(), &argc);
        require(argv != nullptr, "could not parse the updater CreateProcess command line");
        require(argc == 5, "CreateProcess command line lost or added updater arguments");
        require(std::wstring(argv[0]) == LR"(C:\Program Files\XiaomiViewerUpdater.exe)",
                "CreateProcess argv[0] is not the updater executable");
        require(std::wstring(argv[1]) == L"--target",
                "the first updater option is not argv[1]");
        ::LocalFree(argv);

        xv::ProgramUpdate update;
        update.check();
        if (!update.busy()) {
            throw std::runtime_error(
                "the packaged update helper did not start: " + update.status().error);
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (update.busy() && std::chrono::steady_clock::now() < deadline) {
            update.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(!update.busy(), "the packaged update helper did not finish");
        if (update.status().state != xv::ProgramUpdate::State::UpToDate) {
            throw std::runtime_error("native update check failed: " + update.status().error);
        }
        std::cout << "Native ProgramUpdate launch, status polling and GitHub check passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
