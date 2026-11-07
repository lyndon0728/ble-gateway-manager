package platform

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
)

func frontendRequest(t *testing.T, a *App, actor, path string) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	p, e := a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	r := httptest.NewRequest(http.MethodGet, path, nil)
	r = r.WithContext(context.WithValue(r.Context(), authKey{}, &authInfo{Principal: p}))
	router := chi.NewRouter()
	a.RegisterFrontendAPI(router)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, r)
	return w, pm(w.Body.String())
}
func frontendSuccess(t *testing.T, a *App, actor, path, schema string) map[string]any {
	t.Helper()
	w, m := frontendRequest(t, a, actor, path)
	if w.Code != 200 {
		t.Fatalf("GET %s: %d %s", path, w.Code, w.Body.String())
	}
	if e := a.Validate(schema, m); e != nil {
		t.Fatalf("%s: %v", schema, e)
	}
	return m
}
func frontendSQL(t *testing.T, a *App, query string, args ...any) {
	t.Helper()
	if _, e := a.Management.Exec(query, args...); e != nil {
		t.Fatal(e)
	}
}

func TestFrontendConfigurationContainsOnlyVerifiedKnownPaths(t *testing.T) {
	a, actor := gw32Fixture(t)
	a.Management.SetMaxOpenConns(1)
	empty := frontendSuccess(t, a, actor, "/api/v1/gateways/gw_test/configuration/effective", "EffectiveConfiguration")
	if len(Obj(empty, "configuration")) != 0 || len(List(empty, "knownPaths")) != 0 || empty["complete"] != false {
		t.Fatal("fabricated configuration", empty)
	}
	frontendSQL(t, a, "UPDATE gateway_configs SET state='verified',reported_json=?,verified_at_ms=? WHERE gateway_id='gw_test'", `{"scan":{"intervalMs":80},"network":{"ethernet":{"ipv4":{"mode":"STATIC","address":"10.0.0.2","prefixLength":24,"gateway":"10.0.0.1","dns":["10.0.0.1"]}}}}`, Now())
	frontendSQL(t, a, "INSERT INTO gateway_configs(config_id,gateway_id,revision,desired_json,reported_json,content_hash,state,created_by,created_at_ms,verified_at_ms) VALUES('cfg_second','gw_test',2,'{}',?,'hash','verified',?,?,?)", `{"network":{"ethernet":{"ipv4":{"mode":"DHCP"}}}}`, actor, Now(), Now())
	frontendSQL(t, a, "INSERT INTO gateway_configs(config_id,gateway_id,revision,desired_json,content_hash,state,created_by,created_at_ms) VALUES('cfg_unverified','gw_test',3,?,'hash','desired',?,?)", `{"radios":{"esp32":{"scan":{"enabled":true}}}}`, actor, Now())
	v := frontendSuccess(t, a, actor, "/api/v1/gateways/gw_test/configuration/effective", "EffectiveConfiguration")
	cfg := Obj(v, "configuration")
	if Num(Obj(Obj(Obj(cfg, "radios"), "nrf54"), "scan"), "intervalMs") != 80 {
		t.Fatal(v)
	}
	if len(Obj(Obj(cfg, "radios"), "esp32")) > 0 || len(Obj(Obj(cfg, "network"), "wifiSta")) > 0 {
		t.Fatal("invented or pending configuration", v)
	}
	ip := Obj(Obj(Obj(cfg, "network"), "ethernet"), "ipv4")
	if len(ip) != 1 || Str(ip, "mode") != "DHCP" {
		t.Fatal("IPv4 replacement retained stale fields", v)
	}
	if Num(v, "reportedRevision") != 2 {
		t.Fatal(v)
	}
	for _, raw := range List(v, "sources") {
		s := raw.(map[string]any)
		if strings.Contains(Str(s, "path"), "address") {
			t.Fatal("stale provenance", s)
		}
	}
	scoped := autoScopedPrincipal(t, a, "outside_config", "device_admin", "scope_default_project")
	frontendSQL(t, a, "UPDATE role_bindings SET scope_id='scope_default_group' WHERE principal_id=?", scoped)
	frontendSQL(t, a, "INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('other_scope','group','scope_default_project','other',?)", Now())
	frontendSQL(t, a, "UPDATE gateways SET scope_id='other_scope' WHERE gateway_id='gw_test'")
	w, _ := frontendRequest(t, a, scoped, "/api/v1/gateways/gw_test/configuration/effective")
	if w.Code != 404 {
		t.Fatal(w.Code, w.Body.String())
	}
}

