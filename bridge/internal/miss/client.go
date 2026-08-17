// Package miss implements Xiaomi's MISS ("Mi Secure Streaming") protocol.
//
// Derived from go2rtc pkg/xiaomi/miss (MIT). See bridge/NOTICE.md.
//
// MISS is the unified camera protocol Xiaomi introduced in 2020. Every command
// is a 4-byte big-endian id followed by a JSON body, wrapped in an encrypted
// envelope; media arrives as 32-byte headers followed by encrypted Annex-B or
// Opus payloads.
//
// The TUTK transport and the pre-MISS "legacy" formats are deliberately absent:
// they serve older cameras, and both the CW400 and CW500 negotiate CS2.
package miss

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"net"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/crypto"
	"github.com/spec8472/xiaomi-viewer/bridge/internal/miss/cs2"
)

// Codec ids as they appear in the media packet header.
const (
	CodecH264 = 4
	CodecH265 = 5
	CodecPCM  = 1024
	CodecPCMU = 1026
	CodecPCMA = 1027
	CodecOPUS = 1032
)

// Command ids. The playback pair (0x10D/0x10E) is listed for completeness but
// its payload schema is undocumented and nothing here uses it.
const (
	cmdAuthReq           = 0x100
	cmdAuthRes           = 0x101
	cmdVideoStart        = 0x102
	cmdVideoStop         = 0x103
	cmdAudioStart        = 0x104
	cmdAudioStop         = 0x105
	cmdSpeakerStartReq   = 0x106
	cmdSpeakerStartRes   = 0x107
	cmdSpeakerStop       = 0x108
	cmdStreamCtrlReq     = 0x109
	cmdStreamCtrlRes     = 0x10A
	cmdGetAudioFormatReq = 0x10B
	cmdGetAudioFormatRes = 0x10C
	cmdPlaybackReq       = 0x10D
	cmdPlaybackRes       = 0x10E
	cmdDevInfoReq        = 0x110
	cmdDevInfoRes        = 0x111
	cmdMotorReq          = 0x112
	cmdMotorRes          = 0x113
	cmdEncoded           = 0x1001
)

// Models whose default stream quality differs from the usual "2".
//
// Measured against real hardware by sweeping every profile and recording the
// resolution that came back (see scripts/probe-quality.ps1):
//
//	CW400  isa.camera.hlc8a   2 connects and then sends nothing at all,
//	                          3 gives 2560x1440
//	CW500  isa.camera.500dh   2 gives the 640x360 substream,
//	                          3 gives 2560x1440
//
// The CW400 behaviour matches go2rtc#2074 and go2rtc#2313.
const (
	ModelC200   = "chuangmi.camera.046c04"
	ModelC300   = "chuangmi.camera.72ac1"
	ModelHLC8   = "isa.camera.hlc8"  // CW400
	ModelHLC8A  = "isa.camera.hlc8a" // CW400, later revision
	Model500DH  = "isa.camera.500dh" // CW500, dual lens
	Model700SA  = "isa.camera.700sa" // CW700S, hybrid-zoom dual sensor
	ModelMod11  = "mxiang.camera.mod11"
	ModelMoc001 = "mxiang.camera.moc001" // CW300, China
	ModelMoc006 = "mxiang.camera.moc006" // CW300, global/EU
)

// Config is everything needed to open a session, as resolved from the cloud.
type Config struct {
	Host          string
	Transport     string // "", "udp" or "tcp"
	Model         string
	DevicePublic  string
	ClientPublic  string
	ClientPrivate string
	Sign          string
}

type Client struct {
	conn  *cs2.Conn
	key   []byte
	model string
}

// Dial performs the CS2 handshake and the MISS authentication exchange.
func Dial(cfg Config) (*Client, error) {
	key, err := crypto.CalcSharedKey(cfg.DevicePublic, cfg.ClientPrivate)
	if err != nil {
		return nil, fmt.Errorf("miss: shared key: %w", err)
	}

	conn, err := cs2.Dial(cfg.Host, cfg.Transport)
	if err != nil {
		return nil, err
	}

	if err = login(conn, cfg.ClientPublic, cfg.Sign); err != nil {
		_ = conn.Close()
		return nil, err
	}

	return &Client{conn: conn, key: key, model: cfg.Model}, nil
}

