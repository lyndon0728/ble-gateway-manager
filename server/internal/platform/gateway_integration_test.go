package platform

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
)

func integrationCaps(t *testing.T, a *App, caps ...string) {
	t.Helper()
	var raw string
	if e := a.Management.QueryRow("SELECT capabilities_json FROM gateways WHERE gateway_id='gw_test'").Scan(&raw); e != nil {
		t.Fatal(e)
	}
	list := parseList(raw)
	for _, cap := range caps {
		list = append(list, cap)
	}
	if _, e := a.Management.Exec("UPDATE gateways SET capabilities_json=? WHERE gateway_id='gw_test'", encode(list)); e != nil {
		t.Fatal(e)
	}
}
func beaconV2GoldenProfile() map[string]any {
	return pm(`{"scan":{"enabled":true,"active":true,"intervalMs":100,"windowMs":50},"filters":{},"beaconRules":[],"beaconFilterV2":{"version":2,"scope":"ADV_SCAN_RSP_PAIR","pairWindowMs":500,"rules":[{"action":"ALLOW","reportKinds":["LEGACY_ADV"],"fieldMask":31,"name":{"mode":"PREFIX","source":"COMPLETE","valueBase64":"QmNuLQ==","maskBase64":"/////w=="},"ibeaconUuid":{"value":"fda50693-a4e2-4fb1-afcf-c6eb07647825","maskHex":"ffffffffffffffffffffffffffffffff"},"rssi":{"mode":"RANGE","minDbm":-75,"maxDbm":-35},"major":{"value":4608,"mask":65280},"minor":{"value":3,"mask":15}}]}}`)
}
func TestBeaconV2GoldenCompilerAndPredicateValidation(t *testing.T) {
	a, _ := gw32Fixture(t)
	p := beaconV2GoldenProfile()
	b, e := CompileScanProfileV2(p, 1)
	if e != nil {
		t.Fatal(e)
	}
	const want = "020113001000010000000000000003a00050008000000001f401013a0001011f0001000442636e2dfffffffffda50693a4e24fb1afcfc6eb07647825ffffffffffffffffffffffffffffffff00b5dd001200ff03000f00"
	if hex.EncodeToString(b) != want {
		t.Fatalf("golden frame mismatch (%d bytes): %x", len(b), b)
	}
	if e = a.Validate("RadioConfiguration", p); e != nil {
		t.Fatal(e)
	}
	cases := []func(map[string]any){
		func(p map[string]any) { Obj(p, "beaconFilterV2")["version"] = 3 },
		func(p map[string]any) { Obj(p, "scan")["active"] = false },
		func(p map[string]any) { Obj(p, "beaconFilterV2")["pairWindowMs"] = 501 },
		func(p map[string]any) {
			r := List(Obj(p, "beaconFilterV2"), "rules")[0].(map[string]any)
			r["fieldMask"] = 1
		},
		func(p map[string]any) {
			r := List(Obj(p, "beaconFilterV2"), "rules")[0].(map[string]any)
			Obj(r, "name")["maskBase64"] = "AAAAAA=="
		},
		func(p map[string]any) {
			r := List(Obj(p, "beaconFilterV2"), "rules")[0].(map[string]any)
			Obj(r, "rssi")["minDbm"] = -128
		},
		func(p map[string]any) {
			r := List(Obj(p, "beaconFilterV2"), "rules")[0].(map[string]any)
			Obj(r, "ibeaconUuid")["value"] = "FDA50693-A4E2-4FB1-AFCF-C6EB07647825"
		},
		func(p map[string]any) {
			r := List(Obj(p, "beaconFilterV2"), "rules")[0].(map[string]any)
			r["reportKinds"] = []any{"LEGACY_ADV", "LEGACY_SCAN_RSP"}
		},
		func(p map[string]any) {
			p["beaconRules"] = []any{pm(`{"action":"ALLOW","reportKinds":["LEGACY_ADV"],"adType":255,"offset":0,"valueBase64":"AQ==","maskBase64":"/w=="}`)}
		},
	}
	for i, change := range cases {
		p := beaconV2GoldenProfile()
		change(p)
		if _, e = CompileScanProfileV2(p, 1); e == nil {
			t.Errorf("invalid case %d accepted", i)
		}
	}
}
func TestBeaconV2BudgetReplaceAndPreview(t *testing.T) {
	p := beaconV2GoldenProfile()
	uuids, companies := []any{}, []any{}
	for i := 0; i < 128; i++ {
		uuids = append(uuids, fmt.Sprintf("%08x-0000-1000-8000-00805f9b34fb", i))
		companies = append(companies, i)
	}
	p["filters"] = map[string]any{"serviceUuids": uuids, "manufacturerIds": companies}
	rule := List(Obj(p, "beaconFilterV2"), "rules")[0].(map[string]any)
	rule["fieldMask"] = 63
	rule["serviceUuid"] = Obj(rule, "ibeaconUuid")
	Obj(rule, "name")["valueBase64"] = base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{1}, 29))
	Obj(rule, "name")["maskBase64"] = base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{255}, 29))
	rules := []any{}
	for i := 0; i < 8; i++ {
		rules = append(rules, rule)
	}
	Obj(p, "beaconFilterV2")["rules"] = rules
	if _, e := CompileScanProfileV2(p, 1); e == nil {
		t.Fatal("oversized full frame accepted")
	}
	original := beaconV2GoldenProfile()
	merged := gw32Merge(original, map[string]any{"beaconFilterV2": map[string]any{"version": 2}})
	if len(Obj(merged, "beaconFilterV2")) != 1 {
		t.Fatal("V2 object recursively merged")
	}
	a, actor := networkFixture(t)
	profile := beaconV2GoldenProfile()
	delete(profile, "filters")
	body := map[string]any{"radio": "nrf54", "profile": profile}
	path := "/api/v1/gateways/gw_test/scan-profile-preview"
	w, _ := networkRequest(t, a, actor, "", "POST", path, body, nil)
	networkWant(t, w, 422)
	integrationCaps(t, a, "NRF54_BLE_FILTER_V2")
	w, result := networkRequest(t, a, actor, "", "POST", path, body, nil)
	networkWant(t, w, 200)
	if Num(result, "bodyLength") != 87 {
		t.Fatalf("wrong preview: %s", w.Body.String())
	}
}
func TestSignedTimeChallengeAuthenticatesBytesAndFailsClosed(t *testing.T) {
	a, _ := networkFixture(t)
	b := map[string]any{"schemaVersion": "1.0", "nonce": base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{7}, 32)), "bootId": strings.Repeat("a", 32)}
	w, _ := networkRequest(t, a, "", "", "POST", "/api/v1/gateway-time-challenges", b, nil)
	networkWant(t, w, 503)
	private := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{9}, 32))
	a.Config.TimeSigningKey = base64.StdEncoding.EncodeToString(private)
	a.Config.TimeKeyID = "time-test"
	a.Config.TimeSourceVerified = true
	w, response := networkRequest(t, a, "", "", "POST", "/api/v1/gateway-time-challenges", b, nil)
	networkWant(t, w, 200)
	raw, _ := base64.StdEncoding.DecodeString(Str(response, "payloadBase64"))
	sig, _ := base64.StdEncoding.DecodeString(Str(response, "signatureBase64"))
	if !ed25519.Verify(private.Public().(ed25519.PublicKey), append([]byte("gw32-time-v1"), raw...), sig) {
		t.Fatal("invalid domain-separated signature")
	}
	body := pm(string(raw))
	if body["nonce"] != b["nonce"] || body["bootId"] != b["bootId"] || Num(body, "validForMs") != 60000 {
		t.Fatal("time evidence binding lost")
	}
	router := chi.NewRouter()
	a.RegisterGatewayIntegration(router)
	request := httptest.NewRequest("POST", "/api/v1/gateway-time-challenges", strings.NewReader(strings.Replace(encode(b), `"schemaVersion":"1.0"`, `"schemaVersion":"1.0","schemaVersion":"1.0"`, 1)))
	w = httptest.NewRecorder()
	router.ServeHTTP(w, request)
	networkWant(t, w, 400)
	a.Config.TimeSourceVerified = false
	w, _ = networkRequest(t, a, "", "", "POST", "/api/v1/gateway-time-challenges", b, nil)
	networkWant(t, w, 503)
}
func TestComponentChallengeCASAndReplayCannotRefreshEvidence(t *testing.T) {
	a, _ := networkFixture(t)
	integrationCaps(t, a, "COMPONENT_INVENTORY_1")
	challenge := func() map[string]any {
		w, b := networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-component-challenges", map[string]any{"component": "NRF54"}, nil)
		networkWant(t, w, 201)
		return b
	}
	report := func(c map[string]any) map[string]any {
		return map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "component": "NRF54", "challengeId": c["challengeId"], "nonce": c["nonce"], "inventoryGeneration": c["inventoryGeneration"], "controllerBootId": strings.Repeat("b", 32), "evidenceLevel": "VERSION_ONLY", "firmwareVersion": "0.2.1"}
	}
	first := challenge()
	b := report(first)
	w, receipt := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-components", b, nil)
	networkWant(t, w, 200)
	if receipt["inventoryGeneration"] != "1" {
		t.Fatal(receipt)
	}
	if _, e := a.Management.Exec("UPDATE gateway_component_challenges SET expires_at_ms=0 WHERE challenge_id=?", first["challengeId"]); e != nil {
		t.Fatal(e)
	}
	w, replay := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-components", b, nil)
	networkWant(t, w, 200)
	if encode(replay) != encode(receipt) {
		t.Fatal("retry refreshed inventory")
	}
	b["firmwareVersion"] = "conflict"
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-components", b, nil)
	networkWant(t, w, 409)
	old := challenge()
	current := challenge()
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-components", report(old), nil)
	networkWant(t, w, 409)
	b = report(current)
	b["bootConfirmed"] = true
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-components", b, nil)
	networkWant(t, w, 422)
	delete(b, "bootConfirmed")
	w, _ = networkRequest(t, a, "", "gw_other", "PUT", "/api/v1/gateway-components", b, nil)
	networkWant(t, w, 403)
}
func TestRuntimeCapabilitiesRequireSignedManifestAndIntersection(t *testing.T) {
	a, _ := networkFixture(t)
	integrationCaps(t, a, "NRF54_BLE_FILTER_V2")
	b := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "espBootId": strings.Repeat("a", 32), "nrfBootId": strings.Repeat("b", 32), "manifestDigest": strings.Repeat("c", 64), "espImplemented": []any{"NRF54_BLE_FILTER_V2", "ESP32_BLE_FILTER_V2"}, "nrfHelloCapabilities": []any{"NRF54_BLE_FILTER_V2"}, "boardVerified": []any{"NRF54_BLE_FILTER_V2", "ESP32_BLE_FILTER_V2"}, "unavailableReasons": map[string]any{}}
	w, _ := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-runtime-capabilities", b, nil)
	networkWant(t, w, 422)
	_, e := a.Management.Exec("INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,compatibility_json,state,created_at_ms) VALUES('manufacturing','device_manifest','manufacturing','1',?,'manifest.json',1,'{}','{}','verified',?)", b["manifestDigest"], Now())
	if e != nil {
		t.Fatal(e)
	}
	if _, e = a.Management.Exec("UPDATE gateways SET shipment_ref='manufacturing' WHERE gateway_id='gw_test'"); e != nil {
		t.Fatal(e)
	}
	w, result := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-runtime-capabilities", b, nil)
	networkWant(t, w, 200)
	if encode(result["availableCapabilities"]) != `["NRF54_BLE_FILTER_V2"]` {
		t.Fatal(result)
	}
	b["nrfHelloCapabilities"] = []any{}
	w, result = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-runtime-capabilities", b, nil)
	networkWant(t, w, 200)
	if len(List(result, "availableCapabilities")) != 0 {
		t.Fatal("missing HELLO allowed V2")
	}
	b["espImplemented"] = []any{"MADE_UP_AUTHORIZATION"}
	w, _ = networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-runtime-capabilities", b, nil)
	networkWant(t, w, 422)
}
func TestPeripheralAuthorizationBusinessDedupAndSeparateReceipt(t *testing.T) {
	a, actor := networkFixture(t)
	integrationCaps(t, a, "PERIPHERAL_BRIDGE_1", "NRF54_BLE_PERIPHERAL", "PERIPHERAL_NOTIFY", "PERIPHERAL_DISCONNECT")
	integrationRuntimeBoots(t, a, strings.Repeat("d", 32), strings.Repeat("a", 32))
	event := map[string]any{"eventId": "evt_1", "radio": "NRF54", "controllerBootId": strings.Repeat("a", 32), "connectionRef": strings.Repeat("b", 32), "clientId": "mobile_1", "transportSeq": 1, "businessId": strings.Repeat("c", 32), "route": "sensor/read", "payloadBase64": "AQI=", "timeQuality": "UNSYNCED"}
	b := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "batchId": "batch_1", "events": []any{event}}
	send := func(want string) map[string]any {
		w, r := networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-peripheral/batches", b, nil)
		networkWant(t, w, 200)
		if Str(List(r, "items")[0].(map[string]any), "status") != want {
			t.Fatalf("unexpected receipt %s", w.Body.String())
		}
		if r["durability"] != "PERIPHERAL_PERSISTED" {
			t.Fatal("mixed durability contract")
		}
		return r
	}
	send("REJECTED")
	w, _ := networkRequest(t, a, actor, "", "PUT", "/api/v1/gateways/gw_test/peripheral-clients/mobile_1", map[string]any{"enabled": true, "routes": []any{"sensor/read"}}, map[string]string{"Idempotency-Key": "client-auth"})
	networkWant(t, w, 200)
	send("ACCEPTED")
	send("DUPLICATE")
	event["eventId"] = "evt_2"
	event["transportSeq"] = 2
	send("DUPLICATE")
	event["eventId"] = "evt_3"
	event["payloadBase64"] = "AQM="
	r := send("REJECTED")
	if Str(List(r, "items")[0].(map[string]any), "code") != "BUSINESS_ID_CONFLICT" {
		t.Fatal(r)
	}
	var count int
	if e := a.Management.QueryRow("SELECT count(*) FROM peripheral_business").Scan(&count); e != nil || count != 1 {
		t.Fatal("business was executed twice", e, count)
	}
	params := map[string]any{"radio": "NRF54", "connectionRef": strings.Repeat("b", 32), "clientId": "mobile_1", "route": "sensor/read", "payloadBase64": "AQI=", "timeoutMs": 1000}
	request := map[string]any{"gatewayId": "gw_test", "type": "PERIPHERAL_NOTIFY", "parameters": params, "expiresAt": Timestamp(Now() + 60000)}
	w, task := networkRequest(t, a, actor, "", "POST", "/api/v1/tasks", request, map[string]string{"Idempotency-Key": "notify-once"})
	networkWant(t, w, 202)
	w, task2 := networkRequest(t, a, actor, "", "POST", "/api/v1/tasks", request, map[string]string{"Idempotency-Key": "notify-once"})
	networkWant(t, w, 202)
	if task["taskId"] != task2["taskId"] {
		t.Fatal("notify retry changed identity")
	}
	var e error
	var raw string
	if e = a.Management.QueryRow("SELECT request_json FROM command_tasks WHERE task_id=?", task["taskId"]).Scan(&raw); e != nil {
		t.Fatal(e)
	}
	command := pm(raw)
	if len(Str(Obj(command, "parameters"), "businessId")) != 32 {
		t.Fatal("missing persistent business id")
	}
	tx, e := a.Management.BeginTx(context.Background(), nil)
	if e != nil {
		t.Fatal(e)
	}
	defer tx.Rollback()
	details := map[string]any{"radio": "NRF54", "connectionRef": params["connectionRef"], "clientId": "mobile_1", "businessId": Obj(command, "parameters")["businessId"], "businessStatus": "STORED"}
	code, e := a.validateGW32Result(t.Context(), tx, "gw_test", command, map[string]any{"state": "SUCCEEDED", "verification": "VERIFIED", "details": details})
	if e != nil || code != "PERIPHERAL_BUSINESS_NOT_APPLIED" {
		t.Fatal("STORED counted as APPLIED", e, code)
	}
}

