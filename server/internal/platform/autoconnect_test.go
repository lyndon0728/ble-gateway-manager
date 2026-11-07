package platform

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
)

const autoBatteryUUID = "0000180f-0000-1000-8000-00805f9b34fb"

func autoFixture(t *testing.T) (*App, string, string) {
	t.Helper()
	a, actor := gw32Fixture(t)
	if e := a.ensurePipelineDefault(t.Context()); e != nil {
		t.Fatal(e)
	}
	peer := autoCreatePeer(t, a, actor, "AABBCCDDEEFF", "PUBLIC", "scope_default_group")
	return a, actor, peer
}
func autoRequest(t *testing.T, a *App, actor, method, path string, body pipeMap, key string, headers ...map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	p, e := a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	r := httptest.NewRequest(method, path, bytes.NewBufferString(pipeJSON(body)))
	r.Header.Set("Content-Type", "application/json")
	r.Header.Set("Idempotency-Key", key)
	for _, h := range headers {
		for k, v := range h {
			r.Header.Set(k, v)
		}
	}
	r = r.WithContext(context.WithValue(r.Context(), authKey{}, &authInfo{Principal: p}))
	router := chi.NewRouter()
	a.RegisterAutoConnect(router)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, r)
	return w
}
func autoCreatePeer(t *testing.T, a *App, actor, address, kind, scope string) string {
	t.Helper()
	w := autoRequest(t, a, actor, http.MethodPost, "/api/v1/ble-peers", pipeMap{"deviceAddress": address, "addressType": kind, "scopeId": scope, "displayName": "registered test peer"}, "peer_"+address)
	if w.Code != 201 {
		t.Fatal(w.Code, w.Body.String())
	}
	return ps(pm(w.Body.String()), "peerId")
}
func autoRuleBody(peer string, gatewayIDs ...string) pipeMap {
	ids := []any{}
	for _, id := range gatewayIDs {
		ids = append(ids, id)
	}
	return pipeMap{"name": "Battery service connection", "peerId": peer, "gatewayIds": ids, "enabled": true, "priority": 10, "serviceUuid": autoBatteryUUID, "security": "BONDED_ENCRYPTED", "wireBits": 16, "maxAgeMs": 5000, "cooldownMs": 30000}
}
func autoCreateRule(t *testing.T, a *App, actor string, body pipeMap, key string) string {
	t.Helper()
	w := autoRequest(t, a, actor, http.MethodPost, "/api/v1/auto-connect-rules", body, key)
	if w.Code != 201 {
		t.Fatal(w.Code, w.Body.String())
	}
	v := pm(w.Body.String())
	if e := a.Validate("AutoConnectRule", v); e != nil {
		t.Fatal(e)
	}
	return ps(v, "ruleId")
}
func autoObservation(id, kind, radio string, at int64, payload []byte) pipeMap {
	e := captureEvent(id, kind, payload)
	c := po(e, "capture")
	c["radio"] = radio
	e["timeQuality"] = "SYNCED"
	e["capturedAt"] = Timestamp(at)
	return e
}
func autoAccept(t *testing.T, a *App, gateway string, events ...pipeMap) {
	t.Helper()
	batch := captureBatch(a, events...)
	batch["gatewayId"] = gateway
	ack, e := a.AcceptIngress(t.Context(), a.Config.NamespaceID, gateway, batch)
	if e != nil {
		t.Fatal(e)
	}
	for _, item := range ack["items"].([]pipeMap) {
		s := ps(item, "status")
		if s != "ACCEPTED" && s != "DUPLICATE" {
			t.Fatal(ack)
		}
	}
}
func autoDrain(t *testing.T, a *App) {
	t.Helper()
	if _, e := a.ProcessCaptureBatch(t.Context(), 64); e != nil {
		t.Fatal(e)
	}
}
func autoTaskCount(t *testing.T, a *App) int {
	t.Helper()
	var n int
	if e := a.Management.QueryRow("SELECT count(*) FROM auto_connect_triggers").Scan(&n); e != nil {
		t.Fatal(e)
	}
	return n
}
func autoTaskRequest(t *testing.T, a *App) (string, pipeMap) {
	t.Helper()
	var id, raw string
	if e := a.Management.QueryRow("SELECT t.task_id,t.request_json FROM command_tasks t JOIN auto_connect_triggers a ON a.task_id=t.task_id").Scan(&id, &raw); e != nil {
		t.Fatal(e)
	}
	return id, pm(raw)
}

