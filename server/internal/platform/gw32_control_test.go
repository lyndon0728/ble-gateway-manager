package platform

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func gw32Fixture(t *testing.T) (*App, string) {
	t.Helper()
	a, actor := controlFixture(t)
	caps := []string{"GW32_CONTROL_PROFILE_1", "GW32_CAPTURE_CONTEXT_1", "NETWORK_ETHERNET", "NETWORK_WIFI_STA", "NRF54_BLE_SCAN", "NRF54_BLE_BEACON_FILTER", "NRF54_BLE_ADV", "NRF54_BLE_GATT_CENTRAL", "NRF54_BLE_GATT_NOTIFY", "NRF54_BLE_GATT_SERVICE_DISCOVERY", "ESP32_BLE_SCAN", "ESP32_BLE_BEACON_FILTER", "ESP32_BLE_ADV", "ESP32_BLE_GATT_CENTRAL", "ESP32_BLE_GATT_NOTIFY", "CONFIGURE", "REBOOT", "DIAGNOSTICS", "GATT_CONNECT", "GATT_DISCONNECT", "GATT_READ", "GATT_WRITE", "GATT_SUBSCRIBE", "GATT_UNSUBSCRIBE"}
	if _, e := a.Management.Exec("UPDATE gateways SET model='GW32I33V2',capabilities_json=?", encode(caps)); e != nil {
		t.Fatal(e)
	}
	return a, actor
}
func gw32TryTask(a *App, actor, key, gateway, op string, params map[string]any) (map[string]any, error) {
	ctx := context.Background()
	req := map[string]any{"gatewayId": gateway, "type": op, "parameters": params, "expiresAt": Timestamp(Now() + 60000)}
	_, v, e := a.DoIdempotent(ctx, actor, "task.create", key, req, func(tx *sql.Tx) (int, string, any, error) { return a.newTask(ctx, tx, actor, key, req) })
	if e != nil {
		return nil, e
	}
	return v.(map[string]any), nil
}
func gw32ConnectionParams() map[string]any {
	return map[string]any{"radio": "NRF54", "deviceAddress": "AA:BB:CC:DD:EE:FF", "addressType": "PUBLIC", "security": "BONDED_ENCRYPTED", "timeoutMs": int64(5000)}
}
func gw32ConnectionEvidence(ref string) map[string]any {
	return map[string]any{"radio": "NRF54", "connectionRef": ref, "connectionState": "CONNECTED", "deviceAddress": "AABBCCDDEEFF", "addressType": "PUBLIC", "attMtu": int64(23), "securityState": "ENCRYPTED"}
}
func gw32SendResult(t *testing.T, a *App, task, id, state string, seq int64, details map[string]any) map[string]any {
	t.Helper()
	ack, e := a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", controlResult(task, id, seq, state, "VERIFIED", details)))
	if e != nil {
		t.Fatal(e)
	}
	return ack
}
func gw32Open(t *testing.T, a *App, actor string) string {
	t.Helper()
	task, e := gw32TryTask(a, actor, "open", "gw_test", "GATT_CONNECT", gw32ConnectionParams())
	if e != nil {
		t.Fatal(e)
	}
	ref := strings.Repeat("a", 32)
	ack := gw32SendResult(t, a, Str(task, "taskId"), "opened", "SUCCEEDED", 1, gw32ConnectionEvidence(ref))
	if controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(ack)
	}
	return ref
}

