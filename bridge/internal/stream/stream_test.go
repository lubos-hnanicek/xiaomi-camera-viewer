package stream

import (
	"testing"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/miss"
)

// Flags words as a real CW500 sends them, measured with one lens streaming at a
// time so the sender of each was known. The pair per lens is the same picture
// at its two encodings: 2560x1440 and the 640x360 substream.
// See scripts/probe-lens-id.ps1.
const (
	capturedPrimaryFlags      uint32 = 0x000EA000
	capturedPrimaryFlagsSub   uint32 = 0x0006A000
	capturedSecondaryFlags    uint32 = 0x014EA000
	capturedSecondaryFlagsSub uint32 = 0x0146A000
)

func lensTag(flags uint32) uint32 {
	packet := miss.Packet{Flags: flags}
	return packet.LensTag()
}

var (
	capturedPrimaryTag   = lensTag(capturedPrimaryFlags)
	capturedSecondaryTag = lensTag(capturedSecondaryFlags)
)

func TestSharedRouterSeparatesCapturedCW500Lenses(t *testing.T) {
	physical := &sharedPhysical{
		initial:    0,
		firstVideo: make(chan struct{}),
	}

	// The first packet arrived while only lens 1 was requested, which is what
	// anchors its tag to lane 0.
	if lane := physical.laneForLocked(capturedPrimaryTag); lane != 0 {
		t.Fatalf("initial packet routed to lane %d, want 0", lane)
	}
	physical.dual = true

	captured := []struct {
		tag      uint32
		wantLane int
	}{
		{capturedSecondaryTag, 1},
		{capturedPrimaryTag, 0},
		{capturedSecondaryTag, 1},
		{capturedPrimaryTag, 0},
		{capturedPrimaryTag, 0}, // one lens can deliver twice before the other
		{capturedSecondaryTag, 1},
	}

	for _, packet := range captured {
		if lane := physical.laneForLocked(packet.tag); lane != packet.wantLane {
			t.Errorf("tag %#x routed to lane %d, want %d",
				packet.tag, lane, packet.wantLane)
		}
	}
}

// Opening the secondary lens first has to work the same way round, and it is
// the case the constants cannot help with: the anchor is whichever lens was
// asked for, not whichever tag happens to be the smaller number.
func TestSharedRouterAnchorsWhicheverLensOpenedFirst(t *testing.T) {
	physical := &sharedPhysical{
		initial:    1,
		firstVideo: make(chan struct{}),
	}

	if lane := physical.laneForLocked(capturedSecondaryTag); lane != 1 {
		t.Fatalf("initial packet routed to lane %d, want 1", lane)
	}
	physical.dual = true

	if lane := physical.laneForLocked(capturedPrimaryTag); lane != 0 {
		t.Errorf("the other lens routed to lane %d, want 0", lane)
	}
	if lane := physical.laneForLocked(capturedSecondaryTag); lane != 1 {
		t.Errorf("the anchored lens routed to lane %d, want 1", lane)
	}
}

// Sequence numbers used to decide this, and two lenses whose randomly seeded
// counters run close together were exactly what that got wrong. The tag does
// not care how far apart the counters are.
func TestSharedRouterIgnoresSequenceNumbers(t *testing.T) {
	physical := &sharedPhysical{
		initial:    0,
		firstVideo: make(chan struct{}),
	}

	physical.laneForLocked(capturedPrimaryTag)
	physical.dual = true

	for i := 0; i < 500; i++ {
		if lane := physical.laneForLocked(capturedPrimaryTag); lane != 0 {
			t.Fatalf("primary packet %d routed to lane %d, want 0", i, lane)
		}
		if lane := physical.laneForLocked(capturedSecondaryTag); lane != 1 {
			t.Fatalf("secondary packet %d routed to lane %d, want 1", i, lane)
		}
	}
}