// login proves to the camera that the cloud vouched for our ephemeral key.
func login(conn *cs2.Conn, clientPublic, sign string) error {
	s := fmt.Sprintf(`{"public_key":"%s","sign":"%s","uuid":"","support_encrypt":0}`, clientPublic, sign)
	if err := conn.WriteCommand(cmdAuthReq, []byte(s)); err != nil {
		return err
	}

	_, data, err := conn.ReadCommand()
	if err != nil {
		return err
	}

	if !bytes.Contains(data, []byte(`"result":"success"`)) {
		return fmt.Errorf("miss: auth rejected: %s", data)
	}

	return nil
}

func (c *Client) Protocol() string     { return c.conn.Protocol() }
func (c *Client) RemoteAddr() net.Addr { return c.conn.RemoteAddr() }
func (c *Client) Model() string        { return c.model }
func (c *Client) Close() error         { return c.conn.Close() }

// Unhandled passes through the transport's count of data on channels this
// bridge does not use, which is a diagnostic for a command that looks ignored.
func (c *Client) Unhandled() ([4]uint64, [4][]byte) { return c.conn.Unhandled() }

// Tap passes through whatever arrived on those channels, message by message.
func (c *Client) Tap() []cs2.TapMessage { return c.conn.Tap() }
func (c *Client) SetDeadline(t time.Time) error {
	return c.conn.SetDeadline(t)
}

// writeCommand encrypts a command and sends it inside the 0x1001 envelope.
func (c *Client) writeCommand(data []byte) error {
	enc, err := crypto.Encode(data, c.key)
	if err != nil {
		return err
	}
	return c.conn.WriteCommand(cmdEncoded, enc)
}

func command(cmd uint32, body string) []byte {
	data := binary.BigEndian.AppendUint32(nil, cmd)
	return append(data, body...)
}

// StartMedia asks the camera to begin streaming.
//
// quality is "auto", "sd", "hd" or a literal "0".."5". channel selects the lens
// on multi-lens models: empty or "0" is the primary one, anything else is a
// secondary lens, which is requested by pinning the primary to -1 and setting
// videoquality2 instead.
func (c *Client) StartMedia(channel, quality string, audio bool) error {
	body := mediaStartBody(c.model, channel, quality, "", audio)
	return c.writeCommand(command(cmdVideoStart, body))
}

// StartMediaBoth asks a dual-lens camera to put both video streams on this
// connection. The camera interleaves two independent sequence-number lanes on
// the ordinary media channel; the stream package separates them before either
// decoder sees them.
func (c *Client) StartMediaBoth(primaryQuality, secondaryQuality string, audio bool) error {
	body := mediaStartBody(c.model, "both", primaryQuality, secondaryQuality, audio)
	return c.writeCommand(command(cmdVideoStart, body))
}

func mediaStartBody(model, channel, quality, secondaryQuality string, audio bool) string {
	quality = resolveQuality(model, quality)

	audioFlag := "0"
	if audio {
		audioFlag = "1"
	}

	switch channel {
	case "", "0":
		return fmt.Sprintf(`{"videoquality":%s,"enableaudio":%s}`, quality, audioFlag)
	case "both":
		secondaryQuality = resolveQuality(model, secondaryQuality)
		return fmt.Sprintf(
			`{"videoquality":%s,"videoquality2":%s,"enableaudio":%s}`,
			quality, secondaryQuality, audioFlag)
	default:
		return fmt.Sprintf(
			`{"videoquality":-1,"videoquality2":%s,"enableaudio":%s}`,
			quality, audioFlag)
	}
}

