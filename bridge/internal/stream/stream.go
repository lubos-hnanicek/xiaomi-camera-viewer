// Package stream turns a MISS session into a queue of access units.
//
// go2rtc feeds its packets into an RTSP/WebRTC producer pipeline. This bridge
// hands them to a decoder in the same process instead, so the pipeline collapses
// to a bounded queue: a reader goroutine pulls from the camera as fast as it
// arrives, and the consumer pulls whole access units back across the C boundary.
package stream

import (
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/miss"
)

// Frame kinds and codecs, matching the XMB_* constants in include/xmbridge.h.
const (
	KindVideo = 1
	KindAudio = 2

	CodecH264 = 1
	CodecH265 = 2
	CodecPCMA = 3
	CodecOpus = 4
	CodecPCM  = 5
	CodecPCMU = 6
)

// ErrClosed is returned by Read once the session has finished.
var ErrClosed = errors.New("stream: closed")

// Frame is one access unit. Video payloads stay in the Annex-B form the camera
// sends, which is what libavcodec's parser expects, so nothing rewrites them.
type Frame struct {
	Kind     int32
	Codec    int32
	Keyframe bool
	PTS      int64 // milliseconds
	Sequence uint32
	// SampleRate is meaningful for audio only, and is the rate the camera
	// declares in the packet flags rather than one assumed from the codec.
	SampleRate int32
	Data       []byte
}

// Config describes one session.
type Config struct {
	Host      string
	Transport string
	Model     string
	Channel   string
	Quality   string
	Audio     bool

	DevicePublic  string
	ClientPublic  string
	ClientPrivate string
	Sign          string
}

// Stats is a snapshot of session health, surfaced for the UI's per-tile status.
type Stats struct {
	Frames  uint64 `json:"frames"`
	Bytes   uint64 `json:"bytes"`
	Dropped uint64 `json:"dropped"`
	// Messages taken off the command channel. Only interesting as evidence that
	// the channel needs draining at all, since nothing here reads the contents.
	Replies   uint64 `json:"replies"`
	LastReply string `json:"last_reply"`
	// Whether the separate audio command had to be sent because enableaudio
	// alone produced no audio. Diagnostic: it says which of the two a model
	// needs, which is not documented anywhere.
	AudioAsked bool `json:"audio_asked"`
	// Why the session ended, empty while it is running. A probe that provokes a
	// camera into hanging up otherwise sees only that the frames stopped, which
	// looks the same as a camera that went quiet.
	Error string `json:"error"`
}

// reply is one message from the command channel, in the form it arrived.
type reply struct {
	cmd  uint32
	body []byte
}

type Session struct {
	client *miss.Client
	shared *sharedPhysical

	// Whether this session asked the camera for audio, which is what makes the
	// silence of an audio-less session worth acting on.
	wantAudio bool

	frames chan *Frame
	done   chan struct{}

	closeOnce       sync.Once
	framesCloseOnce sync.Once
	err             atomic.Pointer[error]

	frameCount atomic.Uint64
	byteCount  atomic.Uint64
	dropped    atomic.Uint64
	replyCount atomic.Uint64

	// Partial RDT message, kept between reads because a reply larger than one
	// transport message arrives in pieces and only the first piece says how
	// long the whole is.
	rdt []byte

	// A reply too big for one chunk is split into several, each encrypted and
	// length-prefixed on its own, but only the first carrying the command and
	// the total. The rest are payload and nothing else, so the header has to be
	// remembered across them.
	rdtCmd     uint32
	rdtWant    int
	rdtPayload []byte
	audioAsked atomic.Bool
	lastReply  atomic.Pointer[reply]

	replyMu  sync.Mutex
	replyLog []reply

	Protocol   string
	RemoteAddr string
}

// queueDepth is generous enough to ride out a decoder hiccup but short enough
// that a wedged consumer shows up as dropped frames rather than growing latency.
const queueDepth = 90

// replyLogDepth bounds how many replies are remembered for a probe to collect.
// Deep enough to hold everything one command provokes, shallow enough that a
// session nobody is probing does not accumulate anything worth worrying about.
const replyLogDepth = 32