func TestGW32StrictConfigurationShapes(t *testing.T) {
	bad := []map[string]any{
		{"network": map[string]any{"uplinkOrder": []any{"ETHERNET"}}, "upload": map[string]any{"flushIntervalMs": 100}},
		{"radios": map[string]any{"nrf54": map[string]any{"scan": map[string]any{"enabled": true}}, "esp32": map[string]any{"scan": map[string]any{"enabled": true}}}},
		{"radios": map[string]any{"nrf54": map[string]any{"scan": map[string]any{"enabled": true}, "advertising": map[string]any{"enabled": true}}}},
		{"radios": map[string]any{"nrf54": map[string]any{"beaconRules": []any{map[string]any{"action": "ALLOW", "reportKinds": []any{"LEGACY_ADV"}, "adType": 255, "offset": 0, "valueBase64": "AQ==", "maskBase64": "AA=="}}}}},
		{"radios": map[string]any{"esp32": map[string]any{"advertising": map[string]any{"advDataBase64": "AwE="}}}},
		{"network": map[string]any{"wifiSta": map[string]any{"ssid": strings.Repeat("界", 11)}}},
		{"network": map[string]any{"wifiSta": map[string]any{"password": "must-never-be-in-task"}}},
		{"network": map[string]any{"ethernet": map[string]any{"ipv4": map[string]any{"mode": "STATIC", "address": "192.168.1.4", "prefixLength": 24, "gateway": "192.168.2.1", "dns": []any{"1.1.1.1"}}}}},
		{"network": map[string]any{"ethernet": map[string]any{"ipv4": map[string]any{"mode": "DHCP", "address": "192.168.1.4"}}}},
		{"radios": map[string]any{"esp32": map[string]any{"advertising": map[string]any{"mode": "NONCONNECTABLE", "scanResponseBase64": "AgEG"}}}},
	}
	for i, c := range bad {
		if e := ValidateGatewayConfiguration(c); e == nil {
			t.Fatalf("invalid shape %d accepted: %v", i, c)
		}
	}
	good := map[string]any{"radios": map[string]any{"nrf54": map[string]any{"scan": map[string]any{"enabled": true, "intervalMs": float64(100), "windowMs": float64(50)}, "filters": map[string]any{"minRssi": nil, "serviceUuids": []any{"180F", "12345678", "12345678-1234-5678-9abc-def012345678"}}, "beaconRules": []any{map[string]any{"action": "ALLOW", "reportKinds": []any{"LEGACY_ADV"}, "adType": 255, "offset": 0, "valueBase64": "TAACFQ==", "maskBase64": "/////w=="}}}}}
	if e := ValidateGatewayConfiguration(good); e != nil {
		t.Fatal(e)
	}
}

