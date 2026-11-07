package platform

import (
	"context"
	"encoding/base64"
	"errors"
	"strings"
	"testing"
)

func captureFixture(t *testing.T) (*App, string) {
	t.Helper()
	a, actor := pipelineFixture(t)
	_, err := a.Management.Exec("UPDATE gateways SET capabilities_json=? WHERE gateway_id='gw_test'", pipeJSON([]string{CaptureContextCapability}))
	if err != nil {
		t.Fatal(err)
	}
	return a, actor
}
func captureEvent(id, kind string, payload []byte) pipeMap {
	c := pipeMap{"radio": "NRF54", "kind": kind}
	hint := "ble-ad-v1"
	if kind == "ADV" || kind == "SCAN_RSP" {
		c["legacy"] = true
		c["addressType"] = "PUBLIC"
		c["address"] = "AABBCCDDEEFF"
		c["rssiDbm"] = -60
		if kind == "ADV" {
			c["connectable"] = true
		}
	} else {
		c["connectionRef"] = strings.Repeat("a", 32)
		if kind == "GATT_LINK_DOWN" {
			hint = "ble-gatt-link-v1"
			c["linkReason"] = "DISCONNECTED"
		} else {
			hint = "ble-gatt-value-v1"
			c["characteristicHandle"] = 42
		}
	}
	return pipeMap{"eventId": id, "timeQuality": "UNSYNCED", "protocolHint": hint, "payloadBase64": base64.StdEncoding.EncodeToString(payload), "capture": c}
}
func captureBatch(a *App, events ...pipeMap) pipeMap {
	items := []any{}
	for _, event := range events {
		items = append(items, event)
	}
	return pipeMap{"schemaVersion": "1.1", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "batchId": "capture_batch", "events": items}
}

func TestCaptureSemanticValidation(t *testing.T) {
	good := captureEvent("event", "ADV", nil)
	good["capture"].(map[string]any)["observedBy"] = []any{"NRF54", "ESP32"}
	if err := ValidateCaptureEvent("1.1", good, nil); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name    string
		kind    string
		mutate  func(pipeMap)
		payload []byte
	}{
		{"missing-connectable", "ADV", func(e pipeMap) { delete(po(e, "capture"), "connectable") }, nil},
		{"unpaired-address", "ADV", func(e pipeMap) { delete(po(e, "capture"), "addressType") }, nil},
		{"lowercase-address", "ADV", func(e pipeMap) { po(e, "capture")["address"] = "aabbccddeeff" }, nil},
		{"response-connectable", "SCAN_RSP", func(e pipeMap) { po(e, "capture")["connectable"] = false }, nil},
		{"legacy-sid", "ADV", func(e pipeMap) { po(e, "capture")["sid"] = 1 }, nil},
		{"primary-2m", "ADV", func(e pipeMap) { po(e, "capture")["primaryPhy"] = "LE_2M" }, nil},
		{"unknown-rssi-sentinel", "ADV", func(e pipeMap) { po(e, "capture")["rssiDbm"] = -128 }, nil},
		{"merged-esp-primary", "ADV", func(e pipeMap) {
			po(e, "capture")["radio"] = "ESP32"
			po(e, "capture")["observedBy"] = []string{"NRF54", "ESP32"}
		}, nil},
		{"merged-order", "ADV", func(e pipeMap) { po(e, "capture")["observedBy"] = []string{"ESP32", "NRF54"} }, nil},
		{"merged-anonymous", "ADV", func(e pipeMap) {
			c := po(e, "capture")
			delete(c, "address")
			delete(c, "addressType")
			c["observedBy"] = []string{"NRF54", "ESP32"}
		}, nil},
		{"gatt-hint", "GATT_NOTIFY", func(e pipeMap) { e["protocolHint"] = "ble-ad-v1" }, nil},
		{"gatt-handle", "GATT_INDICATE", func(e pipeMap) { po(e, "capture")["characteristicHandle"] = 0 }, nil},
		{"gatt-value-bound", "GATT_NOTIFY", func(e pipeMap) {}, make([]byte, 513)},
		{"gatt-no-ad-metadata", "GATT_NOTIFY", func(e pipeMap) { po(e, "capture")["rssiDbm"] = -20 }, nil},
		{"down-nonempty", "GATT_LINK_DOWN", func(e pipeMap) {}, []byte{1}},
		{"down-handle", "GATT_LINK_DOWN", func(e pipeMap) { po(e, "capture")["characteristicHandle"] = 1 }, nil},
		{"down-reason", "GATT_LINK_DOWN", func(e pipeMap) { po(e, "capture")["linkReason"] = "UNKNOWN" }, nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			e := captureEvent("event", tc.kind, tc.payload)
			tc.mutate(e)
			if err := ValidateCaptureEvent("1.1", e, tc.payload); err == nil {
				t.Fatal("accepted invalid capture", e)
			}
		})
	}
	for _, kind := range []string{"GATT_NOTIFY", "GATT_INDICATE"} {
		if err := ValidateCaptureEvent("1.1", captureEvent("event", kind, nil), nil); err != nil {
			t.Fatal("zero-length ATT values are valid", err)
		}
		payload := make([]byte, 512)
		if err := ValidateCaptureEvent("1.1", captureEvent("event", kind, payload), payload); err != nil {
			t.Fatal(err)
		}
	}
	if err := ValidateCaptureEvent("1.0", good, nil); err == nil {
		t.Fatal("capture accepted in 1.0")
	}
}

