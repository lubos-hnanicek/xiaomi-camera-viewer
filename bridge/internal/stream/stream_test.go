package stream

import (
	"testing"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/miss"
)

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
