package platform

import (
	"bytes"
	"context"
	"encoding/base64"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
)

func networkFixture(t *testing.T) (*App, string) {
	a, actor := gw32Fixture(t)
	a.Config.NetworkCredentialKey = base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{0x71}, 32))
	for _, id := range []string{"gw_test", "gw_other"} {
		if _, e := a.Management.Exec("INSERT INTO credentials(credential_id,gateway_id,kind,verifier,status,not_before_ms,created_at_ms) VALUES(?,?,'device_secret',?,'active',0,?)", ID("cred"), id, TokenHash("test-token-"+id), Now()); e != nil {
			t.Fatal(e)
		}
	}
	return a, actor
}
func networkRequest(t *testing.T, a *App, actor, gateway, method, path string, body any, headers map[string]string) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	r := httptest.NewRequest(method, path, strings.NewReader(encode(body)))
	r.Header.Set("Content-Type", "application/json")
	if actor != "" {
		p, e := a.principal(t.Context(), actor)
		if e != nil {
			t.Fatal(e)
		}
		r = r.WithContext(context.WithValue(r.Context(), authKey{}, &authInfo{Principal: p}))
	}
	if gateway != "" {
		r.Header.Set("Authorization", "Bearer test-token-"+gateway)
	}
	for k, v := range headers {
		r.Header.Set(k, v)
	}
	router := chi.NewRouter()
	a.RegisterNetworkProvisioning(router)
	a.RegisterGatewayIntegration(router)
	a.RegisterControl(router)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, r)
	return w, pm(w.Body.String())
}
func testNetwork() map[string]any {
	return map[string]any{"uplinkOrder": []any{"WIFI_STA", "ETHERNET"}, "ethernet": map[string]any{"enabled": true, "ipv4": map[string]any{"mode": "DHCP"}}, "wifiSta": map[string]any{"enabled": true, "ssid": "site", "security": "WPA2_PSK", "credentialRef": "local-key", "ipv4": map[string]any{"mode": "DHCP"}}}
}
func testNetworkState(gen string, override bool) map[string]any {
	return map[string]any{"networkGeneration": gen, "localRevision": "1", "desiredLocalNetwork": testNetwork(), "actualNetwork": testNetwork(), "activeInterface": "WIFI_STA", "localOverride": override, "verificationState": "PLATFORM_CONNECTED", "lastKnownPlatformRevision": 0, "credentials": []any{map[string]any{"credentialRef": "local-key", "version": "1", "ssid": "site", "security": "WPA2_PSK", "source": "LOCAL", "state": "INSTALLED"}}}
}
func networkWant(t *testing.T, w *httptest.ResponseRecorder, status int) {
	t.Helper()
	if w.Code != status {
		t.Fatalf("HTTP %d want %d: %s", w.Code, status, w.Body.String())
	}
}

