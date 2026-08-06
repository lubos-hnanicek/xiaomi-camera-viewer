#include "media/AudioPlayer.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include "app/Log.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace xv {
namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

// How much audio the jitter buffer holds at most, and how much has to be in it
// before playback starts.
//
// The buffer absorbs the difference between a camera that sends audio in bursts
// over the network and a device that wants a few milliseconds every few
// milliseconds. Too short and every hiccup is audible; too long and the sound
// lags the picture. A fifth of a second is comfortably more than the jitter
// these cameras show and still close enough to the video to pass for live.
constexpr int kBufferMs = 200;
constexpr int kPrimeMs = 60;

// How long to wait before looking for a device again after failing to open one,
// so a machine with no sound card does not spin.
constexpr auto kRetryDelay = 2s;

// The subtype GUIDs for wave formats are all the same template with the format
// tag in the first field, so the tag can be recovered without pulling in the
// kernel streaming headers just for two constants.
WORD effectiveFormatTag(const WAVEFORMATEX* format) {
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
        return format->wFormatTag;
    }
    if (format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return 0;
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return static_cast<WORD>(extensible->SubFormat.Data1);
}

std::string hresult(HRESULT hr) {
    return std::format("0x{:08X}", static_cast<uint32_t>(hr));
}

} // namespace

struct AudioPlayer::Impl {
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::atomic<bool> ready{false};

    mutable std::mutex mutex;

    // The open device's format. Zero rate means there is no device, which is
    // what tells a submitter there is nowhere to put its samples.
    int rate = 0;
    int channels = 0;
    AVSampleFormat format = AV_SAMPLE_FMT_NONE;
    size_t frameBytes = 0;
    // Bumped every time a device is opened, so a resampler built for the
    // previous one is rebuilt rather than quietly writing the wrong format.
    uint64_t generation = 0;

    SwrContext* swr = nullptr;
    uint64_t swrGeneration = 0;
    int swrInRate = 0;
    int swrInFormat = AV_SAMPLE_FMT_NONE;
    int swrInChannels = 0;

    std::vector<uint8_t> ring;
    size_t head = 0;
    size_t used = 0;
    size_t primeBytes = 0;
    bool primed = false;

    std::vector<uint8_t> scratch;

    std::string error;

    void run();
    bool openDevice();

    // Both called with the mutex held.
    void push(const uint8_t* data, size_t bytes);
    size_t pop(uint8_t* out, size_t bytes);
    void discard();

    void releaseResampler();
    bool ensureResampler(const AVFrame* frame);

    void setError(std::string message) {
        std::scoped_lock lock(mutex);
        error = std::move(message);
    }
};

void AudioPlayer::Impl::push(const uint8_t* data, size_t bytes) {
    if (ring.empty() || bytes == 0) {
        return;
    }

    // More than the whole buffer in one go can only mean the newest part of it
    // is worth keeping.
    if (bytes >= ring.size()) {
        data += bytes - ring.size();
        bytes = ring.size();
    }

    if (const size_t free = ring.size() - used; bytes > free) {
        const size_t drop = bytes - free;
        head = (head + drop) % ring.size();
        used -= drop;
    }

    const size_t tail = (head + used) % ring.size();
    const size_t first = std::min(bytes, ring.size() - tail);
    std::memcpy(ring.data() + tail, data, first);
    if (bytes > first) {
        std::memcpy(ring.data(), data + first, bytes - first);
    }
    used += bytes;
}

size_t AudioPlayer::Impl::pop(uint8_t* out, size_t bytes) {
    if (ring.empty()) {
        return 0;
    }

    // Nothing is played until there is a cushion to play from, or the first
    // packets would be chopped up by the device asking faster than the network
    // delivers.
    if (!primed) {
        if (used < primeBytes) {
            return 0;
        }
        primed = true;
    }

    const size_t taken = std::min(bytes, used);
    const size_t first = std::min(taken, ring.size() - head);
    std::memcpy(out, ring.data() + head, first);
    if (taken > first) {
        std::memcpy(out + first, ring.data(), taken - first);
    }

    head = (head + taken) % ring.size();
    used -= taken;

    // Only a request that could not be met is an underrun worth rebuilding the
    // cushion for; an empty buffer that nobody asked more of is just in step.
    if (taken < bytes) {
        primed = false;
    }
    return taken;
}

