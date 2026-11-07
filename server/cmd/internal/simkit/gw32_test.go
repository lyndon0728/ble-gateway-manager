package simkit

import (
	"context"
	"encoding/json"
	"path/filepath"
	"testing"
	"time"
)

func gw32Command(id, kind string, params map[string]any) map[string]any {
	return map[string]any{"schemaVersion": "1.0", "namespaceId": "ns_test", "gatewayId": "gw_test", "taskId": id, "type": kind, "parameters": params, "requestDigest": Hash(params), "expiresAt": time.Now().Add(time.Minute).UTC().Format(time.RFC3339Nano)}
}
func latest(t *testing.T, g *Gateway, task string) map[string]any {
	t.Helper()
	var raw string
	if e := g.DB.QueryRow("SELECT result_json FROM results WHERE task_id=? ORDER BY sequence DESC LIMIT 1", task).Scan(&raw); e != nil {
		t.Fatal(e)
	}
	var v map[string]any
	if e := json.Unmarshal([]byte(raw), &v); e != nil {
		t.Fatal(e)
	}
	return v
}
func TestMixedSpoolVersionsSurviveProfileChange(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "g.db")
	g, e := OpenGateway(path, "ns_test", "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	if e = g.Enqueue(ctx, 2); e != nil {
		t.Fatal(e)
	}
	old, _ := g.Batch(ctx)
	g.DB.Close()
	g, e = OpenGatewayProfile(path, "ns_test", "gw_test", "gw32-profile-1")
	if e != nil {
		t.Fatal(e)
	}
	defer g.DB.Close()
	if e = g.Enqueue(ctx, 1); e != nil {
		t.Fatal(e)
	}
	batch, _ := g.Batch(ctx)
	if Str(batch, "schemaVersion") != "1.0" || len(batch["events"].([]any)) != 2 || Hash(batch["events"]) != Hash(old["events"]) {
		t.Fatal("old spool was rewritten or mixed with 1.1")
	}
	items := []any{}
	for _, event := range batch["events"].([]any) {
		items = append(items, map[string]any{"eventId": Str(event.(map[string]any), "eventId"), "status": "ACCEPTED"})
	}
	ack := map[string]any{"schemaVersion": "1.1", "namespaceId": "ns_test", "gatewayId": "gw_test", "batchId": batch["batchId"], "durability": "RAW_PERSISTED", "items": items}
	if g.Ack(ctx, batch, ack) == nil {
		t.Fatal("accepted ACK for different schema version")
	}
	ack["schemaVersion"] = "1.0"
	if e = g.Ack(ctx, batch, ack); e != nil {
		t.Fatal(e)
	}
	batch, _ = g.Batch(ctx)
	if Str(batch, "schemaVersion") != "1.1" || len(batch["events"].([]any)) != 2 {
		t.Fatal("expected one ADV/SCAN_RSP pair")
	}
	adv := batch["events"].([]any)[0].(map[string]any)
	scan := batch["events"].([]any)[1].(map[string]any)
	if Str(object(adv, "capture"), "kind") != "ADV" || Str(object(scan, "capture"), "kind") != "SCAN_RSP" {
		t.Fatal("wrong capture fixture")
	}
}
func TestGW32ConnectionSubscriptionAndRestartLinkDown(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "g.db")
	g, e := OpenGatewayProfile(path, "ns_test", "gw_test", "gw32-profile-1")
	if e != nil {
		t.Fatal(e)
	}
	connect := gw32Command("connect", "GATT_CONNECT", map[string]any{"radio": "NRF54", "deviceAddress": GW32PeerAddress, "addressType": "PUBLIC", "security": "UNENCRYPTED", "timeoutMs": 1000})
	if e = g.Command(ctx, connect); e != nil {
		t.Fatal(e)
	}
	ref := Str(object(latest(t, g, "connect"), "details"), "connectionRef")
	if len(ref) != 32 {
		t.Fatal("connection reference is not 128-bit hex")
	}
	if e = g.Command(ctx, connect); e != nil {
		t.Fatal(e)
	}
	var executions int
	if e = g.DB.QueryRow("SELECT execution_count FROM journal WHERE task_id='connect'").Scan(&executions); e != nil || executions != 1 {
		t.Fatal("replayed CONNECT executed again")
	}
	subscribe := gw32Command("subscribe", "GATT_SUBSCRIBE", map[string]any{"radio": "NRF54", "connectionRef": ref, "serviceUuid": GW32ServiceUUID, "characteristicUuid": GW32CharacteristicUUID, "mode": "NOTIFY", "timeoutMs": 1000})
	if e = g.Command(ctx, subscribe); e != nil {
		t.Fatal(e)
	}
	if Str(latest(t, g, "subscribe"), "state") != "SUCCEEDED" {
		t.Fatal("subscription failed")
	}
	g.DB.Close()
	g, e = OpenGatewayProfile(path, "ns_test", "gw_test", "gw32-profile-1")
	if e != nil {
		t.Fatal(e)
	}
	defer g.DB.Close()
	batch, _ := g.Batch(ctx)
	events := batch["events"].([]any)
	if len(events) != 2 || Str(object(events[0].(map[string]any), "capture"), "kind") != "GATT_NOTIFY" || Str(object(events[1].(map[string]any), "capture"), "linkReason") != "ESP_RESTART" {
		t.Fatal("restart lost queued value or link-down")
	}
	subscribe["taskId"] = "stale"
	if e = g.Command(ctx, subscribe); e != nil {
		t.Fatal(e)
	}
	if Str(latest(t, g, "stale"), "errorCode") != "STALE_CONNECTION" {
		t.Fatal("old connection remained usable")
	}
}
func TestNetworkTrialRequiresEveryProbeACKAndRollsBackOnRestart(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "g.db")
	g, e := OpenGatewayProfile(path, "ns_test", "gw_test", "gw32-profile-1")
	if e != nil {
		t.Fatal(e)
	}
	config := map[string]any{"network": map[string]any{"uplinkOrder": []any{"ETHERNET", "WIFI_STA"}, "ethernet": map[string]any{"enabled": true, "ipv4": map[string]any{"mode": "DHCP"}}, "wifiSta": map[string]any{"enabled": true, "ssid": "simulated", "security": "WPA2_PSK", "credentialRef": "local-sim", "ipv4": map[string]any{"mode": "DHCP"}}}}
	cmd := gw32Command("network", "CONFIGURE", map[string]any{"desiredRevision": 2, "configuration": config})
	if e = g.Command(ctx, cmd); e != nil {
		t.Fatal(e)
	}
	batch, e := g.Results(ctx)
	if e != nil {
		t.Fatal(e)
	}
	results := batch["results"].([]any)
	items := []any{}
	for _, v := range results[:2] {
		r := v.(map[string]any)
		items = append(items, map[string]any{"taskId": r["taskId"], "resultId": r["resultId"], "status": "ACCEPTED"})
	}
	ack := map[string]any{"namespaceId": "ns_test", "gatewayId": "gw_test", "durability": "RESULT_PERSISTED", "items": items}
	if e = g.AckResults(ctx, batch, ack); e != nil {
		t.Fatal(e)
	}
	if Str(latest(t, g, "network"), "state") != "EXECUTING" {
		t.Fatal("committed before all interface probe ACKs")
	}
	last := results[2].(map[string]any)
	ack["items"] = []any{map[string]any{"taskId": last["taskId"], "resultId": last["resultId"], "status": "ACCEPTED"}}
	if e = g.AckResults(ctx, batch, ack); e != nil {
		t.Fatal(e)
	}
	if Str(latest(t, g, "network"), "state") != "SUCCEEDED" {
		t.Fatal("did not commit after full custody ACK")
	}
	cmd["taskId"] = "network_uncommitted"
	cmd["parameters"] = map[string]any{"desiredRevision": 3, "configuration": config}
	if e = g.Command(ctx, cmd); e != nil {
		t.Fatal(e)
	}
	g.DB.Close()
	g, e = OpenGatewayProfile(path, "ns_test", "gw_test", "gw32-profile-1")
	if e != nil {
		t.Fatal(e)
	}
	defer g.DB.Close()
	if Str(latest(t, g, "network_uncommitted"), "errorCode") != "NETWORK_TRIAL_RESTART_ROLLBACK" {
		t.Fatal("uncommitted candidate was not rolled back")
	}
	var revision int64
	if e = g.DB.QueryRow("SELECT revision FROM identity WHERE singleton=1").Scan(&revision); e != nil || revision != 2 {
		t.Fatalf("committed revision changed during rollback: %d %v", revision, e)
	}
}
func TestConnectionReferenceUsesRadioBootAndLittleEndianNumber(t *testing.T) {
	nrf, e := ConnectionReference("NRF54", "000102030405060708090a0b0c0d0e0f", 1)
	if e != nil {
		t.Fatal(e)
	}
	esp, _ := ConnectionReference("ESP32", "000102030405060708090a0b0c0d0e0f", 1)
	next, _ := ConnectionReference("NRF54", "000102030405060708090a0b0c0d0e0f", 2)
	if nrf == esp || nrf == next || len(nrf) != 32 {
		t.Fatal("connection namespace collision")
	}
}

