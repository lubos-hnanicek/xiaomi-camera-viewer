package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/cloud"
	"github.com/spec8472/xiaomi-viewer/bridge/internal/lan"
	"github.com/spec8472/xiaomi-viewer/bridge/internal/stream"
)

// Version is reported by xmb_version and stamped into the app's about box. It is
// set at link time from the project version by scripts/build-bridge.ps1, so it is
// a var rather than a const; the fallback only shows up in a bare `go build`.
var Version = "0.0.0-dev"

var (
	registry    = cloud.NewRegistry()
	dualStreams = stream.NewSharedPool()

	// The login conversation spans several calls, but the region is only known
	// at the start, so it is parked here until the account can be completed.
	pendingMu     sync.Mutex
	pendingRegion string
)

// --- Response helpers -------------------------------------------------------

func okResponse(fields map[string]any) []byte {
	if fields == nil {
		fields = map[string]any{}
	}
	fields["ok"] = true
	b, err := json.Marshal(fields)
	if err != nil {
		return errResponse(err)
	}
	return b
}

func errResponse(err error) []byte {
	b, marshalErr := json.Marshal(map[string]any{"ok": false, "error": err.Error()})
	if marshalErr != nil {
		// Last resort: hand back something the caller can still parse.
		return []byte(`{"ok":false,"error":"bridge: failed to encode error"}`)
	}
	return b
}

func errString(format string, args ...any) []byte {
	return errResponse(fmt.Errorf(format, args...))
}

// permanentError marks a failure that retrying cannot fix, so the caller can
// stop reconnecting and leave the reason on screen.
func permanentError(format string, args ...any) []byte {
	b, err := json.Marshal(map[string]any{
		"ok":        false,
		"error":     fmt.Sprintf(format, args...),
		"permanent": true,
	})
	if err != nil {
		return errString(format, args...)
	}
	return b
}

// --- Control plane ----------------------------------------------------------

// handleCall dispatches one control-plane request. It never returns an error;
// failures are encoded into the JSON response so the C side has one code path.
func handleCall(method string, request []byte) []byte {
	if len(request) == 0 {
		request = []byte("{}")
	}

	switch method {
	case "login.begin":
		return handleLoginBegin(request)
	case "login.captcha":
		return handleLoginCaptcha(request)
	case "login.verify":
		return handleLoginVerify(request)
	case "login.token":
		return handleLoginToken(request)
	case "account.forget":
		return handleAccountForget(request)
	case "device.list":
		return handleDeviceList(request)
	case "miot.get":
		return handleMiotGet(request)
	case "miot.set":
		return handleMiotSet(request)
	case "miot.action":
		return handleMiotAction(request)
	case "cloud.raw":
		return handleCloudRaw(request)
	default:
		return errString("bridge: unknown method %q", method)
	}
}

func handleLoginBegin(request []byte) []byte {
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
		Region   string `json:"region"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}
	if req.Username == "" || req.Password == "" {
		return errString("bridge: username and password are required")
	}

	pendingMu.Lock()
	pendingRegion = req.Region
	pendingMu.Unlock()

	return loginResult(registry.BeginLogin(req.Username, req.Password))
}

func handleLoginCaptcha(request []byte) []byte {
	var req struct {
		Code string `json:"code"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}
	return loginResult(registry.SubmitCaptcha(req.Code))
}

func handleLoginVerify(request []byte) []byte {
	var req struct {
		Ticket string `json:"ticket"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}
	return loginResult(registry.SubmitVerify(req.Ticket))
}

// loginResult maps the three possible outcomes of a login step onto one shape.
// A challenge is not a failure, so captcha and verification are reported as
// successful calls with a status the caller acts on.
func loginResult(err error) []byte {
	var challenge *cloud.LoginError
	if errors.As(err, &challenge) {
		switch {
		case len(challenge.Captcha) > 0:
			return okResponse(map[string]any{
				"status":      "captcha",
				"captcha_png": base64.StdEncoding.EncodeToString(challenge.Captcha),
			})
		default:
			return okResponse(map[string]any{
				"status":       "verify",
				"verify_phone": challenge.VerifyPhone,
				"verify_email": challenge.VerifyEmail,
			})
		}
	}

	if err != nil {
		return errResponse(err)
	}

	pendingMu.Lock()
	region := pendingRegion
	pendingMu.Unlock()

	acc, err := registry.CompletePending(region)
	if err != nil {
		return errResponse(err)
	}

	return okResponse(map[string]any{
		"status": "success",
		"account": map[string]any{
			"user_id": acc.UserID,
			"token":   acc.Token,
			"region":  acc.Region,
		},
	})
}

func handleLoginToken(request []byte) []byte {
	var req struct {
		UserID string `json:"user_id"`
		Region string `json:"region"`
		Token  string `json:"token"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}
	if req.UserID == "" || req.Token == "" {
		return errString("bridge: user_id and token are required")
	}

	acc, err := registry.Restore(req.UserID, req.Region, req.Token)
	if err != nil {
		return errResponse(err)
	}

	return okResponse(map[string]any{
		"status": "success",
		"account": map[string]any{
			"user_id": acc.UserID,
			"token":   acc.Token,
			"region":  acc.Region,
		},
	})
}