void AudioPlayer::Impl::discard() {
    head = 0;
    used = 0;
    primed = false;
}

void AudioPlayer::Impl::releaseResampler() {
    if (swr != nullptr) {
        swr_free(&swr);
    }
    swrGeneration = 0;
    swrInRate = 0;
    swrInFormat = AV_SAMPLE_FMT_NONE;
    swrInChannels = 0;
}

bool AudioPlayer::Impl::ensureResampler(const AVFrame* frame) {
    const bool matches = swr != nullptr && swrGeneration == generation &&
                         swrInRate == frame->sample_rate && swrInFormat == frame->format &&
                         swrInChannels == frame->ch_layout.nb_channels;
    if (matches) {
        return true;
    }

    releaseResampler();

    AVChannelLayout out{};
    av_channel_layout_default(&out, channels);

    const int rc = swr_alloc_set_opts2(&swr, &out, format, rate, &frame->ch_layout,
                                       static_cast<AVSampleFormat>(frame->format),
                                       frame->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out);

    if (rc < 0 || swr_init(swr) < 0) {
        releaseResampler();
        return false;
    }

    swrGeneration = generation;
    swrInRate = frame->sample_rate;
    swrInFormat = frame->format;
    swrInChannels = frame->ch_layout.nb_channels;
    return true;
}

bool AudioPlayer::Impl::openDevice() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        setError("no audio device enumerator: " + hresult(hr));
        return false;
    }

    ComPtr<IMMDevice> endpoint;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
    if (FAILED(hr)) {
        setError("no audio output device");
        return false;
    }

    ComPtr<IAudioClient> client;
    hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) {
        setError("the audio device could not be opened: " + hresult(hr));
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr) {
        setError("the audio device did not describe its format: " + hresult(hr));
        return false;
    }

    // Shared mode means Windows is already mixing several applications at one
    // format, so matching that format is what avoids a second resampling behind
    // our back. Whatever it is, libswresample can produce it.
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_NONE;
    switch (effectiveFormatTag(mix)) {
    case WAVE_FORMAT_IEEE_FLOAT:
        sampleFormat = AV_SAMPLE_FMT_FLT;
        break;
    case WAVE_FORMAT_PCM:
        sampleFormat = mix->wBitsPerSample == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_NONE;
        break;
    default:
        break;
    }

    if (sampleFormat == AV_SAMPLE_FMT_NONE) {
        setError(std::format("the audio device wants a {}-bit format this build cannot produce",
                             mix->wBitsPerSample));
        ::CoTaskMemFree(mix);
        return false;
    }

    // A period of zero asks for the device's own, which is the lowest latency
    // shared mode offers and is measured in single-digit milliseconds.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix,
                            nullptr);
    if (FAILED(hr)) {
        setError("the audio device rejected the stream: " + hresult(hr));
        ::CoTaskMemFree(mix);
        return false;
    }

    const int deviceRate = static_cast<int>(mix->nSamplesPerSec);
    const int deviceChannels = mix->nChannels;
    const size_t deviceFrameBytes = mix->nBlockAlign;
    ::CoTaskMemFree(mix);
    mix = nullptr;

    HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake == nullptr) {
        setError("could not create the audio wake event");
        return false;
    }

    hr = client->SetEventHandle(wake);
    UINT32 bufferFrames = 0;
    ComPtr<IAudioRenderClient> render;
    if (SUCCEEDED(hr)) {
        hr = client->GetBufferSize(&bufferFrames);
    }
    if (SUCCEEDED(hr)) {
        hr = client->GetService(IID_PPV_ARGS(&render));
    }
    if (FAILED(hr)) {
        setError("the audio device could not be started: " + hresult(hr));
        ::CloseHandle(wake);
        return false;
    }

    {
        std::scoped_lock lock(mutex);
        rate = deviceRate;
        channels = deviceChannels;
        format = sampleFormat;
        frameBytes = deviceFrameBytes;
        ++generation;

        ring.assign(deviceFrameBytes * static_cast<size_t>(deviceRate) * kBufferMs / 1000, 0);
        primeBytes = deviceFrameBytes * static_cast<size_t>(deviceRate) * kPrimeMs / 1000;
        discard();
        releaseResampler();
        error.clear();
    }

    XV_INFO("audio output ready at {} Hz, {} channel(s)", deviceRate, deviceChannels);

    hr = client->Start();
    if (SUCCEEDED(hr)) {
        ready.store(true, std::memory_order_release);
    }

    while (SUCCEEDED(hr) && !stopping.load(std::memory_order_acquire)) {
        // Two seconds is far longer than any period, so reaching it means the
        // device has stopped asking and the session is over.
        if (::WaitForSingleObject(wake, 2000) != WAIT_OBJECT_0) {
            hr = AUDCLNT_E_DEVICE_INVALIDATED;
            break;
        }

        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            break;
        }

        const UINT32 wanted = bufferFrames - padding;
        if (wanted == 0) {
            continue;
        }

        BYTE* buffer = nullptr;
        hr = render->GetBuffer(wanted, &buffer);
        if (FAILED(hr)) {
            break;
        }

        const size_t bytes = static_cast<size_t>(wanted) * deviceFrameBytes;
        size_t filled = 0;
        {
            std::scoped_lock lock(mutex);
            filled = pop(buffer, bytes);
        }

        DWORD flags = 0;
        if (filled == 0) {
            flags = AUDCLNT_BUFFERFLAGS_SILENT;
        } else if (filled < bytes) {
            std::memset(buffer + filled, 0, bytes - filled);
        }
        render->ReleaseBuffer(wanted, flags);
    }

    ready.store(false, std::memory_order_release);
    client->Stop();
    ::CloseHandle(wake);

    {
        std::scoped_lock lock(mutex);
        rate = 0;
        channels = 0;
        format = AV_SAMPLE_FMT_NONE;
        frameBytes = 0;
        ring.clear();
        discard();
        releaseResampler();
    }

    if (FAILED(hr) && !stopping.load(std::memory_order_acquire)) {
        // The default device changing, or headphones being unplugged, ends the
        // session rather than the app's ability to make sound, so the caller
        // simply opens the new default next time round.
        XV_INFO("the audio device went away ({}); looking for another", hresult(hr));
        return false;
    }
    return true;
}