// The anchor must survive the upgrade. Sending the combined command is a
// restart of the media flow, and the old router read the first packet after it
// as evidence about which counter was which; this one has already decided.
func TestSharedRouterKeepsItsAnchorAcrossTheUpgrade(t *testing.T) {
	physical := &sharedPhysical{
		initial:    0,
		firstVideo: make(chan struct{}),
	}

	physical.laneForLocked(capturedPrimaryTag)
	if !physical.initialTagKnown || physical.initialLensTag != capturedPrimaryTag {
		t.Fatalf("anchor is %#x (known=%v), want %#x",
			physical.initialLensTag, physical.initialTagKnown, capturedPrimaryTag)
	}

	physical.dual = true
	// The secondary lens is the first to be heard from after the upgrade.
	if lane := physical.laneForLocked(capturedSecondaryTag); lane != 1 {
		t.Errorf("secondary lens routed to lane %d, want 1", lane)
	}
	if physical.initialLensTag != capturedPrimaryTag {
		t.Errorf("anchor moved to %#x", physical.initialLensTag)
	}
	if lane := physical.laneForLocked(capturedPrimaryTag); lane != 0 {
		t.Errorf("primary lens routed to lane %d, want 0", lane)
	}
}

// Anchoring is what the attach handshake waits for, so it has to happen on the
// first packet and not on the first one that happens to be from lane 0.
func TestSharedRouterSignalsFirstVideoOnTheFirstPacket(t *testing.T) {
	physical := &sharedPhysical{
		initial:    1,
		firstVideo: make(chan struct{}),
	}

	physical.laneForLocked(capturedSecondaryTag)
	select {
	case <-physical.firstVideo:
	default:
		t.Error("the first packet did not release a waiting dual-lens upgrade")
	}
}

// Overriding one tile's quality re-sends the video-start command, and the
// camera then encodes that lens differently. The lens has not changed, so its
// tile must not either. Comparing the whole flags word instead of the lens bits
// would send every packet of the re-encoded lens to the other tile.
func TestSharedRouterSurvivesAQualityChange(t *testing.T) {
	physical := &sharedPhysical{
		initial:    0,
		firstVideo: make(chan struct{}),
	}

	physical.laneForLocked(lensTag(capturedPrimaryFlags))
	physical.dual = true

	if lane := physical.laneForLocked(lensTag(capturedSecondaryFlags)); lane != 1 {
		t.Fatalf("secondary lens routed to lane %d, want 1", lane)
	}

	// The primary tile drops to the substream, the secondary stays as it was.
	if lane := physical.laneForLocked(lensTag(capturedPrimaryFlagsSub)); lane != 0 {
		t.Errorf("the re-encoded primary lens routed to lane %d, want 0", lane)
	}
	if lane := physical.laneForLocked(lensTag(capturedSecondaryFlags)); lane != 1 {
		t.Errorf("secondary lens routed to lane %d, want 1", lane)
	}
	// And the other way round, with the secondary on the substream.
	if lane := physical.laneForLocked(lensTag(capturedSecondaryFlagsSub)); lane != 1 {
		t.Errorf("the re-encoded secondary lens routed to lane %d, want 1", lane)
	}
}

// The tag has to survive everything a lens does to its own picture and still
// separate it from the other lens, which is the whole reason it is masked.
func TestLensTagIdentifiesTheLensAndNothingElse(t *testing.T) {
	if capturedPrimaryTag == capturedSecondaryTag {
		t.Fatalf("both lenses produced tag %#x", capturedPrimaryTag)
	}

	if got := lensTag(capturedPrimaryFlagsSub); got != capturedPrimaryTag {
		t.Errorf("the primary lens on the substream tagged %#x, want %#x",
			got, capturedPrimaryTag)
	}
	if got := lensTag(capturedSecondaryFlagsSub); got != capturedSecondaryTag {
		t.Errorf("the secondary lens on the substream tagged %#x, want %#x",
			got, capturedSecondaryTag)
	}

	// The low half carries the keyframe bit and the audio sample rate, both of
	// which change within one lens's stream.
	if got := lensTag(capturedPrimaryFlags | 1); got != capturedPrimaryTag {
		t.Errorf("a keyframe tagged %#x, want %#x", got, capturedPrimaryTag)
	}
}

