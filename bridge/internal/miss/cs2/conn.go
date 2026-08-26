// Package cs2 implements Xiaomi's CS2 peer-to-peer transport.
//
// Derived from go2rtc pkg/xiaomi/miss/cs2 (MIT). See bridge/NOTICE.md.
//
// The client hole-punches to the camera on UDP 32108, after which the camera
// nominates either UDP or TCP for the session. Both then carry the same framing:
// logical channels multiplexed over "DRW" data messages, channel 0 for commands
// and channel 2 for media. UDP additionally needs per-packet acknowledgement and
// reordering, which TCP gets for free.
package cs2

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/lan"
)

func Dial(host, transport string) (*Conn, error) {
	conn, err := handshake(host, transport)
	if err != nil {
		return nil, err
	}

	_, isTCP := conn.(*tcpConn)

	c := &Conn{
		Conn:  conn,
		isTCP: isTCP,
		// Channel 0 reorders up to 250 command packets; channel 2 buffers 100
		// media packets. Channels 1 and 3 are unused here (3 is the speaker
		// backchannel, which this bridge does not implement).
		channels: [4]*dataChannel{
			newDataChannel(0, 10), nil, newDataChannel(250, 100), nil,
		},
	}
	go c.worker()
	return c, nil
}

type Conn struct {
	net.Conn
	isTCP bool

	err error

	channels [4]*dataChannel

	// Data on a channel this transport does not allocate has nowhere to go and is
	// dropped. Counting it keeps that visible, because a command whose answer
	// comes back on another channel would otherwise be indistinguishable from a
	// command the camera ignored.
	unhandled [4]atomic.Uint64
	sample    [4]atomic.Pointer[[]byte]

	tapMu   sync.Mutex
	taps    [4][]TapMessage
	tapLost [4]uint64
	tapSeen [4]uint64

	// One sequence counter per channel, since each is numbered independently.
	seqMu sync.Mutex
	seq   [4]uint16

	cmdMu  sync.Mutex
	cmdAck func()
}

// Unhandled reports, per channel, how much arrived that this transport has no
// use for, and a sample of the first such message.
func (c *Conn) Unhandled() ([4]uint64, [4][]byte) {
	var counts [4]uint64
	var samples [4][]byte
	for i := range c.unhandled {
		counts[i] = c.unhandled[i].Load()
		if s := c.sample[i].Load(); s != nil {
			samples[i] = *s
		}
	}
	return counts, samples
}

// TapMessage is one data message as it arrived on a channel this transport does
// not reassemble, kept whole rather than parsed.
type TapMessage struct {
	Channel byte
	Seq     uint16
	Data    []byte
}

// tapDepth bounds what the tap remembers per channel. A recording index runs to
// a couple of hundred messages; a second-lens MP4 is several thousand UDP
// datagrams. The pump drains every 20ms, but a LAN burst can fill the ring
// before that, and losing one chunk costs the whole file. 64 was not enough
// for an index; 4096 was not enough for the file.
const tapDepth = 16384

// Tap hands back what arrived on the channels this transport does not open,
// oldest first, and forgets it. Draining on read is what lets a probe attribute
// a message to the command it just sent.
//
// The bytes are the data message verbatim, with no framing assumed. Channels 0,
// 2 and 3 length-prefix their messages, but whether channel 1 does is exactly
// the sort of thing this is here to find out, and a wrong guess would turn one
// misread length into silence for the rest of the session.
func (c *Conn) Tap() []TapMessage {
	c.tapMu.Lock()
	defer c.tapMu.Unlock()

	var out []TapMessage
	for ch := range c.taps {
		out = append(out, c.taps[ch]...)
		c.taps[ch] = c.taps[ch][:0]
	}
	return out
}

// TapLost reports, per channel, how many messages the tap threw away because
// nobody drained it in time, and forgets them. A caller reassembling a stream
// has to know: a message lost in the middle makes every byte after it
// unreadable, and nothing about the bytes themselves says so.
func (c *Conn) TapLost() [4]uint64 {
	c.tapMu.Lock()
	defer c.tapMu.Unlock()

	lost := c.tapLost
	c.tapLost = [4]uint64{}
	return lost
}

// TapSeen reports how many messages the tap was ever handed, per channel. It
// exists to tell "the camera said nothing" apart from "something between the
// wire and the caller lost it", which no other number here distinguishes.
func (c *Conn) TapSeen() [4]uint64 {
	c.tapMu.Lock()
	defer c.tapMu.Unlock()

	return c.tapSeen
}

