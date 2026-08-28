// Package recordings reads what a camera has on its SD card and asks it to play
// a piece back.
//
// Two protocols meet here. The catalogue arrives over RDT, the file-transfer
// channel, as a flat table of clip start times; playback is an ordinary MISS
// command on the command channel. They belong together because neither is
// usable alone: a playback request names a clip by the exact instant it began,
// and nothing but the catalogue knows those instants. Asking for a round minute,
// or for any moment inside a clip rather than its first, is answered
// "filenotfound" by a camera that holds the footage.
//
// Both known models agree on the wire once the request is written as bytes. The
// shapes here were recovered from IMILAB's published firmware for the same
// boards; see scripts/probe-rdt.ps1 and scripts/probe-playback.ps1.
package recordings

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

// RDT commands. The camera's parser recognises a handful and ignores the rest
// without a word, so a wrong number is indistinguishable from a camera that has
// nothing to say.
const (
	// FileCommand asks for one recorded file by timestamp. The first word of
	// this payload is the timestamp; the word at offset 8 is the channel, but
	// only a payload longer than 19 bytes is read that way, so a 12-byte body
	// forces channel 0. record_find_record_info looks the timestamp up in that
	// channel's index, which means the channel has to be one the camera
	// records to: 0 and 10 on a CW500, and 1 is not one of them.
	FileCommand = 1

	// IndexCommand asks for the recording index: every clip on the card,
	// whether or not anything moved in it.
	IndexCommand = 6

	// EventCommand asks for the event index instead, which covers only the
	// clips something was detected in. Kept because it is the other half of
	// what the card holds, though nothing here reads it yet.
	EventCommand = 11
)

// Clip is one recorded file: when it began, and how long it runs.
//
// Start is the whole of its identity. Clips begin at whatever second the
// previous one ended, not on the minute, so a start looks arbitrary and cannot
// be computed -- only read from the index.
//
// Event is set when the index marks the clip as containing a detection. That
// mark lives in the same word as the duration (bit 8), which is why a minute
// of footage can read as 316 seconds if the bit is taken as part of the
// length: 256 + 60. The file is still a minute; the camera says so when it
// opens it.
type Clip struct {
	Start    int64 `json:"start"`
	Duration int32 `json:"duration"`
	Event    bool  `json:"event,omitempty"`
}

// End is the first instant the clip no longer covers.
func (c Clip) End() int64 { return c.Start + int64(c.Duration) }

// indexHeader is the leading word of an index reply, ahead of the table. Both
// models send it and both send it as zero.
const indexHeader = 4

// recordSize is one entry: a little-endian start and a packed length/flags word.
const recordSize = 8

// The second word's low byte is the length in seconds. Bit 8 is a detection
// mark, not 256 extra seconds: a CW500 index of ~20,000 clips contains a few
// thousand 316s, every one of which is 60 with that bit set, and opening one
// is answered duration:60. Nothing else in the high bits has been seen.
const (
	durationMask = 0xFF
	eventBit     = 0x100
)

// Sanity bounds for a parsed record. The index is a fixed-size file the camera
// writes in place, so the tail of it is whatever was there before -- unwritten
// slots decode as timestamps in 1970 or beyond any plausible future, and
// keeping them would put clips in the catalogue that cannot be played.
const (
	minStart    = 1_577_836_800 // 2020-01-01
	maxStart    = 2_524_608_000 // 2050-01-01
	maxDuration = 3600
)

// IndexRequest builds the payload that asks for one channel's recording index.
//
// The request is not JSON, though it looks like somewhere it should be: the
// firmware passes bytes 8..11 of the payload straight to record_get_index_all
// as the channel, and only reads them when the message is long enough. Sending
// JSON here puts its text where the channel belongs, which the camera reads as
// a channel number in the hundreds of millions and refuses.
//
// The channel is a storage channel, and only the ones the camera actually
// records to have an index. A CW500 uses 0 and 10, one per lens, each with its
// own full catalogue; 1 and 2 answer with no bytes at all. The two catalogues
// are independent, and their clip boundaries mostly do not coincide, so a start
// from one is not a valid request against the other.
func IndexRequest(channel uint32) []byte {
	// Long enough that the camera reads the channel however it measures the
	// message, and zero everywhere the format says nothing about.
	payload := make([]byte, 24)
	binary.LittleEndian.PutUint32(payload[8:], channel)
	return payload
}