func TestGW32ConfigurationMergeAndCapabilityGate(t *testing.T) {
	a, actor := gw32Fixture(t)
	tx, e := a.Management.BeginTx(t.Context(), nil)
	if e != nil {
		t.Fatal(e)
	}
	g, e := a.gatewayView(t.Context(), tx, "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	cases := []map[string]any{{"radios": map[string]any{"nrf54": map[string]any{"scan": map[string]any{"intervalMs": float64(10)}}}}, {"network": map[string]any{"ethernet": map[string]any{"enabled": false}}}, {"network": map[string]any{"wifiSta": map[string]any{"enabled": true}}}}
	for _, c := range cases {
		if e = a.validateGatewayConfiguration(t.Context(), tx, g, c); e == nil {
			t.Fatal("invalid merged snapshot accepted", c)
		}
	}
	g["capabilities"] = []any{"CONFIGURE", "GW32_CONTROL_PROFILE_1", "NRF54_BLE_SCAN"}
	if e = a.validateGatewayConfiguration(t.Context(), tx, g, map[string]any{"radios": map[string]any{"nrf54": map[string]any{"beaconRules": []any{}}}}); e == nil {
		t.Fatal("Beacon capability not required")
	}
	g["model"] = "REFERENCE_GW"
	if e = a.validateGatewayConfiguration(t.Context(), tx, g, map[string]any{"network": map[string]any{"uplinkOrder": []any{"ETHERNET"}}}); e == nil {
		t.Fatal("model profile bypass")
	}
	tx.Rollback()
	task, e := gw32TryTask(a, actor, "scan-good", "gw_test", "CONFIGURE", map[string]any{"configuration": map[string]any{"radios": map[string]any{"esp32": map[string]any{"advertising": map[string]any{"enabled": true, "mode": "NONCONNECTABLE", "intervalMs": 250, "advDataBase64": "AgEG", "scanResponseBase64": ""}}}}, "expectedReportedRevision": 0})
	if e != nil {
		t.Fatal(e)
	}
	if Str(task, "type") != "CONFIGURE" {
		t.Fatal(task)
	}
}

func TestGW32NetworkRequiresPriorDurableProbeResults(t *testing.T) {
	a, actor := gw32Fixture(t)
	configuration := map[string]any{"network": map[string]any{"uplinkOrder": []any{"ETHERNET", "WIFI_STA"}, "ethernet": map[string]any{"enabled": true, "ipv4": map[string]any{"mode": "DHCP"}}, "wifiSta": map[string]any{"enabled": true, "ssid": "site", "security": "WPA2_PSK", "credentialRef": "wifi-local-1", "ipv4": map[string]any{"mode": "DHCP"}}}}
	task, e := gw32TryTask(a, actor, "network", "gw_test", "CONFIGURE", map[string]any{"configuration": configuration, "expectedReportedRevision": 0})
	if e != nil {
		t.Fatal(e)
	}
	id := Str(task, "taskId")
	success := controlResult(id, "network_success", 3, "SUCCEEDED", "VERIFIED", map[string]any{"configuration": configuration, "reportedConfigurationRevision": 2})
	probes := []any{}
	for i, name := range []string{"ETHERNET", "WIFI_STA"} {
		probes = append(probes, controlResult(id, "probe_"+name, int64(i+1), "EXECUTING", "NONE", map[string]any{"networkProbe": map[string]any{"interface": name, "candidateRevision": 2, "linkReady": true, "ipReady": true, "dnsReady": true, "tlsReady": true}}))
	}
	batch := controlBatch(a, "gw_test", success)
	batch["results"] = append(probes, success)
	ack, e := a.AcceptTaskResults(t.Context(), "gw_test", batch)
	if e != nil {
		t.Fatal(e)
	}
	items := List(ack, "items")
	if Str(items[2].(map[string]any), "code") != "NETWORK_PROBES_NOT_PERSISTED" {
		t.Fatal("same-batch probe bypass", ack)
	}
	ack, e = a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", success))
	if e != nil || controlAckStatus(ack) != "ACCEPTED" {
		t.Fatalf("committed probes rejected: %v %v", ack, e)
	}
	cfg, e := a.configurationView(t.Context(), a.Management, "gw_test")
	if e != nil || Num(cfg, "reportedRevision") != 2 {
		t.Fatal(cfg, e)
	}
}

func TestGW32ConnectionClaimsAndEvidence(t *testing.T) {
	a, actor := gw32Fixture(t)
	task, e := gw32TryTask(a, actor, "connect", "gw_test", "GATT_CONNECT", gw32ConnectionParams())
	if e != nil {
		t.Fatal(e)
	}
	if _, e = gw32TryTask(a, actor, "duplicate-peer", "gw_other", "GATT_CONNECT", gw32ConnectionParams()); e == nil {
		t.Fatal("same namespace peer claimed twice")
	}
	id := Str(task, "taskId")
	ref := strings.Repeat("b", 32)
	details := gw32ConnectionEvidence(ref)
	details["securityState"] = "UNENCRYPTED"
	ack := gw32SendResult(t, a, id, "weak_security", "SUCCEEDED", 1, details)
	if controlAckStatus(ack) != "REJECTED_PERMANENT" {
		t.Fatal(ack)
	}
	details["securityState"] = "ENCRYPTED"
	ack = gw32SendResult(t, a, id, "connected_ok", "SUCCEEDED", 2, details)
	if controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(ack)
	}
	var state string
	if e = a.Management.QueryRow("SELECT state FROM connection_claims WHERE namespace_id=? AND peer_address='AABBCCDDEEFF'", a.Config.NamespaceID).Scan(&state); e != nil || state != "active" {
		t.Fatal(state, e)
	}
	params := map[string]any{"radio": "ESP32", "connectionRef": ref, "deviceAddress": "AA:BB:CC:DD:EE:FF", "addressType": "PUBLIC", "serviceUuid": "180f", "characteristicUuid": "2a19", "timeoutMs": 1000}
	if _, e = gw32TryTask(a, actor, "wrong-radio", "gw_test", "GATT_READ", params); e == nil {
		t.Fatal("wrong radio connection accepted")
	}
	params["radio"] = "NRF54"
	params["deviceAddress"] = "11:22:33:44:55:66"
	if _, e = gw32TryTask(a, actor, "wrong-peer", "gw_test", "GATT_READ", params); e == nil {
		t.Fatal("wrong peer connection accepted")
	}
	params["deviceAddress"] = "AA:BB:CC:DD:EE:FF"
	params["writeMode"] = "WITH_RESPONSE"
	params["verification"] = "READBACK"
	params["valueBase64"] = base64.StdEncoding.EncodeToString(make([]byte, 21))
	if _, e = gw32TryTask(a, actor, "over-mtu", "gw_test", "GATT_WRITE", params); e == nil {
		t.Fatal("write exceeds MTU-3 but was accepted")
	}
}

