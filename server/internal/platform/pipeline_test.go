package platform

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
)

func pipelineFixture(t *testing.T) (*App, string) {
	t.Helper()
	a, actor := controlFixture(t)
	if err := a.ensurePipelineDefault(context.Background()); err != nil {
		t.Fatal(err)
	}
	return a, actor
}

func TestPipelineConcurrentIngressHasOneCustodyRecord(t *testing.T) {
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	batch := testIngress(a, "evt_race")
	var wg sync.WaitGroup
	errs := make(chan error, 16)
	statuses := make(chan string, 16)
	for i := 0; i < 16; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
			if err != nil {
				errs <- err
				return
			}
			statuses <- ps(ack["items"].([]pipeMap)[0], "status")
		}()
	}
	wg.Wait()
	close(errs)
	close(statuses)
	for err := range errs {
		t.Error(err)
	}
	accepted := 0
	for s := range statuses {
		if s == "ACCEPTED" {
			accepted++
		} else if s != "DUPLICATE" {
			t.Error(s)
		}
	}
	var receipts, jobs int
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM event_receipts").Scan(&receipts)
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM processing_jobs").Scan(&jobs)
	if receipts != 1 || jobs != 1 || accepted != 1 {
		t.Fatalf("receipts=%d jobs=%d accepted=%d", receipts, jobs, accepted)
	}
}