func TestCaptureIngressCapabilityDedupAndVersionPersistence(t *testing.T) {
	a, _ := captureFixture(t)
	ctx := context.Background()
	e := captureEvent("capture_1", "ADV", []byte{3, 3, 0x0f, 0x18})
	batch := captureBatch(a, e)
	ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
	if err != nil {
		t.Fatal(err)
	}
	if ps(ack, "schemaVersion") != "1.1" || ps(ack["items"].([]pipeMap)[0], "status") != "ACCEPTED" {
		t.Fatal(ack)
	}
	if err = a.Validate("IngressAck", ack); err != nil {
		t.Fatal(err)
	}
	var hash, metadata string
	if err = a.Pipeline.QueryRow("SELECT e.immutable_hash,r.metadata_json FROM event_receipts e JOIN raw_events r ON r.event_key=e.event_key WHERE e.event_id='capture_1'").Scan(&hash, &metadata); err != nil {
		t.Fatal(err)
	}
	if hash != pipeHash(pipeMap{"schemaVersion": "1.1", "event": e}) || ps(pm(metadata), "ingressSchemaVersion") != "1.1" || pipeHash(po(po(pm(metadata), "rawEvent"), "capture")) != pipeHash(po(e, "capture")) {
		t.Fatal("capture/version not persisted in custody transaction")
	}
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "DUPLICATE" {
		t.Fatal(ack, err)
	}
	crossVersion := pm(pipeJSON(batch))
	crossVersion["schemaVersion"] = "1.0"
	delete(pl(crossVersion, "events")[0].(map[string]any), "capture")
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", crossVersion)
	if err != nil || ps(ack, "schemaVersion") != "1.0" || ps(ack["items"].([]pipeMap)[0], "status") != "REJECTED_PERMANENT" {
		t.Fatal("frozen event silently changed ingress version", ack, err)
	}
	po(e, "capture")["rssiDbm"] = -61
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "REJECTED_PERMANENT" {
		t.Fatal("changed capture reused event identity", ack, err)
	}
	_, _ = a.Management.Exec("UPDATE gateways SET capabilities_json='[]' WHERE gateway_id='gw_test'")
	_, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", captureBatch(a, captureEvent("without_cap", "ADV", nil)))
	var ie *PipelineIngressError
	if !errors.As(err, &ie) || ie.Code != "CAPTURE_CAPABILITY_REQUIRED" {
		t.Fatal("missing signed capability accepted", err)
	}
	legacy := testIngress(a, "legacy_after_11")
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", legacy)
	if err != nil || ps(ack, "schemaVersion") != "1.0" {
		t.Fatal("old gateway/spool compatibility lost", ack, err)
	}
	var oldHash string
	_ = a.Pipeline.QueryRow("SELECT immutable_hash FROM event_receipts WHERE event_id='legacy_after_11'").Scan(&oldHash)
	if oldHash != pipeHash(pl(legacy, "events")[0]) {
		t.Fatal("changed legacy identity hash")
	}
}