func TestAutoConnectADVAndScanResponseBothArrivalOrders(t *testing.T) {
	for _, rspFirst := range []bool{false, true} {
		t.Run(fmt.Sprintf("rspFirst=%v", rspFirst), func(t *testing.T) {
			a, actor, peer := autoFixture(t)
			rule := autoCreateRule(t, a, actor, autoRuleBody(peer, "gw_test"), "rule")
			at := Now() - 250
			adv := autoObservation("connectable_adv", "ADV", "NRF54", at, []byte{2, 1, 6})
			rsp := autoObservation("service_rsp", "SCAN_RSP", "ESP32", at+100, []byte{3, 3, 0x0f, 0x18})
			first, second := adv, rsp
			if rspFirst {
				first, second = rsp, adv
			}
			autoAccept(t, a, "gw_test", first)
			autoDrain(t, a)
			if n := autoTaskCount(t, a); n != 0 {
				t.Fatal("incomplete observation pair caused a connection", n)
			}
			autoAccept(t, a, "gw_test", second)
			autoDrain(t, a)
			if n := autoTaskCount(t, a); n != 1 {
				t.Fatal("out-of-order pair did not produce one task", n)
			}
			_, request := autoTaskRequest(t, a)
			params := po(request, "parameters")
			trigger := po(params, "autoTrigger")
			if ps(params, "radio") != "NRF54" || ps(trigger, "advEventId") != "connectable_adv" || ps(trigger, "uuidEventId") != "service_rsp" || ps(trigger, "ruleId") != rule || ps(trigger, "serviceUuid") != autoBatteryUUID || pn(trigger, "wireBits") != 16 {
				t.Fatal(request)
			}
			expires, e := time.Parse(time.RFC3339Nano, ps(request, "expiresAt"))
			if e != nil || expires.UnixMilli() > at+5000 {
				t.Fatal("task outlives the observation", request)
			}
			autoAccept(t, a, "gw_test", first, second)
			autoDrain(t, a)
			if n := autoTaskCount(t, a); n != 1 {
				t.Fatal("spool replay created another task", n)
			}
		})
	}
}

func TestAutoConnectRejectsUnsafeObservationCandidates(t *testing.T) {
	for _, name := range []string{"nonconnectable", "esp-only", "stale", "future", "unsynced", "bad-uuid-after-valid", "unknown-address", "random-private", "wire-width", "rssi-unknown", "rssi-low", "rsp-too-far", "extended-sid-mismatch", "legacy-mismatch"} {
		t.Run(name, func(t *testing.T) {
			a, actor, peer := autoFixture(t)
			req := autoRuleBody(peer, "gw_test")
			if name == "wire-width" {
				req["wireBits"] = 128
			}
			if strings.HasPrefix(name, "rssi-") {
				req["minRssi"] = -50
			}
			autoCreateRule(t, a, actor, req, "rule")
			at := Now() - 1000
			adv := autoObservation("unsafe_adv", "ADV", "NRF54", at, []byte{3, 3, 0x0f, 0x18})
			events := []pipeMap{adv}
			c := po(adv, "capture")
			switch name {
			case "nonconnectable":
				c["connectable"] = false
			case "esp-only":
				c["radio"] = "ESP32"
			case "stale":
				adv["capturedAt"] = Timestamp(Now() - 6000)
			case "future":
				adv["capturedAt"] = Timestamp(Now() + 6000)
			case "unsynced":
				adv["timeQuality"] = "UNSYNCED"
				delete(adv, "capturedAt")
			case "bad-uuid-after-valid":
				adv["payloadBase64"] = "AwMPGAIgAA=="
			case "unknown-address":
				c["address"] = "112233445566"
			case "random-private":
				c["address"] = "112233445566"
				c["addressType"] = "RANDOM"
			case "rssi-unknown":
				delete(c, "rssiDbm")
			case "rssi-low":
				c["rssiDbm"] = -90
			case "rsp-too-far", "extended-sid-mismatch", "legacy-mismatch":
				adv["payloadBase64"] = "AgEG"
				rsp := autoObservation("unsafe_rsp", "SCAN_RSP", "ESP32", at+100, []byte{3, 3, 0x0f, 0x18})
				if name == "rsp-too-far" {
					rsp["capturedAt"] = Timestamp(at + 501)
				}
				if name == "extended-sid-mismatch" {
					c["legacy"] = false
					c["sid"] = 1
					po(rsp, "capture")["legacy"] = false
					po(rsp, "capture")["sid"] = 2
				}
				if name == "legacy-mismatch" {
					po(rsp, "capture")["legacy"] = false
					po(rsp, "capture")["sid"] = 1
				}
				events = append(events, rsp)
			}
			autoAccept(t, a, "gw_test", events...)
			autoDrain(t, a)
			if n := autoTaskCount(t, a); n != 0 {
				t.Fatal("unsafe observation created a physical operation", name, n)
			}
			if name == "bad-uuid-after-valid" {
				var state, code string
				if e := a.Pipeline.QueryRow("SELECT outcome,error_code FROM capture_consumptions").Scan(&state, &code); e != nil || state != "invalid" || code != "UUID_FIELD_LENGTH" {
					t.Fatal(state, code, e)
				}
			}
		})
	}
}

