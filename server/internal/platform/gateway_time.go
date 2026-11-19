package platform

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"time"
)

// This public bootstrap response contains no device identity or reusable
// authorization. Its signature, rather than transport trust, binds the nonce.
func (a *App) gatewayTime(w http.ResponseWriter, r *http.Request) {
	a.timeMu.Lock()
	now := time.Now()
	if now.Sub(a.timeRequests.At) >= time.Second {
		a.timeRequests = loginBucket{At: now}
	}
	a.timeRequests.Count++
	allowed := a.timeRequests.Count <= 100
	a.timeMu.Unlock()
	if !allowed {
		Fail(w, r, 429, "RATE_LIMITED", "time challenge capacity reached")
		return
	}
	raw, e := io.ReadAll(http.MaxBytesReader(w, r.Body, 4096))
	r.Body.Close()
	if e != nil {
		Fail(w, r, 400, "INVALID_JSON", "invalid time challenge")
		return
	}
	// Decode explicitly to reject duplicate keys, which a map decoder would hide.
	dec := json.NewDecoder(bytes.NewReader(raw))
	tok, e := dec.Token()
	if e != nil || tok != json.Delim('{') {
		Fail(w, r, 400, "INVALID_JSON", "expected object")
		return
	}
	body := map[string]any{}
	for dec.More() {
		key, e := dec.Token()
		if e != nil {
			Fail(w, r, 400, "INVALID_JSON", "invalid key")
			return
		}
		k, ok := key.(string)
		if !ok {
			Fail(w, r, 400, "INVALID_JSON", "invalid key")
			return
		}
		if _, exists := body[k]; exists {
			Fail(w, r, 400, "INVALID_JSON", "duplicate key")
			return
		}
		var v any
		if e = dec.Decode(&v); e != nil {
			Fail(w, r, 400, "INVALID_JSON", "invalid value")
			return
		}
		body[k] = v
	}
	if _, e = dec.Token(); e != nil {
		Fail(w, r, 400, "INVALID_JSON", "invalid object")
		return
	}
	if e = dec.Decode(new(any)); e != io.EOF {
		Fail(w, r, 400, "INVALID_JSON", "trailing data")
		return
	}
	if e = a.Validate("GatewayTimeChallenge", body); e != nil {
		Fail(w, r, 422, "VALIDATION_FAILED", "invalid time challenge")
		return
	}
	nonce, e := base64.StdEncoding.Strict().DecodeString(Str(body, "nonce"))
	if e != nil || len(nonce) != 32 || base64.StdEncoding.EncodeToString(nonce) != Str(body, "nonce") {
		Fail(w, r, 422, "VALIDATION_FAILED", "nonce requires canonical base64 of 32 bytes")
		return
	}
	key, e := base64.StdEncoding.DecodeString(a.Config.TimeSigningKey)
	if e == nil && len(key) == ed25519.SeedSize {
		key = ed25519.NewKeyFromSeed(key)
	}
	if !a.Config.TimeSourceVerified || a.Config.TimeKeyID == "" || len(a.Config.TimeKeyID) > 128 || e != nil || len(key) != ed25519.PrivateKeySize || Now() <= 0 {
		Fail(w, r, 503, "TIME_SOURCE_UNAVAILABLE", "verified time signer unavailable")
		return
	}
	payload := map[string]any{"schemaVersion": "1.0", "nonce": body["nonce"], "bootId": body["bootId"], "issuedAtUnixMs": strconv.FormatInt(Now(), 10), "uncertaintyMs": 1000, "validForMs": 60000}
	signed, _ := json.Marshal(payload)
	signature := ed25519.Sign(ed25519.PrivateKey(key), append([]byte("gw32-time-v1"), signed...))
	JSON(w, 200, map[string]any{"keyId": a.Config.TimeKeyID, "payloadBase64": base64.StdEncoding.EncodeToString(signed), "signatureBase64": base64.StdEncoding.EncodeToString(signature)})
}