func (c *Conn) tap(ch byte, seq uint16, data []byte) {
	c.tapMu.Lock()
	defer c.tapMu.Unlock()

	if len(c.taps[ch]) == tapDepth {
		c.taps[ch] = append(c.taps[ch][:0], c.taps[ch][1:]...)
		c.tapLost[ch]++
	}
	c.tapSeen[ch]++
	c.taps[ch] = append(c.taps[ch], TapMessage{Channel: ch, Seq: seq, Data: bytes.Clone(data)})
}

const (
	magic        = 0xF1
	magicDrw     = 0xD1
	magicTCP     = 0x68
	msgLanSearch = 0x30
	msgPunchPkt  = 0x41
	msgP2PRdyUDP = 0x42
	msgP2PRdyTCP = 0x43
	msgDrw       = 0xD0
	msgDrwAck    = 0xD1
	msgPing      = 0xE0
	msgPong      = 0xE1
	msgClose     = 0xF0
	msgCloseAck  = 0xF1
)

// HandshakeTimeout bounds each of the two handshake exchanges. Cameras that the
// cloud has just nudged awake can take several seconds to start listening, so
// this is deliberately longer than the round trip needs.
var HandshakeTimeout = 15 * time.Second

func handshake(host, transport string) (net.Conn, error) {
	conn, err := newUDPConn(host, 32108)
	if err != nil {
		return nil, err
	}

	_ = conn.SetDeadline(time.Now().Add(HandshakeTimeout))

	// Some models (the CW400 family among them) ignore a LAN search addressed
	// to them directly and only answer the subnet broadcast, so ask both ways.
	// Replies are still filtered to the camera we actually want.
	udp := conn.(*udpConn)

	var also []*net.UDPAddr
	if b := lan.BroadcastFor(udp.addr.IP); b != nil {
		also = []*net.UDPAddr{{IP: b, Port: 32108}}
	}

	req := []byte{magic, msgLanSearch, 0, 0}
	res, err := udp.WriteUntil(req, also, func(res []byte) bool {
		return res[1] == msgPunchPkt
	})
	if err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("cs2: lan search to %s: %w", host, err)
	}

	var msgUDP, msgTCP byte

	if transport == "" || transport == "udp" {
		msgUDP = msgP2PRdyUDP
	}
	if transport == "" || transport == "tcp" {
		msgTCP = msgP2PRdyTCP
	}

	// The camera's ephemeral port is known now, so the punch is a direct
	// conversation and the broadcast would only be noise.
	res, err = udp.WriteUntil(res, nil, func(res []byte) bool {
		return res[1] == msgUDP || res[1] == msgTCP
	})
	if err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("cs2: punch to %s: %w", host, err)
	}

	_ = conn.SetDeadline(time.Time{})

	if res[1] == msgTCP {
		_ = conn.Close()
		return newTCPConn(conn.RemoteAddr().String())
	}

	return conn, nil
}