func installCaptureBundle(t *testing.T, a *App, action string) {
	t.Helper()
	installPipelineTestBundle(t, a, "before_capture", "http://127.0.0.1:9999/inbox", 1)
	var raw string
	_ = a.Pipeline.QueryRow("SELECT manifest_json FROM pipeline_bundles WHERE bundle_id='before_capture'").Scan(&raw)
	m := pm(raw)
	rule := pl(m, "rules")[0].(map[string]any)
	rule["action"] = action
	if action == "PASSTHROUGH" {
		delete(rule, "parserId")
		delete(rule, "parserVersion")
		rule["schemaVersion"] = "raw-1"
	} else {
		rule["parserId"] = "ble_ad"
		rule["parserVersion"] = "1.1.0"
		rule["schemaVersion"] = "ble-ad-2"
	}
	if err := a.activatePipelineBundle(context.Background(), "capture_bundle", pipeJSON(m), pipeHash(m), 2); err != nil {
		t.Fatal(err)
	}
}

func TestCapturePassthroughPreservesFrozenContext(t *testing.T) {
	a, _ := captureFixture(t)
	ctx := context.Background()
	installCaptureBundle(t, a, "PASSTHROUGH")
	event := captureEvent("capture_pass", "GATT_INDICATE", []byte{0x10, 0x20, 0})
	batch := captureBatch(a, event)
	if _, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch); err != nil {
		t.Fatal(err)
	}
	if work, err := a.ProcessOne(ctx); !work || err != nil {
		t.Fatal(work, err)
	}
	var raw []byte
	if err := a.Pipeline.QueryRow("SELECT wire_payload FROM delivery_outbox").Scan(&raw); err != nil {
		t.Fatal(err)
	}
	wire := pm(string(raw))
	if err := a.Validate("CustomerEvent", wire); err != nil {
		t.Fatal(err)
	}
	if ps(wire, "sourceSchemaVersion") != "1.1" || ps(wire, "schemaVersion") != "raw-1" || pipeHash(po(wire, "capture")) != pipeHash(po(event, "capture")) || ps(wire, "rawBase64") != ps(event, "payloadBase64") {
		t.Fatal("lost source metadata", wire)
	}
	first, err := a.claimDeliveryBatch(ctx)
	if err != nil {
		t.Fatal(err)
	}
	_, _ = a.Pipeline.Exec("UPDATE delivery_outbox SET lease_until_ms=0")
	next, err := a.claimDeliveryBatch(ctx)
	if err != nil || len(next.Items) != 1 {
		t.Fatal(next, err)
	}
	if first.Items[0].ID != next.Items[0].ID || string(first.Items[0].Payload) != string(next.Items[0].Payload) {
		t.Fatal("retry recomputed capture metadata")
	}
}

func TestCaptureBLEAD11QuarantineAndSourceProvenance(t *testing.T) {
	a, _ := captureFixture(t)
	ctx := context.Background()
	installCaptureBundle(t, a, "DECODE")
	valid := captureEvent("uuid_valid", "ADV", []byte{3, 3, 0x0f, 0x18})
	invalid := captureEvent("uuid_invalid", "SCAN_RSP", []byte{3, 3, 0x0f, 0x18, 2, 0x20, 0})
	ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", captureBatch(a, valid, invalid))
	if err != nil {
		t.Fatal(err)
	}
	if len(ack["items"].([]pipeMap)) != 2 {
		t.Fatal(ack)
	}
	for i := 0; i < 2; i++ {
		if _, err = a.ProcessOne(ctx); err != nil {
			t.Fatal(err)
		}
	}
	var raw []byte
	if err = a.Pipeline.QueryRow("SELECT wire_payload FROM delivery_outbox").Scan(&raw); err != nil {
		t.Fatal(err)
	}
	wire := pm(string(raw))
	if err = a.Validate("CustomerEvent", wire); err != nil {
		t.Fatal(err)
	}
	services := pl(po(wire, "data"), "serviceUuids")
	if len(services) != 1 || ps(services[0].(map[string]any), "sourceEventId") != "uuid_valid" || pn(services[0].(map[string]any), "wireBits") != 16 {
		t.Fatal(wire)
	}
	var code, state string
	err = a.Pipeline.QueryRow("SELECT j.state,j.error_code FROM processing_jobs j JOIN event_receipts e ON e.event_key=j.event_key WHERE e.event_id='uuid_invalid'").Scan(&state, &code)
	if err != nil || state != "quarantined" || code != "UUID_FIELD_LENGTH" {
		t.Fatal("invalid service UUID not isolated", state, code, err)
	}
	var count int
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM raw_events").Scan(&count)
	if count != 2 {
		t.Fatal("custody originals lost", count)
	}
}