func TestAutoConnectRechecksCapabilityAndRuleOwnerAuthorization(t *testing.T) {
	for _, mode := range []string{"capture", "discovery", "connect-task", "disabled-owner", "revoked-owner", "disabled-peer", "disabled-rule"} {
		t.Run(mode, func(t *testing.T) {
			a, actor, peer := autoFixture(t)
			autoCreateRule(t, a, actor, autoRuleBody(peer, "gw_test"), "rule")
			autoAccept(t, a, "gw_test", autoObservation("adv", "ADV", "NRF54", Now()-100, []byte{3, 3, 0x0f, 0x18}))
			switch mode {
			case "capture", "discovery", "connect-task":
				missing := map[string]string{"capture": "GW32_CAPTURE_CONTEXT_1", "discovery": "NRF54_BLE_GATT_SERVICE_DISCOVERY", "connect-task": "GATT_CONNECT"}[mode]
				var raw string
				_ = a.Management.QueryRow("SELECT capabilities_json FROM gateways WHERE gateway_id='gw_test'").Scan(&raw)
				var caps []string
				_ = json.Unmarshal([]byte(raw), &caps)
				filtered := []string{}
				for _, c := range caps {
					if c != missing {
						filtered = append(filtered, c)
					}
				}
				_, _ = a.Management.Exec("UPDATE gateways SET capabilities_json=? WHERE gateway_id='gw_test'", pipeJSON(filtered))
			case "disabled-owner":
				_, _ = a.Management.Exec("UPDATE principals SET status='disabled' WHERE principal_id=?", actor)
			case "revoked-owner":
				_, _ = a.Management.Exec("DELETE FROM role_bindings WHERE principal_id=?", actor)
			case "disabled-peer":
				_, _ = a.Management.Exec("UPDATE ble_peers SET state='disabled'")
			case "disabled-rule":
				_, _ = a.Management.Exec("UPDATE auto_connect_rules SET enabled=0")
			}
			autoDrain(t, a)
			if n := autoTaskCount(t, a); n != 0 {
				t.Fatal("runtime authority changed but automation still executed", mode, n)
			}
		})
	}
}