func TestGW32LinkDownTombstoneWinsLateConnection(t *testing.T) {
	a, actor := gw32Fixture(t)
	task, e := gw32TryTask(a, actor, "connect-late", "gw_test", "GATT_CONNECT", gw32ConnectionParams())
	if e != nil {
		t.Fatal(e)
	}
	ref := strings.Repeat("c", 32)
	_, e = a.Management.Exec("INSERT INTO gatt_connections(namespace_id,gateway_id,connection_ref,radio,state,closed_event_key,created_at_ms,updated_at_ms,closed_at_ms) VALUES(?,'gw_test',?,'NRF54','closed','early-link-down',?,?,?)", a.Config.NamespaceID, ref, Now(), Now(), Now())
	if e != nil {
		t.Fatal(e)
	}
	ack := gw32SendResult(t, a, Str(task, "taskId"), "late-connect", "SUCCEEDED", 1, gw32ConnectionEvidence(ref))
	if controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(ack)
	}
	var state string
	if e = a.Management.QueryRow("SELECT state FROM gatt_connections WHERE connection_ref=?", ref).Scan(&state); e != nil || state != "closed" {
		t.Fatal("closed connection resurrected", state, e)
	}
	if e = a.Management.QueryRow("SELECT state FROM connection_claims WHERE task_id=?", Str(task, "taskId")).Scan(&state); e != nil || state != "cooldown" {
		t.Fatal("claim leaked after tombstone", state, e)
	}
}