// resolveQuality maps a friendly quality name onto the numeric profile the
// camera expects. A caller that already passed a number gets it back untouched,
// which is the escape hatch for models not listed here.
func resolveQuality(model, quality string) string {
	switch quality {
	case "", "hd":
		// Most cameras call 2 their high quality. A few expose a better 3, and
		// on others 3 has broken codec parameters, so this is per-model.
		switch model {
		case ModelC200, ModelC300, ModelHLC8, ModelHLC8A, Model500DH, ModelMod11:
			return "3"
		case ModelMoc001, ModelMoc006, Model700SA:
			// A working CW300 go2rtc deployment uses its default profile, and
			// upstream also leaves the untested CW700S at that default. Keep
			// profile 2 explicit so a future fallback change does not silently
			// move either provisional model.
			return "2"
		default:
			return "2"
		}
	case "sd":
		return "1"
	case "auto":
		return "0"
	}
	return quality
}

func (c *Client) StopMedia() error {
	return c.writeCommand(command(cmdVideoStop, ""))
}

func (c *Client) StartAudio() error {
	return c.writeCommand(command(cmdAudioStart, ""))
}

// Motor operations, as accepted by the CW400 and CW500. Each one moves the lens
// a single fixed step and then stops by itself, so there is no stop operation
// and none is needed; 0 and everything above 4 do nothing at all.
//
// Named for their effect on the position the camera reports, which is not the
// direction the view moves: x grows as the lens turns left and y grows as it
// tilts down, so a caller wanting the picture to follow a button has to invert
// both axes.
const (
	MotorPanPlus   = 1 // increasing x, the view turns left
	MotorPanMinus  = 2 // decreasing x, the view turns right
	MotorTiltMinus = 3 // decreasing y, the view tilts up
	MotorTiltPlus  = 4 // increasing y, the view tilts down
)

// MotorStep moves the lens one step.
//
// go2rtc declares this command but never sends it, because it has no PTZ concept
// at all (go2rtc#2162), so there is no upstream payload to copy and none is
// documented anywhere. This shape was found by sending candidates to a CW400 and
// watching its reported position: the {"direction":...,"speed":...} form that
// third-party forks use does nothing on these models. See scripts/probe-ptz.ps1.
func (c *Client) MotorStep(operation int) error {
	return c.MotorRaw(fmt.Sprintf(`{"operation":%d}`, operation))
}

// MotorRaw sends an arbitrary motor payload, for probing a model whose accepted
// shape is not known yet.
func (c *Client) MotorRaw(body string) error {
	return c.writeCommand(command(cmdMotorReq, body))
}

// DeviceInfo asks the camera to describe itself.
func (c *Client) DeviceInfo() error {
	return c.writeCommand(command(cmdDevInfoReq, ""))
}

// SendRaw sends an arbitrary command with an arbitrary body.
//
// The commands this file names are the ones whose payloads are known, and they
// are a minority of the protocol. Working out another one means sending
// candidates to a camera and reading what comes back, which is how the motor
// command was found, so the probe needs to choose the command id as well as the
// body. See scripts/probe-playback.ps1.
func (c *Client) SendRaw(cmd uint32, body string) error {
	return c.writeCommand(command(cmd, body))
}

// SendChannel sends a command on a channel other than the command channel.
//
// The reason to want this is the SD card. Xiaomi's plugin SDK offers plugins two
// ways to reach a camera: MISS commands, which is what everything above sends,
// and a separate RDT path (sendRDTJSONCommandToDevice, bindRDTDataReceiveCallback)
// whose per-device switch is even called setCurrentDeviceUseFixedRdtChannel. RDT
// is the reliable file-transfer channel of the P2P stack CS2 is modelled on, and
// file transfer is what playback of a recording is. This bridge opens channels 0
// and 2 only, so if the answer to a playback request was ever meant to arrive on
// another channel, it has been landing somewhere nothing was listening.
//
// encrypt says whether to wrap the command the way the command channel does.
// Which of the two another channel wants is unknown, so it is the caller's
// choice; everything after authentication on channel 0 is encrypted.
func (c *Client) SendChannel(channel byte, cmd uint32, body string, encrypt bool) error {
	data := command(cmd, body)

	if encrypt {
		enc, err := crypto.Encode(data, c.key)
		if err != nil {
			return err
		}
		// The envelope carries its own command id, as on the command channel.
		data = append(binary.BigEndian.AppendUint32(nil, cmdEncoded), enc...)
	}

	return c.conn.WriteChannel(channel, data)
}