func TestAutoConnectManagementCommitReplayAndConcurrentConsumers(t *testing.T) {
	a, actor, peer := autoFixture(t)
	autoCreateRule(t, a, actor, autoRuleBody(peer, "gw_test"), "rule")
	autoAccept(t, a, "gw_test", autoObservation("replay_adv", "ADV", "NRF54", Now()-100, []byte{3, 3, 0x0f, 0x18}))
	if _, e := a.Pipeline.Exec("CREATE TRIGGER capture_ack_failure BEFORE INSERT ON capture_consumptions BEGIN SELECT RAISE(ABORT,'injected cross-store acknowledgement failure'); END"); e != nil {
		t.Fatal(e)
	}
	if _, e := a.ProcessCaptureBatch(t.Context(), 64); e == nil {
		t.Fatal("fault injection did not interrupt the handoff")
	}
	if n := autoTaskCount(t, a); n != 1 {
		t.Fatal("management effect not durably committed", n)
	}
	_, _ = a.Pipeline.Exec("DROP TRIGGER capture_ack_failure")
	var wg sync.WaitGroup
	errors := make(chan error, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); _, e := a.ProcessCaptureBatch(context.Background(), 64); errors <- e }()
	}
	wg.Wait()
	close(errors)
	for e := range errors {
		if e != nil {
			t.Fatal(e)
		}
	}
	if n := autoTaskCount(t, a); n != 1 {
		t.Fatal("replay caused a duplicate physical task", n)
	}
	var claims, consumed int
	_ = a.Management.QueryRow("SELECT count(*) FROM connection_claims").Scan(&claims)
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM capture_consumptions").Scan(&consumed)
	if claims != 1 || consumed != 1 {
		t.Fatal(claims, consumed)
	}
}

func TestAutoConnectNamespacePeerClaimAndPriority(t *testing.T) {
	a, actor, peer := autoFixture(t)
	low := autoRuleBody(peer, "gw_test", "gw_other")
	low["priority"] = 1
	autoCreateRule(t, a, actor, low, "low")
	high := autoRuleBody(peer, "gw_test", "gw_other")
	high["priority"] = 100
	highID := autoCreateRule(t, a, actor, high, "high")
	at := Now() - 100
	autoAccept(t, a, "gw_test", autoObservation("first_adv", "ADV", "NRF54", at, []byte{3, 3, 0x0f, 0x18}))
	autoDrain(t, a)
	_, request := autoTaskRequest(t, a)
	if ps(po(po(request, "parameters"), "autoTrigger"), "ruleId") != highID {
		t.Fatal("priority selection wrong", request)
	}
	autoAccept(t, a, "gw_other", autoObservation("other_gateway_adv", "ADV", "NRF54", at, []byte{3, 3, 0x0f, 0x18}))
	autoDrain(t, a)
	if n := autoTaskCount(t, a); n != 1 {
		t.Fatal("same peer claimed by two gateways", n)
	}
}

func TestAutoConnectRealLinkDownPrecedesSuccessfulResult(t *testing.T) {
	a, actor, peer := autoFixture(t)
	rule := autoCreateRule(t, a, actor, autoRuleBody(peer, "gw_test"), "rule")
	autoAccept(t, a, "gw_test", autoObservation("adv_late_result", "ADV", "NRF54", Now()-100, []byte{3, 3, 0x0f, 0x18}))
	autoDrain(t, a)
	taskID, _ := autoTaskRequest(t, a)
	ref := strings.Repeat("d", 32)
	down := captureEvent("link_before_result", "GATT_LINK_DOWN", nil)
	po(down, "capture")["connectionRef"] = ref
	autoAccept(t, a, "gw_test", down)
	autoDrain(t, a)
	evidence := gw32ConnectionEvidence(ref)
	evidence["ruleId"] = rule
	evidence["advEventId"] = "adv_late_result"
	evidence["serviceUuid"] = autoBatteryUUID
	ack := gw32SendResult(t, a, taskID, "late_connection_result", "SUCCEEDED", 1, evidence)
	if controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal(ack)
	}
	var state, claim string
	_ = a.Management.QueryRow("SELECT state FROM gatt_connections WHERE connection_ref=?", ref).Scan(&state)
	_ = a.Management.QueryRow("SELECT state FROM connection_claims WHERE task_id=?", taskID).Scan(&claim)
	if state != "closed" || claim != "cooldown" {
		t.Fatal("late result resurrected closed connection", state, claim)
	}
}

