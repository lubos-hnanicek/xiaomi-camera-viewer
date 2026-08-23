#include "media/Recorder.h"

#include <format>

#include "app/Log.h"
#include "media/AudioFormat.h"

namespace xv {

Recorder::~Recorder() {
    close();
}

bool Recorder::open(const std::filesystem::path& path, int codec, int width, int height,
                    const uint8_t* keyframe, size_t size, const AudioTrack& audio,
                    std::string& error) {
    close();

    MatroskaVideoTrack video;
    video.codec = codec;
    video.width = width;
    video.height = height;
    video.extradata = videoParameterSets(keyframe, size, codec);
    video.defaultTrack = true;

    std::vector<MatroskaAudioTrack> audioTracks;
    if (audio.valid() && audio::avCodecId(audio.codec) != 0) {
        audioTracks.push_back(MatroskaAudioTrack{audio, {}, true});
    } else if (audio.valid()) {
        XV_WARN("recording without audio: the camera's {} codec cannot be stored in Matroska",
                audio::codecName(audio.codec));
    }

    if (!muxer_.open(path, {video}, audioTracks, error)) {
        return false;
    }

    hasAudio_ = !audioTracks.empty();
    firstPts_ = -1;
    lastPts_ = -1;
    lastAudioPts_ = -1;

    XV_INFO("single-camera recording is {}x{}, {}", width, height,
            hasAudio_ ? audio::codecName(audio.codec) : "no audio");
    return true;
}

bool Recorder::writeVideo(const uint8_t* data, size_t size, int64_t ptsMs, bool keyframe,
                          std::string& error) {
    if (!muxer_.open()) {
        return false;
    }

    // The first video frame is what the whole file is timed from, audio
    // included, so the two tracks keep the offset the camera sent them with.
    if (firstPts_ < 0) {
        firstPts_ = ptsMs;
    }

    int64_t pts = ptsMs - firstPts_;

    // Matroska will not take a timestamp that goes backwards, and a camera whose
    // clock steps or wraps would otherwise end the recording. Nudging keeps the
    // file going; the frame lands a millisecond after its neighbour instead of
    // being lost.
    if (pts <= lastPts_) {
        pts = lastPts_ + 1;
    }

    if (!muxer_.writeVideo(0, data, size, pts, keyframe, error)) {
        return false;
    }

    lastPts_ = pts;
    return true;
}

bool Recorder::writeAudio(const uint8_t* data, size_t size, int64_t ptsMs, std::string& error) {
    if (!muxer_.open() || !hasAudio_) {
        return true; // a recording without an audio track, which is not a failure
    }

    // Audio that arrived before the opening keyframe belongs to a moment the
    // file does not cover, and would have to be written at a negative time.
    if (firstPts_ < 0) {
        return true;
    }

    int64_t pts = ptsMs - firstPts_;
    if (pts < 0) {
        return true;
    }
    if (pts <= lastAudioPts_) {
        pts = lastAudioPts_ + 1;
    }

    // Every audio packet is independently decodable, so all of them are
    // keyframes as far as the container is concerned.
    if (!muxer_.writeAudio(0, data, size, pts, error)) {
        return false;
    }

    lastAudioPts_ = pts;
    return true;
}

void Recorder::close() {
    muxer_.close();
    hasAudio_ = false;
    firstPts_ = -1;
    lastPts_ = -1;
    lastAudioPts_ = -1;
}

} // namespace xv
