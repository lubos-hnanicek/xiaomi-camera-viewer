#include "app/ProgramUpdate.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void require(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

} // namespace

int wmain() {
    try {
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
