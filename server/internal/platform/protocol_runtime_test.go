package platform

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"math"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
)

func protocolExample(kind string) pipeMap {
	return pm(`{"dslVersion":"1.0","inputKind":"` + kind + `","length":4,"fields":[{"name":"temperature","offset":0,"type":"int16","endian":"little","scale":0.01,"unit":"Cel","min":-100,"max":150},{"name":"humidity","offset":2,"type":"uint16","endian":"big","scale":0.01,"min":0,"max":100}]}`)
}
func TestProtocolDSLNumericAndBounds(t *testing.T) {
	s := protocolExample("RAW")
	payload := []byte{0x2e, 0xfb, 0x13, 0x88}
	rows, e := ParseProtocolSpec(s, payload)
	if e != nil || len(rows) != 1 || rows[0].Data["temperature"] != float64(-12.34) || rows[0].Data["humidity"] != float64(50) || rows[0].Data["temperatureUnit"] != "Cel" {
		t.Fatalf("%v %v", rows, e)
	}
	for _, bad := range [][]byte{payload[:3], append(append([]byte{}, payload...), 0), {0xff, 0x7f, 0x13, 0x88}} {
		if _, e = ParseProtocolSpec(s, bad); e == nil {
			t.Fatal("accepted invalid packet", bad)
		}
	}
	s = pm(`{"dslVersion":"1.0","inputKind":"RAW","length":2,"fields":[{"name":"mode","offset":0,"type":"uint16","endian":"big","bitOffset":4,"bitLength":2,"enum":{"0":"OFF","1":"ON"}},{"name":"raw","offset":0,"type":"bytes","length":2}]}`)
	rows, e = ParseProtocolSpec(s, []byte{0, 0x10})
	if e != nil || rows[0].Data["mode"] != "ON" || rows[0].Data["raw"] != "0010" {
		t.Fatal(rows, e)
	}
	if _, e = ParseProtocolSpec(s, []byte{0, 0x30}); e == nil {
		t.Fatal("unknown enum accepted")
	}
}
func TestProtocolDSLRejectsIncompleteUnsafeGrammar(t *testing.T) {
	cases := map[string]func(pipeMap){
		"unknown":           func(s pipeMap) { s["script"] = "fetch(secret)" },
		"version":           func(s pipeMap) { s["dslVersion"] = "2.0" },
		"missing-offset":    func(s pipeMap) { delete(pl(s, "fields")[0].(map[string]any), "offset") },
		"null-offset":       func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["offset"] = nil },
		"negative-offset":   func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["offset"] = -1 },
		"fractional-offset": func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["offset"] = 0.5 },
		"out-of-bounds":     func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["offset"] = 4 },
		"missing-endian":    func(s pipeMap) { delete(pl(s, "fields")[0].(map[string]any), "endian") },
		"nan":               func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["scale"] = math.NaN() },
		"infinity":          func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["scale"] = math.Inf(1) },
		"unbounded":         func(s pipeMap) { delete(s, "length"); s["minLength"] = 1 },
		"two-lengths":       func(s pipeMap) { s["minLength"] = 1; s["maxLength"] = 9 },
		"unknown-type":      func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["type"] = "javascript" },
		"field-extra":       func(s pipeMap) { pl(s, "fields")[0].(map[string]any)["function"] = "eval" },
		"signed-bits":       func(s pipeMap) { f := pl(s, "fields")[0].(map[string]any); f["bitOffset"] = 0; f["bitLength"] = 1 },
		"duplicate":         func(s pipeMap) { pl(s, "fields")[1].(map[string]any)["name"] = "temperature" },
		"unit-collision":    func(s pipeMap) { pl(s, "fields")[1].(map[string]any)["name"] = "temperatureUnit" },
		"bad-crc":           func(s pipeMap) { s["checks"] = []any{pipeMap{"type": "crc16", "offset": 2}} },
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			s := protocolExample("RAW")
			mutate(s)
			if e := ValidateProtocolSpec(s); e == nil {
				t.Fatal("accepted unsafe/incomplete spec")
			}
		})
	}
}
func TestProtocolDSLCRCAndAdvertisingSource(t *testing.T) {
	s := pm(`{"dslVersion":"1.0","inputKind":"BLE_AD","length":4,"source":{"adType":255,"companyId":4660,"dataOffset":2},"fields":[{"name":"value","offset":0,"type":"uint16","endian":"little"}],"checks":[{"type":"crc16-ccitt-false","start":0,"end":2,"offset":2,"endian":"little"}]}`)
	value := []byte{42, 0}
	value = binary.LittleEndian.AppendUint16(value, ReferenceCRC16(value))
	raw := append([]byte{7, 255, 0x34, 0x12}, value...)
	got, e := ParseProtocolSpec(s, raw)
	if e != nil || got[0].Data["value"] != float64(42) {
		t.Fatal(got, e)
	}
	bad := append([]byte{}, raw...)
	bad[len(bad)-1] ^= 1
	if _, e = ParseProtocolSpec(s, bad); e == nil {
		t.Fatal("bad CRC accepted")
	}
	if _, e = ParseProtocolSpec(s, append(raw, raw...)); e == nil {
		t.Fatal("ambiguous source accepted")
	}
	bad = append([]byte{}, raw...)
	bad[2] = 1
	if _, e = ParseProtocolSpec(s, bad); e == nil {
		t.Fatal("wrong manufacturer accepted")
	}
	service := pm(`{"dslVersion":"1.0","inputKind":"BLE_AD","length":1,"source":{"adType":22,"serviceUuid":"180f","dataOffset":2},"fields":[{"name":"battery","offset":0,"type":"uint8"}]}`)
	got, e = ParseProtocolSpec(service, []byte{4, 0x16, 0x0f, 0x18, 88})
	if e != nil || got[0].Data["battery"] != float64(88) {
		t.Fatal(got, e)
	}
}
func protocolRegister(t *testing.T, a *App, id, version string, s pipeMap) {
	t.Helper()
	tx, e := a.Management.BeginTx(t.Context(), nil)
	if e != nil {
		t.Fatal(e)
	}
	defer tx.Rollback()
	if e = RegisterAIParser(t.Context(), tx, id, version, s); e != nil {
		t.Fatal(e)
	}
	if e = tx.Commit(); e != nil {
		t.Fatal(e)
	}
}
func TestProtocolRegistryPublicationIsImmutable(t *testing.T) {
	a, actor := pipelineFixture(t)
	s := protocolExample("RAW")
	protocolRegister(t, a, "temperature_profile", "1", s)
	protocolRegister(t, a, "temperature_profile", "1", s)
	tx, e := a.Management.BeginTx(t.Context(), nil)
	if e != nil {
		t.Fatal(e)
	}
	changed := protocolExample("RAW")
	pl(changed, "fields")[0].(map[string]any)["scale"] = 1
	if e = RegisterAIParser(t.Context(), tx, "temperature_profile", "1", changed); e == nil {
		t.Fatal("overwrote published version")
	}
	if e = RegisterAIParser(t.Context(), tx, "ble_ad", "9", s); e == nil {
		t.Fatal("replaced built-in parser")
	}
	tx.Rollback()
	if _, e = a.Management.Exec("UPDATE protocol_parsers SET spec_json='{}'"); e == nil {
		t.Fatal("parser immutability trigger absent")
	}
	principal, e := a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	items, e := a.protocolCatalog(t.Context(), principal)
	if e != nil || len(items) != 1 || ps(items[0], "schemaVersion") != ProtocolDefaultSchema {
		t.Fatal(items, e)
	}
}
func protocolCompile(t *testing.T, a *App, rules ...pipeMap) pipeMap {
	t.Helper()
	tx, e := a.Management.BeginTx(t.Context(), nil)
	if e != nil {
		t.Fatal(e)
	}
	defer tx.Rollback()
	list := []any{}
	for _, r := range rules {
		list = append(list, r)
	}
	m, e := a.compileBundle(t.Context(), tx, pipeMap{"name": "Protocols", "rules": list, "defaultAction": "QUARANTINE"})
	if e != nil {
		t.Fatal(e)
	}
	return m
}
func protocolRule(id, parser, version, peer string) pipeMap {
	return pipeMap{"ruleId": id, "priority": 10, "match": pipeMap{"peerIds": []any{peer}}, "action": "DECODE", "parserId": parser, "parserVersion": version, "schemaVersion": ProtocolDefaultSchema, "targetVersions": []any{}}
}
func protocolResult(t *testing.T, a *App, event string) pipeMap {
	t.Helper()
	var raw string
	e := a.Pipeline.QueryRow("SELECT r.result_json FROM processing_results r JOIN processing_jobs j ON j.processing_id=r.processing_id JOIN event_receipts e ON e.event_key=j.event_key WHERE e.event_id=? ORDER BY j.revision DESC LIMIT 1", event).Scan(&raw)
	if e != nil {
		t.Fatal(e)
	}
	return po(pm(raw), "data")
}
func TestProtocolPipelineMultiplePeersAndFrozenVersions(t *testing.T) {
	a, actor := captureFixture(t)
	p1 := autoCreatePeer(t, a, actor, "AABBCCDDEEFF", "PUBLIC", "scope_default_group")
	p2 := autoCreatePeer(t, a, actor, "AABBCCDDEE11", "PUBLIC", "scope_default_group")
	s := protocolExample("RAW")
	protocolRegister(t, a, "sensor_one", "1", s)
	other := protocolExample("RAW")
	pl(other, "fields")[0].(map[string]any)["scale"] = 0.02
	protocolRegister(t, a, "sensor_two", "1", other)
	m := protocolCompile(t, a, protocolRule("one", "sensor_one", "1", p1), protocolRule("two", "sensor_two", "1", p2))
	if e := a.activatePipelineBundle(t.Context(), "protocol_old", pipeJSON(m), pipeHash(m), 1); e != nil {
		t.Fatal(e)
	}
	one := captureEvent("one", "ADV", []byte{100, 0, 0x13, 0x88})
	two := captureEvent("two", "ADV", []byte{100, 0, 0x13, 0x88})
	po(two, "capture")["address"] = "AABBCCDDEE11"
	if _, e := a.AcceptIngress(t.Context(), a.Config.NamespaceID, "gw_test", captureBatch(a, one, two)); e != nil {
		t.Fatal(e)
	}
	protocolRegister(t, a, "sensor_one", "2", other)
	next := protocolCompile(t, a, protocolRule("new", "sensor_one", "2", p1))
	if e := a.activatePipelineBundle(t.Context(), "protocol_new", pipeJSON(next), pipeHash(next), 2); e != nil {
		t.Fatal(e)
	}
	if _, e := a.Management.Exec("UPDATE ble_peers SET device_address='112233445566' WHERE peer_id=?", p1); e != nil {
		t.Fatal(e)
	}
	for range 2 {
		if work, e := a.ProcessOne(t.Context()); e != nil || !work {
			t.Fatal(work, e)
		}
	}
	if protocolResult(t, a, "one")["temperature"] != float64(1) || protocolResult(t, a, "two")["temperature"] != float64(2) {
		t.Fatal("wrong peer parser or mutable version used")
	}
	if _, e := a.Management.Exec("DELETE FROM protocol_parsers"); e != nil {
		t.Fatal(e)
	}
	// Pipeline worker needs only the frozen manifest, not the model service or registry.
	j := pipeJob{Manifest: pipeJSON(m), Meta: pipeJSON(pipeMap{"rawEvent": one}), Raw: []byte{100, 0, 0x13, 0x88}}
	_, records, e := evaluatePipeline(j)
	if e != nil || records[0].Data["temperature"] != float64(1) {
		t.Fatal(records, e)
	}
}
func TestProtocolBundleRejectsBroadAndUnscopedGATT(t *testing.T) {
	a, actor := captureFixture(t)
	peer := autoCreatePeer(t, a, actor, "AABBCCDDEEFF", "PUBLIC", "scope_default_group")
	protocolRegister(t, a, "private", "1", protocolExample("GATT_VALUE"))
	tx, e := a.Management.BeginTx(t.Context(), nil)
	if e != nil {
		t.Fatal(e)
	}
	defer tx.Rollback()
	r := protocolRule("r", "private", "1", peer)
	delete(po(r, "match"), "peerIds")
	if _, e = a.compileBundle(t.Context(), tx, pipeMap{"rules": []any{r}}); e == nil {
		t.Fatal("broad private parser accepted")
	}
	po(r, "match")["peerIds"] = []any{peer}
	if _, e = a.compileBundle(t.Context(), tx, pipeMap{"rules": []any{r}}); e == nil {
		t.Fatal("GATT parser without UUID scope accepted")
	}
}

