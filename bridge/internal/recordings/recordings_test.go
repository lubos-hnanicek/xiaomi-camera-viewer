package recordings

import (
	"encoding/binary"
	"testing"
)

// index builds a reply in the shape both cameras send: a leading word, then
// little-endian start and duration pairs.
func index(t *testing.T, pairs ...[2]uint32) []byte {
	t.Helper()

	out := make([]byte, indexHeader)
	for _, p := range pairs {
		out = binary.LittleEndian.AppendUint32(out, p[0])
		out = binary.LittleEndian.AppendUint32(out, p[1])
	}
	return out
}

func TestMP4FromRDTStartsAtTheFtypBox(t *testing.T) {
	// A real CW500 file reply opened with eight zero bytes ahead of the first
	// box. Feeding those to FFmpeg is harmless-looking until it is not, so the
	// file on disk starts at the box size that precedes `ftyp`.
	prefixed := []byte{
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0x18,
		'f', 't', 'y', 'p', 'i', 's', 'o', '5',
	}
	got := MP4FromRDT(prefixed)
	if !LooksLikeMP4(got) {
		t.Fatalf("stripped payload is not an MP4: %x", got)
	}
	if got[4] != 'f' || got[5] != 't' || got[6] != 'y' || got[7] != 'p' {
		t.Errorf("payload does not start at the ftyp box: %x", got)
	}
	if binary.BigEndian.Uint32(got[0:4]) != 0x18 {
		t.Errorf("box size = %#x, want 0x18", binary.BigEndian.Uint32(got[0:4]))
	}
}

func TestFileRequestPutsTimestampThenChannel(t *testing.T) {
	payload := FileRequest(1787740887, 1)
	if len(payload) != 12 {
		t.Fatalf("payload is %d bytes, want 12 so find looks up channel 0", len(payload))
	}
	if got := binary.LittleEndian.Uint32(payload[0:]); got != 1787740887 {
		t.Errorf("timestamp = %d, want 1787740887", got)
	}
	if got := binary.LittleEndian.Uint32(payload[8:]); got != 1 {
		t.Errorf("channel = %d, want 1", got)
	}
}

func TestIndexRequestPutsTheChannelWhereTheFirmwareReadsIt(t *testing.T) {
	// The firmware reads bytes 8..11 and only when the message is long enough,
	// so both the placement and the length are part of the contract. A request
	// that is merely "long enough for a channel" is answered with an error.
	payload := IndexRequest(0)

	if len(payload) <= 19 {
		t.Fatalf("payload is %d bytes, which is too short for the camera to read a channel from",
			len(payload))
	}
	if got := binary.LittleEndian.Uint32(payload[8:]); got != 0 {
		t.Errorf("channel = %d, want 0", got)
	}

	if got := binary.LittleEndian.Uint32(IndexRequest(1)[8:]); got != 1 {
		t.Errorf("channel = %d, want 1", got)
	}
}

func TestParseIndexReadsPastTheLeadingWord(t *testing.T) {
	// Both models send a zero word ahead of the table. Reading from byte zero
	// instead pairs every duration with the next start and yields timestamps
	// that are all implausible, so this is the difference between a full
	// catalogue and an empty one.
	raw := index(t, [2]uint32{1787601603, 60}, [2]uint32{1787601663, 61})

	clips, err := ParseIndex(raw)
	if err != nil {
		t.Fatalf("ParseIndex: %v", err)
	}
	if len(clips) != 2 {
		t.Fatalf("got %d clips, want 2", len(clips))
	}
	if clips[0].Start != 1787601603 || clips[0].Duration != 60 || clips[0].Event {
		t.Errorf("first clip = %+v", clips[0])
	}
	if got := clips[1].End(); got != 1787601663+61 {
		t.Errorf("End() = %d, want %d", got, 1787601663+61)
	}
}