// FileRequest builds the payload that asks for one MP4 (RDT command 1).
//
// The decrypted message is `cmd`, `length`, then this payload. The handler
// reads the timestamp from offset 8 (the first word of the payload). It reads
// the channel from offset 0x10 (payload bytes 8..11) only when the payload is
// longer than 19 bytes; a 12-byte body forces channel 0. That is the layout
// that actually sends a file: record_find_record_info looks the timestamp up
// in that channel's index. Naming a channel the camera does not record to is
// acknowledged and then refused, because that index is empty -- which is what
// asking for channel 1 on a CW500 did, its second lens being channel 10.
func FileRequest(timestamp, channel uint32) []byte {
	payload := make([]byte, 12)
	binary.LittleEndian.PutUint32(payload[0:], timestamp)
	binary.LittleEndian.PutUint32(payload[8:], channel)
	return payload
}

// LooksLikeMP4 reports whether an RDT file reply contains an ISO-BMFF payload.
func LooksLikeMP4(payload []byte) bool {
	return bytes.Contains(payload, []byte("ftyp"))
}

// MP4FromRDT returns the ISO-BMFF payload inside an RDT file reply.
//
// The camera sometimes prefixes a few zero bytes ahead of the first box. The
// box size sits immediately before `ftyp`, so that is where the file starts.
func MP4FromRDT(payload []byte) []byte {
	i := bytes.Index(payload, []byte("ftyp"))
	if i >= 4 {
		return payload[i-4:]
	}
	if i >= 0 {
		return payload[i:]
	}
	return payload
}

// ParseIndex turns an index reply into clips, oldest first.
//
// Unreadable entries are skipped rather than failing the whole read: the table
// is a fixed-size file whose unused tail holds whatever it held before, and one
// stale slot is no reason to hand back nothing.
func ParseIndex(payload []byte) ([]Clip, error) {
	if len(payload) < indexHeader+recordSize {
		return nil, fmt.Errorf("recordings: %d bytes is too short for an index", len(payload))
	}

	var clips []Clip
	for off := indexHeader; off+recordSize <= len(payload); off += recordSize {
		start := int64(binary.LittleEndian.Uint32(payload[off:]))
		raw := binary.LittleEndian.Uint32(payload[off+4:])
		// Anything set above the length and the event bit is leftover from an
		// unwritten slot, not a longer clip.
		if raw&^(durationMask|eventBit) != 0 {
			continue
		}
		duration := int32(raw & durationMask)
		event := raw&eventBit != 0

		if start < minStart || start > maxStart {
			continue
		}
		if duration <= 0 || duration > maxDuration {
			continue
		}
		clips = append(clips, Clip{Start: start, Duration: duration, Event: event})
	}

	if len(clips) == 0 {
		return nil, fmt.Errorf("recordings: no readable clips in %d bytes", len(payload))
	}

	sort.Slice(clips, func(i, j int) bool { return clips[i].Start < clips[j].Start })

	// A camera that is recording while being read can list the clip it is
	// still writing twice, once from each of two index blocks.
	out := clips[:1]
	for _, c := range clips[1:] {
		if c.Start != out[len(out)-1].Start {
			out = append(out, c)
		}
	}
	return out, nil
}

// IndexInspect is a histogram of one raw index. ParseIndex keeps only duration
// and the event bit; anything else is dropped as junk. A live CW500 table of
// 20,391 slots had only the event bit (0x100) set above the length, no
// duplicate starts, and no second-lens flag; channels 1 and 2 answered empty.
type IndexInspect struct {
	Bytes           int            `json:"bytes"`
	Slots           int            `json:"slots"`
	Clips           int            `json:"clips"`
	DuplicateStarts int            `json:"duplicate_starts"`
	SkippedTime     int            `json:"skipped_time"`
	SkippedLength   int            `json:"skipped_length"`
	SkippedBits     int            `json:"skipped_bits"`
	FirstStart      int64          `json:"first_start,omitempty"`
	LastStart       int64          `json:"last_start,omitempty"`
	ExtraBits       map[string]int `json:"extra_bits,omitempty"`
	DurationHi      map[string]int `json:"duration_hi,omitempty"`
}

// InspectIndex reports how the duration/flags word is actually populated.
func InspectIndex(payload []byte) IndexInspect {
	out := IndexInspect{
		Bytes:      len(payload),
		ExtraBits:  map[string]int{},
		DurationHi: map[string]int{},
	}
	if len(payload) < indexHeader+recordSize {
		return out
	}

	seenStart := map[int64]int{}
	for off := indexHeader; off+recordSize <= len(payload); off += recordSize {
		out.Slots++
		start := int64(binary.LittleEndian.Uint32(payload[off:]))
		raw := binary.LittleEndian.Uint32(payload[off+4:])
		hi := raw >> 8
		if hi != 0 {
			key := fmt.Sprintf("0x%x", hi)
			out.DurationHi[key]++
		}
		extra := raw &^ (durationMask | eventBit)
		if extra != 0 {
			out.SkippedBits++
			key := fmt.Sprintf("0x%x", extra)
			out.ExtraBits[key]++
			continue
		}
		duration := int32(raw & durationMask)
		if start < minStart || start > maxStart {
			out.SkippedTime++
			continue
		}
		if duration <= 0 || duration > maxDuration {
			out.SkippedLength++
			continue
		}
		out.Clips++
		seenStart[start]++
		if out.FirstStart == 0 || start < out.FirstStart {
			out.FirstStart = start
		}
		if start > out.LastStart {
			out.LastStart = start
		}
	}
	for _, n := range seenStart {
		if n > 1 {
			out.DuplicateStarts += n - 1
		}
	}
	if len(out.ExtraBits) == 0 {
		out.ExtraBits = nil
	}
	if len(out.DurationHi) == 0 {
		out.DurationHi = nil
	}
	return out
}

