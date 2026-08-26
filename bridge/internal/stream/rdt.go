package stream

import (
	"encoding/binary"
	"fmt"
	"strings"
	"sync/atomic"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/miss"
	"github.com/spec8472/xiaomi-viewer/bridge/internal/recordings"
)

// rdtMessage is one whole reply from the file-transfer channel.
type rdtMessage struct {
	Cmd     uint32
	Payload []byte
}

// rdtStream rebuilds whole messages out of channel 1's byte stream.
//
// Nothing about a single transport message says where a reply begins or ends. A
// reply arrives as a run of chunks, each encrypted and length-prefixed on its
// own, and only the first carries the command and the total length; the rest are
// payload and nothing else. A recording index runs to a couple of hundred of
// them. So the channel has to be treated as a stream of bytes that happens to
// arrive in pieces, not as a sequence of messages.
type rdtStream struct {
	buf []byte

	cmd     uint32
	want    int
	payload []byte

	// collecting is set when command 1 declared a zero or tiny length. That is
	// an acknowledgement, not an empty file: the MP4 then arrives as more
	// decrypted chunks without a new header. Treating want==0 as complete
	// emitted the ack and then parsed the file as a header, which reset the
	// stream and left FetchRecording with only empties.
	collecting bool
	idleTicks  int

	onPlain func([]byte)

	// How many messages the transport had already thrown away last time this
	// looked, so that only new losses count.
	lost uint64
}

func (r *rdtStream) reset() {
	r.buf, r.cmd, r.want, r.payload = nil, 0, 0, nil
	r.collecting, r.idleTicks = false, 0
}

func (r *rdtStream) push(data []byte) {
	r.buf = append(r.buf, data...)
}

// maxRDTMessage is a corrupt-length guard, not a typical size. A recording
// index is ~170 kB; a minute of footage is a ~7 MB MP4, and that file
// can arrive as one CS2 frame. 1 MiB used to reset the stream on the first
// chunk and leave FetchRecording with only the empty trailer, which it then
// read as "not found".
const maxRDTMessage = 16 << 20

// fileIdleTicks is how many quiet pump intervals close a zero-length file
// transfer. The CW500 seeks between fMP4 fragments; a 500ms gap used to look
// like the end of the file and FetchRecording returned a truncated last mdat.
// 150 * 20ms = 3s is longer than those seeks and still short for the player.
const fileIdleTicks = 150

func plausibleFile(payload []byte) bool {
	return recordings.LooksLikeMP4(payload) || len(payload) > 4096
}

func (r *rdtStream) emitCollected() rdtMessage {
	body := append([]byte(nil), r.payload...)
	r.cmd, r.want, r.payload = 0, 0, nil
	r.collecting, r.idleTicks = false, 0
	return rdtMessage{Cmd: recordings.FileCommand, Payload: body}
}

// flushIfIdle finishes a zero-length file transfer once the camera has gone
// quiet. A four-byte ack is not a file: the CW500 sends that immediately, then
// takes well over 500ms to start reading the MP4 off the card. Emitting the ack
// left FetchRecording with "largest 4 bytes" and parsed the real file as a
// header.
func (r *rdtStream) flushIfIdle() ([]rdtMessage, []string) {
	if !r.collecting {
		return nil, nil
	}
	r.idleTicks++
	if r.idleTicks < fileIdleTicks || !plausibleFile(r.payload) {
		return nil, nil
	}
	return r.takeCollected()
}

func (r *rdtStream) takeCollected() ([]rdtMessage, []string) {
	if !plausibleFile(r.payload) {
		r.cmd, r.want, r.payload = 0, 0, nil
		r.collecting, r.idleTicks = false, 0
		return nil, nil
	}
	msg := r.emitCollected()
	line := fmt.Sprintf("rdt message, cmd %d, %d bytes (collected)%s",
		msg.Cmd, len(msg.Payload), describeRDT(msg.Payload))
	return []rdtMessage{msg}, []string{line}
}