// Open connects, authenticates and starts the media flow.
func Open(cfg Config) (*Session, error) {
	client, err := miss.Dial(miss.Config{
		Host:          cfg.Host,
		Transport:     cfg.Transport,
		Model:         cfg.Model,
		DevicePublic:  cfg.DevicePublic,
		ClientPublic:  cfg.ClientPublic,
		ClientPrivate: cfg.ClientPrivate,
		Sign:          cfg.Sign,
	})
	if err != nil {
		return nil, err
	}

	if err = client.StartMedia(cfg.Channel, cfg.Quality, cfg.Audio); err != nil {
		_ = client.Close()
		return nil, fmt.Errorf("stream: start media: %w", err)
	}

	s := &Session{
		client:     client,
		wantAudio:  cfg.Audio,
		frames:     make(chan *Frame, queueDepth),
		done:       make(chan struct{}),
		Protocol:   client.Protocol(),
		RemoteAddr: client.RemoteAddr().String(),
	}

	go s.reader()
	go s.commandReader()

	return s, nil
}

// commandReader keeps the command channel empty.
//
// This is not optional bookkeeping. The transport gives that channel room for
// only a handful of messages and treats an overflow as a fatal error, so a
// session that sends commands and never takes the answers off the channel dies
// once a few have piled up. Nothing here needs the contents: the only commands
// sent during a session are motor steps, which the camera does not answer.
func (s *Session) commandReader() {
	for {
		cmd, data, err := s.client.ReadCommandReply()
		if err != nil {
			return // the connection has ended; the media reader reports why
		}
		s.replyCount.Add(1)

		// Kept for diagnostics only. A reply that will not decode is not worth
		// ending the drain over, since draining is the part that matters.
		if inner, body, err := s.client.UnwrapReply(cmd, data); err == nil {
			r := reply{cmd: inner, body: body}
			s.lastReply.Store(&r)
			s.logReply(r)
		}
	}
}

func (s *Session) logReply(r reply) {
	s.replyMu.Lock()
	defer s.replyMu.Unlock()

	if len(s.replyLog) == replyLogDepth {
		s.replyLog = append(s.replyLog[:0], s.replyLog[1:]...)
	}
	s.replyLog = append(s.replyLog, r)
}

// Replies hands back the replies seen since the last call, oldest first, and
// forgets them. Draining on read is what lets a probe attribute what came back
// to the command it just sent.
func (s *Session) Replies() []string {
	s.replyMu.Lock()
	defer s.replyMu.Unlock()

	out := make([]string, len(s.replyLog))
	for i, r := range s.replyLog {
		out[i] = fmt.Sprintf("cmd=%#x %s", r.cmd, r.body)
	}
	s.replyLog = s.replyLog[:0]
	return out
}

// audioGrace is how long a session that asked for audio waits for the first
// audio packet before asking again with the separate audio command.
const audioGrace = 3 * time.Second

func (s *Session) reader() {
	defer s.end(nil)

	// Some models answer enableaudio in the start command and some appear to
	// want the separate 0x104 as well. Which is which is not documented, so the
	// second command is sent only when the first one has visibly not worked.
	audioSeen := !s.wantAudio
	audioAsked := false
	audioDeadline := time.Now().Add(audioGrace)

	for {
		// A camera that has gone quiet for ten seconds is gone; without this the
		// read would block forever and the tile would sit on a frozen image.
		_ = s.client.SetDeadline(time.Now().Add(10 * time.Second))

		pkt, err := s.client.ReadPacket()
		if err != nil {
			s.setErr(err)
			return
		}

		frame := toFrame(pkt)
		if frame == nil {
			continue // a codec we do not handle
		}

		if !audioSeen {
			switch {
			case frame.Kind == KindAudio:
				audioSeen = true
			case !audioAsked && time.Now().After(audioDeadline):
				audioAsked = true
				s.audioAsked.Store(true)
				_ = s.client.StartAudio()
			}
		}

		if !s.enqueue(frame) {
			return
		}
	}
}