func TestFrontendTaskEvidenceRedactionSequenceAndReload(t *testing.T) {
	a, actor := controlFixture(t)
	task := controlNewTask(t, a, actor, "request-evidence", "DIAGNOSTICS", map[string]any{"profile": "BASIC"})
	id := Str(task, "taskId")
	var command string
	if e := a.Management.QueryRow("SELECT request_json FROM command_tasks WHERE task_id=?", id).Scan(&command); e != nil {
		t.Fatal(e)
	}
	m := pm(command)
	params := Obj(m, "parameters")
	params["password"] = "must-not-leak"
	params["valueBase64"] = "c2VjcmV0"
	m["parameters"] = params
	frontendSQL(t, a, "UPDATE command_tasks SET request_json=? WHERE task_id=?", encode(m), id)
	for _, sequence := range []int{2, 1} {
		raw := map[string]any{"state": "EXECUTING", "details": map[string]any{"token": "never-show", "valueBase64": "c2VjcmV0", "connectionRef": strings.Repeat("a", 32)}}
		frontendSQL(t, a, "INSERT INTO command_results(result_id,task_id,gateway_id,sequence,stage,content_hash,result_json,received_at_ms,dedup_until_ms) VALUES(?,?,'gw_test',?,'executing','hash',?,?,?)", fmt.Sprint("result_", sequence), id, sequence, encode(raw), Now(), Now()+60000)
	}
	a.Management.SetMaxOpenConns(1)
	v := frontendSuccess(t, a, actor, "/api/v1/tasks/"+id+"/request", "TaskRequestView")
	text := encode(v)
	if strings.Contains(text, "must-not-leak") || strings.Contains(text, "c2VjcmV0") {
		t.Fatal("secret escaped redaction", text)
	}
	first := frontendSuccess(t, a, actor, "/api/v1/tasks/"+id+"/results?limit=1", "TaskResultHistoryPage")
	one := List(first, "items")[0].(map[string]any)
	if Num(one, "sequence") != 1 || strings.Contains(encode(first), "never-show") {
		t.Fatal(first)
	}
	next := frontendSuccess(t, a, actor, "/api/v1/tasks/"+id+"/results?limit=1&cursor="+url.QueryEscape(Str(first, "nextCursor")), "TaskResultHistoryPage")
	if Num(List(next, "items")[0].(map[string]any), "sequence") != 2 {
		t.Fatal(next)
	}
	fresh := frontendSuccess(t, a, actor, "/api/v1/tasks/"+id+"/results", "TaskResultHistoryPage")
	if len(List(fresh, "items")) != 2 {
		t.Fatal("list does not survive reload", fresh)
	}
	frontendSQL(t, a, "UPDATE command_results SET result_json=NULL WHERE result_id='result_1'")
	reclaimed := frontendSuccess(t, a, actor, "/api/v1/tasks/"+id+"/results", "TaskResultHistoryPage")
	firstResult := List(reclaimed, "items")[0].(map[string]any)
	if firstResult["payloadAvailable"] != false || firstResult["result"] != nil {
		t.Fatal("reclaimed task result must remain an explicit metadata-only record", firstResult)
	}
}

