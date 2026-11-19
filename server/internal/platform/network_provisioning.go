package platform

import (
	"bytes"
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/go-chi/chi/v5"
)

func (a *App) RegisterNetworkProvisioning(r chi.Router) {
	r.Post("/api/v1/gateway-network-probes", a.networkProbe)
	r.Put("/api/v1/gateway-network-state", a.putNetworkState)
	r.Get("/api/v1/gateways/{gatewayId}/network-state", a.getNetworkState)
	r.Post("/api/v1/gateways/{gatewayId}/network-credentials", a.createNetworkCredential)
	r.Get("/api/v1/gateways/{gatewayId}/network-credentials", a.listNetworkCredentials)
	r.Get("/api/v1/gateway-network-credentials/pending", a.pendingNetworkCredentials)
	r.Post("/api/v1/gateway-network-credentials/receipts", a.networkCredentialReceipt)
	r.Post("/api/v1/gateways/{gatewayId}/network-reconciliation", a.reconcileNetwork)
}

func (a *App) networkTLS(w http.ResponseWriter, r *http.Request) bool {
	if r.TLS == nil && !a.Config.AllowInsecureDev {
		Fail(w, r, 426, "TLS_REQUIRED", "network provisioning requires TLS")
		return false
	}
	w.Header().Set("Cache-Control", "no-store")
	return true
}
func (a *App) networkDevice(w http.ResponseWriter, r *http.Request) (string, bool) {
	if !a.networkTLS(w, r) {
		return "", false
	}
	id, ok := a.Gateway(r)
	if !ok {
		Fail(w, r, 401, "GATEWAY_AUTH_REQUIRED", "valid gateway credential required")
	}
	return id, ok
}
func (a *App) networkAdmin(w http.ResponseWriter, r *http.Request, permission string) (*Principal, string, bool) {
	if !a.networkTLS(w, r) {
		return nil, "", false
	}
	p, ok := a.Require(w, r, permission)
	if !ok {
		return nil, "", false
	}
	id := chi.URLParam(r, "gatewayId")
	if !a.CanGateway(p, permission, id) {
		Fail(w, r, 404, "NOT_FOUND", "gateway unavailable")
		return nil, "", false
	}
	return p, id, true
}
func networkCounter(v string) (uint64, error) {
	n, e := strconv.ParseUint(v, 10, 64)
	if e != nil || strconv.FormatUint(n, 10) != v {
		return 0, Problem(422, "INVALID_COUNTER", "counter must be a canonical uint64 decimal string")
	}
	return n, nil
}