func handleAccountForget(request []byte) []byte {
	var req struct {
		UserID string `json:"user_id"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}
	registry.Forget(req.UserID)
	return okResponse(nil)
}

func handleDeviceList(request []byte) []byte {
	var req struct {
		UserID string `json:"user_id"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}

	acc, err := registry.Get(req.UserID)
	if err != nil {
		return errResponse(err)
	}

	devices, err := acc.Devices()
	if err != nil {
		return errResponse(err)
	}

	list := make([]map[string]any, 0, len(devices))
	for _, d := range devices {
		list = append(list, map[string]any{
			"did":   d.Did,
			"name":  d.Name,
			"model": d.Model,
			"ip":    d.IP,
			"mac":   d.MAC,
		})
	}

	return okResponse(map[string]any{"devices": list})
}

// --- MIoT -------------------------------------------------------------------

type miotRequest struct {
	UserID string `json:"user_id"`
	Did    string `json:"did"`
	Props  []struct {
		Siid  int `json:"siid"`
		Piid  int `json:"piid"`
		Value any `json:"value"`
	} `json:"props"`
	Siid int   `json:"siid"`
	Aiid int   `json:"aiid"`
	In   []any `json:"in"`
}

func (r *miotRequest) props() []cloud.MiotProp {
	out := make([]cloud.MiotProp, len(r.Props))
	for i, p := range r.Props {
		out[i] = cloud.MiotProp{Siid: p.Siid, Piid: p.Piid, Value: p.Value}
	}
	return out
}

func parseMiot(request []byte) (*cloud.Account, *miotRequest, error) {
	var req miotRequest
	if err := json.Unmarshal(request, &req); err != nil {
		return nil, nil, err
	}
	if req.Did == "" {
		return nil, nil, errors.New("bridge: did is required")
	}
	acc, err := registry.Get(req.UserID)
	if err != nil {
		return nil, nil, err
	}
	return acc, &req, nil
}