void AudioPlayer::Impl::run() {
    // Multithreaded, unlike the UI thread's apartment: nothing here has a
    // message loop to pump, and the audio interfaces do not need one.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (!stopping.load(std::memory_order_acquire)) {
        if (openDevice()) {
            continue; // a clean stop, or a device that ended without failing
        }

        // Sleep in slices so shutting down does not wait out the whole delay.
        for (auto waited = 0ms; waited < kRetryDelay; waited += 100ms) {
            if (stopping.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(100ms);
        }
    }

    if (SUCCEEDED(com)) {
        ::CoUninitialize();
    }
}

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::start(std::string& error) {
    if (impl_->thread.joinable()) {
        return true;
    }

    impl_->stopping.store(false, std::memory_order_release);
    try {
        impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    } catch (const std::exception& e) {
        error = std::string("could not start the audio thread: ") + e.what();
        return false;
    }
    return true;
}

void AudioPlayer::stop() {
    impl_->stopping.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }

    std::scoped_lock lock(impl_->mutex);
    impl_->releaseResampler();
}

void AudioPlayer::reset() {
    std::scoped_lock lock(impl_->mutex);
    impl_->discard();
    // The resampler holds a tail of the previous camera's audio, which would
    // otherwise be the first thing heard from the next one.
    impl_->releaseResampler();
}

void AudioPlayer::submit(const AVFrame* frame) {
    if (frame == nullptr || frame->nb_samples <= 0) {
        return;
    }

    std::scoped_lock lock(impl_->mutex);
    if (impl_->rate == 0 || !impl_->ensureResampler(frame)) {
        return; // no device, or nothing that can feed it
    }

    const int capacity = swr_get_out_samples(impl_->swr, frame->nb_samples);
    if (capacity <= 0) {
        return;
    }

    impl_->scratch.resize(static_cast<size_t>(capacity) * impl_->frameBytes);
    uint8_t* out = impl_->scratch.data();

    const int converted =
        swr_convert(impl_->swr, &out, capacity,
                    const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (converted <= 0) {
        return;
    }

    impl_->push(impl_->scratch.data(), static_cast<size_t>(converted) * impl_->frameBytes);
}

bool AudioPlayer::ready() const {
    return impl_->ready.load(std::memory_order_acquire);
}

std::string AudioPlayer::error() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->error;
}

} // namespace xv