func TestPipelineSlowTargetDoesNotBlockAnotherTarget(t *testing.T) {
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	slowStarted := make(chan struct{})
	release := make(chan struct{})
	fastDone := make(chan struct{})
	receiver := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var batch pipeMap
		_ = json.NewDecoder(r.Body).Decode(&batch)
		event := pl(batch, "events")[0].(map[string]any)
		if r.URL.Path == "/slow" {
			close(slowStarted)
			<-release
		} else {
			close(fastDone)
		}
		pjson(w, 200, pipeMap{"batchId": batch["batchId"], "targetId": event["targetId"], "targetVersion": "1", "durability": "CUSTOMER_INBOX_PERSISTED", "items": []any{pipeMap{"deliveryId": event["deliveryId"], "status": "ACCEPTED"}}})
	}))
	defer receiver.Close()
	installPipelineTestBundle(t, a, "bundle_multi", receiver.URL+"/slow", 1)
	var raw string
	_ = a.Pipeline.QueryRow("SELECT manifest_json FROM pipeline_bundles WHERE bundle_id='bundle_multi'").Scan(&raw)
	m := pm(raw)
	targets := po(m, "targets")
	fast := pm(pipeJSON(targets["target_test@1"]))
	fast["endpoint"] = receiver.URL + "/fast"
	targets["target_fast@1"] = fast
	rule := pl(m, "rules")[0].(map[string]any)
	rule["targetVersions"] = append(pl(rule, "targetVersions"), pipeMap{"targetId": "target_fast", "version": "1", "mappingVersion": "1", "required": true})
	if err := a.activatePipelineBundle(ctx, "bundle_multi2", pipeJSON(m), pipeHash(m), 2); err != nil {
		t.Fatal(err)
	}
	_, _ = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_multi"))
	if _, err := a.ProcessOne(ctx); err != nil {
		t.Fatal(err)
	}
	_, _ = a.Pipeline.Exec("UPDATE delivery_outbox SET next_attempt_at_ms=0 WHERE target_id='target_test'")
	results := make(chan error, 2)
	go func() { _, err := a.DeliverOnce(ctx); results <- err }()
	select {
	case <-slowStarted:
	case <-time.After(3 * time.Second):
		close(release)
		t.Fatal("slow target was not started")
	}
	go func() { _, err := a.DeliverOnce(ctx); results <- err }()
	select {
	case <-fastDone:
	case <-time.After(3 * time.Second):
		close(release)
		t.Fatal("fast target waited for unrelated slow HTTP response")
	}
	close(release)
	for i := 0; i < 2; i++ {
		if err := <-results; err != nil {
			t.Fatal(err)
		}
	}
	var n int
	_ = a.Pipeline.QueryRow("SELECT count(*) FROM delivery_outbox WHERE state='delivered'").Scan(&n)
	if n != 2 {
		t.Fatal(n)
	}
}
func testIngress(a *App, ids ...string) pipeMap {
	events := []any{}
	for _, id := range ids {
		events = append(events, pipeMap{"eventId": id, "timeQuality": "UNSYNCED", "protocolHint": "reference-sensor-v1", "payloadBase64": base64.StdEncoding.EncodeToString(referenceFrame())})
	}
	return pipeMap{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "batchId": "test_batch", "events": events}
}
func installPipelineTestBundle(t *testing.T, a *App, id, endpoint string, expected int64) {
	t.Helper()
	config := pipeMap{"protocol": "HTTP", "endpoint": endpoint, "ackMode": "HTTP_DURABLE_RESPONSE", "mappingVersion": "1", "maxBatchEvents": 200, "maxInflightEvents": 200, "maxQueueBytes": 1048576}
	rule := pipeMap{"ruleId": "reference", "priority": 10, "match": pipeMap{"modelIds": []any{"GW"}}, "action": "DECODE", "parserId": "reference_sensor", "parserVersion": "1.0.0", "schemaVersion": "reference-sensor-1", "targetVersions": []any{pipeMap{"targetId": "target_test", "version": "1", "mappingVersion": "1", "required": true}}}
	m := pipeMap{"name": id, "rules": []any{rule}, "defaultAction": "QUARANTINE", "targets": pipeMap{"target_test@1": config}}
	if err := a.activatePipelineBundle(context.Background(), id, pipeJSON(m), pipeHash(m), expected); err != nil {
		t.Fatal(err)
	}
}
func testAck(b *deliveryBatch, items ...deliveryItem) pipeMap {
	acks := []any{}
	for _, item := range items {
		acks = append(acks, pipeMap{"deliveryId": item.ID, "status": "ACCEPTED"})
	}
	return pipeMap{"batchId": b.ID, "targetId": b.Target, "targetVersion": b.Version, "durability": "CUSTOMER_INBOX_PERSISTED", "items": acks}
}
func TestPipelineT1AtomicReceiptAndImmutableDedup(t *testing.T) {
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	if _, err := a.Pipeline.Exec("CREATE TRIGGER fail_t1 BEFORE INSERT ON processing_jobs BEGIN SELECT RAISE(ABORT,'injected fsync boundary failure'); END"); err != nil {
		t.Fatal(err)
	}
	batch := testIngress(a, "evt_1")
	if ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch); err == nil || ack != nil {
		t.Fatal("ACK escaped before T1 commit")
	}
	var n int
	_ = a.Pipeline.QueryRow("SELECT COUNT(*) FROM event_receipts").Scan(&n)
	if n != 0 {
		t.Fatal("partial receipt leaked")
	}
	_, _ = a.Pipeline.Exec("DROP TRIGGER fail_t1")
	ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
	if err != nil {
		t.Fatal(err)
	}
	if err = a.Validate("IngressAck", ack); err != nil {
		t.Fatal(err)
	}
	if ps(ack["items"].([]pipeMap)[0], "status") != "ACCEPTED" {
		t.Fatal(ack)
	}
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "DUPLICATE" {
		t.Fatal(ack, err)
	}
	pl(batch, "events")[0].(map[string]any)["sequence"] = float64(99)
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "REJECTED_PERMANENT" {
		t.Fatal(ack, err)
	}
	a.Config.MaxQueueBytes = 18
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_2"))
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "REJECTED_RETRYABLE" {
		t.Fatal(ack, err)
	}
	if _, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_other", batch); err == nil {
		t.Fatal("gateway identity accepted")
	}
}
func TestPipelineT2RollbackAndPinnedOldBundle(t *testing.T) {
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	installPipelineTestBundle(t, a, "bundle_old", "http://127.0.0.1:9999/inbox", 1)
	if _, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_old")); err != nil {
		t.Fatal(err)
	}
	m := pipeMap{"rules": []any{}, "targets": pipeMap{}, "defaultAction": "QUARANTINE"}
	if err := a.activatePipelineBundle(ctx, "bundle_new", pipeJSON(m), pipeHash(m), 2); err != nil {
		t.Fatal(err)
	}
	_, _ = a.Pipeline.Exec("CREATE TRIGGER fail_t2 BEFORE INSERT ON delivery_outbox BEGIN SELECT RAISE(ABORT,'injected write failure'); END")
	if _, err := a.ProcessOne(ctx); err == nil {
		t.Fatal("injection failed")
	}
	var n int
	_ = a.Pipeline.QueryRow("SELECT COUNT(*) FROM processing_results").Scan(&n)
	if n != 0 {
		t.Fatal("T2 leaked a result without outbox")
	}
	_, _ = a.Pipeline.Exec("DROP TRIGGER fail_t2")
	_, _ = a.Pipeline.Exec("UPDATE processing_jobs SET lease_until_ms=0 WHERE state='leased'")
	if worked, err := a.ProcessOne(ctx); err != nil || !worked {
		t.Fatal(worked, err)
	}
	var raw []byte
	if err := a.Pipeline.QueryRow("SELECT wire_payload FROM delivery_outbox").Scan(&raw); err != nil {
		t.Fatal(err)
	}
	wire := pm(string(raw))
	if err := a.Validate("CustomerEvent", wire); err != nil {
		t.Fatal(err)
	}
	if ps(wire, "schemaVersion") != "reference-sensor-1" {
		t.Fatal("new bundle changed pinned work")
	}
	if worked, err := a.ProcessOne(ctx); err != nil || worked {
		t.Fatal("job reprocessed", worked, err)
	}
	var state string
	_ = a.Pipeline.QueryRow("SELECT state FROM processing_jobs").Scan(&state)
	if state != "completed" {
		t.Fatal(state)
	}
}
func TestPipelineCustomerPartialAckAndLeaseFencing(t *testing.T) {
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	installPipelineTestBundle(t, a, "bundle_ack", "http://127.0.0.1:9999/inbox", 1)
	_, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_a", "evt_b"))
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if _, err = a.ProcessOne(ctx); err != nil {
			t.Fatal(err)
		}
	}
	b, err := a.claimDeliveryBatch(ctx)
	if err != nil || len(b.Items) != 2 {
		t.Fatal(b, err)
	}
	ack := testAck(b, b.Items[0])
	if err = a.AcceptCustomerAck(ctx, "target_wrong", ack); err == nil {
		t.Fatal("cross-target ACK accepted")
	}
	if err = a.AcceptCustomerAck(ctx, b.Target, ack); err != nil {
		t.Fatal(err)
	}
	var n int
	_ = a.Pipeline.QueryRow("SELECT COUNT(*) FROM delivery_outbox WHERE state='delivered'").Scan(&n)
	if n != 1 {
		t.Fatal("partial ACK acknowledged other item")
	}
	_, _ = a.Pipeline.Exec("UPDATE delivery_outbox SET lease_token=lease_token+1 WHERE delivery_id=?", b.Items[1].ID)
	if err = a.AcceptCustomerAck(ctx, b.Target, testAck(b, b.Items[1])); err != nil {
		t.Fatal(err)
	}
	_ = a.Pipeline.QueryRow("SELECT COUNT(*) FROM delivery_outbox WHERE state='delivered'").Scan(&n)
	if n != 1 {
		t.Fatal("stale lease modified custody")
	}
	_, _ = a.Pipeline.Exec("UPDATE delivery_outbox SET lease_until_ms=0 WHERE state='awaiting_ack'")
	again, err := a.claimDeliveryBatch(ctx)
	if err != nil || len(again.Items) != 1 {
		t.Fatal(again, err)
	}
	if !bytes.Equal(again.Items[0].Payload, b.Items[1].Payload) || again.Items[0].ID != b.Items[1].ID {
		t.Fatal("retry remapped immutable payload")
	}
	if err = a.AcceptCustomerAck(ctx, again.Target, testAck(again, again.Items...)); err != nil {
		t.Fatal(err)
	}
	_ = a.Pipeline.QueryRow("SELECT COUNT(*) FROM event_receipts WHERE custody_state='delivered'").Scan(&n)
	if n != 2 {
		t.Fatal("source delivery states wrong", n)
	}
}
func TestPipelineHTTPRequiresDurableAck(t *testing.T) {
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	var accept atomic.Bool
	var calls atomic.Int32
	receiver := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		var batch pipeMap
		if err := json.NewDecoder(r.Body).Decode(&batch); err != nil {
			t.Error(err)
		}
		if !accept.Load() {
			w.WriteHeader(204)
			return
		}
		items := pl(batch, "events")
		acks := []any{}
		for _, v := range items {
			event := v.(map[string]any)
			if err := a.Validate("CustomerEvent", event); err != nil {
				t.Error(err)
			}
			acks = append(acks, pipeMap{"deliveryId": event["deliveryId"], "status": "ACCEPTED"})
		}
		pjson(w, 200, pipeMap{"batchId": batch["batchId"], "targetId": "target_test", "targetVersion": "1", "durability": "CUSTOMER_INBOX_PERSISTED", "items": acks})
	}))
	defer receiver.Close()
	installPipelineTestBundle(t, a, "bundle_http", receiver.URL, 1)
	_, _ = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_http"))
	if _, err := a.ProcessOne(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err := a.DeliverOnce(ctx); err == nil {
		t.Fatal("204 released custody")
	}
	var state string
	_ = a.Pipeline.QueryRow("SELECT state FROM delivery_outbox").Scan(&state)
	if state != "retry_wait" {
		t.Fatal(state)
	}
	accept.Store(true)
	_, _ = a.Pipeline.Exec("UPDATE delivery_outbox SET next_attempt_at_ms=0")
	if _, err := a.DeliverOnce(ctx); err != nil {
		t.Fatal(err)
	}
	_ = a.Pipeline.QueryRow("SELECT state FROM delivery_outbox").Scan(&state)
	if state != "delivered" || calls.Load() != 2 {
		t.Fatal(state, calls.Load())
	}
	_, _ = a.Pipeline.Exec("UPDATE event_receipts SET raw_retention_until_ms=0")
	if err := a.ReclaimPipelinePayloads(ctx); err != nil {
		t.Fatal(err)
	}
	ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_http"))
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "DUPLICATE" {
		t.Fatal("retention removed dedup anchor", ack, err)
	}
}
func pipelineRequest(t *testing.T, a *App, actor, method, path string, body pipeMap, key string, headers ...map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	p, err := a.principal(context.Background(), actor)
	if err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest(method, path, bytes.NewBufferString(pipeJSON(body)))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Idempotency-Key", key)
	for _, set := range headers {
		for name, value := range set {
			req.Header.Set(name, value)
		}
	}
	req = req.WithContext(context.WithValue(req.Context(), authKey{}, &authInfo{Principal: p}))
	router := chi.NewRouter()
	a.RegisterPipeline(router)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)
	return w
}