func (s *Session) enqueue(frame *Frame) bool {
	s.frameCount.Add(1)
	s.byteCount.Add(uint64(len(frame.Data)))

	select {
	case s.frames <- frame:
		return true
	case <-s.done:
		return false
	default:
		// Prefer fresh frames over a backlog: drop the oldest and retry.
		select {
		case <-s.frames:
			s.dropped.Add(1)
		default:
		}
		select {
		case s.frames <- frame:
			return true
		case <-s.done:
			return false
		default:
			s.dropped.Add(1)
			return true
		}
	}
}

func toFrame(pkt *miss.Packet) *Frame {
	f := &Frame{
		PTS:      int64(pkt.Timestamp),
		Sequence: pkt.Sequence,
		Data:     pkt.Payload,
	}

	switch pkt.CodecID {
	case miss.CodecH264:
		f.Kind, f.Codec = KindVideo, CodecH264
		f.Keyframe = isKeyframeH264(pkt.Payload)
	case miss.CodecH265:
		f.Kind, f.Codec = KindVideo, CodecH265
		f.Keyframe = isKeyframeH265(pkt.Payload)
	case miss.CodecPCMA:
		f.Kind, f.Codec = KindAudio, CodecPCMA
	case miss.CodecOPUS:
		f.Kind, f.Codec = KindAudio, CodecOpus
	case miss.CodecPCM:
		f.Kind, f.Codec = KindAudio, CodecPCM
	case miss.CodecPCMU:
		f.Kind, f.Codec = KindAudio, CodecPCMU
	default:
		return nil
	}

	if f.Kind == KindAudio {
		f.SampleRate = int32(pkt.SampleRate())
	}

	return f
}

// Read blocks for the next access unit.
func (s *Session) Read() (*Frame, error) {
	frame, ok := <-s.frames
	if !ok {
		if e := s.err.Load(); e != nil {
			return nil, *e
		}
		return nil, ErrClosed
	}
	return frame, nil
}

// Step moves the lens one step in the given direction.
//
// A step is a fixed size the camera decides and it stops on its own, so holding
// a direction down means repeating this rather than starting and later stopping
// a movement.
//
// The mapping from a direction to a motor operation is what makes the picture
// follow the button, and the camera's own coordinates run opposite to the view
// on both axes: the reported x grows as the lens turns left and y grows as it
// tilts down. Confirmed by watching the picture, not just the numbers.
func (s *Session) Step(direction string) error {
	operation, err := motorOperation(direction)
	if err != nil {
		return err
	}
	return s.mediaClient().MotorStep(operation)
}

func motorOperation(direction string) (int, error) {
	switch direction {
	case "right":
		return miss.MotorPanMinus, nil
	case "left":
		return miss.MotorPanPlus, nil
	case "up":
		return miss.MotorTiltMinus, nil
	case "down":
		return miss.MotorTiltPlus, nil
	}
	return 0, fmt.Errorf("stream: unknown direction %q", direction)
}

// Motor sends an arbitrary motor payload, for probing a model whose accepted
// payload shape is not known yet.
func (s *Session) Motor(body string) error {
	return s.mediaClient().MotorRaw(body)
}

// Raw sends an arbitrary command, for probing a part of the protocol that has no
// implementation here yet. Whatever the camera answers turns up in Replies.
func (s *Session) Raw(cmd uint32, body string) error {
	return s.mediaClient().SendRaw(cmd, body)
}

// RawChannel sends an arbitrary command on an arbitrary transport channel, for
// probing the RDT path the SD card is suspected to live behind. See
// scripts/probe-rdt.ps1.
func (s *Session) RawChannel(channel byte, cmd uint32, body string, encrypt, envelope bool) error {
	return s.mediaClient().SendChannel(channel, cmd, body, encrypt, envelope)
}