// handleCloudRaw signs an arbitrary IoT API call. The escape hatch for the
// cloud, matching miss.raw for the camera: most of Xiaomi's endpoints are
// undocumented, and the only way to learn what one answers is to call it.
func handleCloudRaw(request []byte) []byte {
	var req struct {
		UserID string `json:"user_id"`
		Path   string `json:"path"`
		Params string `json:"params"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}
	if req.Path == "" {
		return errString("bridge: path is required")
	}

	acc, err := registry.Get(req.UserID)
	if err != nil {
		return errResponse(err)
	}

	res, err := acc.Raw(req.Path, req.Params)
	if err != nil {
		return errResponse(err)
	}
	return okResponse(map[string]any{"result": json.RawMessage(res)})
}

func handleMiotGet(request []byte) []byte {
	acc, req, err := parseMiot(request)
	if err != nil {
		return errResponse(err)
	}
	res, err := acc.MiotGet(req.Did, req.props())
	if err != nil {
		return errResponse(err)
	}
	return okResponse(map[string]any{"result": json.RawMessage(res)})
}

func handleMiotSet(request []byte) []byte {
	acc, req, err := parseMiot(request)
	if err != nil {
		return errResponse(err)
	}
	res, err := acc.MiotSet(req.Did, req.props())
	if err != nil {
		return errResponse(err)
	}
	return okResponse(map[string]any{"result": json.RawMessage(res)})
}

func handleMiotAction(request []byte) []byte {
	acc, req, err := parseMiot(request)
	if err != nil {
		return errResponse(err)
	}
	res, err := acc.MiotAction(req.Did, req.Siid, req.Aiid, req.In)
	if err != nil {
		return errResponse(err)
	}
	return okResponse(map[string]any{"result": json.RawMessage(res)})
}

// --- Media plane ------------------------------------------------------------

type openRequest struct {
	UserID    string `json:"user_id"`
	Did       string `json:"did"`
	Model     string `json:"model"`
	IP        string `json:"ip"`
	Channel   string `json:"channel"`
	Quality   string `json:"quality"`
	Transport string `json:"transport"`
	Audio     bool   `json:"audio"`
}

// openStream resolves the cloud key material and brings up a media session.
func openStream(request []byte) (*stream.Session, []byte) {
	var req openRequest
	if err := json.Unmarshal(request, &req); err != nil {
		return nil, errResponse(err)
	}
	if req.Did == "" {
		return nil, errString("bridge: did is required")
	}

	acc, err := registry.Get(req.UserID)
	if err != nil {
		return nil, errResponse(err)
	}

	// The caller's address is the one saved when the camera was added, and for a
	// camera the cloud had no address for at the time it is 0.0.0.0 forever
	// after. Those are located on the network instead.
	host := req.IP
	if !hasAddress(host) {
		host, err = locateOnLAN(acc, req.Did)
		if err != nil {
			return nil, errResponse(err)
		}
	}

	// Battery-powered doorbells sleep and must be woken over the cloud before
	// they will answer a peer-to-peer handshake.
	if strings.Contains(req.Model, ".cateye.") {
		_ = acc.WakeUp(req.Did)
	}

	cfg := stream.Config{
		Host:      host,
		Transport: req.Transport,
		Model:     req.Model,
		Channel:   req.Channel,
		Quality:   req.Quality,
		Audio:     req.Audio,
	}

	dual := isDualLensModel(req.Model)
	sharedKey := ""
	if dual {
		sharedKey = strings.Join(
			[]string{req.UserID, req.Did, host, req.Transport, req.Model}, "\x00")
		if s, found, attachErr := dualStreams.Attach(sharedKey, cfg); found {
			if attachErr != nil {
				return nil, errString("bridge: shared dual-lens stream: %s", attachErr)
			}
			return s, streamOpenResponse(s, "cs2", host, true)
		}
	}

	sess, err := acc.MissVendor(req.Did)
	if err != nil {
		return nil, errResponse(err)
	}
	if sess.Vendor != "cs2" {
		return nil, permanentError(
			"camera negotiated the %q transport, which this build does not implement",
			sess.Vendor)
	}

	cfg.DevicePublic = sess.DevicePublic
	cfg.ClientPublic = sess.ClientPublic
	cfg.ClientPrivate = sess.ClientPrivate
	cfg.Sign = sess.Sign

	var s *stream.Session
	if dual {
		s, err = dualStreams.Open(sharedKey, cfg)
	} else {
		s, err = stream.Open(cfg)
	}
	if err != nil {
		return nil, errString("bridge: vendor %s: %s", sess.Vendor, err)
	}

	return s, streamOpenResponse(s, sess.Vendor, host, dual)
}

func streamOpenResponse(s *stream.Session, vendor, host string, shared bool) []byte {
	return okResponse(map[string]any{
		"protocol":    s.Protocol,
		"remote_addr": s.RemoteAddr,
		"vendor":      vendor,
		"host":        host,
		"shared":      shared,
	})
}

func isDualLensModel(model string) bool {
	model = strings.ToLower(model)
	return strings.Contains(model, ".hlmax") ||
		strings.Contains(model, "500dh") ||
		strings.Contains(model, "cw500")
}

// lanSearchTimeout bounds the broadcast search for a camera the cloud has no
// address for. Cameras answer within a second, so this is mostly patience for a
// device that is booting.
const lanSearchTimeout = 8 * time.Second

// locateOnLAN finds a camera the caller has no usable address for, using what
// the cloud currently says about it. That is worth re-reading even though the
// caller has just been handed 0.0.0.0: the caller's address is a cached one,
// saved when the camera was added, and the cloud may well have learnt where the
// camera is since. The result is not cached in turn, because a camera whose
// address had to be searched for is exactly the one likely to move again.
func locateOnLAN(acc *cloud.Account, did string) (string, error) {
	devices, err := acc.Devices()
	if err != nil {
		return "", fmt.Errorf("bridge: looking up where the camera is: %w", err)
	}

	var dev *cloud.Device
	for _, d := range devices {
		if d.Did == did {
			dev = d
			break
		}
	}
	if dev == nil {
		return "", fmt.Errorf("bridge: the cloud no longer lists this camera")
	}
	if dev.MAC == "" && !hasAddress(dev.IP) {
		return "", fmt.Errorf(
			"bridge: the cloud has neither an address nor a MAC for this camera")
	}

	ip, err := lan.Find(net.ParseIP(dev.IP), dev.MAC, lanSearchTimeout)
	if err != nil {
		cloudIP := "none"
		if hasAddress(dev.IP) {
			cloudIP = dev.IP
		}
		return "", fmt.Errorf(
			"bridge: could not find this camera on the local network "+
				"(cloud address %s, MAC %s): %w", cloudIP, dev.MAC, err)
	}

	return ip.String(), nil
}

// hasAddress reports whether the cloud gave a real address rather than the
// 0.0.0.0 it hands back for a camera it has not heard from.
func hasAddress(ip string) bool {
	return ip != "" && ip != "0.0.0.0"
}

// handleStreamCommand runs an in-band command against a live session.
func handleStreamCommand(s *stream.Session, request []byte) []byte {
	var req struct {
		Method    string `json:"method"`
		Direction string `json:"direction"`
		Body      string `json:"body"`
		Cmd       uint32 `json:"cmd"`
		Channel   byte   `json:"channel"`
		Encrypt   *bool  `json:"encrypt"`
		Envelope  *bool  `json:"envelope"`
	}
	if err := json.Unmarshal(request, &req); err != nil {
		return errResponse(err)
	}

	switch req.Method {
	// One step per call. The camera has no notion of a movement that continues
	// until stopped, so there is no matching ptz.stop.
	case "ptz.step":
		if err := s.Step(req.Direction); err != nil {
			return errResponse(err)
		}
		return okResponse(nil)

	// A diagnostic: sends an arbitrary motor payload so a candidate shape can be
	// tried against real hardware without a rebuild. Payloads vary by model and
	// none of them are documented, so this stays available for the next model
	// that needs working out. See scripts/probe-ptz.ps1.
	case "ptz.raw":
		if err := s.Motor(req.Body); err != nil {
			return errResponse(err)
		}
		return okResponse(nil)

	// The same escape hatch one level lower: the command id is the probe's to
	// choose. Most of the protocol is undocumented, and the only way to learn
	// what a command wants is to send candidates to a camera and read what comes
	// back with the replies command. See scripts/probe-playback.ps1.
	case "miss.raw":
		if err := s.Raw(req.Cmd, req.Body); err != nil {
			return errResponse(err)
		}
		return okResponse(nil)

	// The same again, one level lower still: the transport channel is the
	// probe's to choose as well. Channel 0 carries commands and 2 carries media;
	// what the other two do is the open question behind SD card playback, and
	// nothing can answer it without sending on them. Encryption defaults to on,
	// as it is on the command channel. See scripts/probe-rdt.ps1.
	case "miss.channel":
		encrypt := req.Encrypt == nil || *req.Encrypt
		// The command channel's envelope is the default because that is what
		// this bridge speaks everywhere else, but the RDT channel does without
		// one, so a probe has to be able to say so.
		envelope := req.Envelope == nil || *req.Envelope
		if err := s.RawChannel(req.Channel, req.Cmd, req.Body, encrypt, envelope); err != nil {
			return errResponse(err)
		}
		return okResponse(nil)

	case "replies":
		return okResponse(map[string]any{
			"replies":   s.Replies(),
			"unhandled": s.Unhandled(),
			"tap":       s.Tap(),
		})

	// A diagnostic for dual-lens cameras: the media headers as they arrived,
	// each with the lens the session routed it to. This is what says whether a
	// tile is being fed its own lens and nothing else, which no amount of
	// reasoning about the routing can establish. See scripts/probe-lens-id.ps1.
	case "media.headers":
		return okResponse(map[string]any{"headers": s.MediaHeaders()})

	case "stats":
		st := s.Stats()
		return okResponse(map[string]any{
			"frames":      st.Frames,
			"bytes":       st.Bytes,
			"dropped":     st.Dropped,
			"replies":     st.Replies,
			"last_reply":  st.LastReply,
			"audio_asked": st.AudioAsked,
			"error":       st.Error,
		})

	default:
		return errString("bridge: unknown stream command %q", req.Method)
	}
}