// drain takes whole messages off the buffer, leaving anything incomplete for the
// next call. It returns the messages and a running commentary for the probes,
// which are the only reader that wants to see the pieces.
func (r *rdtStream) drain(decode func([]byte) ([]byte, error)) ([]rdtMessage, []string) {
	var (
		msgs  []rdtMessage
		lines []string
	)

	for len(r.buf) >= 4 {
		size := int(binary.BigEndian.Uint32(r.buf))

		// A length that could never be right means the stream is not what this
		// thinks it is, and keeping the bytes would only misread every message
		// after them.
		if size <= 0 || size > maxRDTMessage {
			r.reset()
			return msgs, append(lines, fmt.Sprintf("rdt: giving up on a stream claiming %d bytes", size))
		}
		if len(r.buf) < size+4 {
			return msgs, lines
		}

		plain, err := decode(r.buf[4 : 4+size])
		r.buf = r.buf[4+size:]
		if err != nil {
			lines = append(lines, fmt.Sprintf("rdt: %v", err))
			continue
		}
		if r.onPlain != nil {
			r.onPlain(plain)
		}

		if r.collecting {
			// Every decrypted chunk is file data. A continuation that happens
			// to look like a command header is how a 7 MB MP4 became
			// "0 replies" — the four-byte ack was dropped and the file was
			// parsed as a new message with a nonsense length.
			r.payload = append(r.payload, plain...)
			r.idleTicks = 0
			continue
		}

		if r.want == 0 {
			if len(plain) < 8 {
				lines = append(lines, fmt.Sprintf("rdt: a %d byte chunk cannot be a header", len(plain)))
				continue
			}
			r.cmd = binary.LittleEndian.Uint32(plain)
			r.want = int(binary.LittleEndian.Uint32(plain[4:]))
			if r.want < 0 || r.want > maxRDTMessage {
				r.reset()
				return msgs, append(lines, fmt.Sprintf("rdt: giving up on a payload claiming %d bytes", r.want))
			}
			r.payload = append(r.payload[:0], plain[8:]...)
		} else {
			r.payload = append(r.payload, plain...)
		}

		if r.cmd == recordings.FileCommand && r.want == 0 {
			r.collecting = true
			r.idleTicks = 0
			continue
		}
		if r.cmd == recordings.FileCommand && r.want <= 16 && !plausibleFile(r.payload) {
			r.collecting = true
			r.want = 0
			r.idleTicks = 0
			continue
		}

		if len(r.payload) < r.want {
			continue
		}

		body := append([]byte(nil), r.payload[:r.want]...)
		msgs = append(msgs, rdtMessage{Cmd: r.cmd, Payload: body})
		lines = append(lines, fmt.Sprintf("rdt message, cmd %d, %d bytes%s",
			r.cmd, len(body), describeRDT(body)))

		extra := r.payload[r.want:]
		cmd := r.cmd
		r.cmd, r.want, r.payload = 0, 0, nil

		// Bytes past the declared end are not spare. Either the length was read
		// from the wrong place or the chunk carries more than one message, and
		// dropping the tail hides both. For a file reply they are the rest of
		// the MP4: keep them rather than logging them away.
		if len(extra) > 0 {
			lines = append(lines, fmt.Sprintf("rdt: %d bytes past the declared end%s",
				len(extra), describeRDT(extra)))
			if cmd == recordings.FileCommand {
				r.cmd = recordings.FileCommand
				r.collecting = true
				r.payload = append(r.payload[:0], extra...)
				r.idleTicks = 0
			}
		}
	}

	return msgs, lines
}

// rdtPollInterval is how often the pump looks for new traffic. The tap holds
// thousands of messages, so this only has to be short enough that a transfer
// finishes promptly, not short enough to keep up with the camera.
const rdtPollInterval = 20 * time.Millisecond

// rdtPump moves the file-transfer channel from the transport's tap into whole
// messages, for as long as the connection lives.
//
// This runs unprompted because the tap is a ring buffer: a reply nobody collects
// is eventually overwritten, and losing one chunk of a two hundred chunk
// transfer costs the whole transfer. Draining only when asked was what made a
// recording index look like silence.
//
// One pump per physical connection, never one per logical session: two pumps on
// one tap would each take half the chunks and neither would be able to rebuild
// anything.
func rdtPump(client *miss.Client, stop <-chan struct{}, deliver func([]rdtMessage, []string), onPlain, onTap func([]byte), reset *atomic.Bool) {
	assembler := &rdtStream{onPlain: onPlain}

	ticker := time.NewTicker(rdtPollInterval)
	defer ticker.Stop()

	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
		}

		var lines []string

		if reset != nil && reset.CompareAndSwap(true, false) {
			assembler.reset()
			lines = append(lines, "rdt: assembler reset")
		}

		// A gap in the byte stream cannot be reassembled across, and the bytes
		// give no sign of it, so the partial message goes with the gap.
		if lost := client.TapLost(); lost[1] > assembler.lost {
			lines = append(lines, fmt.Sprintf(
				"rdt: lost %d messages, abandoning the partial one", lost[1]-assembler.lost))
			assembler.lost = lost[1]
			assembler.reset()
		}

		for _, m := range client.Tap() {
			if m.Channel == 1 {
				head := m.Data
				if len(head) > 8 {
					head = head[:8]
				}
				lines = append(lines, fmt.Sprintf("rdt tap %d bytes head %x", len(m.Data), head))
				if onTap != nil {
					onTap(m.Data)
				}
				assembler.push(m.Data)
				continue
			}
			// Channel 2 is live media. Dumping it as hex fills the command
			// buffer and the next replies read comes back empty.
		}

		msgs, more := assembler.drain(client.DecodeRDT)
		lines = append(lines, more...)
		idle, idleLines := assembler.flushIfIdle()
		msgs = append(msgs, idle...)
		lines = append(lines, idleLines...)

		if len(msgs) > 0 || len(lines) > 0 {
			deliver(msgs, lines)
		}
	}
}

// describeRDT renders a payload both ways, because the index replies are text
// and the file ones are binary and which is which is not known in advance.
func describeRDT(body []byte) string {
	shown := body
	suffix := ""
	if len(body) > 64 {
		shown = body[:64]
		suffix = fmt.Sprintf(" ... %d more", len(body)-64)
	}
	return fmt.Sprintf("\n  text: %s\n  hex:  %x%s",
		strings.ToValidUTF8(strings.Map(printable, string(shown)), "."), shown, suffix)
}

// printable keeps text readable when a payload turns out to be binary.
func printable(r rune) rune {
	if r == '\n' || r == '\t' || (r >= 0x20 && r < 0x7f) {
		return r
	}
	return '.'
}
