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

// A message on another channel is framed exactly like a command bar the command
// id, which is what lets the speaker backchannel and the command channel share
// one marshaller. Getting the length field wrong would make the far end
// reassemble from the wrong offset for the rest of the session.
func TestMarshalDataFramesAnyChannel(t *testing.T) {
	payload := []byte("IOTC")
	req := marshalData(1, 3, nil, payload)

	if got, want := len(req), 12+len(payload); got != want {
		t.Fatalf("length = %d, want %d", got, want)
	}
	if req[5] != 1 {
		t.Errorf("channel = %d, want 1", req[5])
	}
	if got := binary.BigEndian.Uint16(req[6:]); got != 3 {
		t.Errorf("sequence = %d, want 3", got)
	}
	if got, want := binary.BigEndian.Uint32(req[8:]), uint32(len(payload)); got != want {
		t.Errorf("message size = %d, want %d", got, want)
	}
	if string(req[12:]) != string(payload) {
		t.Errorf("payload = %q, want %q", req[12:], payload)
	}
}

// Each channel numbers its messages independently, so one counter shared across
// all of them would make every channel but the first start mid-sequence.
func TestSequenceIsPerChannel(t *testing.T) {
	c := &Conn{}

	if got := c.nextSeq(0); got != 0 {
		t.Errorf("first sequence on channel 0 = %d, want 0", got)
	}
	if got := c.nextSeq(0); got != 1 {
		t.Errorf("second sequence on channel 0 = %d, want 1", got)
	}
	if got := c.nextSeq(1); got != 0 {
		t.Errorf("first sequence on channel 1 = %d, want 0", got)
	}
}

// The tap is the whole point of probing another channel: a message that arrives
// where nothing is listening has to be readable afterwards, verbatim, since the
// framing it uses is what the probe is trying to establish.
func TestTapKeepsMessagesAndDrains(t *testing.T) {
	c := &Conn{}
	c.tap(1, 5, []byte{0xDE, 0xAD})

	got := c.Tap()
	if len(got) != 1 {
		t.Fatalf("tapped %d messages, want 1", len(got))
	}
	if got[0].Channel != 1 || got[0].Seq != 5 {
		t.Errorf("tapped channel %d seq %d, want channel 1 seq 5", got[0].Channel, got[0].Seq)
	}
	if string(got[0].Data) != "\xde\xad" {
		t.Errorf("tapped data = %x, want dead", got[0].Data)
	}

	if rest := c.Tap(); len(rest) != 0 {
		t.Errorf("a second read returned %d messages, want none", len(rest))
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
