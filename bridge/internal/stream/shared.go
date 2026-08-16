package stream

import (
	"bytes"
	"fmt"
	"sync"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/miss"
)

// SharedPool keeps one physical MISS connection behind the two logical handles
// used by a dual-lens camera's grid tiles.
type SharedPool struct {
	mu      sync.Mutex
	entries map[string]*sharedPhysical
}

func NewSharedPool() *SharedPool {
	return &SharedPool{entries: make(map[string]*sharedPhysical)}
}

// Attach reuses an existing physical connection. The bool is false when the
// caller must obtain fresh cloud key material and call Open instead.
func (p *SharedPool) Attach(key string, cfg Config) (*Session, bool, error) {
	p.mu.Lock()
	physical := p.entries[key]
	p.mu.Unlock()
	if physical == nil {
		return nil, false, nil
	}

	session, err := physical.attach(cfg)
	return session, true, err
}

// Open creates a physical connection, or attaches if another goroutine won the
// race while this caller was obtaining its per-session keys from the cloud.
func (p *SharedPool) Open(key string, cfg Config) (*Session, error) {
	p.mu.Lock()
	defer p.mu.Unlock()

	if physical := p.entries[key]; physical != nil {
		return physical.attach(cfg)
	}

	physical, session, err := newSharedPhysical(p, key, cfg)
	if err != nil {
		return nil, err
	}
	p.entries[key] = physical
	physical.start()
	return session, nil
}

func (p *SharedPool) remove(key string, physical *sharedPhysical) {
	p.mu.Lock()
	if p.entries[key] == physical {
		delete(p.entries, key)
	}
	p.mu.Unlock()
}

type sharedPhysical struct {
	pool   *SharedPool
	key    string
	client *miss.Client

	mu        sync.Mutex
	upgradeMu sync.Mutex
	sessions  map[*Session]int
	quality   [2]string
	initial   int
	dual      bool
	closed    bool

	// The tag the initial lens's pictures carry, learnt from the stream rather
	// than assumed, and the anchor everything else is told apart from.
	initialLensTag  uint32
	initialTagKnown bool

	wantAudio     bool
	audioSeen     bool
	audioAsked    bool
	audioDeadline time.Time

	firstVideo     chan struct{}
	firstVideoOnce sync.Once
	ended          chan struct{}
	endOnce        sync.Once
	shutdownOnce   sync.Once

	headerMu  sync.Mutex
	headerLog []headerSample
}

// headerSample is one video packet's media header kept verbatim, with the lens
// it was routed to. The field that names the lens was found by reading whole
// headers this way, against a capture of each lens on its own, and the next
// multi-lens model has to be worked out the same way. Keeping the capture also
// makes the routing checkable against hardware rather than only against its own
// output. See scripts/probe-lens-id.ps1.
type headerSample struct {
	header []byte
	lane   int
}

// headerLogDepth is a few seconds of two interleaved 25 fps streams, which is
// all it takes to see whether a field is constant per lens.
const headerLogDepth = 512

func (p *sharedPhysical) recordHeader(header []byte, lane int) {
	if len(header) == 0 {
		return
	}

	p.headerMu.Lock()
	defer p.headerMu.Unlock()

	if len(p.headerLog) == headerLogDepth {
		p.headerLog = append(p.headerLog[:0], p.headerLog[1:]...)
	}
	// Cloned because the header is a window onto the whole packet, and keeping
	// the window alive would keep every captured payload alive with it.
	p.headerLog = append(p.headerLog, headerSample{header: bytes.Clone(header), lane: lane})
}

// MediaHeaders hands back the captured headers, oldest first, and forgets them.
func (p *sharedPhysical) MediaHeaders() []string {
	p.headerMu.Lock()
	defer p.headerMu.Unlock()

	out := make([]string, len(p.headerLog))
	for i, s := range p.headerLog {
		out[i] = fmt.Sprintf("lane %d: %x", s.lane, s.header)
	}
	p.headerLog = p.headerLog[:0]
	return out
}

