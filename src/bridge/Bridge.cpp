#include "bridge/Bridge.h"

#include <windows.h>

#include <array>
#include <cstring>

#include "app/Log.h"

namespace xv {
namespace {

// Most control responses are small; device lists and MIoT results are the only
// ones that routinely exceed this, and they get one resize.
constexpr int kInitialResponseCapacity = 8 * 1024;

// A 4K HEVC keyframe runs a few hundred KB. Starting here means the buffer
// almost never has to grow after the first keyframe.
constexpr size_t kInitialFrameCapacity = 1024 * 1024;

std::filesystem::path executableDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

Json parseOrError(const char* data, int length) {
    if (length <= 0) {
        return Json{{"ok", false}, {"error", "bridge: empty response"}};
    }
    Json parsed = Json::parse(std::string_view(data, static_cast<size_t>(length)), nullptr, false);
    if (parsed.is_discarded()) {
        return Json{{"ok", false}, {"error", "bridge: malformed response"}};
    }
    return parsed;
}

} // namespace

Bridge& Bridge::instance() {
    static Bridge bridge;
    return bridge;
}

Bridge::~Bridge() {
    // Deliberately not calling FreeLibrary: the Go runtime inside the bridge
    // does not support being unloaded, and the process is exiting anyway.
}

bool Bridge::load(std::string& error) {
    if (module_ != nullptr) {
        return true;
    }

    const std::filesystem::path dll = executableDirectory() / L"xmbridge.dll";

    HMODULE module = ::LoadLibraryW(dll.c_str());
    if (module == nullptr) {
        error = std::format("Could not load {} (error {}). Build the bridge with scripts/build-bridge.ps1.",
                            dll.string(), ::GetLastError());
        return false;
    }

    const auto resolve = [&](const char* name) -> FARPROC {
        FARPROC proc = ::GetProcAddress(module, name);
        if (proc == nullptr) {
            error = std::format("{} does not export {}. It is probably from an older build.",
                                dll.filename().string(), name);
        }
        return proc;
    };

    auto version = resolve("xmb_version");
    auto call = resolve("xmb_call");
    auto streamOpen = resolve("xmb_stream_open");
    auto streamRead = resolve("xmb_stream_read");
    auto streamCommand = resolve("xmb_stream_command");
    auto streamClose = resolve("xmb_stream_close");

    if (!version || !call || !streamOpen || !streamRead || !streamCommand || !streamClose) {
        ::FreeLibrary(module);
        return false;
    }

    module_ = module;
    version_ = reinterpret_cast<FnVersion>(version);
    call_ = reinterpret_cast<FnCall>(call);
    streamOpen_ = reinterpret_cast<FnStreamOpen>(streamOpen);
    streamRead_ = reinterpret_cast<FnStreamRead>(streamRead);
    streamCommand_ = reinterpret_cast<FnStreamCommand>(streamCommand);
    streamClose_ = reinterpret_cast<FnStreamClose>(streamClose);

    XV_INFO("bridge loaded, version {}", this->version());
    return true;
}

std::string Bridge::version() const {
    if (version_ == nullptr) {
        return "unloaded";
    }
    const char* text = version_();
    return text != nullptr ? std::string(text) : std::string("unknown");
}

template <typename Invoke>
Json Bridge::invokeJson(Invoke&& invoke) {
    std::vector<char> buffer(kInitialResponseCapacity);

    int needed = invoke(buffer.data(), static_cast<int>(buffer.size()));
    if (needed < 0) {
        return Json{{"ok", false}, {"error", std::format("bridge: call failed with status {}", needed)}};
    }

    // The bridge reports the size it needed; if that overflowed our guess, the
    // buffer holds nothing yet and the call has to be repeated.
    if (needed > static_cast<int>(buffer.size())) {
        buffer.resize(static_cast<size_t>(needed));
        needed = invoke(buffer.data(), static_cast<int>(buffer.size()));
        if (needed < 0 || needed > static_cast<int>(buffer.size())) {
            return Json{{"ok", false}, {"error", "bridge: response size kept changing"}};
        }
    }

    return parseOrError(buffer.data(), needed);
}

Json Bridge::call(const std::string& method, const Json& request) {
    if (call_ == nullptr) {
        return Json{{"ok", false}, {"error", "bridge: not loaded"}};
    }

    const std::string payload = request.dump();

    Json response = invokeJson([&](char* out, int cap) {
        return call_(method.c_str(), payload.c_str(), out, cap);
    });

    if (!responseOk(response)) {
        XV_WARN("bridge call {} failed: {}", method, responseError(response));
    }
    return response;
}

Bridge::Stream Bridge::openStream(const Json& request, Json& info) {
    if (streamOpen_ == nullptr) {
        info = Json{{"ok", false}, {"error", "bridge: not loaded"}};
        return nullptr;
    }

    const std::string payload = request.dump();

    // This entry point returns the handle rather than a length, so the buffer is
    // sized generously up front instead of being retried. It is zero-filled and
    // JSON cannot contain a NUL, so the first zero byte marks the end.
    std::vector<char> buffer(kInitialResponseCapacity, '\0');
    Stream stream = streamOpen_(payload.c_str(), buffer.data(), static_cast<int>(buffer.size()));

    const size_t length = ::strnlen(buffer.data(), buffer.size());
    info = parseOrError(buffer.data(), static_cast<int>(length));

    return stream;
}

void Bridge::closeStream(Stream stream) {
    if (streamClose_ != nullptr && stream != nullptr) {
        streamClose_(stream);
    }
}

Json Bridge::streamCommand(Stream stream, const Json& request) {
    if (streamCommand_ == nullptr || stream == nullptr) {
        return Json{{"ok", false}, {"error", "bridge: stream is not open"}};
    }

    const std::string payload = request.dump();
    return invokeJson([&](char* out, int cap) {
        return streamCommand_(stream, payload.c_str(), out, cap);
    });
}

bool Bridge::readFrame(Stream stream, std::vector<uint8_t>& buffer, XmbFrame& meta) {
    if (streamRead_ == nullptr || stream == nullptr) {
        return false;
    }

    if (buffer.size() < kInitialFrameCapacity) {
        buffer.resize(kInitialFrameCapacity);
    }

    meta = XmbFrame{};
    int written = streamRead_(stream, buffer.data(), static_cast<int>(buffer.size()), &meta);

    if (written == XMB_ERR_BUFFER_TOO_SMALL) {
        // The frame that triggered this was dropped; grow so the next one fits.
        const size_t needed = static_cast<size_t>(meta.size) + 64 * 1024;
        XV_WARN("frame of {} bytes exceeded the read buffer, growing to {}", meta.size, needed);
        buffer.resize(needed);
        // meta.size still holds the size that was needed, not the size written.
        // Clearing it keeps the caller from decoding an uninitialised buffer.
        meta.size = 0;
        return true;
    }

    if (written < 0) {
        return false;
    }

    meta.size = static_cast<uint32_t>(written);
    return true;
}

bool responseOk(const Json& response) {
    return response.is_object() && response.value("ok", false);
}

std::string responseError(const Json& response) {
    if (!response.is_object()) {
        return "bridge: unexpected response";
    }
    return response.value("error", std::string("unknown error"));
}

} // namespace xv