// Tap hands back the messages seen on channels this bridge does not open, as
// hex, and forgets them. Unlike Unhandled, which only counts, this is the
// content, which is what a probe has to read to learn anything.
func (s *Session) Tap() []string {
	msgs := s.mediaClient().Tap()

	var out []string

	// A gap in the channel's byte stream cannot be reassembled across, and the
	// bytes give no sign of it, so the partial message goes with the gap.
	if lost := s.mediaClient().TapLost(); lost[1] > 0 {
		out = append(out, fmt.Sprintf("rdt: lost %d messages, abandoning the partial one", lost[1]))
		s.rdt = nil
	}

	taken := 0
	for _, m := range msgs {
		// Channel 1 is the RDT path. Its messages are encrypted, and a reply
		// larger than one transport message is split across several of them
		// with only the first carrying the length, so a message has to be
		// rebuilt from the channel's byte stream before it can be read at all.
		if m.Channel == 1 {
			s.rdt = append(s.rdt, m.Data...)
			taken++
			continue
		}
		out = append(out, fmt.Sprintf("channel %d seq %d: %x", m.Channel, m.Seq, m.Data))
	}

	// Say what was taken in, not only what came out. Reassembly that consumes
	// a hundred messages and completes none looks exactly like a camera that
	// said nothing, and only this line tells them apart.
	if taken > 0 {
		out = append(out, fmt.Sprintf("rdt: took in %d messages, buffer now %d bytes",
			taken, len(s.rdt)))
	}

	return append(out, s.drainRDT()...)
}

// drainRDT takes whole RDT messages off the reassembly buffer and decrypts
// them, leaving anything incomplete for the next read.
func (s *Session) drainRDT() []string {
	var out []string

	for len(s.rdt) >= 4 {
		size := int(binary.BigEndian.Uint32(s.rdt))

		// A length that could never be right means the stream is not what this
		// thinks it is, and keeping the bytes would only misread every message
		// after them.
		if size <= 0 || size > 1<<20 {
			s.rdt = nil
			return append(out, fmt.Sprintf("rdt: giving up on a stream claiming %d bytes", size))
		}
		if len(s.rdt) < size+4 {
			// Say so rather than returning nothing: a read that quietly
			// swallows a hundred messages is indistinguishable from a camera
			// that never answered, and the two want opposite fixes.
			return append(out, fmt.Sprintf("rdt: %d of %d payload bytes, chunk %d of %d in hand",
				len(s.rdtPayload), s.rdtWant, len(s.rdt)-4, size))
		}

		plain, err := s.mediaClient().DecodeRDT(s.rdt[4 : 4+size])
		s.rdt = s.rdt[4+size:]
		if err != nil {
			out = append(out, fmt.Sprintf("rdt: %v", err))
			continue
		}

		if s.rdtWant == 0 {
			if len(plain) < 8 {
				out = append(out, fmt.Sprintf("rdt: a %d byte chunk cannot be a header", len(plain)))
				continue
			}
			s.rdtCmd = binary.LittleEndian.Uint32(plain)
			s.rdtWant = int(binary.LittleEndian.Uint32(plain[4:]))
			s.rdtPayload = append(s.rdtPayload[:0], plain[8:]...)
		} else {
			s.rdtPayload = append(s.rdtPayload, plain...)
		}

		if len(s.rdtPayload) < s.rdtWant {
			continue
		}

		body := s.rdtPayload[:s.rdtWant]
		out = append(out, fmt.Sprintf("rdt message, cmd %d, %d bytes%s", s.rdtCmd, len(body),
			describeRDT(body)))

		// Bytes past the declared end are not spare. Either the length was read
		// from the wrong place or the chunk carries more than one message, and
		// dropping the tail hides both: a camera that answered at length then
		// reads as a camera that answered with nothing.
		if extra := s.rdtPayload[s.rdtWant:]; len(extra) > 0 {
			out = append(out, fmt.Sprintf("rdt: %d bytes past the declared end%s",
				len(extra), describeRDT(extra)))
		}

		s.rdtCmd, s.rdtWant, s.rdtPayload = 0, 0, nil
	}

	return out
}

// describeRDT renders a payload both ways, because the index replies are text
// and the file ones are binary and which is which is not known in advance.
func describeRDT(body []byte) string {
	return fmt.Sprintf("\n  text: %s\n  hex:  %x",
		strings.ToValidUTF8(strings.Map(printable, string(body)), "."), body)
}

