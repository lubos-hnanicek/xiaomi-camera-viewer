// Package stream turns a MISS session into a queue of access units.
//
// go2rtc feeds its packets into an RTSP/WebRTC producer pipeline. This bridge
// hands them to a decoder in the same process instead, so the pipeline collapses
// to a bounded queue: a reader goroutine pulls from the camera as fast as it
// arrives, and the consumer pulls whole access units back across the C boundary.
package stream

import (
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/miss"
	"github.com/spec8472/xiaomi-viewer/bridge/internal/recordings"
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

	// What the media flow was started with, kept so that it can be started
	// again after playback without the caller having to say twice.
	channel string
	quality string

	frames chan *Frame
	done   chan struct{}

	closeOnce       sync.Once
	framesCloseOnce sync.Once
	err             atomic.Pointer[error]

	frameCount atomic.Uint64
	byteCount  atomic.Uint64
	dropped    atomic.Uint64
	replyCount atomic.Uint64

	// Whole messages off the file-transfer channel, and whole replies off the
	// command channel, for a caller waiting on the answer to something it just
	// asked. Both drop their oldest when full: a session nobody is asking
	// anything of must not accumulate, and a stale answer is worth less than
	// the one arriving now.
	rdtFeed   chan rdtMessage
	replyFeed chan reply

	// The pump's commentary, kept for the probes. Separate from rdtFeed because
	// the two have opposite needs: a probe wants every line including the ones
	// that describe a failure, and a caller wants only the finished message.
	tapMu  sync.Mutex
	tapLog []string

	scraping     atomic.Bool
	scrapeMu     sync.Mutex
	scrape       []byte // framed decrypted plains from the assembler
	scrapeRaw    []byte // channel-1 tap bytes as they arrived
	scrapeDirect []byte // each tap message decrypted as a standalone ciphertext
	scrapeSkip4  []byte // same, skipping a leading 4-byte length
	scrapeN      int
	rdtReset     atomic.Bool

	audioAsked atomic.Bool
	lastReply  atomic.Pointer[reply]
	playbackID atomic.Uint32

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
		channel:    cfg.Channel,
		quality:    cfg.Quality,
		frames:     make(chan *Frame, queueDepth),
		done:       make(chan struct{}),
		rdtFeed:    make(chan rdtMessage, feedDepth),
		replyFeed:  make(chan reply, feedDepth),
		Protocol:   client.Protocol(),
		RemoteAddr: client.RemoteAddr().String(),
	}

	go s.reader()
	go s.commandReader()
	go rdtPump(client, s.done, s.deliverRDT, s.scrapePlain, s.noteTap, &s.rdtReset)

	return s, nil
}

// feedDepth bounds the answers held for a caller that may never come. Deep
// enough for the handful a single request provokes, shallow enough that a
// session nobody asks anything of costs nothing.
const feedDepth = 8

// deliverRDT takes the pump's output: whole messages for whoever is waiting,
// commentary for the probes.
func (s *Session) deliverRDT(msgs []rdtMessage, lines []string) {
	for _, m := range msgs {
		offer(s.rdtFeed, m)
	}
	if len(lines) == 0 {
		return
	}

	s.tapMu.Lock()
	defer s.tapMu.Unlock()

	if overflow := len(s.tapLog) + len(lines) - tapLogDepth; overflow > 0 {
		s.tapLog = append(s.tapLog[:0], s.tapLog[min(overflow, len(s.tapLog)):]...)
	}
	s.tapLog = append(s.tapLog, lines...)
}

// tapLogDepth is deep enough to describe a whole recording index, which is a
// couple of hundred chunks, and no deeper.
const tapLogDepth = 4096

// offer puts a value on a feed without ever blocking the pump, dropping the
// oldest to make room. A reader waiting on an answer wants the newest; one that
// has gone away must not be able to stall the connection.
func offer[T any](ch chan T, v T) {
	select {
	case ch <- v:
		return
	default:
	}

	select {
	case <-ch:
	default:
	}
	select {
	case ch <- v:
	default:
	}
}