// PlaybackRequest builds the body of a playback command (0x10D).
//
// Every field is required: the firmware checks for each by name and abandons
// the request without answering if one is missing, which is why an incomplete
// request looks exactly like a camera that ignores playback altogether.
//
// Lenses is for the two-lens models. Their firmware has two parsers, and which
// one runs is decided by whether `channel` is present at all:
//
//   - Without it, the camera logs "chn no array" and reads starttime, endtime,
//     offset and speed as scalars. That is the single-picture path.
//   - With it, all five of channel, starttime, endtime, offset and speed must
//     be arrays, read in step by index; sessionid, autoswitchtolive and
//     avchannelmerge stay scalars. It then starts one playback per position,
//     and answers with one status per picture, each naming its own vchn.
//
// Every element must be a JSON integer. The parser walks the five arrays
// together and abandons the request at the first element that is not, without
// answering -- so a request that arrays only `channel` and leaves the rest
// scalar draws silence, which is indistinguishable from a camera that does not
// support playback at all. That was this function's bug, and it is why the
// second lens looked unplayable rather than mis-asked.
//
// A lens here is a storage channel, not a live channel. On a CW500 they are 0
// and 10; see sdRecordingChannel on the C++ side.
func PlaybackRequest(id int, start, end int64, lenses []int) string {
	fields := []string{
		fmt.Sprintf(`"sessionid":%d`, id),
		`"autoswitchtolive":1`,
		`"avchannelmerge":1`,
	}

	if len(lenses) == 0 {
		fields = append(fields,
			fmt.Sprintf(`"starttime":%d`, start),
			fmt.Sprintf(`"endtime":%d`, end),
			`"offset":0`,
			`"speed":1`,
		)
		return "{" + strings.Join(fields, ",") + "}"
	}

	// One position per lens, all five arrays the same length. The same instant
	// is asked of each: a caller that wants different moments per picture would
	// need a different shape here, and nothing needs that yet.
	repeat := func(value string) string {
		parts := make([]string, len(lenses))
		for i := range parts {
			parts[i] = value
		}
		return "[" + strings.Join(parts, ",") + "]"
	}

	channels := make([]string, len(lenses))
	for i, lens := range lenses {
		channels[i] = fmt.Sprint(lens)
	}

	fields = append(fields,
		fmt.Sprintf(`"starttime":%s`, repeat(fmt.Sprint(start))),
		fmt.Sprintf(`"endtime":%s`, repeat(fmt.Sprint(end))),
		fmt.Sprintf(`"offset":%s`, repeat("0")),
		fmt.Sprintf(`"speed":%s`, repeat("1")),
		fmt.Sprintf(`"channel":[%s]`, strings.Join(channels, ",")),
	)

	return "{" + strings.Join(fields, ",") + "}"
}

// StopRequest asks the camera to leave playback and go back to the live view.
//
// A zero timestamp is the documented way to say so: the firmware calls it out
// as "the timestamp is 0, just switch to live status" rather than treating it
// as a clip that cannot be found.
func StopRequest(id int) string {
	return PlaybackRequest(id, 0, 0, nil)
}

// Status is the camera's answer to a playback request (0x10E).
type Status struct {
	ID     int    `json:"id"`
	Status string `json:"status"`
	Start  int64  `json:"starttime"`
	// Duration is the length of the clip the camera opened, which need not be
	// the length the index gave: the index records what was intended and the
	// file holds what was written.
	Duration int32 `json:"duration"`
	// Lens is which of a two-lens camera's pictures is being sent, absent on
	// the single-lens models.
	Lens *int `json:"vchn,omitempty"`
}

// Found reports whether the camera opened a file.
func (s Status) Found() bool { return s.Status == "filefound" }

// ParseStatus reads a playback reply.
func ParseStatus(body []byte) (Status, error) {
	var status Status

	// The camera pads its replies with NULs, which json rejects as trailing
	// garbage rather than ignoring.
	if err := json.Unmarshal([]byte(strings.TrimRight(string(body), "\x00 \t\r\n")), &status); err != nil {
		return Status{}, fmt.Errorf("recordings: unreadable playback reply %q: %w", body, err)
	}
	return status, nil
}