func TestPipelineConfigurationAPIAndActivationReplay(t *testing.T) {
	a, actor := pipelineFixture(t)
	configuration := pipeMap{"protocol": "HTTP", "endpoint": "http://127.0.0.1:9999/inbox", "ackMode": "HTTP_DURABLE_RESPONSE", "mappingVersion": "1", "maxBatchEvents": 200, "maxInflightEvents": 200, "maxQueueBytes": 1048576}
	w := pipelineRequest(t, a, actor, "POST", "/api/v1/targets", pipeMap{"name": "test target", "configuration": configuration}, "target-key")
	if w.Code != 201 {
		t.Fatal(w.Code, w.Body.String())
	}
	target := pm(w.Body.String())
	if err := a.Validate("Target", target); err != nil {
		t.Fatal(err)
	}
	id := ps(target, "targetId")
	rule := pipeMap{"ruleId": "reference", "priority": 10, "match": pipeMap{"modelIds": []any{"GW"}}, "action": "DECODE", "parserId": "reference_sensor", "parserVersion": "1.0.0", "schemaVersion": "reference-sensor-1", "targetVersions": []any{pipeMap{"targetId": id, "version": "1", "mappingVersion": "1", "required": true}}}
	body := pipeMap{"name": "Reference routing", "rules": []any{rule}, "defaultAction": "QUARANTINE"}
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/rule-bundles", body, "bundle-key")
	if w.Code != 201 {
		t.Fatal(w.Code, w.Body.String())
	}
	bundle := pm(w.Body.String())
	if err := a.Validate("RuleBundle", bundle); err != nil {
		t.Fatal(err)
	}
	bundleID := ps(bundle, "bundleId")
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/rule-bundles/"+bundleID+"/validations", pipeMap{}, "validation-key")
	if w.Code != 202 {
		t.Fatal(w.Code, w.Body.String())
	}
	if err := a.Validate("Operation", pm(w.Body.String())); err != nil {
		t.Fatal(err)
	}
	activation := pipeMap{"dataNodeId": a.Config.NodeID, "expectedActivationRevision": 1}
	if _, err := a.Management.Exec("CREATE TRIGGER activation_finalize_failure BEFORE UPDATE OF state ON api_idempotency WHEN NEW.operation='bundle:activate' AND NEW.state='committed' BEGIN SELECT RAISE(ABORT,'injected management finalization crash'); END"); err != nil {
		t.Fatal(err)
	}
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/rule-bundles/"+bundleID+"/activations", activation, "activation-key", map[string]string{"If-Match": "\"activation-1\""})
	if w.Code != 503 {
		t.Fatal("activation failure injection", w.Code, w.Body.String())
	}
	var committedID string
	if err := a.Pipeline.QueryRow("SELECT bundle_id FROM pipeline_bundles WHERE state='active'").Scan(&committedID); err != nil || committedID != bundleID {
		t.Fatal("pipeline activation did not commit before management failure", committedID, err)
	}
	if _, err := a.Management.Exec("DROP TRIGGER activation_finalize_failure"); err != nil {
		t.Fatal(err)
	}
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/rule-bundles/"+bundleID+"/activations", activation, "activation-key", map[string]string{"If-Match": "\"activation-1\""})
	if w.Code != 202 {
		t.Fatal(w.Code, w.Body.String())
	}
	first := w.Body.String()
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/rule-bundles/"+bundleID+"/activations", activation, "activation-key", map[string]string{"If-Match": "\"activation-1\""})
	if w.Code != 202 || w.Body.String() != first {
		t.Fatal("activation replay changed response", w.Code, w.Body.String())
	}
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/rule-bundles/"+bundleID+"/activations", activation, "different-key", map[string]string{"If-Match": "\"activation-1\""})
	if w.Code != 412 {
		t.Fatal("stale activation accepted", w.Code, w.Body.String())
	}
	configuration["endpoint"] = "http://127.0.0.1:9998/new"
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/targets/"+id+"/versions", configuration, "version-key")
	if w.Code != 201 {
		t.Fatal(w.Code, w.Body.String())
	}
	ctx := context.Background()
	_, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_api"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err = a.ProcessOne(ctx); err != nil {
		t.Fatal(err)
	}
	var raw string
	if err = a.Pipeline.QueryRow("SELECT destination_json FROM delivery_outbox").Scan(&raw); err != nil {
		t.Fatal(err)
	}
	if ps(pm(raw), "endpoint") != "http://127.0.0.1:9999/inbox" {
		t.Fatal("target version mutated pinned bundle", raw)
	}
}