// printable keeps text readable when a payload turns out to be binary.
func printable(r rune) rune {
	if r == '\n' || r == '\t' || (r >= 0x20 && r < 0x7f) {
		return r
	}
	return '.'
}

// MediaHeaders hands back the raw media headers a dual-lens session has seen,
// each with the lens it was routed to, and forgets them. Only a shared session
// captures them, because separating one camera's two interleaved streams is the
// only thing here that has to know more than the header's known fields say.
func (s *Session) MediaHeaders() []string {
	if s.shared == nil {
		return nil
	}
	return s.shared.MediaHeaders()
}

// Unhandled describes traffic the transport had nowhere to put, per channel, as
// a count and a hex sample. Only of interest while probing: a camera that
// answers on a channel this bridge does not open looks silent without it.
func (s *Session) Unhandled() []string {
	counts, samples := s.mediaClient().Unhandled()

	var out []string
	for ch, count := range counts {
		if count == 0 {
			continue
		}
		out = append(out, fmt.Sprintf("channel %d: %d messages, %d offered to the tap, first %x",
			ch, count, s.mediaClient().TapSeen()[ch], samples[ch]))
	}
	return out
}

func (s *Session) Stats() Stats {
	st := Stats{
		Frames:     s.frameCount.Load(),
		Bytes:      s.byteCount.Load(),
		Dropped:    s.dropped.Load(),
		Replies:    s.replyCount.Load(),
		AudioAsked: s.audioAsked.Load(),
	}
	if last := s.lastReply.Load(); last != nil {
		st.LastReply = fmt.Sprintf("cmd=%#x %s", last.cmd, last.body)
	}
	if e := s.err.Load(); e != nil {
		st.Error = (*e).Error()
	}
	return st
}

func (s *Session) setErr(err error) {
	s.err.CompareAndSwap(nil, &err)
}

func (s *Session) end(err error) {
	if err != nil {
		s.setErr(err)
	}
	s.framesCloseOnce.Do(func() { close(s.frames) })
}

func (s *Session) mediaClient() *miss.Client {
	if s.shared != nil {
		return s.shared.client
	}
	return s.client
}

// Close tears the session down and unblocks any waiting Read.
func (s *Session) Close() {
	s.closeOnce.Do(func() {
		close(s.done)
		if s.shared != nil {
			s.shared.detach(s)
			return
		}
		_ = s.client.StopMedia()
		_ = s.client.Close()
		// The reader goroutine observes the closed connection and closes the
		// frame channel, which is what actually releases a blocked Read.
	})
}

// --- Keyframe detection -----------------------------------------------------
//
// The decoder needs to know where it can start, and the UI wants to show
// "waiting for keyframe" rather than a blank tile. Both codecs mark this in the
// NAL header, so a scan of the Annex-B start codes is enough.

func isKeyframeH264(b []byte) bool {
	return scanAnnexB(b, func(nal byte) bool {
		switch nal & 0x1F {
		case 5, 7, 8: // IDR, SPS, PPS
			return true
		}
		return false
	})
}

func isKeyframeH265(b []byte) bool {
	return scanAnnexB(b, func(nal byte) bool {
		switch t := (nal >> 1) & 0x3F; {
		case t >= 16 && t <= 21: // BLA through CRA: the random-access points
			return true
		case t >= 32 && t <= 34: // VPS, SPS, PPS
			return true
		}
		return false
	})
}

// scanAnnexB walks start-code-delimited NAL units and reports whether any of
// their first header bytes satisfies match.
func scanAnnexB(b []byte, match func(nal byte) bool) bool {
	for i := 0; i+3 < len(b); i++ {
		if b[i] != 0 || b[i+1] != 0 {
			continue
		}

		var start int
		switch {
		case b[i+2] == 1:
			start = i + 3
		case b[i+2] == 0 && b[i+3] == 1:
			start = i + 4
		default:
			continue
		}

		if start < len(b) && match(b[start]) {
			return true
		}
		i = start
	}
	return false
}