func TestLensLane(t *testing.T) {
	for _, channel := range []string{"", "0"} {
		if lane := lensLane(channel); lane != 0 {
			t.Errorf("lensLane(%q) = %d, want 0", channel, lane)
		}
	}
	for _, channel := range []string{"1", "2", "secondary"} {
		if lane := lensLane(channel); lane != 1 {
			t.Errorf("lensLane(%q) = %d, want 1", channel, lane)
		}
	}
}

// The mapping is what makes the picture follow the button, and getting it
// backwards is not something a compiler can catch. Both axes are inverted
// against the camera's own coordinates: operation 1 increases the reported x
// while turning the lens left, and 4 increases y while tilting it down.
func TestMotorOperation(t *testing.T) {
	tests := []struct {
		direction string
		want      int
	}{
		{"right", miss.MotorPanMinus},
		{"left", miss.MotorPanPlus},
		{"up", miss.MotorTiltMinus},
		{"down", miss.MotorTiltPlus},
	}

	for _, tt := range tests {
		t.Run(tt.direction, func(t *testing.T) {
			got, err := motorOperation(tt.direction)
			if err != nil {
				t.Fatalf("motorOperation(%q) returned %v", tt.direction, err)
			}
			if got != tt.want {
				t.Errorf("motorOperation(%q) = %d, want %d", tt.direction, got, tt.want)
			}
		})
	}

	// Every direction must map somewhere different, or two buttons do the same
	// thing and the mistake is invisible in the tests above.
	seen := map[int]string{}
	for _, tt := range tests {
		if other, dup := seen[tt.want]; dup {
			t.Errorf("%q and %q both map to operation %d", tt.direction, other, tt.want)
		}
		seen[tt.want] = tt.direction
	}
}

func TestMotorOperationRejectsUnknown(t *testing.T) {
	if _, err := motorOperation("zoom_in"); err == nil {
		t.Error("motorOperation accepted a direction the camera has no motor for")
	}
}

// The codec ids on the wire and the ones the ABI uses are two different
// numberings, and an audio codec left out of the mapping is dropped silently
// rather than reported, which is how audio went unnoticed for so long.
func TestToFrameAudio(t *testing.T) {
	tests := []struct {
		name      string
		codecID   uint32
		flags     uint32
		wantCodec int32
		wantRate  int32
	}{
		{"A-law at 8 kHz", miss.CodecPCMA, 0, CodecPCMA, 8000},
		{"A-law at 16 kHz", miss.CodecPCMA, 1 << 3, CodecPCMA, 16000},
		{"mu-law", miss.CodecPCMU, 0, CodecPCMU, 8000},
		{"PCM", miss.CodecPCM, 0, CodecPCM, 8000},
		{"Opus at 16 kHz", miss.CodecOPUS, 1 << 3, CodecOpus, 16000},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			frame := toFrame(&miss.Packet{CodecID: tt.codecID, Flags: tt.flags, Payload: []byte{1}})
			if frame == nil {
				t.Fatal("the packet was dropped")
			}
			if frame.Kind != KindAudio {
				t.Errorf("kind = %d, want %d", frame.Kind, KindAudio)
			}
			if frame.Codec != tt.wantCodec {
				t.Errorf("codec = %d, want %d", frame.Codec, tt.wantCodec)
			}
			if frame.SampleRate != tt.wantRate {
				t.Errorf("sample rate = %d, want %d", frame.SampleRate, tt.wantRate)
			}
			if frame.Keyframe {
				t.Error("an audio packet was marked as a keyframe")
			}
		})
	}
}

