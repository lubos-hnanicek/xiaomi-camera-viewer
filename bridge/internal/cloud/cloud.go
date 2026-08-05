// Package cloud speaks to the Xiaomi account and IoT cloud services.
//
// Derived from go2rtc pkg/xiaomi/cloud.go (MIT). See bridge/NOTICE.md.
//
// Two separate things live here. Login is the Mi account "serviceLogin" dance,
// which may divert through a captcha and/or a two-step verification ticket
// before it yields a pass token. Request is the signed and RC4-encrypted
// envelope that every subsequent IoT API call has to be wrapped in.
package cloud

import (
	"bytes"
	"crypto/md5"
	"crypto/rand"
	"crypto/rc4"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// AppXiaomiHome is the service id the Mi Home app authenticates against.
const AppXiaomiHome = "xiaomiio"

// Cloud is one authenticated Mi account session. It is safe for concurrent use
// once logged in; the login steps themselves are sequential by nature.
type Cloud struct {
	mu     sync.Mutex
	client *http.Client

	sid       string
	cookies   string // session cookies for API calls
	ssecurity []byte // per-session secret used to derive request keys

	userID    string
	passToken string

	auth map[string]string // carries state across multi-step login
}

func NewCloud(sid string) *Cloud {
	return &Cloud{
		client: &http.Client{Timeout: 15 * time.Second},
		sid:    sid,
	}
}

// Login starts a username/password sign-in. It returns a *LoginError when the
// service demands a captcha or a verification ticket; that is an expected
// control-flow signal, not a failure.
func (c *Cloud) Login(username, password string) error {
	res, err := c.client.Get("https://account.xiaomi.com/pass/serviceLogin?_json=true&sid=" + c.sid)
	if err != nil {
		return err
	}

	var v1 struct {
		Qs       string `json:"qs"`
		Sign     string `json:"_sign"`
		Sid      string `json:"sid"`
		Callback string `json:"callback"`
	}
	if _, err = readLoginResponse(res.Body, &v1); err != nil {
		return err
	}

	hash := fmt.Sprintf("%X", md5.Sum([]byte(password)))

	form := url.Values{
		"_json":    {"true"},
		"hash":     {hash},
		"sid":      {v1.Sid},
		"callback": {v1.Callback},
		"_sign":    {v1.Sign},
		"qs":       {v1.Qs},
		"user":     {username},
	}
	cookies := "deviceId=" + randString(16, 62)

	// Retrying login after the user solved a captcha.
	if c.auth != nil && c.auth["captcha_code"] != "" {
		form.Set("captCode", c.auth["captcha_code"])
		cookies += "; ick=" + c.auth["ick"]
	}

	req := Request{
		Method:     "POST",
		URL:        "https://account.xiaomi.com/pass/serviceLoginAuth2",
		Body:       form,
		RawCookies: cookies,
	}.Encode()

	res, err = c.client.Do(req)
	if err != nil {
		return err
	}

	var v2 struct {
		Ssecurity []byte `json:"ssecurity"`
		PassToken string `json:"passToken"`
		Location  string `json:"location"`

		CaptchaURL      string `json:"captchaURL"`
		NotificationURL string `json:"notificationUrl"`
	}
	body, err := readLoginResponse(res.Body, &v2)
	if err != nil {
		return err
	}

	// Remember the credentials so a captcha retry can replay this request.
	c.auth = map[string]string{
		"username": username,
		"password": password,
	}

	if v2.CaptchaURL != "" {
		return c.getCaptcha(v2.CaptchaURL)
	}

	if v2.NotificationURL != "" {
		return c.authStart(v2.NotificationURL)
	}

	if v2.Location == "" {
		return fmt.Errorf("xiaomi: %s", body)
	}

	c.auth = nil
	c.ssecurity = v2.Ssecurity
	c.passToken = v2.PassToken

	return c.finishAuth(v2.Location)
}

// LoginWithCaptcha supplies the code from the captcha image and resumes.
func (c *Cloud) LoginWithCaptcha(captcha string) error {
	if c.auth == nil || c.auth["ick"] == "" {
		return errors.New("xiaomi: no captcha is pending")
	}

	c.auth["captcha_code"] = captcha

	// The captcha may have been raised during verification rather than login.
	if c.auth["flag"] != "" {
		return c.sendTicket()
	}

	return c.Login(c.auth["username"], c.auth["password"])
}

// LoginWithVerify supplies the two-step verification code and resumes.
func (c *Cloud) LoginWithVerify(ticket string) error {
	if c.auth == nil || c.auth["flag"] == "" {
		return errors.New("xiaomi: no verification is pending")
	}

	req := Request{
		Method:     "POST",
		URL:        "https://account.xiaomi.com/identity/auth/verify" + c.verifyName(),
		RawParams:  "_flag" + c.auth["flag"] + "&ticket=" + ticket + "&trust=false&_json=true",
		RawCookies: "identity_session=" + c.auth["identity_session"],
	}.Encode()

	res, err := c.client.Do(req)
	if err != nil {
		return err
	}

	var v1 struct {
		Location string `json:"location"`
	}
	body, err := readLoginResponse(res.Body, &v1)
	if err != nil {
		return err
	}
	if v1.Location == "" {
		return fmt.Errorf("xiaomi: %s", body)
	}

	return c.finishAuth(v1.Location)
}

func (c *Cloud) getCaptcha(captchaURL string) error {
	res, err := c.client.Get("https://account.xiaomi.com" + captchaURL)
	if err != nil {
		return err
	}
	defer res.Body.Close()

	body, err := io.ReadAll(res.Body)
	if err != nil {
		return err
	}

	c.auth["ick"] = findCookie(res, "ick")

	return &LoginError{Captcha: body}
}

func (c *Cloud) authStart(notificationURL string) error {
	rawURL := strings.Replace(notificationURL, "/fe/service/identity/authStart", "/identity/list", 1)
	res, err := c.client.Get(rawURL)
	if err != nil {
		return err
	}

	var v1 struct {
		Code int `json:"code"`
		Flag int `json:"flag"`
	}
	if _, err = readLoginResponse(res.Body, &v1); err != nil {
		return err
	}

	c.auth["flag"] = strconv.Itoa(v1.Flag)
	c.auth["identity_session"] = findCookie(res, "identity_session")

	return c.sendTicket()
}

func findCookie(res *http.Response, name string) string {
	for _, cookie := range res.Cookies() {
		if cookie.Name == name {
			return cookie.Value
		}
	}
	return ""
}

// verifyName maps the account's second-factor flag onto the endpoint suffix.
func (c *Cloud) verifyName() string {
	switch c.auth["flag"] {
	case "4":
		return "Phone"
	case "8":
		return "Email"
	}
	return ""
}

func (c *Cloud) sendTicket() error {
	name := c.verifyName()
	cookies := "identity_session=" + c.auth["identity_session"]

	req := Request{
		URL:        "https://account.xiaomi.com/identity/auth/verify" + name,
		RawParams:  "_flag=" + c.auth["flag"] + "&_json=true",
		RawCookies: cookies,
	}.Encode()

	res, err := c.client.Do(req)
	if err != nil {
		return err
	}

	var v1 struct {
		Code        int    `json:"code"`
		MaskedPhone string `json:"maskedPhone"`
		MaskedEmail string `json:"maskedEmail"`
	}
	if _, err = readLoginResponse(res.Body, &v1); err != nil {
		return err
	}

	captCode := c.auth["captcha_code"]
	if captCode != "" {
		cookies += "; ick=" + c.auth["ick"]
	}

	form := url.Values{
		"_json": {"true"},
		"icode": {captCode},
		"retry": {"0"},
	}

	req = Request{
		Method:     "POST",
		URL:        "https://account.xiaomi.com/identity/auth/send" + name + "Ticket",
		Body:       form,
		RawCookies: cookies,
	}.Encode()

	res, err = c.client.Do(req)
	if err != nil {
		return err
	}

	var v2 struct {
		Code       int    `json:"code"`
		CaptchaURL string `json:"captchaURL"`
	}
	body, err := readLoginResponse(res.Body, &v2)
	if err != nil {
		return err
	}

	if v2.CaptchaURL != "" {
		return c.getCaptcha(v2.CaptchaURL)
	}

	if v2.Code != 0 {
		return fmt.Errorf("xiaomi: %s", body)
	}

	return &LoginError{
		VerifyPhone: v1.MaskedPhone,
		VerifyEmail: v1.MaskedEmail,
	}
}

// LoginError signals that login needs more input from the user. Exactly one of
// its fields is populated.
type LoginError struct {
	Captcha     []byte `json:"captcha,omitempty"`
	VerifyPhone string `json:"verify_phone,omitempty"`
	VerifyEmail string `json:"verify_email,omitempty"`
}

func (l *LoginError) Error() string {
	switch {
	case len(l.Captcha) > 0:
		return "xiaomi: captcha required"
	case l.VerifyPhone != "":
		return "xiaomi: phone verification required"
	case l.VerifyEmail != "":
		return "xiaomi: email verification required"
	}
	return "xiaomi: additional authentication required"
}

// finishAuth walks the redirect chain, harvesting the session cookies and the
// ssecurity value that request signing depends on.
func (c *Cloud) finishAuth(location string) error {
	res, err := c.client.Get(location)
	if err != nil {
		return err
	}
	defer res.Body.Close()

	var cUserID, serviceToken string

	for res != nil {
		for _, cookie := range res.Cookies() {
			switch cookie.Name {
			case "userId":
				c.userID = cookie.Value
			case "cUserId":
				cUserID = cookie.Value
			case "serviceToken":
				serviceToken = cookie.Value
			case "passToken":
				c.passToken = cookie.Value
			}
		}

		if s := res.Header.Get("Extension-Pragma"); s != "" {
			var v1 struct {
				Ssecurity []byte `json:"ssecurity"`
			}
			if err = json.Unmarshal([]byte(s), &v1); err != nil {
				return err
			}
			c.ssecurity = v1.Ssecurity
		}

		res = res.Request.Response
	}

	if c.userID == "" || serviceToken == "" {
		return errors.New("xiaomi: login did not yield a session")
	}

	c.cookies = fmt.Sprintf("userId=%s; cUserId=%s; serviceToken=%s", c.userID, cUserID, serviceToken)

	return nil
}

// LoginWithToken restores a session from a previously saved pass token, which
// is what lets the app start up without asking for the password again.
func (c *Cloud) LoginWithToken(userID, passToken string) error {
	req, err := http.NewRequest("GET", "https://account.xiaomi.com/pass/serviceLogin?_json=true&sid="+c.sid, nil)
	if err != nil {
		return err
	}

	req.Header.Set("Cookie", fmt.Sprintf("userId=%s; passToken=%s", userID, passToken))

	res, err := c.client.Do(req)
	if err != nil {
		return err
	}

	var v1 struct {
		Ssecurity []byte `json:"ssecurity"`
		PassToken string `json:"passToken"`
		Location  string `json:"location"`
	}
	if _, err = readLoginResponse(res.Body, &v1); err != nil {
		return err
	}

	if v1.Location == "" {
		return errors.New("xiaomi: saved token is no longer valid")
	}

	c.ssecurity = v1.Ssecurity
	c.passToken = v1.PassToken

	return c.finishAuth(v1.Location)
}

// UserToken returns the account id and the pass token worth persisting.
func (c *Cloud) UserToken() (string, string) {
	return c.userID, c.passToken
}

// Request performs a signed, RC4-encrypted IoT API call and returns the
// decrypted `result` member of the response.
func (c *Cloud) Request(baseURL, apiURL, params string, headers map[string]string) ([]byte, error) {
	c.mu.Lock()
	cookies := c.cookies
	ssecurity := c.ssecurity
	c.mu.Unlock()

	if cookies == "" {
		return nil, errors.New("xiaomi: not signed in")
	}

	form := url.Values{"data": {params}}

	nonce := genNonce()
	signedNonce := genSignedNonce(ssecurity, nonce)

	// 1. Hash the plaintext data parameter.
	form.Set("rc4_hash__", genSignature64("POST", apiURL, form, signedNonce))

	// 2. Encrypt every parameter, including that hash.
	for _, v := range form {
		ciphertext, err := crypt(signedNonce, []byte(v[0]))
		if err != nil {
			return nil, err
		}
		v[0] = base64.StdEncoding.EncodeToString(ciphertext)
	}

	// 3. Sign the encrypted form.
	form.Set("signature", genSignature64("POST", apiURL, form, signedNonce))

	// 4. Publish the nonce so the server can derive the same key.
	form.Set("_nonce", base64.StdEncoding.EncodeToString(nonce))

	req, err := http.NewRequest("POST", baseURL+apiURL, strings.NewReader(form.Encode()))
	if err != nil {
		return nil, err
	}

	req.Header.Set("Cookie", cookies)
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

	for k, v := range headers {
		req.Header.Set(k, v)
	}

	res, err := c.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()

	if res.StatusCode != http.StatusOK {
		return nil, errors.New(res.Status)
	}

	body, err := io.ReadAll(res.Body)
	if err != nil {
		return nil, err
	}

	ciphertext, err := base64.StdEncoding.DecodeString(string(body))
	if err != nil {
		return nil, err
	}

	plaintext, err := crypt(signedNonce, ciphertext)
	if err != nil {
		return nil, err
	}

	var res1 struct {
		Code    int             `json:"code"`
		Message string          `json:"message"`
		Result  json.RawMessage `json:"result"`
	}
	if err = json.Unmarshal(plaintext, &res1); err != nil {
		return nil, err
	}

	if res1.Code != 0 {
		return nil, errors.New("xiaomi: " + res1.Message)
	}

	return res1.Result, nil
}

// readLoginResponse strips the anti-JSON-hijacking prefix the account service
// puts in front of every response body.
func readLoginResponse(rc io.ReadCloser, v any) ([]byte, error) {
	defer rc.Close()

	body, err := io.ReadAll(rc)
	if err != nil {
		return nil, err
	}

	body, ok := bytes.CutPrefix(body, []byte("&&&START&&&"))
	if !ok {
		return nil, fmt.Errorf("xiaomi: %s", body)
	}

	return body, json.Unmarshal(body, &v)
}

func genNonce() []byte {
	ts := time.Now().Unix() / 60

	nonce := make([]byte, 12)
	_, _ = rand.Read(nonce[:8])
	binary.BigEndian.PutUint32(nonce[8:], uint32(ts))
	return nonce
}

func genSignedNonce(ssecurity, nonce []byte) []byte {
	hasher := sha256.New()
	hasher.Write(ssecurity)
	hasher.Write(nonce)
	return hasher.Sum(nil)
}

// crypt applies RC4, discarding the first 1024 bytes of keystream as the
// Mi Home app does.
func crypt(key, plaintext []byte) ([]byte, error) {
	cipher, err := rc4.NewCipher(key)
	if err != nil {
		return nil, err
	}

	tmp := make([]byte, 1024)
	cipher.XORKeyStream(tmp, tmp)

	ciphertext := make([]byte, len(plaintext))
	cipher.XORKeyStream(ciphertext, plaintext)

	return ciphertext, nil
}

func genSignature64(method, path string, values url.Values, signedNonce []byte) string {
	s := method + "&" + path + "&data=" + values.Get("data")
	if values.Has("rc4_hash__") {
		s += "&rc4_hash__=" + values.Get("rc4_hash__")
	}
	s += "&" + base64.StdEncoding.EncodeToString(signedNonce)

	hasher := sha1.New()
	hasher.Write([]byte(s))
	signature := hasher.Sum(nil)

	return base64.StdEncoding.EncodeToString(signature)
}

// Request is a small builder for the account service's idiosyncratic requests,
// which mix raw query strings with form bodies and hand-rolled cookie headers.
type Request struct {
	Method     string
	URL        string
	RawParams  string
	Body       url.Values
	Headers    url.Values
	RawCookies string
}

func (r Request) Encode() *http.Request {
	if r.RawParams != "" {
		r.URL += "?" + r.RawParams
	}

	var body io.Reader
	if r.Body != nil {
		body = strings.NewReader(r.Body.Encode())
	}

	req, err := http.NewRequest(r.Method, r.URL, body)
	if err != nil {
		return nil
	}

	if r.Headers != nil {
		req.Header = http.Header(r.Headers)
	}
	if r.Body != nil {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	if r.RawCookies != "" {
		req.Header.Set("Cookie", r.RawCookies)
	}

	return req
}

const symbols = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_"

// randString mirrors go2rtc's core.RandString, inlined to avoid vendoring the
// whole pkg/core for one helper.
func randString(size, base byte) string {
	b := make([]byte, size)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	if base == 0 {
		return string(b)
	}
	for i := byte(0); i < size; i++ {
		b[i] = symbols[b[i]%base]
	}
	return string(b)
}
