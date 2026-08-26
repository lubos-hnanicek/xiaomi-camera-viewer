package stream

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// identity stands in for decryption, so these tests are about framing only.
func identity(data []byte) ([]byte, error) { return data, nil }

// chunk wraps a piece the way the transport delivers it: a big-endian length
// and then the body. Note the endianness fights the little-endian header
// inside, which is exactly the trap this reassembly has to get right.
func chunk(body []byte) []byte {
	return append(binary.BigEndian.AppendUint32(nil, uint32(len(body))), body...)
}

// message builds a whole reply: a header chunk carrying the command and the
// total length, then the payload split into pieces of the given size.
func message(cmd uint32, payload []byte, headerCarries, pieceSize int) []byte {
	head := binary.LittleEndian.AppendUint32(nil, cmd)
	head = binary.LittleEndian.AppendUint32(head, uint32(len(payload)))
	head = append(head, payload[:headerCarries]...)

	out := chunk(head)
	for off := headerCarries; off < len(payload); off += pieceSize {
		end := min(off+pieceSize, len(payload))
		out = append(out, chunk(payload[off:end])...)
	}
	return out
}

func TestRDTStreamReadsAWholeMessage(t *testing.T) {
	payload := bytes.Repeat([]byte{0xAB}, 32)

	var r rdtStream
	r.push(message(6, payload, len(payload), 1))

	msgs, _ := r.drain(identity)
	if len(msgs) != 1 {
		t.Fatalf("got %d messages, want 1", len(msgs))
	}
	if msgs[0].Cmd != 6 {
		t.Errorf("cmd = %d, want 6", msgs[0].Cmd)
	}
	if !bytes.Equal(msgs[0].Payload, payload) {
		t.Errorf("payload = %x, want %x", msgs[0].Payload, payload)
	}
}

func TestRDTStreamRebuildsAcrossChunksAndArrivals(t *testing.T) {
	// The real case: a recording index is a couple of hundred chunks, and the
	// transport hands over byte runs that have nothing to do with where the
	// chunks begin. Reassembly that only works when a chunk arrives whole is
	// reassembly that works in a test and not against a camera.
	payload := make([]byte, 5000)
	for i := range payload {
		payload[i] = byte(i)
	}

	wire := message(6, payload, 100, 512)

	var r rdtStream
	var msgs []rdtMessage
	for off := 0; off < len(wire); off += 7 {
		r.push(wire[off:min(off+7, len(wire))])
		found, _ := r.drain(identity)
		msgs = append(msgs, found...)
	}

	if len(msgs) != 1 {
		t.Fatalf("got %d messages, want 1", len(msgs))
	}
	if !bytes.Equal(msgs[0].Payload, payload) {
		t.Errorf("payload of %d bytes does not match the %d sent",
			len(msgs[0].Payload), len(payload))
	}
}

func TestRDTStreamKeepsMessagesApart(t *testing.T) {
	// Two answers in the buffer at once must not merge: an index read twice
	// would otherwise come back as one table of double the length, which reads
	// as a camera listing every clip twice.
	first := bytes.Repeat([]byte{1}, 40)
	second := bytes.Repeat([]byte{2}, 40)

	var r rdtStream
	r.push(append(message(6, first, 40, 40), message(11, second, 10, 8)...))

	msgs, _ := r.drain(identity)
	if len(msgs) != 2 {
		t.Fatalf("got %d messages, want 2", len(msgs))
	}
	if !bytes.Equal(msgs[0].Payload, first) || msgs[0].Cmd != 6 {
		t.Errorf("first message = cmd %d, %x", msgs[0].Cmd, msgs[0].Payload)
	}
	if !bytes.Equal(msgs[1].Payload, second) || msgs[1].Cmd != 11 {
		t.Errorf("second message = cmd %d, %x", msgs[1].Cmd, msgs[1].Payload)
	}
}

func TestRDTStreamReportsBytesPastTheDeclaredEnd(t *testing.T) {
	// Extra bytes mean the length was read from the wrong place or a chunk
	// holds more than one message. Silently dropping them would hide both.
	payload := bytes.Repeat([]byte{7}, 16)

	head := binary.LittleEndian.AppendUint32(nil, uint32(6))
	head = binary.LittleEndian.AppendUint32(head, uint32(len(payload)))
	head = append(head, payload...)
	head = append(head, 0xFF, 0xFF)

	var r rdtStream
	r.push(chunk(head))

	msgs, lines := r.drain(identity)
	if len(msgs) != 1 || len(msgs[0].Payload) != len(payload) {
		t.Fatalf("got %d messages, first of %d bytes", len(msgs), len(msgs[0].Payload))
	}
	if !containsLine(lines, "past the declared end") {
		t.Errorf("nothing reported the extra bytes: %q", lines)
	}
}

