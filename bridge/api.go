package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/cloud"
	"github.com/spec8472/xiaomi-viewer/bridge/internal/lan"
	"github.com/spec8472/xiaomi-viewer/bridge/internal/stream"
)

// Version is reported by xmb_version and stamped into the app's about box.
const Version = "0.1.0"

var (
	registry = cloud.NewRegistry()

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
		Siid  int  `json:"siid"`
		Piid  int  `json:"piid"`
		Value any  `json:"value"`
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

	// The cloud caches the address a camera last reported and sometimes has
	// none at all, handing back 0.0.0.0. It still knows the MAC, and cameras
	// answer a broadcast, so the camera can be found on the network instead.
	host := req.IP
	if host == "" || host == "0.0.0.0" {
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

	sess, err := acc.MissVendor(req.Did)
	if err != nil {
		return nil, errResponse(err)
	}
	if sess.Vendor != "cs2" {
		return nil, permanentError(
			"camera negotiated the %q transport, which this build does not implement",
			sess.Vendor)
	}

	s, err := stream.Open(stream.Config{
		Host:          host,
		Transport:     req.Transport,
		Model:         req.Model,
		Channel:       req.Channel,
		Quality:       req.Quality,
		Audio:         req.Audio,
		DevicePublic:  sess.DevicePublic,
		ClientPublic:  sess.ClientPublic,
		ClientPrivate: sess.ClientPrivate,
		Sign:          sess.Sign,
	})
	if err != nil {
		return nil, errString("bridge: vendor %s: %s", sess.Vendor, err)
	}

	return s, okResponse(map[string]any{
		"protocol":    s.Protocol,
		"remote_addr": s.RemoteAddr,
		"vendor":      sess.Vendor,
		"host":        host,
	})
}

// lanSearchTimeout bounds the broadcast search for a camera the cloud has no
// address for. Cameras answer within a second, so this is mostly patience for a
// device that is booting.
const lanSearchTimeout = 8 * time.Second

// locateOnLAN finds a camera by the MAC the cloud knows for it. The address is
// not cached: it is only reached on the path where the cloud already failed to
// supply one, and a camera whose address is unknown is exactly the one likely
// to move again.
func locateOnLAN(acc *cloud.Account, did string) (string, error) {
	devices, err := acc.Devices()
	if err != nil {
		return "", fmt.Errorf("bridge: looking up the camera's hardware address: %w", err)
	}

	var mac string
	for _, d := range devices {
		if d.Did == did {
			mac = d.MAC
			break
		}
	}
	if mac == "" {
		return "", fmt.Errorf(
			"bridge: the cloud has neither an address nor a MAC for this camera")
	}

	ip, err := lan.FindByMAC(mac, lanSearchTimeout)
	if err != nil {
		return "", fmt.Errorf(
			"bridge: the cloud has no address for this camera and nothing with MAC %s "+
				"answered on the local network", mac)
	}

	return ip.String(), nil
}

// handleStreamCommand runs an in-band command against a live session.
func handleStreamCommand(s *stream.Session, request []byte) []byte {
	var req struct {
		Method    string `json:"method"`
		Direction string `json:"direction"`
		Body      string `json:"body"`
		Cmd       uint32 `json:"cmd"`
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

	case "replies":
		return okResponse(map[string]any{
			"replies":   s.Replies(),
			"unhandled": s.Unhandled(),
		})

	case "stats":
		st := s.Stats()
		return okResponse(map[string]any{
			"frames":  st.Frames,
			"bytes":   st.Bytes,
			"dropped":    st.Dropped,
			"replies":    st.Replies,
			"last_reply": st.LastReply,
		})

	default:
		return errString("bridge: unknown stream command %q", req.Method)
	}
}
