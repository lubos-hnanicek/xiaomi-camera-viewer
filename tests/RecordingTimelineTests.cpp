#include <cassert>

#include "media/GlobalRecorder.h"
#include "media/RecordingPlayer.h"

int main() {
    xv::RecordingTimeline timeline;
    assert(!timeline.anchored());
    assert(!timeline.map(1000).has_value());

    timeline.anchor(10'000, 250);
    assert(timeline.anchored());
    assert(timeline.map(10'000) == 250);
    assert(timeline.map(10'500) == 750);

    // A fresh camera epoch maps to the host time at which reconnection became
    // usable, retaining the real gap instead of splicing source timestamps.
    timeline.reset();
    assert(!timeline.map(10'600).has_value());
    timeline.anchor(40, 4'200);
    assert(timeline.map(70) == 4'230);

    // Logical views of one dual-lens camera can reconnect independently.
    xv::RecordingTimeline sibling;
    sibling.anchor(2'000, 300);
    timeline.reset();
    assert(!timeline.map(80).has_value());
    assert(sibling.map(2'250) == 550);

    assert(xv::monotonicRecordingPts(100, 99) == 100);
    assert(xv::monotonicRecordingPts(99, 100) == 101);
    assert(xv::monotonicRecordingPts(100, 100) == 101);

    assert(xv::clampPlaybackPosition(-1, 10'000) == 0);
    assert(xv::clampPlaybackPosition(4'500, 10'000) == 4'500);
    assert(xv::clampPlaybackPosition(12'000, 10'000) == 10'000);

    return 0;
}