func TestPipelineCursorBindsSnapshotAndQuery(t *testing.T) {
	a, actor := pipelineFixture(t)
	_, err := a.AcceptIngress(context.Background(), a.Config.NamespaceID, "gw_test", testIngress(a, "evt_page_a", "evt_page_b"))
	if err != nil {
		t.Fatal(err)
	}
	w := pipelineRequest(t, a, actor, "GET", "/api/v1/events?limit=1", pipeMap{}, "")
	if w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	page := pm(w.Body.String())
	cursor := ps(page, "nextCursor")
	if cursor == "" {
		t.Fatal("missing next cursor")
	}
	var after string
	{
		data, _ := base64.RawURLEncoding.DecodeString(strings.Split(cursor, ".")[0])
		after = ps(pm(string(data)), "after")
	}
	late := ""
	for i := 0; i < 1000; i++ {
		id := fmt.Sprintf("late_%d", i)
		if pipeStable("event", []string{a.Config.NamespaceID, "gw_test", id}) > after {
			late = id
			break
		}
	}
	if late == "" {
		t.Fatal("could not generate later test identity")
	}
	_, err = a.AcceptIngress(context.Background(), a.Config.NamespaceID, "gw_test", testIngress(a, late))
	if err != nil {
		t.Fatal(err)
	}
	w = pipelineRequest(t, a, actor, "GET", "/api/v1/events?limit=200&cursor="+url.QueryEscape(cursor), pipeMap{}, "")
	if w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	next := pm(w.Body.String())
	items := pl(next, "items")
	if len(items) != 1 || ps(items[0].(map[string]any), "eventId") == late || ps(next, "snapshotAt") != ps(page, "snapshotAt") {
		t.Fatal("new arrival changed cursor membership", w.Body.String())
	}
	w = pipelineRequest(t, a, actor, "GET", "/api/v1/events?gatewayId=gw_test&cursor="+url.QueryEscape(cursor), pipeMap{}, "")
	if w.Code != 400 {
		t.Fatal("cursor crossed query", w.Code)
	}
	w = pipelineRequest(t, a, actor, "GET", "/api/v1/events?cursor="+url.QueryEscape(cursor+"x"), pipeMap{}, "")
	if w.Code != 400 {
		t.Fatal("cursor signature not checked", w.Code)
	}
}