func integrationRuntimeBoots(t *testing.T, a *App, esp string, nrf any) map[string]any {
	t.Helper()
	digest := strings.Repeat("e", 64)
	_, e := a.Management.Exec("INSERT OR IGNORE INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,compatibility_json,state,created_at_ms) VALUES('peripheral_manufacturing','device_manifest','manufacturing','1',?,'manifest.json',1,'{}','{}','verified',?)", digest, Now())
	if e != nil {
		t.Fatal(e)
	}
	if _, e = a.Management.Exec("UPDATE gateways SET shipment_ref='peripheral_manufacturing' WHERE gateway_id='gw_test'"); e != nil {
		t.Fatal(e)
	}
	var raw string
	if e = a.Management.QueryRow("SELECT capabilities_json FROM gateways WHERE gateway_id='gw_test'").Scan(&raw); e != nil {
		t.Fatal(e)
	}
	caps := parseList(raw)
	hello := []any{}
	if nrf != nil {
		for _, cap := range caps {
			if strings.HasPrefix(cap.(string), "NRF54_") {
				hello = append(hello, cap)
			}
		}
	}
	b := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "espBootId": esp, "nrfBootId": nrf, "manifestDigest": digest, "espImplemented": caps, "nrfHelloCapabilities": hello, "boardVerified": caps, "unavailableReasons": map[string]any{}}
	w, _ := networkRequest(t, a, "", "gw_test", "PUT", "/api/v1/gateway-runtime-capabilities", b, nil)
	networkWant(t, w, 200)
	return b
}

