package main

import (
	"encoding/json"
	"testing"
)

func decode(t *testing.T, payload []byte) map[string]any {
	t.Helper()
	var out map[string]any
	if err := json.Unmarshal(payload, &out); err != nil {
		t.Fatalf("response is not valid JSON: %v (%s)", err, payload)
	}
	return out
}

// Every response, success or failure, must carry an "ok" member: the C side
// has exactly one code path for reading results.
func TestHandleCallAlwaysReturnsEnvelope(t *testing.T) {
	methods := []string{
		"login.begin",
		"login.captcha",
		"login.verify",
		"login.token",
		"account.forget",
		"device.list",
		"miot.get",
		"miot.set",
		"miot.action",
		"nonsense.method",
	}

	for _, method := range methods {
		t.Run(method, func(t *testing.T) {
			out := decode(t, handleCall(method, []byte("{}")))
			if _, present := out["ok"]; !present {
				t.Errorf("response for %s has no ok member: %v", method, out)
			}
		})
	}
}

func TestUnknownMethodIsRejected(t *testing.T) {
	out := decode(t, handleCall("does.not.exist", nil))

	if out["ok"] != false {
		t.Errorf("expected ok=false, got %v", out["ok"])
	}
	if out["error"] == nil || out["error"] == "" {
		t.Error("expected an explanatory error message")
	}
}

func TestMalformedRequestIsRejected(t *testing.T) {
	out := decode(t, handleCall("login.begin", []byte("{not json")))

	if out["ok"] != false {
		t.Errorf("expected ok=false for malformed JSON, got %v", out["ok"])
	}
}

// An empty request body is treated as an empty object rather than crashing,
// because the C caller may legitimately pass NULL for a no-argument method.
func TestEmptyRequestIsTreatedAsEmptyObject(t *testing.T) {
	out := decode(t, handleCall("account.forget", nil))

	if out["ok"] != true {
		t.Errorf("expected account.forget with no arguments to succeed, got %v", out)
	}
}

func TestLoginRequiresCredentials(t *testing.T) {
	out := decode(t, handleCall("login.begin", []byte(`{"username":"","password":""}`)))

	if out["ok"] != false {
		t.Error("expected empty credentials to be rejected before any network call")
	}
}

func TestCallsAgainstAnUnknownAccountFail(t *testing.T) {
	for _, method := range []string{"device.list", "miot.get", "miot.set", "miot.action"} {
		t.Run(method, func(t *testing.T) {
			out := decode(t, handleCall(method, []byte(`{"user_id":"nobody","did":"1"}`)))
			if out["ok"] != false {
				t.Errorf("expected %s to fail for an account that is not signed in", method)
			}
		})
	}
}

func TestStreamOpenRequiresDidAndAddress(t *testing.T) {
	session, response := openStream([]byte(`{"user_id":"nobody"}`))

	if session != nil {
		t.Error("expected no session for an incomplete request")
	}
	out := decode(t, response)
	if out["ok"] != false {
		t.Errorf("expected ok=false, got %v", out)
	}
}

func TestErrorResponseIsAlwaysParseable(t *testing.T) {
	out := decode(t, errString("failure with %q and %d", "quotes\"inside", 42))

	if out["ok"] != false {
		t.Error("expected ok=false")
	}
	if out["error"] == nil {
		t.Error("expected an error member")
	}
}