func TestPipelineTargetMappingIsFrozenAndFailureQuarantines(t *testing.T) {
	for _, source := range []string{"temperature", "missing_field"} {
		t.Run(source, func(t *testing.T) {
			a, _ := pipelineFixture(t)
			ctx := context.Background()
			installPipelineTestBundle(t, a, "bundle_before_mapping", "http://127.0.0.1:9999/inbox", 1)
			var raw string
			_ = a.Pipeline.QueryRow("SELECT manifest_json FROM pipeline_bundles WHERE bundle_id='bundle_before_mapping'").Scan(&raw)
			m := pm(raw)
			config := po(po(m, "targets"), "target_test@1")
			config["mappingVersion"] = "fahrenheit-v1"
			config["mapping"] = pipeMap{"fields": []any{pipeMap{"source": source, "target": "temperatureF", "scale": 1.8, "offset": 32, "unit": "degF"}}}
			rule := pl(m, "rules")[0].(map[string]any)
			pl(rule, "targetVersions")[0].(map[string]any)["mappingVersion"] = "fahrenheit-v1"
			if err := a.activatePipelineBundle(ctx, "bundle_mapping", pipeJSON(m), pipeHash(m), 2); err != nil {
				t.Fatal(err)
			}
			_, _ = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_map"))
			if _, err := a.ProcessOne(ctx); err != nil {
				t.Fatal(err)
			}
			if source == "missing_field" {
				var state, code string
				var n int
				_ = a.Pipeline.QueryRow("SELECT state,error_code FROM processing_jobs").Scan(&state, &code)
				_ = a.Pipeline.QueryRow("SELECT count(*) FROM processing_results").Scan(&n)
				if state != "quarantined" || code != "MAPPING_FAILED" || n != 0 {
					t.Fatal(state, code, n)
				}
				if work, err := a.ProcessOne(ctx); work || err != nil {
					t.Fatal("deterministic mapping failure retried", work, err)
				}
				return
			}
			var payload []byte
			if err := a.Pipeline.QueryRow("SELECT wire_payload FROM delivery_outbox").Scan(&payload); err != nil {
				t.Fatal(err)
			}
			wire := pm(string(payload))
			if err := a.Validate("CustomerEvent", wire); err != nil {
				t.Fatal(err)
			}
			data := po(wire, "data")
			if data["temperatureF"] != float64(77) || ps(data, "temperatureFUnit") != "degF" || ps(wire, "mappingVersion") != "fahrenheit-v1" {
				t.Fatal(wire)
			}
		})
	}
}