func TestCaptureLinkDownWaitsForDurableControlConsumption(t *testing.T) {
	a, _ := captureFixture(t)
	ctx := context.Background()
	event := captureEvent("link_down", "GATT_LINK_DOWN", nil)
	if _, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", captureBatch(a, event)); err != nil {
		t.Fatal(err)
	}
	if work, err := a.ProcessOne(ctx); !work || err != nil {
		t.Fatal(work, err)
	}
	key := pipeStable("event", []string{a.Config.NamespaceID, "gw_test", "link_down"})
	view, err := a.eventView(ctx, key, true)
	if err != nil || ps(view, "state") != "FILTERED" {
		t.Fatal(view, err)
	}
	if err = a.Validate("EventView", view); err != nil {
		t.Fatal(err)
	}
	var outboxes int
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM delivery_outbox").Scan(&outboxes)
	if outboxes != 0 {
		t.Fatal("link-down entered customer data route")
	}
	_, _ = a.Pipeline.Exec("UPDATE event_receipts SET raw_retention_until_ms=0")
	if err = a.ReclaimPipelinePayloads(ctx); err != nil {
		t.Fatal(err)
	}
	var remaining int
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM raw_events").Scan(&remaining)
	if remaining != 1 {
		t.Fatal("unconsumed link-down was reclaimed")
	}
	_, err = a.Pipeline.Exec("INSERT INTO capture_consumptions(event_key,consumed_at_ms,outcome) VALUES(?,?,'control_applied')", key, pnow())
	if err != nil {
		t.Fatal(err)
	}
	if err = a.ReclaimPipelinePayloads(ctx); err != nil {
		t.Fatal(err)
	}
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM raw_events").Scan(&remaining)
	if remaining != 0 {
		t.Fatal("consumed control raw did not expire")
	}
	ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", captureBatch(a, event))
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "DUPLICATE" {
		t.Fatal("control event dedup anchor removed", ack, err)
	}
}

func TestCaptureValidationPrecedesAnyBatchWrite(t *testing.T) {
	a, _ := captureFixture(t)
	ctx := context.Background()
	valid := captureEvent("valid_before_bad", "ADV", []byte{2, 1, 6})
	invalid := captureEvent("bad_gatt", "GATT_NOTIFY", make([]byte, 513))
	if ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", captureBatch(a, valid, invalid)); err == nil || ack != nil {
		t.Fatal("invalid capture accepted", ack, err)
	}
	var n int
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM event_receipts").Scan(&n)
	if n != 0 {
		t.Fatal("invalid envelope partially committed")
	}
	if _, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", captureBatch(a, valid)); err != nil {
		t.Fatal(err)
	}
}

func TestCaptureOversizedADIsDurablyQuarantinedNotDropped(t *testing.T) {
	a, _ := captureFixture(t)
	ctx := context.Background()
	installCaptureBundle(t, a, "DECODE")
	event := captureEvent("oversized_ad", "ADV", make([]byte, 1651))
	ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", captureBatch(a, event))
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "ACCEPTED" {
		t.Fatal("original AD was dropped before custody", ack, err)
	}
	if _, err = a.ProcessOne(ctx); err != nil {
		t.Fatal(err)
	}
	var state, code string
	_ = a.Pipeline.QueryRow("SELECT state,error_code FROM processing_jobs").Scan(&state, &code)
	if state != "quarantined" || code != "PAYLOAD_TOO_LARGE" {
		t.Fatal(state, code)
	}
}