func autoScopedPrincipal(t *testing.T, a *App, id, role, scope string) string {
	t.Helper()
	now := Now()
	if _, e := a.Management.Exec("INSERT INTO principals(principal_id,kind,name,status,created_at_ms,updated_at_ms) VALUES(?,'human',?,'active',?,?)", id, id, now, now); e != nil {
		t.Fatal(e)
	}
	if _, e := a.Management.Exec("INSERT INTO role_bindings VALUES(?,?,?,?,?)", ID("binding"), id, role, scope, now); e != nil {
		t.Fatal(e)
	}
	return id
}
func TestAutoConnectRuleRBACAndCAS(t *testing.T) {
	a, actor, peer := autoFixture(t)
	_, e := a.Management.Exec("INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('scope_other','group','scope_default_project','Other group',?)", Now())
	if e != nil {
		t.Fatal(e)
	}
	_, _ = a.Management.Exec("UPDATE gateways SET scope_id='scope_other' WHERE gateway_id='gw_other'")
	operator := autoScopedPrincipal(t, a, "operator_user", "operator", "scope_default_group")
	deviceAdmin := autoScopedPrincipal(t, a, "device_user", "device_admin", "scope_default_group")
	w := autoRequest(t, a, operator, "POST", "/api/v1/auto-connect-rules", autoRuleBody(peer, "gw_test"), "forbidden")
	if w.Code != 403 {
		t.Fatal("operator created connection automation", w.Code, w.Body.String())
	}
	w = autoRequest(t, a, deviceAdmin, "POST", "/api/v1/auto-connect-rules", autoRuleBody(peer, "gw_other"), "outside")
	if w.Code != 404 && w.Code != 403 {
		t.Fatal("rule escaped gateway scope", w.Code, w.Body.String())
	}
	otherPeer := autoCreatePeer(t, a, actor, "112233445566", "PUBLIC", "scope_other")
	w = autoRequest(t, a, deviceAdmin, "POST", "/api/v1/auto-connect-rules", autoRuleBody(otherPeer, "gw_test"), "other-peer")
	if w.Code != 404 {
		t.Fatal("peer scope escaped", w.Code, w.Body.String())
	}
	id := autoCreateRule(t, a, deviceAdmin, autoRuleBody(peer, "gw_test"), "allowed")
	w = autoRequest(t, a, deviceAdmin, "PATCH", "/api/v1/auto-connect-rules/"+id, pipeMap{"enabled": false}, "", map[string]string{"If-Match": ETag(1)})
	if w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	w = autoRequest(t, a, deviceAdmin, "PATCH", "/api/v1/auto-connect-rules/"+id, pipeMap{"enabled": true}, "", map[string]string{"If-Match": ETag(1)})
	if w.Code != 412 {
		t.Fatal("stale revision overwrote rule", w.Code, w.Body.String())
	}
	for _, address := range []string{"000000000000", "FFFFFFFFFFFF", "112233445577"} {
		w = autoRequest(t, a, actor, "POST", "/api/v1/ble-peers", pipeMap{"displayName": "invalid", "scopeId": "scope_default_group", "deviceAddress": address, "addressType": "RANDOM"}, "invalid-"+address)
		if w.Code != 422 {
			t.Fatal("invalid/random-private address registered", address, w.Code)
		}
	}
}

func TestAutoConnectAPIsDoNotNestSingleConnectionQueries(t *testing.T) {
	a, actor, peer := autoFixture(t)
	autoCreateRule(t, a, actor, autoRuleBody(peer, "gw_test"), "rule")
	gw32Open(t, a, actor)
	for _, tc := range []struct {
		method, path string
		body         pipeMap
		etag         string
	}{{"GET", "/api/v1/ble-peers", pipeMap{}, ""}, {"GET", "/api/v1/auto-connect-rules", pipeMap{}, ""}, {"GET", "/api/v1/gatt-connections", pipeMap{}, ""}, {"PATCH", "/api/v1/ble-peers/" + peer, pipeMap{"displayName": "updated"}, ETag(1)}} {
		t.Run(tc.method+tc.path, func(t *testing.T) {
			p, e := a.principal(t.Context(), actor)
			if e != nil {
				t.Fatal(e)
			}
			r := httptest.NewRequest(tc.method, tc.path, bytes.NewBufferString(pipeJSON(tc.body)))
			r.Header.Set("Content-Type", "application/json")
			r.Header.Set("If-Match", tc.etag)
			r = r.WithContext(context.WithValue(r.Context(), authKey{}, &authInfo{Principal: p}))
			router := chi.NewRouter()
			a.RegisterAutoConnect(router)
			w := httptest.NewRecorder()
			a.Management.SetMaxOpenConns(1)
			done := make(chan struct{})
			go func() { router.ServeHTTP(w, r); close(done) }()
			select {
			case <-done:
				a.Management.SetMaxOpenConns(8)
				if w.Code != 200 {
					t.Fatal(w.Code, w.Body.String())
				}
			case <-time.After(1500 * time.Millisecond):
				a.Management.SetMaxOpenConns(8)
				select {
				case <-done:
				case <-time.After(3 * time.Second):
					t.Fatal("handler did not release after restoring pool")
				}
				t.Fatal("authorization nested a second database query while holding the only connection")
			}
		})
	}
}

