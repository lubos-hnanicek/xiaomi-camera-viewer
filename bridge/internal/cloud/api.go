package cloud

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/crypto"
)

// GetBaseURL maps a Mi account region onto its IoT API host. Accounts created
// outside mainland China live in a regional shard and will 404 on the default.
func GetBaseURL(region string) string {
	switch region {
	case "de", "i2", "ru", "sg", "us":
		return "https://" + region + ".api.io.mi.com/app"
	}
	return "https://api.io.mi.com/app"
}

// Account is a signed-in Mi account bound to a region.
type Account struct {
	UserID string
	Region string
	Token  string

	cloud *Cloud
}

func (a *Account) request(apiURL, params string) ([]byte, error) {
	return a.cloud.Request(GetBaseURL(a.Region), apiURL, params, nil)
}

// Device is one entry from the account's device list.
type Device struct {
	Did   string `json:"did"`
	Name  string `json:"name"`
	Model string `json:"model"`
	MAC   string `json:"mac"`
	IP    string `json:"localip"`
}

// IsCamera reports whether a device exposes a video stream. Doorbells use the
// `.cateye.` infix rather than `.camera.`.
func (d *Device) IsCamera() bool {
	return strings.Contains(d.Model, ".camera.") || strings.Contains(d.Model, ".cateye.")
}

// Devices returns every camera on the account.
func (a *Account) Devices() ([]*Device, error) {
	res, err := a.request("/v2/home/device_list_page", "{}")
	if err != nil {
		return nil, err
	}

	var v struct {
		List []*Device `json:"list"`
	}
	if err = json.Unmarshal(res, &v); err != nil {
		return nil, err
	}

	cameras := make([]*Device, 0, len(v.List))
	for _, d := range v.List {
		if d.IsCamera() {
			cameras = append(cameras, d)
		}
	}
	return cameras, nil
}

// MissSession is everything needed to open a peer-to-peer media session: the
// transport to use and the key material to authenticate and decrypt it with.
//
// This is the one step that unavoidably needs the internet. The video itself
// never leaves the LAN, but the camera will not hand it over without a key that
// only Xiaomi's cloud can vouch for, and a fresh one is issued per session.
type MissSession struct {
	Vendor        string // "cs2", "tutk", "agora" or "mtp"
	UID           string // TUTK peer id, only set for vendor "tutk"
	DevicePublic  string
	ClientPublic  string
	ClientPrivate string
	Sign          string
}

// MissVendor negotiates a media session for one device.
func (a *Account) MissVendor(did string) (*MissSession, error) {
	clientPublic, clientPrivate, err := crypto.GenerateKey()
	if err != nil {
		return nil, err
	}

	params := fmt.Sprintf(
		`{"app_pubkey":"%x","did":"%s","support_vendors":"TUTK_CS2_MTP"}`,
		clientPublic, did,
	)

	res, err := a.request("/v2/device/miss_get_vendor", params)
	if err != nil {
		return nil, err
	}

	var v struct {
		Vendor struct {
			ID     byte `json:"vendor"`
			Params struct {
				UID string `json:"p2p_id"`
			} `json:"vendor_params"`
		} `json:"vendor"`
		PublicKey string `json:"public_key"`
		Sign      string `json:"sign"`
	}
	if err = json.Unmarshal(res, &v); err != nil {
		return nil, err
	}

	return &MissSession{
		Vendor:        vendorName(v.Vendor.ID),
		UID:           v.Vendor.Params.UID,
		DevicePublic:  v.PublicKey,
		ClientPublic:  hex.EncodeToString(clientPublic),
		ClientPrivate: hex.EncodeToString(clientPrivate),
		Sign:          v.Sign,
	}, nil
}

func vendorName(i byte) string {
	switch i {
	case 1:
		return "tutk"
	case 3:
		return "agora"
	case 4:
		return "cs2"
	case 6:
		return "mtp"
	}
	return fmt.Sprintf("%d", i)
}

// WakeUp nudges a battery-powered camera or doorbell awake before connecting.
func (a *Account) WakeUp(did string) error {
	const params = `{"id":1,"method":"wakeup","params":{"video":"1"}}`
	_, err := a.request("/home/rpc/"+did, params)
	return err
}

// --- MIoT ------------------------------------------------------------------
//
// Camera settings and PTZ presets are ordinary MIoT properties and actions, so
// they ride the same authenticated session as everything else rather than
// needing a separately extracted device token.