func protocolNotify(id, ref string, value []byte) pipeMap {
	e := captureEvent(id, "GATT_NOTIFY", value)
	c := po(e, "capture")
	c["connectionRef"] = ref
	c["addressType"] = "PUBLIC"
	c["address"] = "AABBCCDDEEFF"
	return e
}
func protocolSnapshot(t *testing.T, a *App, ref, char string) {
	t.Helper()
	_, e := a.Management.Exec("INSERT INTO gatt_discovery_snapshots(snapshot_id,namespace_id,gateway_id,radio,connection_ref,address_type,device_address,characteristic_handle,service_uuid,characteristic_uuid,source_task_id,created_at_ms) VALUES(?,?,'gw_test','NRF54',?,'PUBLIC','AABBCCDDEEFF',42,?,?,?,?)", ID("snapshot"), a.Config.NamespaceID, ref, autoBatteryUUID, gw32CanonicalUUID(char), "verified_task", Now())
	if e != nil {
		t.Fatal(e)
	}
}
func TestProtocolGATTHistoricalEvidenceFrozenPerRevision(t *testing.T) {
	a, actor := captureFixture(t)
	peer := autoCreatePeer(t, a, actor, "AABBCCDDEEFF", "PUBLIC", "scope_default_group")
	s := protocolExample("GATT_VALUE")
	protocolRegister(t, a, "gatt_sensor", "1", s)
	rule := protocolRule("gatt", "gatt_sensor", "1", peer)
	po(rule, "match")["serviceUuids"] = []any{autoBatteryUUID}
	po(rule, "match")["characteristicUuids"] = []any{gw32CanonicalUUID("2a19")}
	m := protocolCompile(t, a, rule)
	if e := a.activatePipelineBundle(t.Context(), "gatt_history", pipeJSON(m), pipeHash(m), 1); e != nil {
		t.Fatal(e)
	}
	ref := strings.Repeat("a", 32)
	otherRef := strings.Repeat("b", 32)
	protocolSnapshot(t, a, ref, "2a19")
	protocolSnapshot(t, a, otherRef, "2a1a")
	event := protocolNotify("historical", ref, []byte{100, 0, 0x13, 0x88})
	if _, e := a.AcceptIngress(t.Context(), a.Config.NamespaceID, "gw_test", captureBatch(a, event)); e != nil {
		t.Fatal(e)
	}
	// A conflicting newly delivered discovery must not rewrite accepted evidence.
	protocolSnapshot(t, a, ref, "2a1a")
	if _, e := a.ProcessOne(t.Context()); e != nil {
		t.Fatal(e)
	}
	if protocolResult(t, a, "historical")["temperature"] != float64(1) {
		t.Fatal("historical mapping was replaced")
	}
	context, e := a.freezeCaptureProtocolContext(t.Context(), "gw_test", event)
	if e != nil || ps(context, "state") != "AMBIGUOUS" {
		t.Fatal(context, e)
	}
	event["eventId"] = "conflicted"
	if _, e = a.AcceptIngress(t.Context(), a.Config.NamespaceID, "gw_test", captureBatch(a, event)); e != nil {
		t.Fatal(e)
	}
	if _, e = a.ProcessOne(t.Context()); e != nil {
		t.Fatal(e)
	}
	var state string
	if e = a.Pipeline.QueryRow("SELECT j.state FROM processing_jobs j JOIN event_receipts e ON e.event_key=j.event_key WHERE e.event_id='conflicted'").Scan(&state); e != nil || state != "quarantined" {
		t.Fatal(state, e)
	}
}
func TestProtocolGATTVerifiedSubscriptionRecordsImmutableSnapshot(t *testing.T) {
	a, actor := gw32Fixture(t)
	ref := gw32Open(t, a, actor)
	params := pipeMap{"radio": "NRF54", "connectionRef": ref, "serviceUuid": "180f", "characteristicUuid": "2a19", "mode": "NOTIFY", "timeoutMs": 1000}
	task, e := gw32TryTask(a, actor, "protocol_subscription", "gw_test", "GATT_SUBSCRIBE", params)
	if e != nil {
		t.Fatal(e)
	}
	evidence := pipeMap{"radio": "NRF54", "connectionRef": ref, "serviceUuid": "180f", "characteristicUuid": "2a19", "mode": "NOTIFY", "characteristicHandle": 42, "cccHandle": 43, "cccEnabled": true}
	ack := gw32SendResult(t, a, Str(task, "taskId"), "protocol_subscribed", "SUCCEEDED", 1, evidence)
	if controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(ack)
	}
	if _, e = a.Management.Exec("UPDATE gatt_connections SET state='closed'"); e != nil {
		t.Fatal(e)
	}
	if _, e = a.Management.Exec("UPDATE gatt_subscriptions SET state='closed'"); e != nil {
		t.Fatal(e)
	}
	frozen, e := a.freezeCaptureProtocolContext(t.Context(), "gw_test", protocolNotify("late", ref, []byte{1}))
	if e != nil || ps(frozen, "state") != "VERIFIED" || ps(frozen, "characteristicUuid") != gw32CanonicalUUID("2a19") {
		t.Fatal(frozen, e)
	}
}
func protocolRequest(t *testing.T, a *App, actor, method, path string, body pipeMap, key string) *httptest.ResponseRecorder {
	t.Helper()
	p, e := a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	req := httptest.NewRequest(method, path, bytes.NewBufferString(pipeJSON(body)))
	req.Header.Set("Idempotency-Key", key)
	req = req.WithContext(context.WithValue(req.Context(), authKey{}, &authInfo{Principal: p}))
	router := chi.NewRouter()
	a.RegisterProtocol(router)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)
	return w
}
func TestProtocolBindingsIdempotentAndRouteVersions(t *testing.T) {
	a, actor := captureFixture(t)
	peer := autoCreatePeer(t, a, actor, "AABBCCDDEEFF", "PUBLIC", "scope_default_group")
	protocolRegister(t, a, "binding_sensor", "1", protocolExample("RAW"))
	body := pipeMap{"peerId": peer, "parserId": "binding_sensor", "parserVersion": "1"}
	first := protocolRequest(t, a, actor, "POST", "/api/v1/protocol-bindings", body, "binding-once")
	if first.Code != 201 {
		t.Fatal(first.Code, first.Body.String())
	}
	second := protocolRequest(t, a, actor, "POST", "/api/v1/protocol-bindings", body, "binding-once")
	if second.Code != 201 || first.Body.String() != second.Body.String() {
		t.Fatal("binding replay differs")
	}
	binding := ps(pm(first.Body.String()), "bindingId")
	r := protocolRule("bound", "binding_sensor", "1", "")
	r["match"] = pipeMap{"protocolBindingIds": []any{binding}}
	m := protocolCompile(t, a, r)
	j := pipeJob{Manifest: pipeJSON(m), Meta: pipeJSON(pipeMap{"rawEvent": captureEvent("bound", "ADV", nil)}), Raw: []byte{100, 0, 0x13, 0x88}}
	if _, rows, e := evaluatePipeline(j); e != nil || rows[0].Data["temperature"] != float64(1) {
		t.Fatal(rows, e)
	}
}
func TestProtocolLateDiscoveryRequiresExplicitReprocess(t *testing.T) {
	a, actor := captureFixture(t)
	peer := autoCreatePeer(t, a, actor, "AABBCCDDEEFF", "PUBLIC", "scope_default_group")
	protocolRegister(t, a, "late_sensor", "1", protocolExample("GATT_VALUE"))
	r := protocolRule("late", "late_sensor", "1", peer)
	po(r, "match")["serviceUuids"] = []any{autoBatteryUUID}
	po(r, "match")["characteristicUuids"] = []any{gw32CanonicalUUID("2a19")}
	m := protocolCompile(t, a, r)
	if e := a.activatePipelineBundle(t.Context(), "late_bundle", pipeJSON(m), pipeHash(m), 1); e != nil {
		t.Fatal(e)
	}
	_, e := a.Management.Exec("INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,state,created_at_ms) VALUES('late_artifact','bundle','late_bundle','1',?,'test:bundle',1,'test','verified',?)", pipeHash(m), Now())
	if e != nil {
		t.Fatal(e)
	}
	_, e = a.Management.Exec("INSERT INTO rule_bundles(bundle_id,version,artifact_id,content_hash,manifest_json,state,created_by,created_at_ms) VALUES('late_bundle',1,'late_artifact',?,?,'validated',?,?)", pipeHash(m), pipeJSON(m), actor, Now())
	if e != nil {
		t.Fatal(e)
	}
	ref := strings.Repeat("c", 32)
	event := protocolNotify("late_event", ref, []byte{100, 0, 0x13, 0x88})
	if _, e = a.AcceptIngress(t.Context(), a.Config.NamespaceID, "gw_test", captureBatch(a, event)); e != nil {
		t.Fatal(e)
	}
	protocolSnapshot(t, a, ref, "2a19")
	if _, e = a.ProcessOne(t.Context()); e != nil {
		t.Fatal(e)
	}
	var state, original string
	if e = a.Pipeline.QueryRow("SELECT state,protocol_context_json FROM processing_jobs").Scan(&state, &original); e != nil || state != "quarantined" || ps(pm(original), "state") != "MISSING" {
		t.Fatal(state, original, e)
	}
	selector := pipeMap{"gatewayIds": []any{"gw_test"}, "from": time.Now().Add(-time.Hour).UTC().Format(time.RFC3339Nano), "to": time.Now().Add(time.Hour).UTC().Format(time.RFC3339Nano)}
	preview := pipelineRequest(t, a, actor, "POST", "/api/v1/reprocess-previews", pipeMap{"selector": selector, "bundleId": "late_bundle", "maxSampleEvents": 1}, "late-preview")
	if preview.Code != 200 {
		t.Fatal(preview.Code, preview.Body.String())
	}
	pv := pm(preview.Body.String())
	execute := pipelineRequest(t, a, actor, "POST", "/api/v1/reprocess-jobs", pipeMap{"previewId": pv["previewId"], "selectionDigest": pv["selectionDigest"], "reason": "verified late discovery evidence", "correctionMode": "REPLACE_EVENT_SET", "maxEventsPerSecond": 10}, "late-reprocess")
	if execute.Code != 202 {
		t.Fatal(execute.Code, execute.Body.String())
	}
	if _, e = a.ProcessOne(t.Context()); e != nil {
		t.Fatal(e)
	}
	if protocolResult(t, a, "late_event")["temperature"] != float64(1) {
		t.Fatal("reprocess did not use explicit new evidence")
	}
	var historical string
	if e = a.Pipeline.QueryRow("SELECT protocol_context_json FROM processing_jobs WHERE revision=1").Scan(&historical); e != nil || historical != original {
		t.Fatal("reprocess changed historical revision")
	}
}
func TestProtocolParserTestUsesPublishedVersion(t *testing.T) {
	a, actor := pipelineFixture(t)
	protocolRegister(t, a, "api_sensor", "1", protocolExample("RAW"))
	w := pipelineRequest(t, a, actor, "POST", "/api/v1/parsers/api_sensor/versions/1/tests", pipeMap{"payloadBase64": base64.StdEncoding.EncodeToString([]byte{100, 0, 0x13, 0x88}), "metadata": pipeMap{}}, "")
	if w.Code != 200 || !pb(pm(w.Body.String()), "passed") {
		t.Fatal(w.Code, w.Body.String())
	}
	if e := a.Validate("ParserTestResult", pm(w.Body.String())); e != nil {
		t.Fatal(e)
	}
	rows, e := ParseProtocolSpec(protocolExample("RAW"), []byte{1})
	var pf *ParserFailure
	if len(rows) != 0 || !errors.As(e, &pf) || pf.Code != "INVALID_LENGTH" {
		t.Fatal(rows, e)
	}
}