func (c *Conn) worker() {
	defer func() {
		c.channels[0].Close()
		c.channels[2].Close()
	}()

	var keepaliveTS time.Time // TCP only

	buf := make([]byte, maxCS2Frame)

	for {
		n, err := c.Conn.Read(buf)
		if err != nil {
			c.err = fmt.Errorf("cs2: %w", err)
			return
		}

		// 0  f1d0  magic
		// 2  005d  size = total size + 4
		// 4  d1    magic
		// 5  00    channel
		// 6  0000  seq
		switch buf[1] {
		case msgDrw:
			ch := buf[5]
			if int(ch) >= len(c.channels) {
				continue // not a channel this protocol has, so not ours to answer
			}

			seqHI, seqLO := buf[6], buf[7]
			seq := uint16(seqHI)<<8 | uint16(seqLO)

			channel := c.channels[ch]
			if channel == nil {
				c.unhandled[ch].Add(1)
				if c.sample[ch].Load() == nil {
					sample := append([]byte(nil), buf[8:min(n, 8+64)]...)
					c.sample[ch].Store(&sample)
				}
				c.tap(ch, seq, buf[8:n])

				// Acknowledge it even though nothing here consumes it. A sender
				// that gets no acknowledgement retries and then gives up, so
				// staying silent would make a channel that is working look like
				// one that sent a single message and stopped.
				if !c.isTCP {
					ack := []byte{magic, msgDrwAck, 0, 6, magicDrw, ch, 0, 1, seqHI, seqLO}
					_, _ = c.Conn.Write(ack)
				}
				continue
			}

			if c.isTCP {
				// The official Mi Home app pings about once a second on TCP and
				// the camera drops the session without it.
				if now := time.Now(); now.After(keepaliveTS) {
					_, _ = c.Conn.Write([]byte{magic, msgPing, 0, 0})
					keepaliveTS = now.Add(time.Second)
				}

				err = channel.Push(buf[8:n])
			} else {
				var pushed int

				pushed, err = channel.PushSeq(seq, buf[8:n])

				if pushed >= 0 {
					ack := []byte{magic, msgDrwAck, 0, 6, magicDrw, ch, 0, 1, seqHI, seqLO}
					_, _ = c.Conn.Write(ack)
				}
			}

			if err != nil {
				c.err = fmt.Errorf("cs2: %w", err)
				return
			}

		case msgPing:
			_, _ = c.Conn.Write([]byte{magic, msgPong, 0, 0})
		case msgPong, msgP2PRdyUDP, msgP2PRdyTCP, msgClose, msgCloseAck:
			// nothing to do
		case msgDrwAck: // UDP only
			if c.cmdAck != nil {
				c.cmdAck()
			}
		}
	}
}

func (c *Conn) Protocol() string {
	if c.isTCP {
		return "cs2+tcp"
	}
	return "cs2+udp"
}

func (c *Conn) Version() string {
	return "CS2"
}

func (c *Conn) Error() error {
	if c.err != nil {
		return c.err
	}
	return io.EOF
}

func (c *Conn) ReadCommand() (cmd uint32, data []byte, err error) {
	buf, ok := c.channels[0].Pop()
	if !ok {
		return 0, nil, c.Error()
	}
	if len(buf) < 4 {
		return 0, nil, fmt.Errorf("cs2: short command")
	}
	// Big-endian, to match marshalCmd on the way out. Upstream reads this one
	// field little-endian, which nothing there notices: the only reply it ever
	// reads is the authentication result, and that is recognised by searching
	// the body rather than by its command id.
	cmd = binary.BigEndian.Uint32(buf)
	data = buf[4:]
	return
}

// WriteCommand sends on the command channel. Over UDP it retries until the
// camera acknowledges, since there is no transport-level reliability.
func (c *Conn) WriteCommand(cmd uint32, data []byte) error {
	c.cmdMu.Lock()
	defer c.cmdMu.Unlock()

	req := marshalCmd(0, c.nextSeq(0), cmd, data)

	if c.isTCP {
		_, err := c.Conn.Write(req)
		return err
	}

	var repeat atomic.Int32
	repeat.Store(5)

	timeout := time.NewTicker(time.Second)
	defer timeout.Stop()

	c.cmdAck = func() {
		repeat.Store(0)
		timeout.Reset(1)
	}

	for {
		if _, err := c.Conn.Write(req); err != nil {
			return err
		}
		<-timeout.C
		r := repeat.Add(-1)
		if r < 0 {
			return nil
		}
		if r == 0 {
			return fmt.Errorf("cs2: can't send command %d", cmd)
		}
	}
}

const hdrSize = 32

func (c *Conn) ReadPacket() (hdr, payload []byte, err error) {
	data, ok := c.channels[2].Pop()
	if !ok {
		return nil, nil, c.Error()
	}
	if len(data) < hdrSize {
		return nil, nil, fmt.Errorf("cs2: short media packet")
	}
	return data[:hdrSize], data[hdrSize:], nil
}

// WriteChannel sends one data message on an arbitrary channel.
//
// Only channels 0 (commands) and 2 (media) are read here, and 3 is the speaker
// backchannel, so this exists for probing the rest: what a camera does with a
// message on a channel nothing sends on is not documented and cannot be read out
// of any implementation. See scripts/probe-rdt.ps1.
//
// Unlike WriteCommand this sends once and does not wait to be acknowledged, so
// over UDP a lost message stays lost. A probe repeats rather than relies on it,
// and the retry logic belongs to the command channel it was written for.
func (c *Conn) WriteChannel(channel byte, payload []byte) error {
	if int(channel) >= len(c.channels) {
		return fmt.Errorf("cs2: no channel %d", channel)
	}

	c.cmdMu.Lock()
	defer c.cmdMu.Unlock()

	_, err := c.Conn.Write(marshalData(channel, c.nextSeq(channel), nil, payload))
	return err
}