func TestToFrameDropsUnknownCodec(t *testing.T) {
	if frame := toFrame(&miss.Packet{CodecID: 4242, Payload: []byte{1}}); frame != nil {
		t.Errorf("a codec nothing can decode was passed on as kind %d", frame.Kind)
	}
}

// A sample rate on a video frame would be read as one by anything that trusts
// the field, so the flags are only interpreted for audio.
func TestToFrameLeavesVideoWithoutASampleRate(t *testing.T) {
	frame := toFrame(&miss.Packet{
		CodecID: miss.CodecH265,
		Flags:   1 << 3,
		Payload: annexB([]byte{19 << 1, 0x01}),
	})
	if frame == nil {
		t.Fatal("the packet was dropped")
	}
	if frame.SampleRate != 0 {
		t.Errorf("sample rate = %d, want 0", frame.SampleRate)
	}
}

// annexB builds a byte stream of NAL units with 4-byte start codes.
func annexB(nals ...[]byte) []byte {
	var out []byte
	for _, nal := range nals {
		out = append(out, 0, 0, 0, 1)
		out = append(out, nal...)
	}
	return out
}

func TestIsKeyframeH265(t *testing.T) {
	tests := []struct {
		name string
		data []byte
		want bool
	}{
		{"IDR_W_RADL", annexB([]byte{19 << 1, 0x01, 0xAA}), true},
		{"CRA", annexB([]byte{21 << 1, 0x01, 0xAA}), true},
		{"VPS then SPS then PPS then IDR",
			annexB([]byte{32 << 1, 0x01}, []byte{33 << 1, 0x01}, []byte{34 << 1, 0x01},
				[]byte{19 << 1, 0x01}),
			true},
		{"TRAIL_R only", annexB([]byte{1 << 1, 0x01, 0xAA}), false},
		{"TRAIL_N only", annexB([]byte{0 << 1, 0x01, 0xAA}), false},
		{"prefix SEI then trailing", annexB([]byte{39 << 1, 0x01}, []byte{1 << 1, 0x01}), false},
		{"empty", nil, false},
		{"truncated start code", []byte{0, 0, 0}, false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := isKeyframeH265(tt.data); got != tt.want {
				t.Errorf("isKeyframeH265 = %v, want %v", got, tt.want)
			}
		})
	}
}

func TestIsKeyframeH264(t *testing.T) {
	tests := []struct {
		name string
		data []byte
		want bool
	}{
		{"IDR", annexB([]byte{0x65, 0xAA}), true},
		{"SPS then PPS then IDR", annexB([]byte{0x67}, []byte{0x68}, []byte{0x65}), true},
		{"non-IDR slice", annexB([]byte{0x41, 0xAA}), false},
		{"SEI only", annexB([]byte{0x06, 0xAA}), false},
		{"empty", nil, false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := isKeyframeH264(tt.data); got != tt.want {
				t.Errorf("isKeyframeH264 = %v, want %v", got, tt.want)
			}
		})
	}
}

// Cameras use both 3- and 4-byte start codes, sometimes within one access unit.
func TestScanAnnexBHandlesThreeByteStartCodes(t *testing.T) {
	data := []byte{
		0, 0, 1, 0x67, 0x11, // 3-byte start code, SPS
		0, 0, 0, 1, 0x41, // 4-byte start code, non-IDR
	}
	if !isKeyframeH264(data) {
		t.Error("expected the SPS behind a 3-byte start code to be found")
	}
}

func TestScanAnnexBIgnoresEmulationPreventionPattern(t *testing.T) {
	// 00 00 03 is an emulation prevention sequence, not a start code, so the
	// byte after it must not be read as a NAL header.
	data := []byte{0, 0, 0, 1, 0x41, 0x00, 0x00, 0x03, 0x65}
	if isKeyframeH264(data) {
		t.Error("emulation prevention bytes were mistaken for a start code")
	}
}