func TestPeripheralOfflineEventsCannotRecreateOldBootSession(t *testing.T) {
	for _, radio := range []string{"NRF54", "ESP32"} {
		t.Run(radio, func(t *testing.T) {
			a, actor := networkFixture(t)
			integrationCaps(t, a, "PERIPHERAL_BRIDGE_1", "NRF54_BLE_PERIPHERAL", "ESP32_BLE_PERIPHERAL", "PERIPHERAL_NOTIFY", "PERIPHERAL_DISCONNECT")
			w, _ := networkRequest(t, a, actor, "", "PUT", "/api/v1/gateways/gw_test/peripheral-clients/mobile_1", map[string]any{"enabled": true, "routes": []any{"sensor/read"}}, map[string]string{"Idempotency-Key": "authorize-boot-test"})
			networkWant(t, w, 200)
			oldBoot, currentBoot := strings.Repeat("a", 32), strings.Repeat("c", 32)
			event := map[string]any{"radio": radio, "controllerBootId": oldBoot, "connectionRef": strings.Repeat("b", 32), "clientId": "mobile_1", "transportSeq": 1, "route": "sensor/read", "payloadBase64": "AQI=", "timeQuality": "UNSYNCED"}
			send := func(sequence int, want string) {
				t.Helper()
				event["eventId"] = fmt.Sprintf("evt_boot_%d", sequence)
				event["businessId"] = fmt.Sprintf("%032x", sequence)
				b := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "batchId": "batch_boot", "events": []any{event}}
				w, receipt := networkRequest(t, a, "", "gw_test", "POST", "/api/v1/gateway-peripheral/batches", b, nil)
				networkWant(t, w, 200)
				if Str(List(receipt, "items")[0].(map[string]any), "status") != want {
					t.Fatalf("unexpected receipt: %s", w.Body.String())
				}
			}
			connections := func(want int) {
				t.Helper()
				var count int
				if e := a.Management.QueryRow("SELECT count(*) FROM peripheral_connections WHERE gateway_id='gw_test'").Scan(&count); e != nil || count != want {
					t.Fatalf("session count %d, want %d: %v", count, want, e)
				}
			}
			publishBoot := func(boot string) map[string]any {
				if radio == "NRF54" {
					return integrationRuntimeBoots(t, a, strings.Repeat("d", 32), boot)
				}
				return integrationRuntimeBoots(t, a, boot, strings.Repeat("d", 32))
			}
			send(1, "ACCEPTED") // No current runtime: persist data without inventing a session.
			connections(0)
			publishBoot(oldBoot)
			send(1, "DUPLICATE") // A transport replay cannot become new live evidence.
			connections(0)
			send(2, "ACCEPTED")
			connections(1)
			runtime := publishBoot(currentBoot)
			connections(0)
			send(3, "ACCEPTED") // Old boot's first offline delivery must not recreate its lease.
			connections(0)
			event["controllerBootId"] = currentBoot
			event["connectionRef"] = strings.Repeat("f", 32)
			send(4, "ACCEPTED")
			connections(1)

			// Retain a stale row deliberately: task admission must independently
			// recheck runtime, even if an old database row survived cleanup.
			key := "nrfBootId"
			if radio == "ESP32" {
				key = "espBootId"
			}
			runtime[key] = oldBoot
			if _, e := a.Management.Exec("UPDATE gateway_runtime_capabilities SET report_json=? WHERE gateway_id='gw_test'", encode(runtime)); e != nil {
				t.Fatal(e)
			}
			denyTasks := func(suffix string) {
				t.Helper()
				for _, op := range []string{"PERIPHERAL_NOTIFY", "PERIPHERAL_DISCONNECT"} {
					params := map[string]any{"radio": radio, "connectionRef": event["connectionRef"], "clientId": "mobile_1"}
					if op == "PERIPHERAL_NOTIFY" {
						params["route"], params["payloadBase64"], params["timeoutMs"] = "sensor/read", "AQI=", 1000
					}
					request := map[string]any{"gatewayId": "gw_test", "type": op, "parameters": params, "expiresAt": Timestamp(Now() + 60000)}
					w, response := networkRequest(t, a, actor, "", "POST", "/api/v1/tasks", request, map[string]string{"Idempotency-Key": suffix + op})
					networkWant(t, w, 409)
					if Str(Obj(response, "error"), "code") != "STALE_PERIPHERAL_SESSION" {
						t.Fatalf("unexpected stale session error: %s", w.Body.String())
					}
				}
			}
			denyTasks("changed-boot-")
			if _, e := a.Management.Exec("DELETE FROM gateway_runtime_capabilities WHERE gateway_id='gw_test'"); e != nil {
				t.Fatal(e)
			}
			denyTasks("missing-runtime-")
			publishBoot(currentBoot)
			if radio == "NRF54" {
				integrationRuntimeBoots(t, a, strings.Repeat("d", 32), nil)
				connections(0)
				send(5, "ACCEPTED")
				connections(0)
			}
		})
	}
}