func TestGW32SubscriptionsReadbackAndExpiry(t *testing.T) {
	a, actor := gw32Fixture(t)
	ref := gw32Open(t, a, actor)
	params := map[string]any{"radio": "NRF54", "connectionRef": ref, "serviceUuid": "180f", "characteristicUuid": "2a19", "mode": "NOTIFY", "timeoutMs": 1000}
	task, e := gw32TryTask(a, actor, "subscribe", "gw_test", "GATT_SUBSCRIBE", params)
	if e != nil {
		t.Fatal(e)
	}
	evidence := map[string]any{"radio": "NRF54", "connectionRef": ref, "serviceUuid": "180f", "characteristicUuid": "2a19", "mode": "NOTIFY", "characteristicHandle": 42, "cccHandle": 43, "cccEnabled": true}
	ack := gw32SendResult(t, a, Str(task, "taskId"), "subscribed", "SUCCEEDED", 1, evidence)
	if controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(ack)
	}
	var count int
	if e = a.Management.QueryRow("SELECT count(*) FROM gatt_subscriptions WHERE state='active'").Scan(&count); e != nil || count != 1 {
		t.Fatal(count, e)
	}
	write := map[string]any{"radio": "NRF54", "connectionRef": ref, "deviceAddress": "AA:BB:CC:DD:EE:FF", "addressType": "PUBLIC", "serviceUuid": "180f", "characteristicUuid": "2a19", "timeoutMs": 1000, "writeMode": "WITH_RESPONSE", "verification": "READBACK", "valueBase64": "AQI="}
	task, e = gw32TryTask(a, actor, "write", "gw_test", "GATT_WRITE", write)
	if e != nil {
		t.Fatal(e)
	}
	we := map[string]any{"radio": "NRF54", "connectionRef": ref, "deviceAddress": "AABBCCDDEEFF", "addressType": "PUBLIC", "serviceUuid": "180f", "characteristicUuid": "2a19", "attResponse": true, "readbackBase64": "AwQ="}
	ack = gw32SendResult(t, a, Str(task, "taskId"), "bad-readback", "SUCCEEDED", 1, we)
	if controlAckStatus(ack) != "REJECTED_PERMANENT" {
		t.Fatal("readback mismatch accepted", ack)
	}
	if _, e = a.PullCommands(t.Context(), "gw_test"); e != nil {
		t.Fatal(e)
	}
	if _, e = a.Management.Exec("UPDATE command_tasks SET expires_at_ms=? WHERE task_id=?", Now()-1, Str(task, "taskId")); e != nil {
		t.Fatal(e)
	}
	if e = a.expireCommands(t.Context()); e != nil {
		t.Fatal(e)
	}
	var state string
	if e = a.Management.QueryRow("SELECT state FROM gatt_connections WHERE connection_ref=?", ref).Scan(&state); e != nil || state != "unknown" {
		t.Fatal("uncertain physical write did not invalidate connection", state, e)
	}
	if _, e = gw32TryTask(a, actor, "diagnose-unknown", "gw_test", "DIAGNOSTICS", map[string]any{"profile": "BLE", "durationSeconds": 1}); e != nil {
		t.Fatal("unknown task blocks diagnostic reconciliation", e)
	}
}

func TestGW32HTTPTasksAndWireSchema(t *testing.T) {
	a, actor := gw32Fixture(t)
	p, e := a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	token := "gw32-http-device-secret-with-32-characters"
	if e = a.ProvisionGateway(t.Context(), "gw_test", token); e != nil {
		t.Fatal(e)
	}
	handler := a.Handler()
	call := func(method, path string, body any, device bool, headers map[string]string) *httptest.ResponseRecorder {
		t.Helper()
		var data []byte
		if body != nil {
			data = []byte(encode(body))
		}
		req := httptest.NewRequest(method, path, bytes.NewReader(data))
		req.Header.Set("Content-Type", "application/json")
		for k, v := range headers {
			req.Header.Set(k, v)
		}
		if device {
			req.Header.Set("Authorization", "Bearer "+token)
		} else {
			req = req.WithContext(context.WithValue(req.Context(), authKey{}, &authInfo{Principal: p}))
		}
		out := httptest.NewRecorder()
		handler.ServeHTTP(out, req)
		return out
	}
	request := map[string]any{"gatewayId": "gw_test", "type": "GATT_CONNECT", "parameters": gw32ConnectionParams(), "expiresAt": Timestamp(Now() + 60000)}
	response := call(http.MethodPost, "/api/v1/tasks", request, false, map[string]string{"Idempotency-Key": "http-connect"})
	if response.Code != 202 {
		t.Fatalf("GATT_CONNECT HTTP %d %s", response.Code, response.Body.String())
	}
	var task map[string]any
	json.Unmarshal(response.Body.Bytes(), &task)
	if e = a.Validate("Task", task); e != nil {
		t.Fatal(e)
	}
	response = call(http.MethodGet, "/api/v1/gateway-ingress/commands", nil, true, nil)
	if response.Code != 200 {
		t.Fatal(response.Code, response.Body.String())
	}
	var commands map[string]any
	json.Unmarshal(response.Body.Bytes(), &commands)
	if len(List(commands, "commands")) != 1 {
		t.Fatal(commands)
	}
	if e = a.Validate("Command", List(commands, "commands")[0]); e != nil {
		t.Fatal(e)
	}
	result := controlResult(Str(task, "taskId"), "http-connected", 1, "SUCCEEDED", "VERIFIED", gw32ConnectionEvidence(strings.Repeat("d", 32)))
	response = call(http.MethodPost, "/api/v1/gateway-ingress/results", controlBatch(a, "gw_test", result), true, nil)
	if response.Code != 200 {
		t.Fatal(response.Code, response.Body.String())
	}
	var ack map[string]any
	json.Unmarshal(response.Body.Bytes(), &ack)
	if controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(ack)
	}
	cfg := map[string]any{"expectedReportedRevision": 0, "configuration": map[string]any{"network": map[string]any{"ethernet": map[string]any{"enabled": true, "ipv4": map[string]any{"mode": "STATIC", "address": "192.168.1.42", "prefixLength": 24, "gateway": "192.168.1.1", "dns": []any{"1.1.1.1", "8.8.8.8"}}}, "uplinkOrder": []any{"ETHERNET"}}}, "expiresAt": Timestamp(Now() + 60000)}
	response = call(http.MethodPut, "/api/v1/gateways/gw_test/configuration", cfg, false, map[string]string{"Idempotency-Key": "http-network", "If-Match": "\"1\""})
	if response.Code != 202 {
		t.Fatalf("network ConfigurationUpdate HTTP %d %s", response.Code, response.Body.String())
	}
	response = call(http.MethodGet, "/api/v1/gateway-ingress/commands", nil, true, nil)
	json.Unmarshal(response.Body.Bytes(), &commands)
	if len(List(commands, "commands")) != 1 {
		t.Fatal(commands)
	}
	if e = a.Validate("Command", List(commands, "commands")[0]); e != nil {
		t.Fatal(e)
	}
	Obj(request, "parameters")["autoTrigger"] = map[string]any{"ruleId": "injected"}
	response = call(http.MethodPost, "/api/v1/tasks", request, false, map[string]string{"Idempotency-Key": "injected"})
	if response.Code != 422 {
		t.Fatalf("public autoTrigger accepted: %d", response.Code)
	}
}

