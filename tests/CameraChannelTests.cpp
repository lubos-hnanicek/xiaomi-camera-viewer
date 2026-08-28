// Which storage channel a lens records under.
//
// This is a number recovered from firmware and confirmed against hardware, not
// one that can be reasoned out from the live channel: a CW500's second lens
// streams as channel 1 and records as channel 10. Getting it wrong is silent --
// a channel the camera does not record to has an empty index, so the card reads
// as empty rather than as misaddressed -- which is exactly how this went
// unnoticed. Hence a test that pins the constant.

#include <cassert>

#include "config/Config.h"

int main() {
    using xv::isDualLens;
    using xv::sdRecordingChannel;

    // Single-lens models have one channel whatever the tile says.
    assert(sdRecordingChannel("isa.camera.hlc8a", "") == 0);
    assert(sdRecordingChannel("isa.camera.hlc8a", "0") == 0);
    assert(sdRecordingChannel("isa.camera.hlc8a", "1") == 0);
    assert(sdRecordingChannel("mxiang.camera.moc001", "1") == 0);

    // The CW500 records its two lenses as 0 and 10. Channel 1 is a slot the
    // firmware never allocates.
    assert(isDualLens("isa.camera.500dh"));
    assert(sdRecordingChannel("isa.camera.500dh", "") == 0);
    assert(sdRecordingChannel("isa.camera.500dh", "0") == 0);
    assert(sdRecordingChannel("isa.camera.500dh", "1") == 10);

    // The other spellings of the same board have to agree, or one config would
    // browse an empty catalogue while another worked.
    assert(sdRecordingChannel("chuangmi.camera.cw500", "1") == 10);
    assert(sdRecordingChannel("isa.camera.hlmax", "1") == 10);

    // Only the motor belongs to one lens; both pictures reach the card.
    xv::CameraConfig second;
    second.model = "isa.camera.500dh";
    second.channel = "1";
    assert(!second.motorised());
    assert(sdRecordingChannel(second.model, second.channel) == 10);

    return 0;
}