func TestSnapshotIncompleteBaselineStableGenerationAndScope(t *testing.T) {
	a, _ := networkFixture(t)
	w, b := networkRequest(t, a, "", "gw_test", "GET", "/api/v1/gateway-configuration-snapshot", nil, nil)
	networkWant(t, w, 200)
	if b["complete"] != false || len(List(b, "missingGroups")) != 6 {
		t.Fatal("defaults fabricated as complete baseline", b)
	}
	want := Str(b, "digest")
	delete(b, "digest")
	if networkSnapshotID(b) != want {
		t.Fatal("snapshot digest mismatch")
	}
	w, again := networkRequest(t, a, "", "gw_test", "GET", "/api/v1/gateway-configuration-snapshot", nil, nil)
	networkWant(t, w, 200)
	if b["snapshotId"] != again["snapshotId"] || b["generation"] != again["generation"] {
		t.Fatal("unchanged snapshot generation moved")
	}
	w, other := networkRequest(t, a, "", "gw_other", "GET", "/api/v1/gateway-configuration-snapshot", nil, nil)
	networkWant(t, w, 200)
	if other["gatewayId"] != "gw_other" || other["snapshotId"] == again["snapshotId"] {
		t.Fatal("cross-gateway snapshot")
	}
	var d any
	if e := json.Unmarshal([]byte(encode(again)), &d); e != nil {
		t.Fatal(e)
	}
}