func TestNetworkCredentialEncryptionInstallationAndIsolation(t *testing.T) {
	a, actor := networkFixture(t)
	secret := "test-secret-never-log"
	body := map[string]any{"ssid": "site", "security": "WPA2_PSK", "password": secret}
	path := "/api/v1/gateways/gw_test/network-credentials"
	w, c := networkRequest(t, a, actor, "", "POST", path, body, map[string]string{"Idempotency-Key": "install-1"})
	networkWant(t, w, 201)
	if strings.Contains(w.Body.String(), secret) {
		t.Fatal("secret in management response")
	}
	w, same := networkRequest(t, a, actor, "", "POST", path, body, map[string]string{"Idempotency-Key": "install-1"})
	networkWant(t, w, 201)
	if c["credentialRef"] != same["credentialRef"] {
		t.Fatal("idempotency created a new credential")
	}
	var ciphertext []byte
	if e := a.Management.QueryRow("SELECT ciphertext FROM gateway_network_credentials WHERE credential_ref=?", c["credentialRef"]).Scan(&ciphertext); e != nil {
		t.Fatal(e)
	}
	if bytes.Contains(ciphertext, []byte(secret)) {
		t.Fatal("plaintext secret stored")
	}
	var saved string
	if e := a.Management.QueryRow("SELECT response_json FROM api_idempotency WHERE operation='network.credential.create'").Scan(&saved); e != nil {
		t.Fatal(e)
	}
	if strings.Contains(saved, secret) {
		t.Fatal("secret retained in idempotency response")
	}
	w, _ = networkRequest(t, a, "", "gw_other", "GET", "/api/v1/gateway-network-credentials/pending", nil, nil)
	networkWant(t, w, 200)
	if strings.Contains(w.Body.String(), secret) {
		t.Fatal("cross device secret leak")
	}
	w, pending := networkRequest(t, a, "", "gw_test", "GET", "/api/v1/gateway-network-credentials/pending", nil, nil)
	networkWant(t, w, 200)
	if List(pending, "items")[0].(map[string]any)["password"] != secret {
		t.Fatal("secret did not decrypt")
	}
	receipt := map[string]any{"deliveryId": c["deliveryId"], "credentialRef": c["credentialRef"], "version": "1", "state": "INSTALLED"}
	w, _ = networkRequest(t, a, "", "gw_other", "POST", "/api/v1/gateway-network-credentials/receipts", receipt, nil)
	networkWant(t, w, 404)
	w, _ = networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-network-credentials/receipts", receipt, nil)
	networkWant(t, w, 200)
	w, _ = networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-network-credentials/receipts", receipt, nil)
	networkWant(t, w, 200)
	receipt["state"] = "FAILED"
	w, _ = networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-network-credentials/receipts", receipt, nil)
	networkWant(t, w, 409)
	w, pending = networkRequest(t, a, "", "gw_test", "GET", "/api/v1/gateway-network-credentials/pending", nil, nil)
	networkWant(t, w, 200)
	if len(List(pending, "items")) != 0 {
		t.Fatal("acknowledged secret still delivered")
	}
	w, _ = networkRequest(t, a, "", "gw_test", "POST", path, body, map[string]string{"Idempotency-Key": "no-admin"})
	networkWant(t, w, 401)
	a.Config.AllowInsecureDev = false
	w, _ = networkRequest(t, a, actor, "", "POST", path, body, map[string]string{"Idempotency-Key": "tls"})
	networkWant(t, w, 426)
}
func TestNetworkProbeReplayAndStateCAS(t *testing.T) {
	a, actor := networkFixture(t)
	probe := map[string]any{"transactionId": "local-one", "interface": "WIFI_STA", "snapshotId": networkSnapshotID(testNetwork()), "nonce": strings.Repeat("1", 32)}
	w, first := networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-network-probes", probe, nil)
	networkWant(t, w, 200)
	w, again := networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-network-probes", probe, nil)
	networkWant(t, w, 200)
	if first["receiptId"] != again["receiptId"] {
		t.Fatal("probe replay changed receipt")
	}
	probe["interface"] = "ETHERNET"
	w, _ = networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-network-probes", probe, nil)
	networkWant(t, w, 409)
	state := testNetworkState("9007199254740993", true)
	w, ack := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-network-state", state, nil)
	networkWant(t, w, 200)
	w, view := networkRequest(t, a, actor, "", "GET", "/api/v1/gateways/gw_test/network-state", nil, nil)
	networkWant(t, w, 200)
	if view["networkGeneration"] != "9007199254740993" || view["snapshotId"] != ack["snapshotId"] {
		t.Fatal("lossy state")
	}
	state["networkGeneration"] = "9007199254740992"
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-network-state", state, nil)
	networkWant(t, w, 409)
	state["networkGeneration"] = "9007199254740993"
	Obj(Obj(state, "desiredLocalNetwork"), "wifiSta")["ssid"] = "different"
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-network-state", state, nil)
	networkWant(t, w, 409)
	state = testNetworkState("9007199254740994", false)
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-network-state", state, nil)
	networkWant(t, w, 409)
}
func TestNetworkLocalOverrideBlocksQueuedAndReconciles(t *testing.T) {
	a, actor := networkFixture(t)
	old := controlNewTask(t, a, actor, "old-network", "CONFIGURE", map[string]any{"configuration": map[string]any{"network": testNetwork()}, "expectedReportedRevision": 0})
	w, _ := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-network-state", testNetworkState("1", true), nil)
	networkWant(t, w, 200)
	commands, e := a.PullCommands(t.Context(), "gw_test")
	if e != nil || len(commands) != 0 {
		t.Fatalf("stale command dispatched: %v %v", commands, e)
	}
	calls := 0
	a.Publish = func(context.Context, string, any) error { calls++; return nil }
	if e = a.dispatchControl(t.Context()); e != nil {
		t.Fatal(e)
	}
	if calls != 0 {
		t.Fatal("MQTT dispatched stale command")
	}
	w, view := networkRequest(t, a, actor, "", "GET", "/api/v1/gateways/gw_test/network-state", nil, nil)
	networkWant(t, w, 200)
	etag := w.Header().Get("ETag")
	body := map[string]any{"action": "ADOPT_LOCAL", "expectedNetworkGeneration": "1", "snapshotId": view["snapshotId"], "expiresAt": Timestamp(Now() + 60000), "cancelQueued": true}
	w, task := networkRequest(t, a, actor, "", "POST", "/api/v1/gateways/gw_test/network-reconciliation", body, map[string]string{"If-Match": etag, "Idempotency-Key": "adopt-one"})
	networkWant(t, w, 202)
	var oldState string
	_ = a.Management.QueryRow("SELECT state FROM command_tasks WHERE task_id=?", old["taskId"]).Scan(&oldState)
	if oldState != "cancelled" {
		t.Fatal("old queued task not cancelled")
	}
	commands, e = a.PullCommands(t.Context(), "gw_test")
	if e != nil || len(commands) != 1 {
		t.Fatalf("new command unavailable: %v %v", commands, e)
	}
	cmd := commands[0].(map[string]any)
	if Str(cmd, "taskId") != Str(task, "taskId") {
		t.Fatal("wrong task")
	}
	pre := Obj(Obj(cmd, "parameters"), "networkPrecondition")
	if Str(pre, "resolutionToken") == "" || Str(pre, "expectedNetworkGeneration") != "1" {
		t.Fatal("missing generation binding")
	}
	if e = a.Validate("Command", cmd); e != nil {
		t.Fatal(e)
	}
	// A sent task cannot be cancelled by a second reconciliation.
	w, _ = networkRequest(t, a, actor, "", "POST", "/api/v1/gateways/gw_test/network-reconciliation", body, map[string]string{"If-Match": etag, "Idempotency-Key": "adopt-two"})
	networkWant(t, w, 409)
}
func TestNetworkKeyAndCipherTamperingFailClosed(t *testing.T) {
	a, actor := networkFixture(t)
	a.Config.NetworkCredentialKey = ""
	w, _ := networkRequest(t, a, actor, "", "POST", "/api/v1/gateways/gw_test/network-credentials", map[string]any{"ssid": "site", "security": "WPA2_PSK", "password": "some-password"}, map[string]string{"Idempotency-Key": "no-key"})
	networkWant(t, w, 503)
	key := bytes.Repeat([]byte{7}, 32)
	blob, e := networkSeal(key, "device/ref", []byte("secret-password"))
	if e != nil {
		t.Fatal(e)
	}
	if _, e = networkOpen(key, "other/ref", blob); e == nil {
		t.Fatal("wrong identity authenticated")
	}
	blob[len(blob)-1] ^= 1
	if _, e = networkOpen(key, "device/ref", blob); e == nil {
		t.Fatal("ciphertext tamper accepted")
	}
}