func TestFrontendPayloadReadsRequireScopedSensitivePermission(t *testing.T) {
	a, actor := pipelineFixture(t)
	installPipelineTestBundle(t, a, "frontend_bundle", "http://127.0.0.1:12345/customer", 1)
	if _, e := a.AcceptIngress(t.Context(), a.Config.NamespaceID, "gw_test", testIngress(a, "frontend_event")); e != nil {
		t.Fatal(e)
	}
	if _, e := a.ProcessOne(t.Context()); e != nil {
		t.Fatal(e)
	}
	var out, delivery string
	if e := a.Pipeline.QueryRow("SELECT output_id,delivery_id FROM delivery_outbox LIMIT 1").Scan(&out, &delivery); e != nil {
		t.Fatal(e)
	}
	operator := autoScopedPrincipal(t, a, "read_operator", "operator", "scope_default_group")
	developer := autoScopedPrincipal(t, a, "read_developer", "developer", "scope_default_group")
	frontendSQL(t, a, "INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('no_access','group','scope_default_project','other',?)", Now())
	outsider := autoScopedPrincipal(t, a, "other_developer", "developer", "no_access")
	for _, path := range []string{"/api/v1/outputs/" + out, "/api/v1/deliveries/" + delivery + "/payload"} {
		w, _ := frontendRequest(t, a, operator, path)
		if w.Code != 403 {
			t.Fatal("operator read sensitive payload", w.Code, w.Body.String())
		}
		w, _ = frontendRequest(t, a, outsider, path)
		if w.Code != 404 {
			t.Fatal("out-of-scope identity exposed", w.Code, w.Body.String())
		}
	}
	v := frontendSuccess(t, a, developer, "/api/v1/outputs/"+out, "OutputRecord")
	if v["payloadAvailable"] != true || len(Obj(v, "record")) == 0 {
		t.Fatal(v)
	}
	payload := frontendSuccess(t, a, developer, "/api/v1/deliveries/"+delivery+"/payload", "DeliveryPayload")
	if Str(Obj(payload, "payload"), "deliveryId") != delivery {
		t.Fatal(payload)
	}
	links := frontendSuccess(t, a, operator, "/api/v1/gateways/gw_test/events/frontend_event/deliveries", "DeliveryPage")
	if len(List(links, "items")) != 1 {
		t.Fatal(links)
	}
	if _, e := a.Pipeline.Exec("UPDATE processing_results SET result_json=NULL,purged_at_ms=? WHERE output_id=?", Now(), out); e != nil {
		t.Fatal(e)
	}
	if _, e := a.Pipeline.Exec("UPDATE delivery_outbox SET state='terminated',terminated_reason='test retention',wire_payload=NULL,payload_purged_at_ms=? WHERE delivery_id=?", Now(), delivery); e != nil {
		t.Fatal(e)
	}
	for _, tc := range []struct{ path, schema, field string }{{"/api/v1/outputs/" + out, "OutputRecord", "record"}, {"/api/v1/deliveries/" + delivery + "/payload", "DeliveryPayload", "payload"}} {
		v := frontendSuccess(t, a, actor, tc.path, tc.schema)
		if v["payloadAvailable"] != false || v[tc.field] != nil {
			t.Fatal("reclaimed payload invented", v)
		}
	}
}

func TestFrontendHistoryOwnershipAndArtifactScope(t *testing.T) {
	a, admin := pipelineFixture(t)
	developer := autoScopedPrincipal(t, a, "history_developer", "developer", "scope_default_group")
	device := autoScopedPrincipal(t, a, "history_device", "device_admin", "scope_default_group")
	var bundle string
	if e := a.Pipeline.QueryRow("SELECT bundle_id FROM pipeline_bundles WHERE state='active'").Scan(&bundle); e != nil {
		t.Fatal(e)
	}
	for i, owner := range []string{admin, developer} {
		id := fmt.Sprint("job_frontend_", i)
		if _, e := a.Pipeline.Exec("INSERT INTO reprocess_jobs(reprocess_id,bundle_id,requested_by,idempotency_key,request_hash,selection_json,reason,state,created_at_ms) VALUES(?,?,?,?,?,'{}','test','pending',?)", id, bundle, owner, id, "hash", Now()); e != nil {
			t.Fatal(e)
		}
		frontendSQL(t, a, "INSERT INTO maintenance_jobs(job_id,kind,created_by,request_json,state,checkpoint_json,created_at_ms,finished_at_ms) VALUES(?,'diagnostic',?,'{}','succeeded','{}',?,?)", id, owner, Now(), Now())
	}
	for i := 0; i < 2; i++ {
		frontendSQL(t, a, "INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,state,created_at_ms) VALUES(?,'firmware',?,'1',?,'unused',1,'signed','verified',?)", fmt.Sprint("artifact_f_", i), fmt.Sprint("firmware", i), strings.Repeat("0", 64), Now())
	}
	a.Management.SetMaxOpenConns(1)
	a.Pipeline.SetMaxOpenConns(1)
	for _, owner := range []string{admin, developer} {
		v := frontendSuccess(t, a, owner, "/api/v1/reprocess-jobs", "ReprocessJobPage")
		if len(List(v, "items")) != 1 {
			t.Fatal("owner-only reprocess list violated", owner, v)
		}
	}
	v := frontendSuccess(t, a, developer, "/api/v1/operations", "OperationPage")
	if len(List(v, "items")) != 1 {
		t.Fatal(v)
	}
	v = frontendSuccess(t, a, admin, "/api/v1/operations", "OperationPage")
	if len(List(v, "items")) != 2 {
		t.Fatal(v)
	}
	w, _ := frontendRequest(t, a, device, "/api/v1/artifacts")
	if w.Code != 403 {
		t.Fatal("group-scoped device admin read global artifacts", w.Code)
	}
	first := frontendSuccess(t, a, admin, "/api/v1/artifacts?limit=1", "ArtifactPage")
	next := frontendSuccess(t, a, admin, "/api/v1/artifacts?limit=1&cursor="+url.QueryEscape(Str(first, "nextCursor")), "ArtifactPage")
	if Str(List(first, "items")[0].(map[string]any), "artifactId") == Str(List(next, "items")[0].(map[string]any), "artifactId") {
		t.Fatal("artifact page identities collide")
	}
	filtered := frontendSuccess(t, a, admin, "/api/v1/artifacts?kind=PLATFORM_PACKAGE", "ArtifactPage")
	if len(List(filtered, "items")) != 0 {
		t.Fatal("artifact filter ignored", filtered)
	}
	frontendSQL(t, a, "WITH RECURSIVE numbers(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM numbers WHERE n<10001) INSERT INTO maintenance_jobs(job_id,kind,created_by,request_json,state,checkpoint_json,created_at_ms,finished_at_ms) SELECT 'op_many_'||n,'diagnostic',?,'{}','succeeded','{}',?,? FROM numbers", admin, Now(), Now())
	w, _ = frontendRequest(t, a, admin, "/api/v1/operations")
	if w.Code != 422 {
		t.Fatal("large history returned silently incomplete data", w.Code)
	}
	filtered = frontendSuccess(t, a, admin, "/api/v1/operations?state=FAILED", "OperationPage")
	if len(List(filtered, "items")) != 0 {
		t.Fatal("operation filter not applied before pagination bound", filtered)
	}
}