func TestPipelineReprocessRateGateSurvivesOverdueQueue(t *testing.T) {
	a, actor := pipelineFixture(t)
	ctx := context.Background()
	installPipelineTestBundle(t, a, "bundle_rate", "http://127.0.0.1:9999/inbox", 1)
	if _, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_rate_a", "evt_rate_b")); err != nil {
		t.Fatal(err)
	}
	_, err := a.Pipeline.Exec("INSERT INTO reprocess_jobs(reprocess_id,bundle_id,requested_by,idempotency_key,request_hash,selection_json,cursor_json,reason,state,created_at_ms) VALUES('rate_job','bundle_rate',?,'rate_key','hash','{}','{\"maxEventsPerSecond\":1}','test','pending',?)", actor, pnow())
	if err != nil {
		t.Fatal(err)
	}
	_, _ = a.Pipeline.Exec("UPDATE processing_jobs SET reprocess_id='rate_job',next_attempt_at_ms=0")
	if worked, err := a.ProcessOne(ctx); !worked || err != nil {
		t.Fatal(worked, err)
	}
	if worked, err := a.ProcessOne(ctx); worked || err != nil {
		t.Fatal("overdue replay bypassed persisted rate gate", worked, err)
	}
	_, _ = a.Pipeline.Exec("UPDATE reprocess_jobs SET cursor_json=json_set(cursor_json,'$.nextReleaseAtMs',0)")
	if worked, err := a.ProcessOne(ctx); !worked || err != nil {
		t.Fatal(worked, err)
	}
}