func TestParseIndexSkipsUnwrittenSlots(t *testing.T) {
	// The index is a fixed-size file written in place, so its tail holds
	// whatever was there before. Those slots decode as timestamps far outside
	// any plausible range, and keeping them would offer clips that cannot be
	// played.
	raw := index(t,
		[2]uint32{1787601603, 60},
		[2]uint32{0, 0},
		[2]uint32{1, 60},
		[2]uint32{1787601663, 0},
		[2]uint32{1787601723, 99999},
		[2]uint32{1787601783, 60},
	)

	clips, err := ParseIndex(raw)
	if err != nil {
		t.Fatalf("ParseIndex: %v", err)
	}
	if len(clips) != 2 {
		t.Fatalf("got %d clips, want 2: %+v", len(clips), clips)
	}
}

func TestParseIndexSortsAndDropsRepeats(t *testing.T) {
	// A camera recording while it is read can list the clip it is still
	// writing from two index blocks at once.
	raw := index(t,
		[2]uint32{1787601663, 60},
		[2]uint32{1787601603, 60},
		[2]uint32{1787601663, 60},
	)

	clips, err := ParseIndex(raw)
	if err != nil {
		t.Fatalf("ParseIndex: %v", err)
	}
	if len(clips) != 2 {
		t.Fatalf("got %d clips, want 2: %+v", len(clips), clips)
	}
	if clips[0].Start >= clips[1].Start {
		t.Errorf("clips are not oldest first: %+v", clips)
	}
}

func TestParseIndexTreatsBit8AsAnEventMarkNotALength(t *testing.T) {
	// 316 is 0x13C: 60 seconds with bit 8 set. Taking the whole word as a
	// duration would list a minute of footage as five minutes overlapping the
	// four clips that follow it. Playing that start is answered duration:60.
	raw := index(t, [2]uint32{1787601603, 316}, [2]uint32{1787601663, 60})

	clips, err := ParseIndex(raw)
	if err != nil {
		t.Fatalf("ParseIndex: %v", err)
	}
	if len(clips) != 2 {
		t.Fatalf("got %d clips, want 2: %+v", len(clips), clips)
	}
	if clips[0].Duration != 60 || !clips[0].Event {
		t.Errorf("flagged clip = %+v, want 60s with Event", clips[0])
	}
	if clips[1].Duration != 60 || clips[1].Event {
		t.Errorf("plain clip = %+v, want 60s without Event", clips[1])
	}
	if clips[0].End() > clips[1].Start {
		t.Errorf("flagged clip still overlaps its neighbour: end %d start %d",
			clips[0].End(), clips[1].Start)
	}
}

func TestInspectIndexReportsExtraFlagBits(t *testing.T) {
	// A second lens marked in the same table would set a bit above the event
	// flag. ParseIndex drops those slots; this is how we would see them.
	raw := index(t,
		[2]uint32{1787601603, 60},
		[2]uint32{1787601663, 60 | 0x200},
		[2]uint32{1787601723, 316},
		[2]uint32{1787601603, 60},
	)

	got := InspectIndex(raw)
	if got.Clips != 3 {
		t.Errorf("clips = %d, want 3 (two starts, one of them twice, plus an event clip)", got.Clips)
	}
	if got.DuplicateStarts != 1 {
		t.Errorf("duplicate_starts = %d, want 1", got.DuplicateStarts)
	}
	if got.SkippedBits != 1 || got.ExtraBits["0x200"] != 1 {
		t.Errorf("extra bits = %+v skipped=%d, want 0x200:1", got.ExtraBits, got.SkippedBits)
	}
	if got.DurationHi["0x1"] != 1 { // event bit 0x100 >> 8
		t.Errorf("duration_hi = %+v, want 0x1 from the event-marked clip", got.DurationHi)
	}
	if got.FirstStart != 1787601603 || got.LastStart != 1787601723 {
		t.Errorf("span = %d..%d, want 1787601603..1787601723", got.FirstStart, got.LastStart)
	}
}