func TestProtocolPrivateParserScopesAreEnforced(t *testing.T) {
	a, admin := captureFixture(t)
	if _, e := a.Management.Exec("INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('scope_protocol_other','group','scope_default_project','Other',?)", Now()); e != nil {
		t.Fatal(e)
	}
	developer := autoScopedPrincipal(t, a, "protocol_developer", "developer", "scope_default_group")
	deviceAdmin := autoScopedPrincipal(t, a, "protocol_device_admin", "device_admin", "scope_default_group")
	peer := autoCreatePeer(t, a, admin, "AABBCCDDEEFF", "PUBLIC", "scope_default_group")
	for _, item := range []struct{ id, scope string }{{"own_parser", "scope_default_group"}, {"private_other", "scope_protocol_other"}} {
		s := protocolExample("RAW")
		s["description"] = "private-evidence-" + item.id
		pl(s, "fields")[0].(map[string]any)["description"] = "private-field-description-" + item.id
		pl(s, "fields")[0].(map[string]any)["evidence"] = "private-field-evidence-" + item.id
		tx, e := a.Management.BeginTx(t.Context(), nil)
		if e != nil {
			t.Fatal(e)
		}
		if e = RegisterAIParser(t.Context(), tx, item.id, "1", s, item.scope); e != nil {
			tx.Rollback()
			t.Fatal(e)
		}
		if e = tx.Commit(); e != nil {
			t.Fatal(e)
		}
	}
	list := pipelineRequest(t, a, developer, "GET", "/api/v1/parsers", nil, "")
	if list.Code != 200 || !strings.Contains(list.Body.String(), "own_parser") || strings.Contains(list.Body.String(), "private_other") {
		t.Fatal("private parser leaked", list.Code, list.Body.String())
	}
	if !strings.Contains(list.Body.String(), "private-field-evidence-own_parser") {
		t.Fatal("authorized document reader lost parser evidence", list.Body.String())
	}
	restricted := pipelineRequest(t, a, deviceAdmin, "GET", "/api/v1/parsers", nil, "")
	if restricted.Code != 200 || !strings.Contains(restricted.Body.String(), "own_parser") || strings.Contains(restricted.Body.String(), "private-") {
		t.Fatal("parser catalog bypassed document read permission", restricted.Code, restricted.Body.String())
	}
	var reusable pipeMap
	for _, raw := range pl(pm(restricted.Body.String()), "items") {
		item := raw.(map[string]any)
		if ps(item, "parserId") == "own_parser" {
			reusable = po(item, "spec")
		}
	}
	if records, e := ParseProtocolSpec(reusable, []byte{100, 0, 0x13, 0x88}); e != nil || records[0].Data["temperature"] != float64(1) {
		t.Fatal("redaction damaged executable parser", records, e)
	}
	body := pipeMap{"payloadBase64": base64.StdEncoding.EncodeToString([]byte{100, 0, 0x13, 0x88}), "metadata": pipeMap{}}
	for _, parser := range []string{"private_other", "nonexistent"} {
		result := pipelineRequest(t, a, developer, "POST", "/api/v1/parsers/"+parser+"/versions/1/tests", body, "")
		m := pm(result.Body.String())
		if result.Code != 404 || pb(m, "passed") || ps(m, "implementationDigest") != "" || strings.Contains(result.Body.String(), "private-evidence") {
			t.Fatal("unauthorized parser trial disclosed data", result.Body.String())
		}
	}
	allowed := pipelineRequest(t, a, developer, "POST", "/api/v1/parsers/own_parser/versions/1/tests", body, "")
	if allowed.Code != 200 || !pb(pm(allowed.Body.String()), "passed") {
		t.Fatal(allowed.Code, allowed.Body.String())
	}
	bind := protocolRequest(t, a, deviceAdmin, "POST", "/api/v1/protocol-bindings", pipeMap{"peerId": peer, "parserId": "private_other", "parserVersion": "1"}, "private-bind")
	if bind.Code != 422 {
		t.Fatal("cross-scope binding allowed", bind.Code, bind.Body.String())
	}
	bind = protocolRequest(t, a, deviceAdmin, "POST", "/api/v1/protocol-bindings", pipeMap{"peerId": peer, "parserId": "own_parser", "parserVersion": "1"}, "own-bind")
	if bind.Code != 201 {
		t.Fatal(bind.Code, bind.Body.String())
	}
	// Existing idempotency response is not readable after losing peer scope.
	if _, e := a.Management.Exec("UPDATE ble_peers SET scope_id='scope_protocol_other' WHERE peer_id=?", peer); e != nil {
		t.Fatal(e)
	}
	replayed := protocolRequest(t, a, deviceAdmin, "POST", "/api/v1/protocol-bindings", pipeMap{"peerId": peer, "parserId": "own_parser", "parserVersion": "1"}, "own-bind")
	if replayed.Code != 404 {
		t.Fatal("replay bypassed changed permission", replayed.Code, replayed.Body.String())
	}
}