func (c *Conn) nextSeq(channel byte) uint16 {
	c.seqMu.Lock()
	defer c.seqMu.Unlock()

	seq := c.seq[channel]
	c.seq[channel]++
	return seq
}

// marshalCmd frames a command, which is a data message whose first four bytes
// are the command id.
func marshalCmd(channel byte, seq uint16, cmd uint32, payload []byte) []byte {
	return marshalData(channel, seq, binary.BigEndian.AppendUint32(nil, cmd), payload)
}

// marshalData frames one data message. prefix and payload are concatenated and
// counted as one message; the split exists only so a command id can be prepended
// without copying the body twice.
func marshalData(channel byte, seq uint16, prefix, payload []byte) []byte {
	size := len(prefix) + len(payload)
	req := make([]byte, 4+4+4+size)

	// 1. message header
	req[0] = magic
	req[1] = msgDrw
	binary.BigEndian.PutUint16(req[2:], uint16(4+4+size))

	// 2. data header
	req[4] = magicDrw
	req[5] = channel
	binary.BigEndian.PutUint16(req[6:], seq)

	// 3. message size, which is what the far end reassembles against
	binary.BigEndian.PutUint32(req[8:], uint32(size))

	copy(req[12:], prefix)
	copy(req[12+len(prefix):], payload)

	return req
}

func newUDPConn(host string, port int) (net.Conn, error) {
	// A raw UDPConn, because the peer address changes during the handshake when
	// the camera answers from a different port than we punched to.
	conn, err := net.ListenUDP("udp", nil)
	if err != nil {
		return nil, err
	}

	addr, err := net.ResolveUDPAddr("udp", host)
	if err != nil {
		addr = &net.UDPAddr{IP: net.ParseIP(host), Port: port}
	}

	return &udpConn{UDPConn: conn, addr: addr}, nil
}

type udpConn struct {
	*net.UDPConn
	addr *net.UDPAddr
}

func (c *udpConn) Read(b []byte) (n int, err error) {
	var addr *net.UDPAddr
	for {
		n, addr, err = c.UDPConn.ReadFromUDP(b)
		if err != nil {
			return 0, err
		}

		if addr.IP.Equal(c.addr.IP) || n >= 8 {
			return
		}
	}
}

func (c *udpConn) Write(b []byte) (n int, err error) {
	return c.UDPConn.WriteToUDP(b, c.addr)
}

func (c *udpConn) RemoteAddr() net.Addr {
	return c.addr
}

// WriteUntil retransmits req once a second until a response satisfies ok. Each
// attempt also goes to every address in also, which is how the LAN search
// reaches models that only answer a broadcast.
//
// Datagrams that do not match are skipped, which on a silent camera makes a
// timeout indistinguishable from "the reply arrived but we rejected it". The
// counters exist so the timeout error can tell those apart.
func (c *udpConn) WriteUntil(req []byte, also []*net.UDPAddr, ok func(res []byte) bool) ([]byte, error) {
	var t *time.Timer
	t = time.AfterFunc(1, func() {
		_, err := c.Write(req)
		for _, addr := range also {
			_, _ = c.UDPConn.WriteToUDP(req, addr)
		}
		if err == nil && t != nil {
			t.Reset(time.Second)
		}
	})
	defer t.Stop()

	buf := make([]byte, 1200)

	var wrongHost, tooShort, unmatched int

	for {
		n, addr, err := c.UDPConn.ReadFromUDP(buf)
		if err != nil {
			switch {
			case wrongHost > 0 && tooShort == 0 && unmatched == 0:
				// Other devices answered the broadcast, so the network and the
				// request are fine and this camera specifically is not there.
				// A stale address in the cloud's device list looks like this.
				return nil, fmt.Errorf(
					"the camera did not answer, though %d datagram(s) arrived from other "+
						"devices on the subnet, so its address has probably changed: %w",
					wrongHost, err)
			case tooShort > 0 || unmatched > 0:
				return nil, fmt.Errorf(
					"%w (the camera replied but not as expected: %d too short, %d unrecognised)",
					err, tooShort, unmatched)
			default:
				return nil, fmt.Errorf("nothing on the subnet answered: %w", err)
			}
		}

		if !addr.IP.Equal(c.addr.IP) {
			wrongHost++
			continue // another host on the LAN answering a broadcast
		}
		if n < 16 {
			tooShort++
			continue
		}

		if ok(buf[:n]) {
			c.addr.Port = addr.Port
			return buf[:n], nil
		}
		unmatched++
	}
}