func newSharedPhysical(
	pool *SharedPool, key string, cfg Config,
) (*sharedPhysical, *Session, error) {
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
		return nil, nil, err
	}

	if err = client.StartMedia(cfg.Channel, cfg.Quality, cfg.Audio); err != nil {
		_ = client.Close()
		return nil, nil, fmt.Errorf("stream: start media: %w", err)
	}

	lane := lensLane(cfg.Channel)
	physical := &sharedPhysical{
		pool:          pool,
		key:           key,
		client:        client,
		sessions:      make(map[*Session]int),
		initial:       lane,
		wantAudio:     cfg.Audio,
		audioSeen:     !cfg.Audio,
		audioDeadline: time.Now().Add(audioGrace),
		firstVideo:    make(chan struct{}),
		ended:         make(chan struct{}),
	}
	physical.quality[lane] = cfg.Quality

	session := physical.newSession(cfg, lane)
	physical.sessions[session] = lane

	return physical, session, nil
}

func (p *sharedPhysical) start() {
	go p.reader()
	go p.commandReader()
}

func (p *sharedPhysical) newSession(cfg Config, lane int) *Session {
	return &Session{
		shared:     p,
		wantAudio:  cfg.Audio,
		frames:     make(chan *Frame, queueDepth),
		done:       make(chan struct{}),
		Protocol:   p.client.Protocol(),
		RemoteAddr: p.client.RemoteAddr().String(),
	}
}

const dualAttachTimeout = 5 * time.Second

func (p *sharedPhysical) attach(cfg Config) (*Session, error) {
	p.upgradeMu.Lock()
	defer p.upgradeMu.Unlock()

	lane := lensLane(cfg.Channel)

	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return nil, ErrClosed
	}

	oppositePresent := false
	for _, existingLane := range p.sessions {
		if existingLane != lane {
			oppositePresent = true
			break
		}
	}
	needsUpgrade := !p.dual && oppositePresent
	p.mu.Unlock()

	// Only the first lens is streaming until the combined command goes out, so
	// a packet seen before it is the one thing that identifies that lens's tag
	// beyond doubt. Waiting for one makes the interleaving that follows
	// deterministic even when the two app workers connect at the same moment.
	if needsUpgrade {
		select {
		case <-p.firstVideo:
		case <-p.ended:
			return nil, ErrClosed
		case <-time.After(dualAttachTimeout):
			return nil, fmt.Errorf("stream: no video arrived before dual-lens upgrade")
		}
	}

	p.mu.Lock()
	if p.closed {
		p.mu.Unlock()
		return nil, ErrClosed
	}

	session := p.newSession(cfg, lane)
	p.sessions[session] = lane

	oldQuality := p.quality[lane]
	oldWantAudio := p.wantAudio
	p.quality[lane] = cfg.Quality
	p.wantAudio = p.anyWantAudioLocked()

	if cfg.Audio && !oldWantAudio {
		p.audioSeen = false
		p.audioAsked = false
		p.audioDeadline = time.Now().Add(audioGrace)
	}

	sendCombined := p.dual && (oldQuality != cfg.Quality || oldWantAudio != p.wantAudio)
	if needsUpgrade {
		p.dual = true
		sendCombined = true
	}

	primaryQuality := p.quality[0]
	secondaryQuality := p.quality[1]
	audio := p.wantAudio
	p.mu.Unlock()

	if sendCombined {
		if err := p.client.StartMediaBoth(primaryQuality, secondaryQuality, audio); err != nil {
			p.mu.Lock()
			delete(p.sessions, session)
			p.wantAudio = p.anyWantAudioLocked()
			if !p.hasBothLanesLocked() {
				p.dual = false
			}
			p.mu.Unlock()
			session.end(err)
			return nil, fmt.Errorf("stream: start both lenses: %w", err)
		}
	}

	return session, nil
}