// MiotProp addresses one property, optionally carrying a value to write.
type MiotProp struct {
	Did   string `json:"did"`
	Siid  int    `json:"siid"`
	Piid  int    `json:"piid"`
	Value any    `json:"value,omitempty"`
}

// Raw performs an arbitrary signed call against the account's regional IoT API
// and returns the decrypted result.
//
// The named calls above are the endpoints this app needs. Xiaomi has many more,
// none of them documented, and reaching one means signing a request the same way
// a named call does. Firmware lookup is the reason it exists: the OTA image a
// camera would install is the only published artifact containing the camera's
// own side of the protocol.
func (a *Account) Raw(apiURL, params string) (json.RawMessage, error) {
	if params == "" {
		params = "{}"
	}
	return a.request(apiURL, params)
}

func (a *Account) MiotGet(did string, props []MiotProp) (json.RawMessage, error) {
	for i := range props {
		props[i].Did = did
	}
	body, err := json.Marshal(map[string]any{"params": props})
	if err != nil {
		return nil, err
	}
	return a.request("/miotspec/prop/get", string(body))
}

func (a *Account) MiotSet(did string, props []MiotProp) (json.RawMessage, error) {
	for i := range props {
		props[i].Did = did
	}
	body, err := json.Marshal(map[string]any{"params": props})
	if err != nil {
		return nil, err
	}
	return a.request("/miotspec/prop/set", string(body))
}

func (a *Account) MiotAction(did string, siid, aiid int, in []any) (json.RawMessage, error) {
	if in == nil {
		in = []any{}
	}
	body, err := json.Marshal(map[string]any{
		"params": map[string]any{
			"did": did, "siid": siid, "aiid": aiid, "in": in,
		},
	})
	if err != nil {
		return nil, err
	}
	return a.request("/miotspec/action", string(body))
}

// --- Registry --------------------------------------------------------------

// Registry keeps the signed-in accounts and the in-progress login, so the
// bridge's stateless JSON API can still model a multi-step conversation.
type Registry struct {
	mu       sync.Mutex
	accounts map[string]*Account
	pending  *Cloud
}

func NewRegistry() *Registry {
	return &Registry{accounts: map[string]*Account{}}
}

// BeginLogin starts a fresh username/password login, discarding any previous
// half-finished attempt.
func (r *Registry) BeginLogin(username, password string) error {
	r.mu.Lock()
	r.pending = NewCloud(AppXiaomiHome)
	c := r.pending
	r.mu.Unlock()

	return c.Login(username, password)
}

func (r *Registry) SubmitCaptcha(code string) error {
	c, err := r.pendingCloud()
	if err != nil {
		return err
	}
	return c.LoginWithCaptcha(code)
}

func (r *Registry) SubmitVerify(ticket string) error {
	c, err := r.pendingCloud()
	if err != nil {
		return err
	}
	return c.LoginWithVerify(ticket)
}

func (r *Registry) pendingCloud() (*Cloud, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.pending == nil {
		return nil, errors.New("xiaomi: no login is in progress")
	}
	return r.pending, nil
}

// CompletePending promotes a finished login into a stored account. Region is
// not discoverable from the login itself, so the caller supplies it.
func (r *Registry) CompletePending(region string) (*Account, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.pending == nil {
		return nil, errors.New("xiaomi: no login is in progress")
	}

	userID, token := r.pending.UserToken()
	if userID == "" {
		return nil, errors.New("xiaomi: login did not complete")
	}

	acc := &Account{UserID: userID, Region: region, Token: token, cloud: r.pending}
	r.accounts[userID] = acc
	r.pending = nil
	return acc, nil
}

// Restore signs in from a saved pass token.
func (r *Registry) Restore(userID, region, token string) (*Account, error) {
	c := NewCloud(AppXiaomiHome)
	if err := c.LoginWithToken(userID, token); err != nil {
		return nil, err
	}

	newID, newToken := c.UserToken()
	if newID == "" {
		newID = userID
	}
	if newToken == "" {
		newToken = token
	}

	acc := &Account{UserID: newID, Region: region, Token: newToken, cloud: c}

	r.mu.Lock()
	r.accounts[newID] = acc
	r.mu.Unlock()

	return acc, nil
}

// Get returns a signed-in account, or an error naming the missing one.
func (r *Registry) Get(userID string) (*Account, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if acc := r.accounts[userID]; acc != nil {
		return acc, nil
	}
	return nil, fmt.Errorf("xiaomi: account %s is not signed in", userID)
}

func (r *Registry) Forget(userID string) {
	r.mu.Lock()
	delete(r.accounts, userID)
	r.mu.Unlock()
}