func TestNetworkSnapshotCanonicalUnicodeAndCredentialGate(t *testing.T) {
	actual := testNetwork()
	literal := testNetwork()
	Obj(actual, "wifiSta")["ssid"] = "<&\u2028"
	Obj(literal, "wifiSta")["ssid"] = `<&\u2028`
	if networkSnapshotID(actual) == networkSnapshotID(literal) {
		t.Fatal("canonical Unicode escape collapsed distinct SSIDs")
	}
	a, actor := networkFixture(t)
	w, c := networkRequest(t, a, actor, "", "POST", "/api/v1/gateways/gw_test/network-credentials", map[string]any{"ssid": "site", "security": "WPA2_PSK", "password": "pending-password"}, map[string]string{"Idempotency-Key": "pending-gate"})
	networkWant(t, w, 201)
	target := testNetwork()
	Obj(target, "wifiSta")["credentialRef"] = c["credentialRef"]
	if _, e := gw32TryTask(a, actor, "too-soon", "gw_test", "CONFIGURE", map[string]any{"configuration": map[string]any{"network": target}, "expectedReportedRevision": 0}); e == nil {
		t.Fatal("uninstalled secret accepted by legacy task path")
	}
}

func TestNetworkReconciliationRequiresEveryProbeAndVerifiedClear(t *testing.T) {
	a, actor := networkFixture(t)
	// Both server and device already know the same target. Reconciliation still
	// validates every enabled uplink, including the unchanged secondary link.
	target := map[string]any{"network": testNetwork()}
	if _, e := a.Management.Exec("UPDATE gateway_configs SET state='verified',reported_json=?,desired_json=? WHERE gateway_id='gw_test'", encode(target), encode(target)); e != nil {
		t.Fatal(e)
	}
	w, _ := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-network-state", testNetworkState("1", true), nil)
	networkWant(t, w, 200)
	w, state := networkRequest(t, a, actor, "", "GET", "/api/v1/gateways/gw_test/network-state", nil, nil)
	networkWant(t, w, 200)
	body := map[string]any{"action": "ADOPT_LOCAL", "expectedNetworkGeneration": "1", "snapshotId": state["snapshotId"], "expiresAt": Timestamp(Now() + 60000)}
	w, task := networkRequest(t, a, actor, "", "POST", "/api/v1/gateways/gw_test/network-reconciliation", body, map[string]string{"If-Match": w.Header().Get("ETag"), "Idempotency-Key": "prove-both"})
	networkWant(t, w, 202)
	commands, e := a.PullCommands(t.Context(), "gw_test")
	if e != nil || len(commands) != 1 {
		t.Fatal(e, commands)
	}
	params := Obj(commands[0].(map[string]any), "parameters")
	taskID := Str(task, "taskId")
	probe := func(iface, id string, seq int64) {
		t.Helper()
		result := controlResult(taskID, id, seq, "EXECUTING", "NONE", map[string]any{"networkProbe": map[string]any{"interface": iface, "candidateRevision": params["desiredRevision"], "linkReady": true, "ipReady": true, "dnsReady": true, "tlsReady": true}})
		ack, e := a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", result))
		if e != nil || controlAckStatus(ack) != "ACCEPTED" {
			t.Fatal(e, ack)
		}
	}
	probe("WIFI_STA", "first-proof", 1)
	success := controlResult(taskID, "verified-network", 3, "SUCCEEDED", "VERIFIED", map[string]any{"configuration": target, "reportedConfigurationRevision": params["desiredRevision"]})
	ack, e := a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", success))
	if e != nil || controlAckStatus(ack) == "ACCEPTED" {
		t.Fatal("missing secondary proof accepted", e, ack)
	}
	probe("ETHERNET", "second-proof", 2)
	ack, e = a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", success))
	if e != nil || controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(e, ack)
	}
	report := testNetworkState("2", false)
	report["appliedTaskId"] = taskID
	report["lastKnownPlatformRevision"] = params["desiredRevision"]
	report["verificationState"] = "PLATFORM_SYNCED"
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-network-state", report, nil)
	networkWant(t, w, 200)
}