func TestRDTStreamAcceptsAFileSizedTransportFrame(t *testing.T) {
	// A cmd-1 MP4 is several megabytes. Capping the transport frame at 1 MiB
	// reset the assembler on the first chunk and left only the empty trailer.
	payload := bytes.Repeat([]byte{0xAB}, 1<<20+100)
	copy(payload[4:], []byte("ftypiso5"))

	var r rdtStream
	r.push(message(1, payload, len(payload), len(payload)))

	msgs, lines := r.drain(identity)
	if containsLine(lines, "giving up") {
		t.Fatalf("abandoned a %d byte file: %q", len(payload), lines)
	}
	if len(msgs) != 1 {
		t.Fatalf("got %d messages, want 1", len(msgs))
	}
	if msgs[0].Cmd != 1 || !bytes.Equal(msgs[0].Payload, payload) {
		t.Errorf("cmd %d, %d bytes", msgs[0].Cmd, len(msgs[0].Payload))
	}
}

func TestRDTStreamAbandonsAnImpossibleLength(t *testing.T) {
	// A length that cannot be right means the buffer is not this stream, and
	// keeping the bytes would misread every message after them.
	var r rdtStream
	r.push(binary.BigEndian.AppendUint32(nil, 0xFFFFFFFF))
	r.push(bytes.Repeat([]byte{0}, 64))

	msgs, lines := r.drain(identity)
	if len(msgs) != 0 {
		t.Errorf("got %d messages from nonsense", len(msgs))
	}
	if !containsLine(lines, "giving up") {
		t.Errorf("nothing reported the bad length: %q", lines)
	}
	if len(r.buf) != 0 {
		t.Errorf("%d bytes kept after giving up", len(r.buf))
	}
}

func TestRDTStreamWaitsRatherThanGuessing(t *testing.T) {
	// A partial message must produce nothing at all, not a short one: a
	// truncated index parses happily into fewer clips, and a player would then
	// show a fortnight of footage as an afternoon.
	payload := bytes.Repeat([]byte{9}, 100)
	wire := message(6, payload, 20, 40)

	var r rdtStream
	r.push(wire[:len(wire)-10])

	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("got %d messages from an unfinished transfer", len(msgs))
	}

	r.push(wire[len(wire)-10:])
	msgs, _ := r.drain(identity)
	if len(msgs) != 1 || !bytes.Equal(msgs[0].Payload, payload) {
		t.Errorf("the finished transfer did not come back whole")
	}
}

func TestOfferDropsTheOldestRatherThanBlocking(t *testing.T) {
	// The pump must never be held up by a caller that has gone away, and what
	// it holds must be the newest, since a stale answer is worth less than the
	// one arriving now.
	ch := make(chan int, 2)
	for i := 1; i <= 5; i++ {
		offer(ch, i)
	}

	if len(ch) != 2 {
		t.Fatalf("feed holds %d, want 2", len(ch))
	}
	if got := <-ch; got == 1 {
		t.Errorf("feed kept the oldest value")
	}
}

func TestRDTStreamCollectsAZeroLengthFile(t *testing.T) {
	// Command 1 with a declared length of zero is an ack. The MP4 follows as
	// more decrypted chunks with no new header. Emitting the empty ack made
	// the assembler parse the file as a header, reset, and hand FetchRecording
	// nothing but empties.
	mp4 := make([]byte, 100)
	copy(mp4[4:], []byte("ftypiso5"))

	var r rdtStream
	r.push(message(1, nil, 0, 1))
	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("emitted %d messages from the empty ack", len(msgs))
	}

	r.push(chunk(mp4))
	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("emitted %d messages before the transfer went idle", len(msgs))
	}

	var msgs []rdtMessage
	for i := 0; i < fileIdleTicks; i++ {
		found, _ := r.flushIfIdle()
		msgs = append(msgs, found...)
	}
	if len(msgs) != 1 {
		t.Fatalf("got %d messages after idle, want 1", len(msgs))
	}
	if msgs[0].Cmd != 1 || !bytes.Equal(msgs[0].Payload, mp4) {
		t.Errorf("cmd %d, %d bytes", msgs[0].Cmd, len(msgs[0].Payload))
	}
}