// drain empties a feed, so that what arrives next is an answer to what is about
// to be asked rather than a leftover from before.
func drain[T any](ch chan T) {
	for {
		select {
		case <-ch:
		default:
			return
		}
	}
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
	// Offered before it is logged, because the log is for a probe reading at
	// its leisure and the feed is for a caller blocked on this very answer.
	offer(s.replyFeed, r)

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

// SendRDT writes an RDT request as bytes, not as a JSON string. File downloads
// and indexes carry timestamps whose bytes are above 0x7F, which a JSON body
// would turn into UTF-8 and shift.
func (s *Session) SendRDT(cmd uint32, payload []byte) error {
	return s.mediaClient().SendRDT(cmd, payload)
}

// RawChannel sends an arbitrary command on an arbitrary transport channel, for
// probing the RDT path the SD card is suspected to live behind. See
// scripts/probe-rdt.ps1.
func (s *Session) RawChannel(channel byte, cmd uint32, body string, encrypt, envelope bool) error {
	return s.mediaClient().SendChannel(channel, cmd, body, encrypt, envelope)
}

// Tap hands back what the file-transfer pump has made of channels this bridge
// does not open, and forgets it. Unlike Unhandled, which only counts, this is
// the content, which is what a probe has to read to learn anything.
func (s *Session) Tap() []string {
	s.tapMu.Lock()
	defer s.tapMu.Unlock()

	out := s.tapLog
	s.tapLog = nil
	return out
}

// indexTimeout is how long to wait for a recording index. A full card's
// catalogue is around 170 kB in a couple of hundred chunks and has taken about
// ten seconds over TCP, so this is generous rather than tight: the cost of
// waiting too long is a slow error, and the cost of not waiting long enough is
// an empty catalogue for a camera that has a fortnight of footage.
const indexTimeout = 45 * time.Second

// Recordings asks the camera what it holds on its SD card.
//
// The answer is every clip, oldest first, whether or not anything moved in it;
// the separate event index covers only the clips something was detected in.
// Both models answer the same request with the same table, so nothing here
// branches on the model.
//
// This is the only way to learn a clip's start, and a start is the only thing
// Play accepts, so a player has to read this before it can show anything.
func (s *Session) Recordings(channel uint32) ([]recordings.Clip, error) {
	payload, err := s.fetchIndex(channel)
	if err != nil {
		return nil, err
	}
	return recordings.ParseIndex(payload)
}

// InspectIndex reports how the duration/flags word is populated, including
// bits ParseIndex drops. A second lens marked in the same table would show up
// here rather than as a second catalogue.
func (s *Session) InspectIndex(channel uint32) (recordings.IndexInspect, error) {
	payload, err := s.fetchIndex(channel)
	if err != nil {
		return recordings.IndexInspect{}, err
	}
	return recordings.InspectIndex(payload), nil
}

func (s *Session) fetchIndex(channel uint32) ([]byte, error) {
	// Anything already on the feed answers an earlier question.
	drain(s.rdtFeed)

	if err := s.mediaClient().SendRDT(recordings.IndexCommand, recordings.IndexRequest(channel)); err != nil {
		return nil, fmt.Errorf("stream: ask for the recording index: %w", err)
	}

	deadline := time.After(indexTimeout)
	for {
		select {
		case msg := <-s.rdtFeed:
			// The camera sends unprompted messages on this channel too, so a
			// reply to something else is not an error, only not the answer.
			if msg.Cmd != recordings.IndexCommand {
				continue
			}
			return msg.Payload, nil

		case <-deadline:
			return nil, fmt.Errorf(
				"stream: no recording index after %s; the camera may have no card", indexTimeout)

		case <-s.done:
			return nil, ErrClosed
		}
	}
}

// fileAckWait is how long an acknowledgement without a body is allowed to
// stand. Command 1 with channel 1 is answered with one 24-byte transport
// frame (a 12-byte plaintext ack) because that lens has no recording index.
// Waiting a minute for an MP4 that will not come made the player sit on
// "Fetching the recording".
const fileAckWait = 8 * time.Second

// fileTimeout is how long to wait once bytes are actually arriving.
const fileTimeout = 60 * time.Second

// FetchRecording downloads one recorded MP4 (RDT command 1).
//
// A two-lens CW500 will not stream channel 1: that picture has no index, so
// playback.start answers filenotfound. Command 1 looks the timestamp up in
// the same index, so channel 1 is acknowledged with a 12-byte RDT reply and
// no file. Channel 0 does send the MP4 (ftyp/iso5, one HEVC track). The
// second file (`%timestamp_1.mp4`) is still on the card; this command cannot
// name it.
//
// A missing file used to come back as a nil payload. That hid a dropped
// transfer as "the camera no longer has that recording". Failure is now an
// error with how far the transfer got; the player shows that string.
func (s *Session) FetchRecording(start int64, channel uint32) ([]byte, error) {
	if start <= 0 || start > int64(^uint32(0)) {
		return nil, fmt.Errorf("stream: recording start %d is not a valid timestamp", start)
	}

	drain(s.rdtFeed)
	s.beginScrape()
	defer s.endScrape()
	s.requestRDTReset()
	// One pump tick so the assembler drops leftover catalogue bytes before the
	// file reply is mixed into them.
	time.Sleep(rdtPollInterval + 5*time.Millisecond)

	if err := s.mediaClient().SendRDT(
		recordings.FileCommand,
		recordings.FileRequest(uint32(start), channel),
	); err != nil {
		return nil, fmt.Errorf("stream: ask for the recording file: %w", err)
	}

	deadline := time.Now().Add(fileTimeout)
	ackDeadline := time.Now().Add(fileAckWait)
	tick := time.NewTicker(50 * time.Millisecond)
	defer tick.Stop()

	n := 0
	largest := 0
	for {
		select {
		case msg := <-s.rdtFeed:
			n++
			if len(msg.Payload) > largest {
				largest = len(msg.Payload)
			}
			body := recordings.MP4FromRDT(msg.Payload)
			if recordings.LooksLikeMP4(body) || len(body) > 4096 {
				return body, nil
			}

		case <-tick.C:
			body, sizes, total := s.bestScrape()
			if total > largest {
				largest = total
			}
			if time.Now().After(ackDeadline) && total <= 64 {
				return nil, fmt.Errorf(
					"stream: camera acknowledged the request but sent no file (%s)", sizes)
			}
			if time.Now().After(deadline) {
				if recordings.LooksLikeMP4(body) && len(body) > 4096 {
					return body, nil
				}
				return nil, fmt.Errorf(
					"stream: no recording file after %s (%d replies, %s, %d chunks)",
					fileTimeout, n, sizes, s.scrapeChunks())
			}

		case <-s.done:
			return nil, ErrClosed
		}
	}
}

func (s *Session) requestRDTReset() {
	if s.shared != nil {
		s.shared.rdtReset.Store(true)
		return
	}
	s.rdtReset.Store(true)
}

func (s *Session) beginScrape() {
	s.scrapeMu.Lock()
	s.scrape, s.scrapeRaw, s.scrapeDirect, s.scrapeSkip4 = nil, nil, nil, nil
	s.scrapeN = 0
	s.scrapeMu.Unlock()
	s.scraping.Store(true)
}

func (s *Session) endScrape() {
	s.scraping.Store(false)
	s.scrapeMu.Lock()
	s.scrape, s.scrapeRaw, s.scrapeDirect, s.scrapeSkip4 = nil, nil, nil, nil
	s.scrapeN = 0
	s.scrapeMu.Unlock()
}

func (s *Session) scrapePlain(plain []byte) {
	if !s.scraping.Load() {
		return
	}
	s.scrapeMu.Lock()
	s.scrape = append(s.scrape, plain...)
	s.scrapeMu.Unlock()
}

func (s *Session) noteTap(raw []byte) {
	if !s.scraping.Load() {
		return
	}
	client := s.mediaClient()
	var direct, skip []byte
	if len(raw) >= 8 {
		direct, _ = client.DecodeRDT(raw)
	}
	if len(raw) >= 12 {
		skip, _ = client.DecodeRDT(raw[4:])
	}
	s.scrapeMu.Lock()
	s.scrapeRaw = append(s.scrapeRaw, raw...)
	s.scrapeDirect = append(s.scrapeDirect, direct...)
	s.scrapeSkip4 = append(s.scrapeSkip4, skip...)
	s.scrapeN++
	s.scrapeMu.Unlock()
}

func (s *Session) scrapeChunks() int {
	s.scrapeMu.Lock()
	defer s.scrapeMu.Unlock()
	return s.scrapeN
}

func (s *Session) bestScrape() (body []byte, sizes string, total int) {
	s.scrapeMu.Lock()
	defer s.scrapeMu.Unlock()
	cands := []struct {
		name string
		data []byte
	}{
		{"framed", s.scrape},
		{"raw", s.scrapeRaw},
		{"direct", s.scrapeDirect},
		{"skip4", s.scrapeSkip4},
	}
	var parts []string
	for _, c := range cands {
		total += len(c.data)
		mark := ""
		extracted := recordings.MP4FromRDT(c.data)
		if recordings.LooksLikeMP4(extracted) {
			mark = "+ftyp"
			if len(extracted) > len(body) {
				body = extracted
			}
		}
		parts = append(parts, fmt.Sprintf("%s=%d%s", c.name, len(c.data), mark))
	}
	if body != nil {
		body = append([]byte(nil), body...)
	}
	return body, strings.Join(parts, " "), total
}

// playbackTimeout is how long to wait for the camera to say whether it found a
// file. It answers in well under a second when it answers at all.
const playbackTimeout = 5 * time.Second

// Play asks the camera to send a recording instead of the live picture.
//
// start must be a clip's exact start as Recordings gave it. This is not a
// seek: a camera holding a fortnight of continuous footage answers
// "filenotfound" for any instant that is not the first second of a file,
// including round minutes and instants the file plainly covers.
//
// The recording arrives as ordinary frames on the same path as the live
// picture, so a caller that is already reading frames need do nothing else. The
// camera stops sending live video the moment it accepts this.
//
// lenses picks a picture on the two-lens models, whose firmware wants an array
// and refuses a bare number. Leaving it empty lets the camera choose, and it
// names its choice in the status. On the CW500 that choice is always the
// primary lens: sending the channel key as an array looks up an empty index
// and is answered filenotfound. The second picture is FetchRecording.
func (s *Session) Play(start, end int64, lenses []int) (recordings.Status, error) {
	return s.playback(recordings.PlaybackRequest(s.nextPlaybackID(), start, end, lenses))
}

// StopPlayback returns the camera to the live picture.
//
// Nothing is waited for, because nothing answers: the camera treats the zero
// timestamp as a switch rather than a request, does it, and says nothing. That
// silence is the normal outcome and must not be reported as a failure.
//
// The live picture is then asked for again, which is not belt and braces. A
// CW400 resumes on its own, but a CW500 leaves playback and sends nothing at
// all -- a tile that goes black and stays black, with a session that is still
// open and still healthy, so nothing anywhere reports a fault.
func (s *Session) StopPlayback() error {
	if err := s.mediaClient().SendRaw(miss.CmdPlaybackReq, recordings.StopRequest(s.nextPlaybackID())); err != nil {
		return fmt.Errorf("stream: ask to stop playback: %w", err)
	}
	// A single-lens camera switches back to live on the zero timestamp alone.
	// Asking it to start media again on top of that is what left a CW400 in
	// playback with no way out. A dual-lens session is different: it shares one
	// connection and goes silent after playback unless both lenses are asked
	// for again.
	if s.shared != nil {
		return s.shared.resumeLive()
	}
	return nil
}

// nextPlaybackID labels a request so its answer can be told from the answer to
// the one before it, which matters because a request that is refused and one
// that is ignored look alike until the ids are compared.
func (s *Session) nextPlaybackID() int {
	return int(s.playbackID.Add(1))
}

func (s *Session) playback(body string) (recordings.Status, error) {
	drain(s.replyFeed)

	if err := s.mediaClient().SendRaw(miss.CmdPlaybackReq, body); err != nil {
		return recordings.Status{}, fmt.Errorf("stream: ask for playback: %w", err)
	}

	deadline := time.After(playbackTimeout)
	for {
		select {
		case r := <-s.replyFeed:
			if r.cmd != miss.CmdPlaybackRes {
				continue
			}
			return recordings.ParseStatus(r.body)

		case <-deadline:
			// Silence here is the camera refusing to parse the request rather
			// than failing to find the file, which it says plainly. See
			// recordings.PlaybackRequest for what it insists on.
			return recordings.Status{}, fmt.Errorf(
				"stream: no answer to the playback request after %s", playbackTimeout)

		case <-s.done:
			return recordings.Status{}, ErrClosed
		}
	}
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