func TestFrontendBLEReadModelsRespectPeerAndGatewayScopes(t *testing.T) {
	a, admin, peer := autoFixture(t)
	rule := autoCreateRule(t, a, admin, autoRuleBody(peer, "gw_test"), "frontend_rule")
	ref := gw32Open(t, a, admin)
	frontendSQL(t, a, "INSERT INTO gatt_subscriptions VALUES(?,?,?,?,?,?,?,?,?)", a.Config.NamespaceID, "gw_test", ref, autoBatteryUUID, "00002a19-0000-1000-8000-00805f9b34fb", 18, "NOTIFY", "active", Now())
	frontendSQL(t, a, "INSERT INTO gatt_subscriptions VALUES(?,?,?,?,?,?,?,?,?)", a.Config.NamespaceID, "gw_test", ref, autoBatteryUUID, "00002a1a-0000-1000-8000-00805f9b34fb", 19, "NOTIFY", "active", Now())
	var task string
	if e := a.Management.QueryRow("SELECT task_id FROM command_tasks LIMIT 1").Scan(&task); e != nil {
		t.Fatal(e)
	}
	frontendSQL(t, a, "INSERT INTO auto_connect_triggers VALUES(?,?,?,?,?,?,?,?,?,?)", "trigger_frontend", a.Config.NamespaceID, "gw_test", rule, 1, "adv", "rsp", autoBatteryUUID, task, Now())
	a.Management.SetMaxOpenConns(1)
	frontendSuccess(t, a, admin, "/api/v1/ble-peers/"+peer, "BLEPeer")
	frontendSuccess(t, a, admin, "/api/v1/auto-connect-rules/"+rule, "AutoConnectRule")
	v := frontendSuccess(t, a, admin, "/api/v1/gatt-subscriptions?gatewayId=gw_test&limit=1", "GATTSubscriptionPage")
	next := frontendSuccess(t, a, admin, "/api/v1/gatt-subscriptions?gatewayId=gw_test&limit=1&cursor="+url.QueryEscape(Str(v, "nextCursor")), "GATTSubscriptionPage")
	if Str(List(v, "items")[0].(map[string]any), "subscriptionId") == Str(List(next, "items")[0].(map[string]any), "subscriptionId") {
		t.Fatal("subscriptions collide on connectionRef")
	}
	v = frontendSuccess(t, a, admin, "/api/v1/auto-connect-triggers?ruleId="+rule, "AutoConnectTriggerPage")
	if len(List(v, "items")) != 1 {
		t.Fatal(v)
	}
	frontendSQL(t, a, "INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('ble_other','group','scope_default_project','other',?)", Now())
	outside := autoScopedPrincipal(t, a, "ble_outside", "device_admin", "ble_other")
	for _, path := range []string{"/api/v1/ble-peers/" + peer, "/api/v1/auto-connect-rules/" + rule, "/api/v1/gatt-subscriptions?gatewayId=gw_test", "/api/v1/auto-connect-triggers?ruleId=" + rule} {
		w, _ := frontendRequest(t, a, outside, path)
		if w.Code != 404 {
			t.Fatalf("%s escaped scope: %d", path, w.Code)
		}
	}
}