// Network snapshots use only ASCII property names and bounded integral IPv4
// prefixes. Sorted JSON keys, unescaped Unicode and HTML form the JCS subset.
func networkSnapshotID(v map[string]any) string {
	var b bytes.Buffer
	e := json.NewEncoder(&b)
	e.SetEscapeHTML(false)
	_ = e.Encode(v)
	raw := strings.TrimSuffix(b.String(), "\n")
	// Decode only actual JSON escapes. A literal six-character "\\u2028"
	// within an SSID must remain distinct from a Unicode line separator.
	var canonical strings.Builder
	for i := 0; i < len(raw); {
		if raw[i] == '\\' && i+1 < len(raw) {
			if strings.HasPrefix(raw[i:], `\u2028`) {
				canonical.WriteRune('\u2028')
				i += 6
				continue
			}
			if strings.HasPrefix(raw[i:], `\u2029`) {
				canonical.WriteRune('\u2029')
				i += 6
				continue
			}
			canonical.WriteString(raw[i : i+2])
			i += 2
			continue
		}
		canonical.WriteByte(raw[i])
		i++
	}
	raw = canonical.String()
	sum := sha256.Sum256([]byte(raw))
	return hex.EncodeToString(sum[:])
}
func validateCompleteNetwork(v map[string]any) error {
	if len(v) == 0 {
		return Problem(422, "INVALID_NETWORK", "complete network snapshot required")
	}
	if e := validateGW32ConfigurationShape(map[string]any{"network": v}); e != nil {
		return e
	}
	for _, k := range []string{"uplinkOrder", "ethernet", "wifiSta"} {
		if _, ok := v[k]; !ok {
			return Problem(422, "INVALID_NETWORK", "complete network snapshot required")
		}
	}
	for _, k := range []string{"ethernet", "wifiSta"} {
		m := Obj(v, k)
		if _, ok := m["enabled"].(bool); !ok {
			return Problem(422, "INVALID_NETWORK", "enabled boolean required")
		}
		if len(Obj(m, "ipv4")) == 0 {
			return Problem(422, "INVALID_NETWORK", "IPv4 object required")
		}
	}
	order := List(v, "uplinkOrder")
	if len(order) < 1 || len(order) > 2 {
		return Problem(422, "INVALID_NETWORK", "one or two uplinks required")
	}
	for _, i := range order {
		k := "ethernet"
		if i == "WIFI_STA" {
			k = "wifiSta"
		}
		if Obj(v, k)["enabled"] != true {
			return Problem(422, "INVALID_NETWORK", "uplink must be enabled")
		}
	}
	wifi := Obj(v, "wifiSta")
	if wifi["enabled"] == true && (len([]byte(Str(wifi, "ssid"))) < 1 || len([]byte(Str(wifi, "ssid"))) > 32 || Str(wifi, "credentialRef") == "" || Str(wifi, "security") == "") {
		return Problem(422, "INVALID_NETWORK", "enabled Wi-Fi requires SSID, security and credential reference")
	}
	return nil
}
func networkState(ctx context.Context, q ctlQuery, id string) (map[string]any, int64, error) {
	var raw string
	var version int64
	e := q.QueryRowContext(ctx, "SELECT state_json,version FROM gateway_network_states WHERE gateway_id=?", id).Scan(&raw, &version)
	if e != nil {
		return nil, 0, e
	}
	return pm(raw), version, nil
}
func (a *App) networkProbe(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	body, ok := a.Decode(w, r, "NetworkProbeRequest")
	if !ok {
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	now := Now()
	_, e = tx.ExecContext(r.Context(), "DELETE FROM gateway_network_probes WHERE received_at_ms<?", now-86400000)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	var hash, receipt string
	e = tx.QueryRowContext(r.Context(), "SELECT request_hash,receipt_id FROM gateway_network_probes WHERE gateway_id=? AND nonce=?", id, Str(body, "nonce")).Scan(&hash, &receipt)
	if e == nil && hash != Hash(body) {
		Fail(w, r, 409, "PROBE_CONFLICT", "nonce cannot be reused for a different probe")
		return
	}
	if errors.Is(e, sql.ErrNoRows) {
		var count int
		e = tx.QueryRowContext(r.Context(), "SELECT count(*) FROM gateway_network_probes WHERE gateway_id=?", id).Scan(&count)
		if e == nil && count >= 2048 {
			Fail(w, r, 429, "PROBE_RATE_LIMIT", "probe quota reached")
			return
		}
		receipt = ID("nprobe")
		if e == nil {
			_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_network_probes VALUES(?,?,?,?,?)", id, Str(body, "nonce"), Hash(body), receipt, now)
		}
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if e = tx.Commit(); e != nil {
		RespondError(w, r, e)
		return
	}
	body["receiptId"] = receipt
	body["gatewayId"] = id
	body["namespaceId"] = a.Config.NamespaceID
	JSON(w, 200, body)
}
func (a *App) putNetworkState(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	body, ok := a.Decode(w, r, "NetworkStateReport")
	if !ok {
		return
	}
	gen, e := networkCounter(Str(body, "networkGeneration"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	local, e := networkCounter(Str(body, "localRevision"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	for _, k := range []string{"desiredLocalNetwork", "actualNetwork"} {
		if e = validateCompleteNetwork(Obj(body, k)); e != nil {
			RespondError(w, r, e)
			return
		}
	}
	body["snapshotId"] = networkSnapshotID(Obj(body, "desiredLocalNetwork"))
	body["actualSnapshotId"] = networkSnapshotID(Obj(body, "actualNetwork"))
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	old, version, e := networkState(r.Context(), tx, id)
	if e != nil && !errors.Is(e, sql.ErrNoRows) {
		RespondError(w, r, e)
		return
	}
	if old != nil {
		oldgen, _ := networkCounter(Str(old, "networkGeneration"))
		oldlocal, _ := networkCounter(Str(old, "localRevision"))
		if gen < oldgen || local < oldlocal || (gen == oldgen && (local != oldlocal || Str(body, "snapshotId") != Str(old, "snapshotId"))) {
			Fail(w, r, 409, "NETWORK_STATE_STALE", "configuration generation regressed or conflicted")
			return
		}
	}
	for _, item := range List(body, "credentials") {
		c := item.(map[string]any)
		ref := Str(c, "credentialRef")
		var ver, ssid, sec, source string
		e = tx.QueryRowContext(r.Context(), "SELECT credential_version,ssid,security,source FROM gateway_network_credentials WHERE gateway_id=? AND credential_ref=?", id, ref).Scan(&ver, &ssid, &sec, &source)
		if e == nil {
			if ver != Str(c, "version") || ssid != Str(c, "ssid") || sec != Str(c, "security") || source != "LOCAL" {
				Fail(w, r, 409, "CREDENTIAL_CONFLICT", "credential reference is immutable")
				return
			}
			continue
		}
		if !errors.Is(e, sql.ErrNoRows) {
			RespondError(w, r, e)
			return
		}
		var count int
		e = tx.QueryRowContext(r.Context(), "SELECT count(*) FROM gateway_network_credentials WHERE gateway_id=?", id).Scan(&count)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		if count >= 128 {
			Fail(w, r, 429, "CREDENTIAL_QUOTA", "credential quota reached")
			return
		}
		_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_network_credentials(gateway_id,credential_ref,credential_version,ssid,security,source,state,created_at_ms,updated_at_ms) VALUES(?,?,?,?,?,'LOCAL','INSTALLED',?,?)", id, ref, Str(c, "version"), Str(c, "ssid"), Str(c, "security"), Now(), Now())
		if e != nil {
			RespondError(w, r, e)
			return
		}
	}
	// Clearing a known local override requires a verified platform task. A newer
	// local change cannot be cleared by an older successful task.
	if old != nil && old["localOverride"] == true && body["localOverride"] == false {
		var command string
		e = tx.QueryRowContext(r.Context(), "SELECT request_json FROM command_tasks WHERE task_id=? AND gateway_id=? AND operation='CONFIGURE' AND state='succeeded'", Str(body, "appliedTaskId"), id).Scan(&command)
		cp := Obj(Obj(pm(command), "parameters"), "networkPrecondition")
		g, _ := networkCounter(Str(cp, "expectedNetworkGeneration"))
		if e != nil || g == ^uint64(0) || gen != g+1 || Str(cp, "snapshotId") != Str(old, "snapshotId") {
			Fail(w, r, 409, "RECONCILIATION_REQUIRED", "verified matching platform task required to clear local override")
			return
		}
	}
	digest := Hash(body)
	var oldHash string
	if old != nil {
		oldHash = Hash(old)
	}
	if digest != oldHash {
		version++
		_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_network_states VALUES(?,?,?,?,?) ON CONFLICT(gateway_id) DO UPDATE SET state_json=excluded.state_json,state_hash=excluded.state_hash,version=excluded.version,received_at_ms=excluded.received_at_ms", id, encode(body), digest, version, Now())
		if e != nil {
			RespondError(w, r, e)
			return
		}
	}
	if e = tx.Commit(); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"syncToken": digest, "stateVersion": version, "snapshotId": body["snapshotId"], "actualSnapshotId": body["actualSnapshotId"]})
}
func (a *App) getNetworkState(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.networkAdmin(w, r, "gateway:read")
	if !ok {
		return
	}
	body, ver, e := networkState(r.Context(), a.Management, id)
	if errors.Is(e, sql.ErrNoRows) {
		Fail(w, r, 404, "NETWORK_STATE_UNKNOWN", "device has not reported network state")
		return
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(ver))
	JSON(w, 200, body)
}
func (a *App) networkSecretKey() ([]byte, error) {
	key, e := base64.StdEncoding.DecodeString(a.Config.NetworkCredentialKey)
	if e != nil || len(key) != 32 {
		return nil, Problem(503, "CREDENTIAL_STORE_UNAVAILABLE", "configure a 32-byte GW_NETWORK_CREDENTIAL_KEY before provisioning secrets")
	}
	return key, nil
}
func networkSeal(key []byte, aad string, plain []byte) ([]byte, error) {
	b, e := aes.NewCipher(key)
	if e != nil {
		return nil, e
	}
	g, e := cipher.NewGCM(b)
	if e != nil {
		return nil, e
	}
	nonce := make([]byte, g.NonceSize())
	if _, e = rand.Read(nonce); e != nil {
		return nil, e
	}
	return g.Seal(nonce, nonce, plain, []byte(aad)), nil
}
func networkOpen(key []byte, aad string, blob []byte) ([]byte, error) {
	b, e := aes.NewCipher(key)
	if e != nil {
		return nil, e
	}
	g, e := cipher.NewGCM(b)
	if e != nil {
		return nil, e
	}
	if len(blob) < g.NonceSize() {
		return nil, errors.New("invalid encrypted credential")
	}
	return g.Open(nil, blob[:g.NonceSize()], blob[g.NonceSize():], []byte(aad))
}
func (a *App) createNetworkCredential(w http.ResponseWriter, r *http.Request) {
	p, id, ok := a.networkAdmin(w, r, "gateway:credentials")
	if !ok {
		return
	}
	body, ok := a.Decode(w, r, "NetworkCredentialCreate")
	if !ok {
		return
	}
	password := []byte(Str(body, "password"))
	defer clear(password)
	if !utf8.Valid(password) || len(password) < 8 || len(password) > 63 || strings.IndexByte(string(password), 0) >= 0 || len([]byte(Str(body, "ssid"))) > 32 {
		Fail(w, r, 422, "INVALID_CREDENTIAL", "SSID or passphrase byte length invalid")
		return
	}
	key, e := a.networkSecretKey()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer clear(key)
	mac := hmac.New(sha256.New, key)
	mac.Write([]byte(id))
	mac.Write(password)
	sanitized := map[string]any{"gatewayId": id, "ssid": body["ssid"], "security": body["security"], "secretHMAC": hex.EncodeToString(mac.Sum(nil))}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "network.credential.create", r.Header.Get("Idempotency-Key"), sanitized, func(tx *sql.Tx) (int, string, any, error) {
		var count int
		if e := tx.QueryRowContext(r.Context(), "SELECT count(*) FROM gateway_network_credentials WHERE gateway_id=?", id).Scan(&count); e != nil {
			return 0, "", nil, e
		}
		if count >= 128 {
			return 0, "", nil, Problem(429, "CREDENTIAL_QUOTA", "credential quota reached")
		}
		ref, delivery := ID("netcred"), ID("netdelivery")
		ciphertext, e := networkSeal(key, id+"/"+ref+"/"+delivery, password)
		if e != nil {
			return 0, "", nil, e
		}
		_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_network_credentials VALUES(?,?,?, ?,?,?,'PLATFORM','PENDING',?,?,?)", id, ref, "1", delivery, Str(body, "ssid"), Str(body, "security"), ciphertext, Now(), Now())
		if e != nil {
			return 0, "", nil, e
		}
		if e = a.Audit(r.Context(), tx, p.ID, "network.credential.create", "gateway", id, map[string]any{"credentialRef": ref, "deliveryId": delivery}); e != nil {
			return 0, "", nil, e
		}
		return 201, delivery, map[string]any{"credentialRef": ref, "version": "1", "deliveryId": delivery, "state": "PENDING"}, nil
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}
func (a *App) listNetworkCredentials(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.networkAdmin(w, r, "gateway:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT credential_ref,credential_version,COALESCE(delivery_id,''),ssid,security,source,state FROM gateway_network_credentials WHERE gateway_id=? ORDER BY created_at_ms DESC LIMIT 128", id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var ref, ver, delivery, ssid, sec, source, state string
		if e = rows.Scan(&ref, &ver, &delivery, &ssid, &sec, &source, &state); e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, map[string]any{"credentialRef": ref, "version": ver, "deliveryId": delivery, "ssid": ssid, "security": sec, "source": source, "state": state})
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"items": items})
}
func (a *App) pendingNetworkCredentials(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	key, e := a.networkSecretKey()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer clear(key)
	rows, e := a.Management.QueryContext(r.Context(), "SELECT credential_ref,credential_version,delivery_id,ssid,security,ciphertext FROM gateway_network_credentials WHERE gateway_id=? AND source='PLATFORM' AND state='PENDING' ORDER BY created_at_ms LIMIT 8", id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var ref, ver, delivery, ssid, sec string
		var blob []byte
		if e = rows.Scan(&ref, &ver, &delivery, &ssid, &sec, &blob); e != nil {
			RespondError(w, r, e)
			return
		}
		plain, er := networkOpen(key, id+"/"+ref+"/"+delivery, blob)
		if er != nil {
			Fail(w, r, 503, "CREDENTIAL_DECRYPT_FAILED", "credential key unavailable or storage corrupt")
			return
		}
		items = append(items, map[string]any{"credentialRef": ref, "version": ver, "deliveryId": delivery, "ssid": ssid, "security": sec, "password": string(plain)})
		clear(plain)
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"items": items})
}
func (a *App) networkCredentialReceipt(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	body, ok := a.Decode(w, r, "NetworkCredentialReceipt")
	if !ok {
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	var old string
	e = tx.QueryRowContext(r.Context(), "SELECT state FROM gateway_network_credentials WHERE gateway_id=? AND credential_ref=? AND credential_version=? AND delivery_id=? AND source='PLATFORM'", id, Str(body, "credentialRef"), Str(body, "version"), Str(body, "deliveryId")).Scan(&old)
	if errors.Is(e, sql.ErrNoRows) {
		Fail(w, r, 404, "CREDENTIAL_NOT_FOUND", "credential delivery not found")
		return
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	state := Str(body, "state")
	if old != "PENDING" && old != state {
		Fail(w, r, 409, "CREDENTIAL_RECEIPT_CONFLICT", "terminal installation receipt conflicts")
		return
	}
	_, e = tx.ExecContext(r.Context(), "UPDATE gateway_network_credentials SET state=?,updated_at_ms=?,ciphertext=NULL WHERE gateway_id=? AND delivery_id=?", state, Now(), id, Str(body, "deliveryId"))
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"status": "ACCEPTED", "deliveryId": body["deliveryId"], "state": state})
}

// Called inside task creation's transaction. Legacy devices with no state
// report retain the existing protocol; provisioning-aware devices are fenced.
func (a *App) prepareNetworkTask(ctx context.Context, tx *sql.Tx, gateway string, params map[string]any) error {
	patch := Obj(Obj(params, "configuration"), "network")
	if len(patch) == 0 {
		if len(Obj(params, "networkPrecondition")) > 0 {
			return Problem(422, "INVALID_NETWORK_PRECONDITION", "network precondition requires a network configuration")
		}
		return nil
	}
	state, _, e := networkState(ctx, tx, gateway)
	if errors.Is(e, sql.ErrNoRows) {
		if len(Obj(params, "networkPrecondition")) > 0 {
			return Problem(409, "NETWORK_STATE_UNKNOWN", "device network state required")
		}
		// The new secret delivery API must never bypass its install gate,
		// including when a legacy device has not yet reported network state.
		ref := Str(Obj(patch, "wifiSta"), "credentialRef")
		if ref != "" {
			var installed, ssid, security string
			err := tx.QueryRowContext(ctx, "SELECT state,ssid,security FROM gateway_network_credentials WHERE gateway_id=? AND credential_ref=?", gateway, ref).Scan(&installed, &ssid, &security)
			if errors.Is(err, sql.ErrNoRows) && strings.HasPrefix(ref, "netcred_") {
				return Problem(409, "CREDENTIAL_NOT_INSTALLED", "platform credential is not installed for this device")
			}
			if err == nil {
				before, readErr := a.gw32Snapshot(ctx, tx, gateway, 1<<62)
				if readErr != nil {
					return readErr
				}
				wifi := Obj(Obj(gw32ApplyConfig(before, map[string]any{"network": patch}), "network"), "wifiSta")
				if installed != "INSTALLED" || ssid != Str(wifi, "ssid") || security != Str(wifi, "security") {
					return Problem(409, "CREDENTIAL_NOT_INSTALLED", "credential installation has not been acknowledged")
				}
			}
			if err != nil && !errors.Is(err, sql.ErrNoRows) {
				return err
			}
		}
		return nil
	}
	if e != nil {
		return e
	}
	pre := Obj(params, "networkPrecondition")
	if len(pre) == 0 {
		pre = map[string]any{"expectedNetworkGeneration": state["networkGeneration"], "snapshotId": state["snapshotId"]}
		params["networkPrecondition"] = pre
	}
	if Str(pre, "expectedNetworkGeneration") != Str(state, "networkGeneration") || Str(pre, "snapshotId") != Str(state, "snapshotId") {
		return Problem(409, "NETWORK_GENERATION_CONFLICT", "local network changed; refresh state")
	}
	token := Str(pre, "resolutionToken")
	if state["localOverride"] == true && token == "" {
		return Problem(409, "NETWORK_RECONCILIATION_REQUIRED", "explicit adoption or replacement of local settings required")
	}
	if token != "" {
		var gen, snapshot, candidate string
		var task sql.NullString
		e = tx.QueryRowContext(ctx, "SELECT generation,snapshot_id,candidate_hash,task_id FROM gateway_network_resolutions WHERE resolution_token=? AND gateway_id=?", token, gateway).Scan(&gen, &snapshot, &candidate, &task)
		if e != nil || task.Valid || gen != Str(pre, "expectedNetworkGeneration") || snapshot != Str(pre, "snapshotId") || candidate != networkSnapshotID(patch) {
			return Problem(409, "NETWORK_RESOLUTION_INVALID", "resolution does not match this configuration")
		}
	}
	target := gw32Merge(Obj(state, "desiredLocalNetwork"), patch)
	wifi := Obj(target, "wifiSta")
	if wifi["enabled"] == true {
		var installed, ssid, sec string
		e = tx.QueryRowContext(ctx, "SELECT state,ssid,security FROM gateway_network_credentials WHERE gateway_id=? AND credential_ref=?", gateway, Str(wifi, "credentialRef")).Scan(&installed, &ssid, &sec)
		if e != nil || installed != "INSTALLED" || ssid != Str(wifi, "ssid") || sec != Str(wifi, "security") {
			return Problem(409, "CREDENTIAL_NOT_INSTALLED", "matching Wi-Fi credential must be installed on this gateway first")
		}
	}
	return nil
}
func (a *App) networkCommandBlocked(ctx context.Context, q ctlQuery, gateway string, command map[string]any) (bool, error) {
	if Str(command, "type") != "CONFIGURE" || len(Obj(Obj(Obj(command, "parameters"), "configuration"), "network")) == 0 {
		return false, nil
	}
	state, _, e := networkState(ctx, q, gateway)
	if errors.Is(e, sql.ErrNoRows) {
		return false, nil
	}
	if e != nil {
		return true, e
	}
	pre := Obj(Obj(command, "parameters"), "networkPrecondition")
	return Str(pre, "expectedNetworkGeneration") != Str(state, "networkGeneration") || Str(pre, "snapshotId") != Str(state, "snapshotId") || (state["localOverride"] == true && Str(pre, "resolutionToken") == ""), nil
}
func (a *App) reconcileNetwork(w http.ResponseWriter, r *http.Request) {
	p, id, ok := a.networkAdmin(w, r, "gateway:configure")
	if !ok {
		return
	}
	if !a.CanGateway(p, "task:create", id) {
		Fail(w, r, 403, "FORBIDDEN", "task creation denied")
		return
	}
	body, ok := a.Decode(w, r, "NetworkReconciliationRequest")
	if !ok {
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "network.reconcile", r.Header.Get("Idempotency-Key"), map[string]any{"gatewayId": id, "body": body, "ifMatch": r.Header.Get("If-Match")}, func(tx *sql.Tx) (int, string, any, error) {
		state, ver, e := networkState(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, Problem(409, "NETWORK_STATE_UNKNOWN", "device network state required")
		}
		if e = Match(r, ver); e != nil {
			return 0, "", nil, e
		}
		if Str(body, "expectedNetworkGeneration") != Str(state, "networkGeneration") || Str(body, "snapshotId") != Str(state, "snapshotId") {
			return 0, "", nil, Problem(409, "NETWORK_GENERATION_CONFLICT", "local network changed")
		}
		target := Obj(state, "desiredLocalNetwork")
		if Str(body, "action") == "APPLY_PLATFORM" {
			target = Obj(body, "network")
		}
		if e = validateCompleteNetwork(target); e != nil {
			return 0, "", nil, e
		}
		if body["cancelQueued"] == true {
			if !a.canTx(r.Context(), tx, p, "task:cancel", id) {
				return 0, "", nil, Problem(403, "FORBIDDEN", "task cancellation denied")
			}
			rows, e := tx.QueryContext(r.Context(), "SELECT t.task_id,t.request_json FROM command_tasks t JOIN command_outbox o ON o.task_id=t.task_id AND o.kind='command' WHERE t.gateway_id=? AND t.state='queued' AND o.attempts=0", id)
			if e != nil {
				return 0, "", nil, e
			}
			ids := []string{}
			for rows.Next() {
				var task, raw string
				if e = rows.Scan(&task, &raw); e != nil {
					rows.Close()
					return 0, "", nil, e
				}
				if len(Obj(Obj(Obj(pm(raw), "parameters"), "configuration"), "network")) > 0 {
					ids = append(ids, task)
				}
			}
			e = rows.Err()
			rows.Close()
			if e != nil {
				return 0, "", nil, e
			}
			for _, task := range ids {
				if _, e = tx.ExecContext(r.Context(), "UPDATE command_tasks SET state='cancelled',version=version+1,updated_at_ms=? WHERE task_id=?", Now(), task); e != nil {
					return 0, "", nil, e
				}
				if _, e = tx.ExecContext(r.Context(), "UPDATE command_outbox SET state='cancelled' WHERE task_id=? AND kind='command'", task); e != nil {
					return 0, "", nil, e
				}
				if e = a.Audit(r.Context(), tx, p.ID, "network.cancel_undispatched", "task", task, map[string]any{"gatewayId": id}); e != nil {
					return 0, "", nil, e
				}
			}
		}
		token := ID("netresolve")
		_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_network_resolutions(resolution_token,gateway_id,generation,snapshot_id,candidate_hash,created_at_ms) VALUES(?,?,?,?,?,?)", token, id, Str(state, "networkGeneration"), Str(state, "snapshotId"), networkSnapshotID(target), Now())
		if e != nil {
			return 0, "", nil, e
		}
		config, e := a.configurationView(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, e
		}
		params := map[string]any{"configuration": map[string]any{"network": target}, "expectedReportedRevision": config["reportedRevision"], "networkPrecondition": map[string]any{"expectedNetworkGeneration": state["networkGeneration"], "snapshotId": state["snapshotId"], "resolutionToken": token}}
		req := map[string]any{"gatewayId": id, "type": "CONFIGURE", "parameters": params, "expiresAt": body["expiresAt"]}
		return a.newTask(r.Context(), tx, p.ID, r.Header.Get("Idempotency-Key"), req)
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}

// Scope checks inside an open SQLite transaction must not acquire a second
// connection when the application uses a single writer.
func (a *App) canTx(ctx context.Context, tx *sql.Tx, p *Principal, permission, gateway string) bool {
	var scope string
	if tx.QueryRowContext(ctx, "SELECT scope_id FROM gateways WHERE gateway_id=?", gateway).Scan(&scope) != nil {
		return false
	}
	return canQuery(ctx, tx, p, permission, scope)
}
