#pragma once

#include <memory>
#include <string>

struct AVFrame;

namespace xv {

// AudioPlayer is the one speaker the app has.
//
// There is a single instance because only one camera is ever audible: four
// cameras playing at once is noise, not information. Switching cameras means
// pointing this at a different stream worker rather than starting a second
// output, which also keeps the Windows volume mixer showing one entry for the
// app instead of one per tile.
//
// The device lives on its own thread. Submitting is non-blocking and safe from
// any thread: samples go into a short jitter buffer and the thread hands them
// to Windows on its own schedule. A camera that stalls plays silence, and a
// camera that runs ahead loses its oldest samples rather than growing the
// delay, because on a live view being late is worse than skipping.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Starts the output thread, which opens the default device and keeps trying
    // if there is none yet. Returns false only if the thread cannot be started.
    bool start(std::string& error);
    void stop();

    // Throws away everything buffered. Called when the audible camera changes,
    // so the new one is not heard behind the tail of the old one.
    void reset();

    // Hands over decoded samples in whatever format the decoder produced.
    // Resampling to the device format happens here, on the caller's thread, so
    // the audio thread only ever copies.
    void submit(const AVFrame* frame);

    // Whether a device is open and taking samples right now.
    [[nodiscard]] bool ready() const;

    // Why there is no sound, empty when there is nothing wrong.
    [[nodiscard]] std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xv