func TestGW32SubscriptionRequiresCaptureCapability(t *testing.T) {
	a, actor := gw32Fixture(t)
	ref := gw32Open(t, a, actor)
	var raw string
	if e := a.Management.QueryRow("SELECT capabilities_json FROM gateways WHERE gateway_id='gw_test'").Scan(&raw); e != nil {
		t.Fatal(e)
	}
	var caps []string
	if e := json.Unmarshal([]byte(raw), &caps); e != nil {
		t.Fatal(e)
	}
	filtered := []string{}
	for _, cap := range caps {
		if cap != "GW32_CAPTURE_CONTEXT_1" {
			filtered = append(filtered, cap)
		}
	}
	if _, e := a.Management.Exec("UPDATE gateways SET capabilities_json=? WHERE gateway_id='gw_test'", encode(filtered)); e != nil {
		t.Fatal(e)
	}
	_, e := gw32TryTask(a, actor, "capture-required", "gw_test", "GATT_SUBSCRIBE", map[string]any{"radio": "NRF54", "connectionRef": ref, "serviceUuid": "180f", "characteristicUuid": "2a19", "mode": "NOTIFY", "timeoutMs": 1000})
	if e == nil {
		t.Fatal("subscription without capture context capability accepted")
	}
}

func TestGW32LegacyGATTContractRemainsAvailable(t *testing.T) {
	a, actor := controlFixture(t)
	if _, e := a.Management.Exec("UPDATE gateways SET capabilities_json='[\"GATT_WRITE\"]' WHERE gateway_id='gw_test'"); e != nil {
		t.Fatal(e)
	}
	params := map[string]any{"deviceAddress": "AA:BB:CC:DD:EE:FF", "addressType": "PUBLIC", "serviceUuid": "180f", "characteristicUuid": "2a19", "timeoutMs": 1000, "writeMode": "WITHOUT_RESPONSE", "verification": "NONE", "valueBase64": "AQ=="}
	if _, e := gw32TryTask(a, actor, "legacy-write", "gw_test", "GATT_WRITE", params); e != nil {
		t.Fatalf("unchanged 1.0 reference task rejected: %v", e)
	}
}
