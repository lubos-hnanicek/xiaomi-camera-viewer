package cs2

import (
	"encoding/binary"
	"testing"
)

func TestMarshalCmd(t *testing.T) {
	payload := []byte(`{"videoquality":2}`)
	req := marshalCmd(0, 7, cmdForTest, payload)

	if got, want := len(req), 16+len(payload); got != want {
		t.Fatalf("length = %d, want %d", got, want)
	}
	if req[0] != magic || req[1] != msgDrw {
		t.Errorf("message header = %x %x, want %x %x", req[0], req[1], magic, msgDrw)
	}
	if got, want := binary.BigEndian.Uint16(req[2:]), uint16(12+len(payload)); got != want {
		t.Errorf("size field = %d, want %d", got, want)
	}
	if req[4] != magicDrw || req[5] != 0 {
		t.Errorf("data header = %x channel %d, want %x channel 0", req[4], req[5], magicDrw)
	}
	if got := binary.BigEndian.Uint16(req[6:]); got != 7 {
		t.Errorf("sequence = %d, want 7", got)
	}
	if got, want := binary.BigEndian.Uint32(req[8:]), uint32(4+len(payload)); got != want {
		t.Errorf("payload size = %d, want %d", got, want)
	}
	if got := binary.BigEndian.Uint32(req[12:]); got != cmdForTest {
		t.Errorf("command = %d, want %d", got, cmdForTest)
	}
	if string(req[16:]) != string(payload) {
		t.Errorf("payload = %q, want %q", req[16:], payload)
	}
}

const cmdForTest = 0x102

// A command id must read back the way it was written. Upstream writes this field
// big-endian and reads it little-endian, which turns the 0x1001 envelope every
// reply arrives in into 0x01100000 and makes every reply unrecognisable.
func TestReadCommandMatchesMarshalEndianness(t *testing.T) {
	payload := []byte(`{"ret":0}`)
	req := marshalCmd(0, 0, cmdForTest, payload)

	// The command id and payload as a data channel hands them over, which is
	// everything after the 12-byte transport and length headers.
	c := &Conn{channels: [4]*dataChannel{newDataChannel(0, 1), nil, nil, nil}}
	c.channels[0].popBuf <- req[12:]

	cmd, data, err := c.ReadCommand()
	if err != nil {
		t.Fatalf("ReadCommand returned %v", err)
	}
	if cmd != cmdForTest {
		t.Errorf("command = %#x, want %#x", cmd, cmdForTest)
	}
	if string(data) != string(payload) {
		t.Errorf("payload = %q, want %q", data, payload)
	}
}

// msgClose and msgCloseAck are distinct values upstream; conflating them makes
// the worker treat a close as an unknown message.
func TestCloseConstantsAreDistinct(t *testing.T) {
	if msgClose == msgCloseAck {
		t.Fatalf("msgClose and msgCloseAck are both %#x", msgClose)
	}
	if msgClose != 0xF0 || msgCloseAck != 0xF1 {
		t.Errorf("close constants = %#x/%#x, want 0xF0/0xF1", msgClose, msgCloseAck)
	}
}