func TestParseIndexRejectsWhatCannotBeAnIndex(t *testing.T) {
	// An empty answer is what a camera sends when it understood the request and
	// has nothing to give, and it must not read as a catalogue of zero clips
	// that succeeded.
	for _, raw := range [][]byte{nil, {}, {0, 0, 0, 0}, index(t, [2]uint32{0, 0})} {
		if _, err := ParseIndex(raw); err == nil {
			t.Errorf("ParseIndex(%d bytes) succeeded, want an error", len(raw))
		}
	}
}

func TestPlaybackRequestCarriesEveryRequiredField(t *testing.T) {
	// The firmware looks for each of these by name and gives up without
	// answering if one is missing, so a dropped field does not produce an error
	// reply -- it produces silence, which reads like a camera that cannot play
	// back at all.
	body := PlaybackRequest(1, 1787601603, 1787601663, nil)

	for _, want := range []string{
		`"sessionid":1`,
		`"starttime":1787601603`,
		`"endtime":1787601663`,
		`"autoswitchtolive"`,
		`"offset"`,
		`"speed"`,
		`"avchannelmerge"`,
	} {
		if !contains(body, want) {
			t.Errorf("body %s is missing %s", body, want)
		}
	}

	if contains(body, `"channel"`) {
		t.Errorf("body %s names a lens when none was asked for", body)
	}
}

func TestPlaybackRequestWritesLensesAsAnArray(t *testing.T) {
	// The two-lens firmware refuses a bare number outright, logging
	// "chn no array", so the brackets are not cosmetic. The rest of the
	// request then has to be arrays as well; see PlaybackRequest.
	if got, want := PlaybackRequest(2, 1, 2, []int{0}), `"channel":[0]`; !contains(got, want) {
		t.Errorf("body %s does not contain %s", got, want)
	}
	if got, want := PlaybackRequest(2, 1, 2, []int{0, 1}), `"channel":[0,1]`; !contains(got, want) {
		t.Errorf("body %s does not contain %s", got, want)
	}
	if got, want := PlaybackRequest(2, 1, 2, []int{1}), `"channel":[1]`; !contains(got, want) {
		t.Errorf("body %s does not contain %s", got, want)
	}
}

func TestStopRequestAsksForTheZeroInstant(t *testing.T) {
	// Zero is the camera's documented way of saying "go back to live" rather
	// than a clip it will fail to find.
	if got, want := StopRequest(3), `"starttime":0`; !contains(got, want) {
		t.Errorf("body %s does not contain %s", got, want)
	}
}

func TestParseStatusReadsAFoundFile(t *testing.T) {
	// Exactly as a CW500 sends it: spaces after the commas, a lens, and the
	// trailing NUL that makes json reject the whole reply if it is not trimmed.
	status, err := ParseStatus([]byte(
		"{\"id\":1, \"status\":\"filefound\", \"starttime\":1787601603, \"duration\":61, \"vchn\":0}\x00"))
	if err != nil {
		t.Fatalf("ParseStatus: %v", err)
	}

	if !status.Found() {
		t.Errorf("status %q is not a found file", status.Status)
	}
	if status.Start != 1787601603 || status.Duration != 61 {
		t.Errorf("status = %+v", status)
	}
	if status.Lens == nil || *status.Lens != 0 {
		t.Errorf("lens = %v, want 0", status.Lens)
	}
}

func TestParseStatusReadsAMissingFile(t *testing.T) {
	// A single-lens camera names no lens, and a miss carries no times, so
	// neither absence may be read as a failure to parse.
	status, err := ParseStatus([]byte(`{"id":4, "status":"filenotfound"}`))
	if err != nil {
		t.Fatalf("ParseStatus: %v", err)
	}
	if status.Found() {
		t.Errorf("status %q reads as found", status.Status)
	}
	if status.Lens != nil {
		t.Errorf("lens = %v, want none", *status.Lens)
	}
}

func contains(haystack, needle string) bool {
	return len(haystack) >= len(needle) && indexOf(haystack, needle) >= 0
}

func indexOf(haystack, needle string) int {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return i
		}
	}
	return -1
}