func TestPipelineDiskReserveAndMetadataBudget(t *testing.T) {
	if code := pipelineStorageCheck(pipelineDiskStats{Stored: 100, Available: pipelineDiskReserve + 1024}, 2048, 1<<30); code != "DISK_RESERVE" {
		t.Fatal(code)
	}
	if code := pipelineStorageCheck(pipelineDiskStats{Stored: 2000, Available: 1 << 30}, 2000, 3000); code != "QUEUE_FULL" {
		t.Fatal(code)
	}
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	batch := testIngress(a, "evt_disk")
	if _, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch); err != nil {
		t.Fatal(err)
	}
	stats, err := a.pipelineStorageStats()
	if err != nil || stats.Stored <= 18 {
		t.Fatal(stats, err)
	}
	a.Config.MaxQueueBytes = stats.Stored + 10
	ack, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_disk_new"))
	if err != nil {
		t.Fatal(err)
	}
	item := ack["items"].([]pipeMap)[0]
	if ps(item, "status") != "REJECTED_RETRYABLE" || ps(item, "code") != "QUEUE_FULL" {
		t.Fatal(ack)
	}
	ack, err = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch)
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "DUPLICATE" {
		t.Fatal("disk-full duplicate should retain original acceptance", ack, err)
	}
	if _, err = a.ProcessOne(ctx); err != nil {
		t.Fatal(err)
	}
}

