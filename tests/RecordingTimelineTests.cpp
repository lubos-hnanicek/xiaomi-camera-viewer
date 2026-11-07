#include <cassert>

#include "media/MatroskaMuxer.h"
#include "media/RecordingPlayer.h"

int main() {
    const auto start = std::chrono::steady_clock::time_point{};
    assert(xv::recordingElapsedMs(start, start) == 0);
    assert(xv::recordingElapsedMs(start, start + std::chrono::milliseconds(67)) == 67);
    assert(xv::recordingElapsedMs(start, start + std::chrono::milliseconds(134)) == 134);

    xv::RecordingTimeline healthy;
    assert(healthy.mapVideo(10'000, 0) == 0);
    assert(healthy.mapVideo(10'067, 70) == 67);
    assert(healthy.mapAudio(10'034, 36) == 34);

    xv::RecordingTimeline underRunning;
    assert(underRunning.mapVideo(20'000, 0) == 0);
    assert(underRunning.mapVideo(20'001, 67) == 67);
    assert(underRunning.mapVideo(20'002, 134) == 134);
    assert(underRunning.mapAudio(20'003, 150) == 150);

    assert(xv::monotonicRecordingPts(100, 99) == 100);
    assert(xv::monotonicRecordingPts(99, 100) == 101);
    assert(xv::monotonicRecordingPts(100, 100) == 101);

    assert(xv::clampPlaybackPosition(-1, 10'000) == 0);
    assert(xv::clampPlaybackPosition(4'500, 10'000) == 4'500);
    assert(xv::clampPlaybackPosition(12'000, 10'000) == 10'000);

    return 0;
}