func newTCPConn(addr string) (net.Conn, error) {
	conn, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		return nil, err
	}
	return &tcpConn{conn.(*net.TCPConn), bufio.NewReader(conn)}, nil
}

type tcpConn struct {
	*net.TCPConn
	rd *bufio.Reader
}

// maxCS2Frame is the TCP wrapper's uint16 length, which is also a safe UDP
// datagram bound. The worker used to read into 1200 bytes, which is a typical
// UDP packet but not a TCP one: a command-1 MP4 chunk over TCP is several
// kilobytes, and Read then returned "tcp buffer too small" and tore the
// session down. The empty ack had already been delivered, so FetchRecording
// waited out the timeout and reported the file missing.
const maxCS2Frame = 65535

func (c *tcpConn) Read(p []byte) (n int, err error) {
	tmp := make([]byte, 8)
	if _, err = io.ReadFull(c.rd, tmp); err != nil {
		return
	}
	n = int(binary.BigEndian.Uint16(tmp))
	if len(p) < n {
		return 0, fmt.Errorf("cs2: tcp buffer too small")
	}
	_, err = io.ReadFull(c.rd, p[:n])
	return
}

func (c *tcpConn) Write(req []byte) (n int, err error) {
	n = len(req)
	buf := make([]byte, 8+n)
	binary.BigEndian.PutUint16(buf, uint16(n))
	buf[2] = magicTCP
	copy(buf[8:], req)
	_, err = c.TCPConn.Write(buf)
	return
}

func newDataChannel(pushSize, popSize int) *dataChannel {
	c := &dataChannel{}
	if pushSize > 0 {
		c.pushBuf = make(map[uint16][]byte, pushSize)
		c.pushSize = pushSize
	}
	if popSize >= 0 {
		c.popBuf = make(chan []byte, popSize)
	}
	return c
}

// dataChannel reassembles one logical stream. Messages are length-prefixed and
// may straddle or share transport packets, so bytes are accumulated until a
// whole message is available.
type dataChannel struct {
	waitSeq  uint16
	pushBuf  map[uint16][]byte
	pushSize int

	waitData []byte
	waitSize int
	popBuf   chan []byte

	closeOnce sync.Once
}

func (c *dataChannel) Push(b []byte) error {
	c.waitData = append(c.waitData, b...)

	for len(c.waitData) > 4 {
		if c.waitSize == 0 {
			c.waitSize = int(binary.BigEndian.Uint32(c.waitData))
			c.waitData = c.waitData[4:]
		}
		if c.waitSize > len(c.waitData) {
			break
		}

		select {
		case c.popBuf <- bytes.Clone(c.waitData[:c.waitSize]):
		default:
			return fmt.Errorf("pop buffer is full")
		}

		c.waitData = c.waitData[c.waitSize:]
		c.waitSize = 0
	}
	return nil
}

func (c *dataChannel) Pop() ([]byte, bool) {
	data, ok := <-c.popBuf
	return data, ok
}

func (c *dataChannel) Close() {
	c.closeOnce.Do(func() { close(c.popBuf) })
}

// PushSeq returns how many sequence numbers were consumed, 0 if the packet was
// buffered for later or already seen, and -1 if it could not be buffered.
func (c *dataChannel) PushSeq(seq uint16, data []byte) (int, error) {
	diff := int16(seq - c.waitSeq)

	// Arrived early: hold it until the gap fills.
	if diff > 0 {
		if c.pushSize == 0 {
			return -1, nil
		}
		if c.pushBuf[seq] == nil {
			if len(c.pushBuf) == c.pushSize {
				return -1, nil
			}
			c.pushBuf[seq] = bytes.Clone(data)
		}
		return 0, nil
	}

	// Arrived late: already processed.
	if diff < 0 {
		return 0, nil
	}

	for i := 1; ; i++ {
		if err := c.Push(data); err != nil {
			return i, err
		}
		c.waitSeq++
		if data = c.pushBuf[c.waitSeq]; data != nil {
			delete(c.pushBuf, c.waitSeq)
		} else {
			return i, nil
		}
	}
}