func TestGW32RebootInvalidatesRetainedAutomaticObservation(t *testing.T) {
	ctx := context.Background()
	g, e := OpenGatewayProfile(filepath.Join(t.TempDir(), "g.db"), "ns_test", "gw_test", "gw32-profile-1")
	if e != nil {
		t.Fatal(e)
	}
	defer g.DB.Close()
	if e = g.Enqueue(ctx, 1); e != nil {
		t.Fatal(e)
	}
	batch, _ := g.Batch(ctx)
	adv := batch["events"].([]any)[0].(map[string]any)
	params := map[string]any{"radio": "NRF54", "deviceAddress": GW32PeerAddress, "addressType": "PUBLIC", "security": "UNENCRYPTED", "timeoutMs": 1000, "autoTrigger": map[string]any{"advEventId": adv["eventId"], "ruleId": "test_rule", "serviceUuid": GW32ServiceUUID, "maxAgeMs": 5000}}
	if e = g.Command(ctx, gw32Command("automatic_first", "GATT_CONNECT", params)); e != nil {
		t.Fatal(e)
	}
	if Str(latest(t, g, "automatic_first"), "state") != "SUCCEEDED" {
		t.Fatal("fresh observation did not connect")
	}
	if e = g.Command(ctx, gw32Command("reboot", "REBOOT", map[string]any{"reason": "test"})); e != nil {
		t.Fatal(e)
	}
	if e = g.Command(ctx, gw32Command("automatic_after_reboot", "GATT_CONNECT", params)); e != nil {
		t.Fatal(e)
	}
	if Str(latest(t, g, "automatic_after_reboot"), "errorCode") != "STALE_OBSERVATION" {
		t.Fatal("reused pre-reboot evidence")
	}
}