const hdrSize = 32

// Packet is one decrypted media access unit.
type Packet struct {
	CodecID   uint32
	Sequence  uint32
	Flags     uint32
	Timestamp uint64 // milliseconds
	Payload   []byte

	// Header is the 32-byte media header verbatim. Only 20 of those bytes have
	// a known meaning, and what a camera puts in the rest is worth reading when
	// a stream carries something the known fields do not describe -- which lens
	// of a dual-lens camera sent a packet, for one. See scripts/probe-lens-id.ps1.
	Header []byte
}

// SampleRate decodes the audio sample rate the flags bitfield encodes.
func (p *Packet) SampleRate() uint32 {
	if (p.Flags>>3)&0b1111 != 0 {
		return 16000
	}
	return 8000
}

// lensTagMask selects the bits of the flags word that say which lens of a
// multi-lens camera produced a packet.
//
// A dual-lens CW500 sends both lenses down one media channel and nothing
// documents how to tell them apart, so this was measured: each lens was
// captured on its own at every quality profile, which is the only situation
// where the sender is known for certain. See scripts/probe-lens-id.ps1.
//
//	profile   primary   secondary
//	0, 3-5     0x000E      0x014E
//	1, 2       0x0006      0x0146
//
// Two bits separate the lenses at every profile and neither moves when the
// encoding does, which is what makes them the lens and not the picture. The
// remaining difference, 0x0008, follows the profile: the two that give the
// 640x360 substream clear it and the 2560x1440 ones set it. Masking it off is
// the point of this constant, because a tile whose quality is overridden
// mid-session would otherwise stop matching its own lens.
const lensTagMask = 0x0140

// LensTag identifies which picture of a multi-lens camera a packet belongs to.
// The value means nothing on its own: it is only ever compared with the tag of
// a packet whose lens is known.
func (p *Packet) LensTag() uint32 { return (p.Flags >> 16) & lensTagMask }

func (c *Client) ReadPacket() (*Packet, error) {
	hdr, payload, err := c.conn.ReadPacket()
	if err != nil {
		return nil, fmt.Errorf("miss: read media: %w", err)
	}

	if len(hdr) < hdrSize {
		return nil, fmt.Errorf("miss: packet header too small")
	}

	payload, err = crypto.Decode(payload, c.key)
	if err != nil {
		return nil, err
	}

	return &Packet{
		CodecID:   binary.LittleEndian.Uint32(hdr[4:]),
		Sequence:  binary.LittleEndian.Uint32(hdr[8:]),
		Flags:     binary.LittleEndian.Uint32(hdr[12:]),
		Timestamp: binary.LittleEndian.Uint64(hdr[16:]),
		Payload:   payload,
		Header:    hdr,
	}, nil
}

// ReadCommandReply takes one message off the command channel, still wrapped.
//
// Something must keep calling this for as long as a session lives, even if the
// answers are of no interest. The command channel holds only a handful of
// messages and the transport treats a full one as fatal, so replies nobody reads
// eventually drop the connection. It fails only once the connection has ended,
// which is what makes it safe to drain in a loop.
func (c *Client) ReadCommandReply() (uint32, []byte, error) {
	return c.conn.ReadCommand()
}

// UnwrapReply decrypts a command reply and returns the inner command with its
// body, so a caller matches against a command id such as cmdMotorRes rather than
// the 0x1001 envelope everything after authentication arrives in.
func (c *Client) UnwrapReply(cmd uint32, data []byte) (uint32, []byte, error) {
	if cmd != cmdEncoded {
		// The authentication result arrives in the clear; nothing else does.
		return cmd, data, nil
	}

	plain, err := crypto.Decode(data, c.key)
	if err != nil {
		return cmd, nil, fmt.Errorf("miss: decrypt reply: %w", err)
	}
	if len(plain) < 4 {
		return cmd, nil, fmt.Errorf("miss: reply shorter than its command id")
	}

	return binary.BigEndian.Uint32(plain), plain[4:], nil
}