func TestRDTStreamKeepsMP4BytesSittingPastAZeroLength(t *testing.T) {
	// The ack and the start of the file can share one decrypted chunk. The
	// bytes past want==0 used to be logged and discarded.
	mp4 := make([]byte, 64)
	copy(mp4[4:], []byte("ftypiso5"))

	head := binary.LittleEndian.AppendUint32(nil, uint32(1))
	head = binary.LittleEndian.AppendUint32(head, 0)
	head = append(head, mp4...)

	var r rdtStream
	r.push(chunk(head))
	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("emitted %d messages from a still-open file", len(msgs))
	}

	var msgs []rdtMessage
	for i := 0; i < fileIdleTicks; i++ {
		found, _ := r.flushIfIdle()
		msgs = append(msgs, found...)
	}
	if len(msgs) != 1 || !bytes.Equal(msgs[0].Payload, mp4) {
		t.Fatalf("collected %d messages, first of %d bytes", len(msgs), payloadLen(msgs))
	}
}

func TestRDTStreamWaitsPastAFourByteAck(t *testing.T) {
	// The CW500 answers command 1 with four status bytes, then seeks the card.
	// Flushing that ack after 500ms of silence made FetchRecording report
	// "largest 4 bytes" and parse the MP4 that followed as a header.
	ack := []byte{0, 0, 0, 0}
	mp4 := make([]byte, 100)
	copy(mp4[4:], []byte("ftypiso5"))

	head := binary.LittleEndian.AppendUint32(nil, uint32(1))
	head = binary.LittleEndian.AppendUint32(head, 0)
	head = append(head, ack...)

	var r rdtStream
	r.push(chunk(head))
	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("emitted %d messages from the four-byte ack", len(msgs))
	}

	for i := 0; i < fileIdleTicks*2; i++ {
		if found, _ := r.flushIfIdle(); len(found) != 0 {
			t.Fatalf("flushed %d bytes after idle, before the file arrived", len(found[0].Payload))
		}
	}
	if !r.collecting {
		t.Fatal("gave up collecting during the card seek")
	}

	r.push(chunk(mp4))
	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("emitted %d messages before the transfer went idle", len(msgs))
	}

	var msgs []rdtMessage
	for i := 0; i < fileIdleTicks; i++ {
		found, _ := r.flushIfIdle()
		msgs = append(msgs, found...)
	}
	want := append(append([]byte(nil), ack...), mp4...)
	if len(msgs) != 1 || !bytes.Equal(msgs[0].Payload, want) {
		t.Fatalf("got %d messages, first of %d bytes, want %d",
			len(msgs), payloadLen(msgs), len(want))
	}
}

func TestRDTStreamEmptyAckThenDeclaredFile(t *testing.T) {
	payload := bytes.Repeat([]byte{0xAB}, 32)
	copy(payload[4:], []byte("ftypiso5"))

	var r rdtStream
	r.push(message(1, nil, 0, 1))
	r.push(message(1, payload, len(payload), len(payload)))

	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("emitted %d messages before idle, want the file collected", len(msgs))
	}

	var msgs []rdtMessage
	for i := 0; i < fileIdleTicks; i++ {
		found, _ := r.flushIfIdle()
		msgs = append(msgs, found...)
	}
	if len(msgs) != 1 || !bytes.Contains(msgs[0].Payload, []byte("ftyp")) {
		t.Fatalf("got %d messages after an empty ack and a sized file", len(msgs))
	}
}

func TestRDTStreamKeepsCollectingWhenAChunkLooksLikeAHeader(t *testing.T) {
	ack := []byte{0, 0, 0, 0}
	head := binary.LittleEndian.AppendUint32(nil, uint32(1))
	head = binary.LittleEndian.AppendUint32(head, 0)
	head = append(head, ack...)

	looksLikeIndex := binary.LittleEndian.AppendUint32(nil, uint32(6))
	looksLikeIndex = binary.LittleEndian.AppendUint32(looksLikeIndex, 100)
	mp4 := make([]byte, 64)
	copy(mp4[4:], []byte("ftypiso5"))

	var r rdtStream
	r.push(chunk(head))
	r.drain(identity)
	r.push(chunk(append(looksLikeIndex, mp4...)))
	if msgs, _ := r.drain(identity); len(msgs) != 0 {
		t.Fatalf("dropped the collect on a header-like chunk: %d messages", len(msgs))
	}

	var msgs []rdtMessage
	for i := 0; i < fileIdleTicks; i++ {
		found, _ := r.flushIfIdle()
		msgs = append(msgs, found...)
	}
	if len(msgs) != 1 || !bytes.Contains(msgs[0].Payload, []byte("ftypiso5")) {
		t.Fatalf("lost the MP4 behind a header-like chunk")
	}
}

func payloadLen(msgs []rdtMessage) int {
	if len(msgs) == 0 {
		return 0
	}
	return len(msgs[0].Payload)
}

func containsLine(lines []string, needle string) bool {
	for _, line := range lines {
		if bytes.Contains([]byte(line), []byte(needle)) {
			return true
		}
	}
	return false
}