func TestPipelineRestartRetainsCustodyAndFrozenOutput(t *testing.T) {
	a, _ := pipelineFixture(t)
	ctx := context.Background()
	installPipelineTestBundle(t, a, "bundle_restart", "http://127.0.0.1:9999/inbox", 1)
	batch := testIngress(a, "evt_restart")
	if _, err := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", batch); err != nil {
		t.Fatal(err)
	}
	config := a.Config
	if err := a.Close(); err != nil {
		t.Fatal(err)
	}
	restarted, err := New(config)
	if err != nil {
		t.Fatal(err)
	}
	defer restarted.Close()
	ack, err := restarted.AcceptIngress(ctx, config.NamespaceID, "gw_test", batch)
	if err != nil || ps(ack["items"].([]pipeMap)[0], "status") != "DUPLICATE" {
		t.Fatal(ack, err)
	}
	if worked, err := restarted.ProcessOne(ctx); !worked || err != nil {
		t.Fatal(worked, err)
	}
	claimed, err := restarted.claimDeliveryBatch(ctx)
	if err != nil || len(claimed.Items) != 1 {
		t.Fatal(claimed, err)
	}
	if err = restarted.Close(); err != nil {
		t.Fatal(err)
	}
	second, err := New(config)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	_, _ = second.Pipeline.Exec("UPDATE delivery_outbox SET lease_until_ms=0")
	retry, err := second.claimDeliveryBatch(ctx)
	if err != nil || len(retry.Items) != 1 {
		t.Fatal(retry, err)
	}
	if retry.Items[0].ID != claimed.Items[0].ID || !bytes.Equal(retry.Items[0].Payload, claimed.Items[0].Payload) {
		t.Fatal("restart changed customer identity or payload")
	}
	if err = second.AcceptCustomerAck(ctx, retry.Target, testAck(retry, retry.Items...)); err != nil {
		t.Fatal(err)
	}
}
func TestPipelineRestoredRevisionHighwater(t *testing.T) {
	a, actor := pipelineFixture(t)
	ctx := context.Background()
	_, _ = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_restore"))
	var bundle string
	_ = a.Pipeline.QueryRow("SELECT bundle_id FROM pipeline_bundles WHERE state='active'").Scan(&bundle)
	selection := pipeMap{"eventKeys": []any{pipeStable("event", []string{a.Config.NamespaceID, "gw_test", "evt_restore"})}, "selector": pipeMap{"gatewayIds": []any{"gw_test"}}, "expiresAtMs": pnow() + 60000, "selectionDigest": pipeHash("selected")}
	_, err := a.Pipeline.Exec("INSERT INTO reprocess_jobs(reprocess_id,bundle_id,requested_by,idempotency_key,request_hash,selection_json,reason,state,created_at_ms) VALUES('preview_test',?,?,'preview_test','hash',?,'preview','preview',?)", bundle, actor, pipeJSON(selection), pnow())
	if err != nil {
		t.Fatal(err)
	}
	body := pipeMap{"previewId": "preview_test", "selectionDigest": selection["selectionDigest"], "reason": "restore reconciliation", "maxEventsPerSecond": 10, "correctionMode": "UPSERT_RECORD"}
	marker := filepath.Join(a.Config.DataDir, "restore-reconciliation.required")
	if err = os.WriteFile(marker, []byte("required"), 0600); err != nil {
		t.Fatal(err)
	}
	w := pipelineRequest(t, a, actor, "POST", "/api/v1/reprocess-jobs", body, "restore-key")
	if w.Code != 409 {
		t.Fatal(w.Code, w.Body.String())
	}
	_ = os.Remove(marker)
	floor := pipeMap{"namespaceId": a.Config.NamespaceID, "complete": true, "revisions": []pipeMap{{"gatewayId": "gw_test", "eventId": "evt_restore", "processingRevision": 9}}}
	if err = os.WriteFile(filepath.Join(a.Config.DataDir, "revision-highwater.json"), []byte(pipeJSON(floor)), 0600); err != nil {
		t.Fatal(err)
	}
	w = pipelineRequest(t, a, actor, "POST", "/api/v1/reprocess-jobs", body, "restore-key")
	if w.Code != 202 {
		t.Fatal(w.Code, w.Body.String())
	}
	var rev int64
	if err = a.Pipeline.QueryRow("SELECT latest_revision FROM event_receipts WHERE event_id='evt_restore'").Scan(&rev); err != nil || rev != 10 {
		t.Fatal("revision regression", rev, err)
	}
}