func (p *sharedPhysical) reader() {
	for {
		_ = p.client.SetDeadline(time.Now().Add(10 * time.Second))

		packet, err := p.client.ReadPacket()
		if err != nil {
			p.fail(err)
			return
		}

		frame := toFrame(packet)
		if frame == nil {
			continue
		}

		askAudio := false

		p.mu.Lock()
		if p.closed {
			p.mu.Unlock()
			return
		}

		if frame.Kind == KindAudio {
			p.audioSeen = true
			for session := range p.sessions {
				if session.wantAudio {
					session.enqueue(frame)
				}
			}
		} else {
			lane := p.laneForLocked(packet.LensTag())
			p.recordHeader(packet.Header, lane)
			for session, sessionLane := range p.sessions {
				if sessionLane == lane {
					session.enqueue(frame)
				}
			}
		}

		if p.wantAudio && !p.audioSeen && !p.audioAsked &&
			time.Now().After(p.audioDeadline) {
			p.audioAsked = true
			askAudio = true
			for session := range p.sessions {
				if session.wantAudio {
					session.audioAsked.Store(true)
				}
			}
		}
		p.mu.Unlock()

		if askAudio {
			_ = p.client.StartAudio()
		}
	}
}

func (p *sharedPhysical) commandReader() {
	for {
		cmd, data, err := p.client.ReadCommandReply()
		if err != nil {
			return
		}

		inner, body, unwrapErr := p.client.UnwrapReply(cmd, data)

		p.mu.Lock()
		for session := range p.sessions {
			session.replyCount.Add(1)
			if unwrapErr == nil {
				r := reply{cmd: inner, body: body}
				session.lastReply.Store(&r)
				session.logReply(r)
			}
		}
		p.mu.Unlock()
	}
}

// laneForLocked decides which lens sent a packet, from the tag the camera puts
// in the header of every one.
//
// Only the lens that was asked for can be streaming before the combined command
// is sent, so the first tag to arrive is that lens's and is anchored here rather
// than assumed. After the upgrade a packet belongs to that lens if it carries
// that tag and to the other lens if it does not. Waiting for the anchor is what
// the firstVideo handshake in attach is for.
//
// This used to be inferred from the sequence numbers instead, on the reasoning
// that each lens's counter advances by one while the other is far away. It held
// for most packets and quietly failed for a few per minute, which is worse than
// failing outright: a stray access unit decoded into the wrong tile, and a stray
// keyframe left the wrong lens on screen until the next real one.
func (p *sharedPhysical) laneForLocked(tag uint32) int {
	if !p.initialTagKnown {
		p.initialLensTag = tag
		p.initialTagKnown = true
		p.firstVideoOnce.Do(func() { close(p.firstVideo) })
	}

	if tag == p.initialLensTag {
		return p.initial
	}
	return 1 - p.initial
}

func (p *sharedPhysical) detach(session *Session) {
	p.mu.Lock()
	if _, exists := p.sessions[session]; !exists {
		p.mu.Unlock()
		return
	}
	delete(p.sessions, session)
	session.end(nil)
	p.wantAudio = p.anyWantAudioLocked()
	last := len(p.sessions) == 0
	if last {
		p.closed = true
	}
	p.mu.Unlock()

	if last {
		p.finish()
	}
}

func (p *sharedPhysical) fail(err error) {
	p.mu.Lock()
	p.closed = true
	for session := range p.sessions {
		session.end(err)
	}
	p.mu.Unlock()
	p.finish()
}

func (p *sharedPhysical) finish() {
	p.endOnce.Do(func() { close(p.ended) })
	p.pool.remove(p.key, p)
	p.shutdownOnce.Do(func() {
		_ = p.client.StopMedia()
		_ = p.client.Close()
	})
}

func (p *sharedPhysical) anyWantAudioLocked() bool {
	for session := range p.sessions {
		if session.wantAudio {
			return true
		}
	}
	return false
}

func (p *sharedPhysical) hasBothLanesLocked() bool {
	var lanes [2]bool
	for _, lane := range p.sessions {
		lanes[lane] = true
	}
	return lanes[0] && lanes[1]
}

func lensLane(channel string) int {
	if channel == "" || channel == "0" {
		return 0
	}
	return 1
}