func TestAutoConnectCandidateRechecksAuthorityInsideCreationTransaction(t *testing.T) {
	for _, mode := range []string{"unchanged", "owner-disabled", "binding-revoked", "scope-disabled", "gateway-moved"} {
		t.Run(mode, func(t *testing.T) {
			a, _, peer := autoFixture(t)
			owner := autoScopedPrincipal(t, a, "candidate_owner", "device_admin", "scope_default_group")
			id := autoCreateRule(t, a, owner, autoRuleBody(peer, "gw_test"), "candidate-rule")
			var raw string
			if e := a.Management.QueryRow("SELECT rule_json FROM auto_connect_rules WHERE rule_id=?", id).Scan(&raw); e != nil {
				t.Fatal(e)
			}
			rule := autoCandidateRule{ID: id, Owner: owner, Scope: "scope_default_group", Address: "AABBCCDDEEFF", AddressType: "PUBLIC", Revision: 1, Definition: pm(raw)}
			event := autoObservation("stale_authority_candidate", "ADV", "NRF54", Now()-100, []byte{3, 3, 0x0f, 0x18})
			autoAccept(t, a, "gw_test", event)
			at, _ := time.Parse(time.RFC3339Nano, ps(event, "capturedAt"))
			observation := captureObservation{Key: pipeStable("event", []string{a.Config.NamespaceID, "gw_test", ps(event, "eventId")}), Namespace: a.Config.NamespaceID, Gateway: "gw_test", Event: ps(event, "eventId"), Captured: at.UnixMilli(), Accepted: Now(), Raw: event, Payload: []byte{3, 3, 0x0f, 0x18}}
			var e error
			switch mode {
			case "owner-disabled":
				_, e = a.Management.Exec("UPDATE principals SET status='disabled' WHERE principal_id=?", owner)
			case "binding-revoked":
				_, e = a.Management.Exec("DELETE FROM role_bindings WHERE principal_id=?", owner)
			case "scope-disabled":
				_, e = a.Management.Exec("UPDATE resource_scopes SET status='archived' WHERE scope_id='scope_default_group'")
			case "gateway-moved":
				_, e = a.Management.Exec("INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('moved_group','group','scope_default_project','Moved group',?)", Now())
				if e == nil {
					_, e = a.Management.Exec("UPDATE gateways SET scope_id='moved_group' WHERE gateway_id='gw_test'")
				}
			}
			if e != nil {
				t.Fatal(e)
			}
			a.Management.SetMaxOpenConns(1)
			defer a.Management.SetMaxOpenConns(8)
			created, e := a.createAutomaticTask(t.Context(), rule, observation, observation, ServiceUUID{UUID: autoBatteryUUID, WireBits: 16, ADType: 3, Source: "UUID_LIST", SourceEventID: observation.Event})
			if e != nil {
				t.Fatal(e)
			}
			if created != (mode == "unchanged") {
				t.Fatal("candidate used stale authorization", mode, created)
			}
			if count := autoTaskCount(t, a); count != map[bool]int{true: 1, false: 0}[mode == "unchanged"] {
				t.Fatal(mode, count)
			}
		})
	}
}
