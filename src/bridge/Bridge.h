#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "xmbridge.h"

namespace xv {

using Json = nlohmann::json;

// Bridge is the process-wide handle on xmbridge.dll.
//
// The DLL is resolved at runtime rather than linked, so a missing or mismatched
// bridge produces a clear message in the UI instead of a loader failure box
// before main() ever runs.
class Bridge {
public:
    using Stream = void*;

    static Bridge& instance();

    // Loads the DLL from next to the executable. Returns false and fills
    // `error` if it is missing or does not export what we expect.
    bool load(std::string& error);
    [[nodiscard]] bool loaded() const { return module_ != nullptr; }

    [[nodiscard]] std::string version() const;

    // Control plane. Always returns a decoded object; transport problems are
    // reported the same way protocol problems are, as {"ok":false,"error":...}.
    Json call(const std::string& method, const Json& request);

    // Media plane.
    Stream openStream(const Json& request, Json& info);
    void closeStream(Stream stream);
    Json streamCommand(Stream stream, const Json& request);

    // Reads the next access unit into `buffer`, growing it if the frame does not
    // fit. Returns false at end of stream.
    bool readFrame(Stream stream, std::vector<uint8_t>& buffer, XmbFrame& meta);

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

private:
    Bridge() = default;
    ~Bridge();

    // Signatures mirror include/xmbridge.h.
    using FnVersion = const char* (*)();
    using FnCall = int (*)(const char*, const char*, char*, int);
    using FnStreamOpen = void* (*)(const char*, char*, int);
    using FnStreamRead = int (*)(void*, unsigned char*, int, XmbFrame*);
    using FnStreamCommand = int (*)(void*, const char*, char*, int);
    using FnStreamClose = void (*)(void*);

    // Invokes a (out, cap) style entry point, sizing the buffer from its first
    // reply if the initial guess was too small.
    template <typename Invoke>
    Json invokeJson(Invoke&& invoke);

    void* module_ = nullptr;

    FnVersion version_ = nullptr;
    FnCall call_ = nullptr;
    FnStreamOpen streamOpen_ = nullptr;
    FnStreamRead streamRead_ = nullptr;
    FnStreamCommand streamCommand_ = nullptr;
    FnStreamClose streamClose_ = nullptr;
};

// Convenience predicates for the {"ok":...} envelope every response uses.
bool responseOk(const Json& response);
std::string responseError(const Json& response);

} // namespace xv