func TestBeaconV2PreviewUsesRawPairAndMaskSemantics(t *testing.T) {
	adv, _ := hex.DecodeString("0201061aff4c000215fda50693a4e24fb1afcfc6eb0764782512340003c5")
	response, _ := hex.DecodeString("070942636e2d3031")
	sample := map[string]any{"reportKind": "LEGACY_ADV", "rssiDbm": -60, "payloadBase64": base64.StdEncoding.EncodeToString(adv), "responseBase64": base64.StdEncoding.EncodeToString(response), "responseDelayMs": 100, "candidateCount": 1}
	profile := beaconV2GoldenProfile()
	r := BeaconV2Preview(profile, sample)
	if r["passed"] != true {
		t.Fatal("golden pair rejected", r)
	}
	sample["responseDelayMs"] = 501
	if Str(BeaconV2Preview(profile, sample), "reason") != "PAIR_TIMEOUT" {
		t.Fatal("late response accepted")
	}
	sample["responseDelayMs"] = 500
	sample["candidateCount"] = 2
	if Str(BeaconV2Preview(profile, sample), "reason") != "PAIR_AMBIGUOUS" {
		t.Fatal("ambiguous pair accepted")
	}
	sample["candidateCount"] = 1
	sample["rssiDbm"] = -128
	if Str(BeaconV2Preview(profile, sample), "reason") != "UNKNOWN_RSSI" {
		t.Fatal("unknown RSSI accepted")
	}
	sample["rssiDbm"] = -60
	sample["responseBase64"] = base64.StdEncoding.EncodeToString(append(response, []byte{2, 9, 'X'}...))
	if Str(BeaconV2Preview(profile, sample), "reason") != "UNSUPPORTED_CONTEXT" {
		t.Fatal("conflicting complete names accepted")
	}
	sample["responseBase64"] = base64.StdEncoding.EncodeToString(response)
	sample["payloadBase64"] = base64.StdEncoding.EncodeToString(append(adv, 0, 1))
	if Str(BeaconV2Preview(profile, sample), "reason") != "MALFORMED_AD" {
		t.Fatal("nonzero AD padding accepted")
	}
	profile = pm(`{"scan":{"enabled":true},"filters":{},"beaconFilterV2":{"version":2,"scope":"REPORT","rules":[{"action":"ALLOW","reportKinds":["LEGACY_ADV"],"fieldMask":4,"rssi":{"mode":"BITS","valueDbm":-64,"mask":240}}]}}`)
	sample = map[string]any{"reportKind": "LEGACY_ADV", "rssiDbm": -49, "payloadBase64": ""}
	if BeaconV2Preview(profile, sample)["passed"] != true {
		t.Fatal("RSSI mask upper endpoint rejected")
	}
	sample["rssiDbm"] = -48
	if BeaconV2Preview(profile, sample)["passed"] != false {
		t.Fatal("RSSI bit mask treated as threshold")
	}
}
func TestComponentArtifactLookupDoesNotInventInventory(t *testing.T) {
	a, _ := networkFixture(t)
	integrationCaps(t, a, "COMPONENT_INVENTORY_1")
	hash := strings.Repeat("d", 64)
	path := "/api/v1/gateway-component-artifact?component=GATEWAY&sha256=" + hash
	w, _ := networkRequest(t, a, "", "gw_test", "GET", path, nil, nil)
	networkWant(t, w, 404)
	compat := map[string]any{"kind": "FIRMWARE", "component": "GATEWAY", "sha256": hash, "compatibleModels": []any{"GW32I33V2"}}
	_, e := a.Management.Exec("INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,compatibility_json,state,created_at_ms) VALUES('local-image','firmware','local-image','1.2',?,'local.bin',123,'{}',?,'verified',?)", hash, encode(compat), Now())
	if e != nil {
		t.Fatal(e)
	}
	w, b := networkRequest(t, a, "", "gw_test", "GET", path, nil, nil)
	networkWant(t, w, 200)
	if b["artifactId"] != "local-image" || b["component"] != "GATEWAY" || Num(b, "sizeBytes") != 123 {
		t.Fatal(b)
	}
	var count int
	if e = a.Management.QueryRow("SELECT count(*) FROM gateway_component_inventory").Scan(&count); e != nil || count != 0 {
		t.Fatal("metadata lookup invented running inventory", e)
	}
	w, _ = networkRequest(t, a, "", "gw_test", "GET", strings.Replace(path, "GATEWAY", "NRF54", 1), nil, nil)
	networkWant(t, w, 404)
}
func TestSnapshotCompleteHistoryAndExplicitSingleGroupRestore(t *testing.T) {
	a, actor := networkFixture(t)
	integrationCaps(t, a, "CONFIG_SNAPSHOT_1")
	initial := gw32Defaults()
	Obj(Obj(initial, "network"), "wifiSta")["ipv4"] = map[string]any{"mode": "DHCP"}
	initial["upload"] = map[string]any{"batchMaxRecords": 32, "flushIntervalMs": 1000}
	for _, radio := range []string{"nrf54", "esp32"} {
		v := Obj(Obj(initial, "radios"), radio)
		v["filters"] = map[string]any{"minRssi": nil, "serviceUuids": []any{}, "manufacturerIds": []any{}}
		v["beaconFilterV2"] = nil
	}
	for i, group := range snapshotGroups {
		value := snapshotPatchGroups(initial)[group]
		var patch map[string]any
		switch group {
		case "network", "upload":
			patch = map[string]any{group: value}
		default:
			radio := "nrf54"
			if strings.HasPrefix(group, "esp32") {
				radio = "esp32"
			}
			part := value
			if strings.HasSuffix(group, "Advertising") {
				part = map[string]any{"advertising": value}
			}
			patch = map[string]any{"radios": map[string]any{radio: part}}
		}
		cfg, task := fmt.Sprintf("baseline_cfg_%d", i), fmt.Sprintf("baseline_task_%d", i)
		_, e := a.Management.Exec("INSERT INTO gateway_configs(config_id,gateway_id,revision,desired_json,reported_json,content_hash,state,created_by,created_at_ms,verified_at_ms) VALUES(?,'gw_test',?,?,?,?,'verified',?,?,?)", cfg, i+2, encode(patch), encode(patch), Hash(patch), actor, Now(), Now())
		if e != nil {
			t.Fatal(e)
		}
		_, e = a.Management.Exec("INSERT INTO command_tasks(task_id,gateway_id,created_by,operation,idempotency_key,request_hash,request_json,config_id,state,risk_class,expires_at_ms,created_at_ms,updated_at_ms) VALUES(?,'gw_test',?,'CONFIGURE',?,'h','{}',?,'succeeded','readback',?,?,?)", task, actor, task, cfg, Now()+60000, Now(), Now())
		if e != nil {
			t.Fatal(e)
		}
	}
	w, snapshot := networkRequest(t, a, "", "gw_test", "GET", "/api/v1/gateway-configuration-snapshot", nil, nil)
	networkWant(t, w, 200)
	if snapshot["complete"] != true {
		t.Fatal("proven complete groups rejected", snapshot)
	}
	if e := a.Validate("GatewayConfigurationSnapshot", snapshot); e != nil {
		t.Fatal(e)
	}
	source := Str(snapshot, "snapshotId")
	request := map[string]any{"gatewayId": "gw_test", "type": "CONFIGURE", "parameters": map[string]any{"configuration": map[string]any{"upload": initial["upload"]}, "expectedReportedRevision": 7, "sourceSnapshotId": source}, "expiresAt": Timestamp(Now() + 60000)}
	w, _ = networkRequest(t, a, actor, "", "POST", "/api/v1/tasks", request, map[string]string{"Idempotency-Key": "restore-upload"})
	networkWant(t, w, 202)
	w, latest := networkRequest(t, a, "", "gw_test", "GET", "/api/v1/gateway-configuration-snapshot", nil, nil)
	networkWant(t, w, 200)
	if latest["complete"] != false || latest["snapshotId"] == source {
		t.Fatal("pending restoration not represented")
	}
	w, history := networkRequest(t, a, "", "gw_test", "GET", "/api/v1/gateway-configuration-snapshot?snapshotId="+source, nil, nil)
	networkWant(t, w, 200)
	if encode(history) != encode(snapshot) {
		t.Fatal("immutable restoration source changed")
	}
	w, _ = networkRequest(t, a, "", "gw_other", "GET", "/api/v1/gateway-configuration-snapshot?snapshotId="+source, nil, nil)
	networkWant(t, w, 404)
}
