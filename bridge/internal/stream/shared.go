package stream

import (
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

type sequenceTrack struct {
	valid bool
	last  uint32
}

type sharedPhysical struct {
	pool   *SharedPool
	key    string
	client *miss.Client

	mu        sync.Mutex
	upgradeMu sync.Mutex
	sessions  map[*Session]int
	quality   [2]string
	tracks    [2]sequenceTrack
	initial   int
	dual      bool
	closed    bool

	wantAudio     bool
	audioSeen     bool
	audioAsked    bool
	audioDeadline time.Time

	firstVideo     chan struct{}
	firstVideoOnce sync.Once
	ended          chan struct{}
	endOnce        sync.Once
	shutdownOnce   sync.Once
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

	// The packets sent before the combined command identify the first lens's
	// sequence lane. Waiting for one makes the later interleaving deterministic
	// even when the two app workers connect at almost exactly the same time.
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
			lane := p.logicalLaneLocked(frame.Sequence)
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

func (p *sharedPhysical) logicalLaneLocked(sequence uint32) int {
	lane := p.routeLocked(sequence)
	if p.dual {
		// Measured on isa.camera.500dh: switching from either single-lens
		// request to the combined command reverses which picture owns the
		// anchored sequence lane. Keep the logical channel stable for the UI.
		return 1 - lane
	}
	return lane
}

// routeLocked separates the two sequence-number lanes observed in a combined
// CW500 stream. Before the upgrade, every packet belongs to the known initial
// lens. Afterwards, each lane normally advances by one and the wrong lane is
// thousands of sequence numbers away.
func (p *sharedPhysical) routeLocked(sequence uint32) int {
	if !p.dual {
		p.observeLocked(p.initial, sequence)
		return p.initial
	}

	switch {
	case p.tracks[0].valid && !p.tracks[1].valid:
		if sequence-p.tracks[0].last <= maxInitialSequenceGap {
			p.observeLocked(0, sequence)
			return 0
		}
		p.observeLocked(1, sequence)
		return 1
	case !p.tracks[0].valid && p.tracks[1].valid:
		if sequence-p.tracks[1].last <= maxInitialSequenceGap {
			p.observeLocked(1, sequence)
			return 1
		}
		p.observeLocked(0, sequence)
		return 0
	case !p.tracks[0].valid && !p.tracks[1].valid:
		p.observeLocked(p.initial, sequence)
		return p.initial
	}

	distance0 := sequence - p.tracks[0].last
	distance1 := sequence - p.tracks[1].last
	lane := 0
	if distance1 < distance0 {
		lane = 1
	}
	p.observeLocked(lane, sequence)
	return lane
}

// CS2 reorders and retransmits before ReadPacket returns, so consecutive video
// access units from one lens advance by exactly one. A wider window can mistake
// the other lens's randomly seeded counter for this one when the seeds happen
// to be close, permanently swapping the two tiles.
const maxInitialSequenceGap uint32 = 1

func (p *sharedPhysical) observeLocked(lane int, sequence uint32) {
	p.tracks[lane] = sequenceTrack{valid: true, last: sequence}
	if lane == p.initial {
		p.firstVideoOnce.Do(func() { close(p.firstVideo) })
	}
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
