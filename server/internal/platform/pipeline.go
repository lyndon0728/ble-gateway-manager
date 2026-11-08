package platform

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
)

type pipeMap = map[string]any

func pipeID(prefix string) string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return prefix + "_" + hex.EncodeToString(b)
}
func pipeJSON(v any) string {
	b, e := json.Marshal(v)
	if e != nil {
		panic(e)
	}
	return string(b)
}
func pipeHash(v any) string                  { h := sha256.Sum256([]byte(pipeJSON(v))); return hex.EncodeToString(h[:]) }
func pipeStable(prefix string, v any) string { return prefix + "_" + pipeHash(v)[:40] }
func ps(m pipeMap, k string) string          { v, _ := m[k].(string); return v }
func po(m pipeMap, k string) pipeMap {
	v, _ := m[k].(map[string]any)
	if v == nil {
		return pipeMap{}
	}
	return v
}
func pl(m pipeMap, k string) []any {
	v, _ := m[k].([]any)
	if v == nil {
		return []any{}
	}
	return v
}
func pn(m pipeMap, k string) int64 {
	switch v := m[k].(type) {
	case json.Number:
		n, _ := v.Int64()
		return n
	case float64:
		return int64(v)
	case int:
		return int64(v)
	case int64:
		return v
	}
	return 0
}
func pb(m pipeMap, k string) bool { v, _ := m[k].(bool); return v }
func pm(s string) pipeMap         { m := pipeMap{}; _ = json.Unmarshal([]byte(s), &m); return m }
func pts(ms int64) string         { return time.UnixMilli(ms).UTC().Format(time.RFC3339Nano) }
func pnow() int64                 { return time.Now().UnixMilli() }
func pjson(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func pfail(w http.ResponseWriter, r *http.Request, status int, code, msg string) {
	pjson(w, status, pipeMap{"error": pipeMap{"code": code, "message": msg, "retryable": status == 429 || status == 503, "traceId": pipeID("trace")}})
}
func pcontains(v any, s string) bool {
	a, _ := v.([]any)
	if len(a) == 0 {
		return true
	}
	for _, x := range a {
		if x == s {
			return true
		}
	}
	return false
}

func (a *App) pipeNamespace(ctx context.Context) (string, error) { return a.Config.NamespaceID, nil }
func (a *App) pipeGlobal(w http.ResponseWriter, r *http.Request, perm string) (*Principal, bool) {
	p, ok := a.Require(w, r, perm)
	if !ok {
		return nil, false
	}
	var scope string
	if a.Management.QueryRowContext(r.Context(), "SELECT scope_id FROM resource_scopes WHERE kind='instance' LIMIT 1").Scan(&scope) != nil || !a.Can(p, perm, scope) {
		pfail(w, r, 403, "FORBIDDEN", "instance-scoped permission required")
		return nil, false
	}
	return p, true
}
func pipeAudit(ctx context.Context, tx *sql.Tx, actor, action, id string) error {
	_, err := tx.ExecContext(ctx, "INSERT INTO audit_events(audit_id,actor_id,action,object_type,object_id,outcome,correlation_id,detail_json,occurred_at_ms,retention_until_ms) VALUES(?,?,?,?,?,'succeeded',?,'{}',?,?)", pipeID("audit"), actor, action, "pipeline", id, pipeID("correlation"), pnow(), pnow()+90*86400000)
	return err
}

// pipeMutation keeps management mutations, their audit and replay response atomic.
func (a *App) pipeMutation(w http.ResponseWriter, r *http.Request, p *Principal, operation string, request pipeMap, fn func(*sql.Tx) (int, pipeMap, error)) {
	key := r.Header.Get("Idempotency-Key")
	if len(key) < 1 || len(key) > 128 {
		pfail(w, r, 400, "IDEMPOTENCY_REQUIRED", "Idempotency-Key of 1..128 bytes required")
		return
	}
	tx, err := a.Management.BeginTx(r.Context(), nil)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "management store unavailable")
		return
	}
	defer tx.Rollback()
	var hash, resp, state string
	var status int
	err = tx.QueryRowContext(r.Context(), "SELECT request_hash,COALESCE(response_json,''),COALESCE(http_status,0),state FROM api_idempotency WHERE principal_id=? AND operation=? AND idempotency_key=?", p.ID, operation, key).Scan(&hash, &resp, &status, &state)
	if err == nil {
		if hash != pipeHash(request) {
			pfail(w, r, 409, "IDEMPOTENCY_CONFLICT", "same key used for different request")
			return
		}
		if state != "committed" {
			pfail(w, r, 409, "OPERATION_PENDING", "operation is still being reconciled")
			return
		}
		pjson(w, status, pm(resp))
		return
	}
	if !errors.Is(err, sql.ErrNoRows) {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "cannot read idempotency record")
		return
	}
	status, result, err := fn(tx)
	if err != nil {
		pfail(w, r, 422, "INVALID_OPERATION", err.Error())
		return
	}
	id := ps(result, "bundleId")
	if id == "" {
		id = ps(result, "targetId")
	}
	if id == "" {
		id = pipeID("resource")
	}
	opID := ps(result, "operationId")
	if opID == "" {
		opID = pipeID("op")
	}
	_, err = tx.ExecContext(r.Context(), "INSERT INTO api_idempotency(operation_id,principal_id,operation,idempotency_key,request_hash,state,resource_type,resource_id,http_status,response_json,created_at_ms,expires_at_ms) VALUES(?,?,?,?,?,'committed','pipeline',?,?,?,?,?)", opID, p.ID, operation, key, pipeHash(request), id, status, pipeJSON(result), pnow(), pnow()+7*86400000)
	if err == nil {
		err = pipeAudit(r.Context(), tx, p.ID, operation, id)
	}
	if err == nil {
		err = tx.Commit()
	}
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "mutation not committed")
		return
	}
	pjson(w, status, result)
}

func (a *App) RegisterPipeline(r chi.Router) {
	r.Get("/api/v1/parsers", a.pipelineParsers)
	r.Post("/api/v1/parsers/{parserId}/versions/{parserVersion}/tests", a.pipelineParserTest)
	r.Post("/api/v1/secrets", a.pipelineSecret)
	r.Get("/api/v1/targets", a.pipelineTargets)
	r.Post("/api/v1/targets", a.pipelineTargetCreate)
	r.Get("/api/v1/targets/{targetId}", a.pipelineTargetGet)
	r.Post("/api/v1/targets/{targetId}/versions", a.pipelineTargetVersion)
	r.Get("/api/v1/rule-bundles", a.pipelineBundles)
	r.Post("/api/v1/rule-bundles", a.pipelineBundleCreate)
	r.Get("/api/v1/rule-bundles/{bundleId}", a.pipelineBundleGet)
	r.Post("/api/v1/rule-bundles/{bundleId}/validations", a.pipelineBundleValidate)
	r.Post("/api/v1/rule-bundles/{bundleId}/activations", a.pipelineBundleActivate)
	r.Post("/api/v1/gateway-ingress/batches", a.pipelineIngress)
	r.Get("/api/v1/events", a.pipelineEvents)
	r.Get("/api/v1/gateways/{gatewayId}/events/{eventId}", a.pipelineEvent)
	r.Get("/api/v1/quarantine", a.pipelineQuarantine)
	r.Post("/api/v1/reprocess-previews", a.pipelinePreview)
	r.Post("/api/v1/reprocess-jobs", a.pipelineReprocess)
	r.Get("/api/v1/reprocess-jobs/{jobId}", a.pipelineReprocessGet)
	r.Get("/api/v1/deliveries", a.pipelineDeliveries)
	r.Get("/api/v1/deliveries/{deliveryId}", a.pipelineDelivery)
	r.Post("/api/v1/deliveries/{deliveryId}/retries", a.pipelineDeliveryRetry)
}

func (a *App) ensurePipelineDefault(ctx context.Context) error {
	ns, err := a.pipeNamespace(ctx)
	if err != nil {
		return err
	}
	var n int
	if err = a.Pipeline.QueryRowContext(ctx, "SELECT COUNT(*) FROM pipeline_bundles WHERE namespace_id=?", ns).Scan(&n); err != nil {
		return err
	}
	if n > 0 {
		return nil
	}
	manifest := pipeMap{"name": "Initial quarantine policy", "rules": []any{}, "defaultAction": "QUARANTINE", "targets": pipeMap{}}
	_, err = a.Pipeline.ExecContext(ctx, "INSERT OR IGNORE INTO pipeline_bundles(bundle_id,namespace_id,activation_epoch,content_hash,manifest_json,state,stored_at_ms,activated_at_ms) VALUES(?,?,1,?,?,'active',?,?)", pipeStable("bundle", ns), ns, pipeHash(manifest), pipeJSON(manifest), pnow(), pnow())
	return err
}

func (a *App) StartPipeline(ctx context.Context) {
	if err := a.ensurePipelineDefault(ctx); err != nil {
		a.Logger.Error("pipeline initialization failed", "error", err)
		return
	}
	var wg sync.WaitGroup
	wg.Add(5)
	go func() {
		defer wg.Done()
		t := time.NewTicker(a.Config.WorkerInterval)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				for i := 0; i < 50; i++ {
					worked, err := a.ProcessOne(ctx)
					if err != nil {
						if ctx.Err() == nil {
							a.Logger.Error("processing worker", "error", err)
						}
						break
					}
					if !worked {
						break
					}
				}
			}
		}
	}()
	for worker := 0; worker < 4; worker++ {
		go func() {
			defer wg.Done()
			t := time.NewTicker(a.Config.WorkerInterval)
			defer t.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-t.C:
					if _, err := a.DeliverOnce(ctx); err != nil && ctx.Err() == nil {
						a.Logger.Warn("delivery worker", "error", err)
					}
				}
			}
		}()
	}
	t := time.NewTicker(time.Minute)
	defer t.Stop()
	defer func() { wg.Wait(); a.closeDeliveryClients() }()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if err := a.ReclaimPipelinePayloads(ctx); err != nil {
				a.Logger.Warn("pipeline retention", "error", err)
			}
		}
	}
}

func (a *App) pipelineIngress(w http.ResponseWriter, r *http.Request) {
	batch, ok := a.Decode(w, r, "IngressBatch")
	if !ok {
		return
	}
	if !a.RequireGateway(w, r, ps(batch, "namespaceId"), ps(batch, "gatewayId")) {
		return
	}
	ack, err := a.AcceptIngress(r.Context(), ps(batch, "namespaceId"), ps(batch, "gatewayId"), batch)
	if err != nil {
		var ingressErr *PipelineIngressError
		if errors.As(err, &ingressErr) {
			pfail(w, r, ingressErr.Status, ingressErr.Code, ingressErr.Detail)
			return
		}
		pfail(w, r, 503, "INGRESS_UNAVAILABLE", err.Error())
		return
	}
	pjson(w, 200, ack)
}

// AcceptIngress transfers custody only after the atomic original-event transaction.
// MQTT callers must authenticate the topic identity before calling this method.
func (a *App) AcceptIngress(ctx context.Context, namespace, gateway string, batch pipeMap) (pipeMap, error) {
	if err := a.Validate("IngressBatch", batch); err != nil {
		return nil, &PipelineIngressError{400, "INGRESS_INVALID", "invalid ingress envelope"}
	}
	if ps(batch, "namespaceId") != namespace || ps(batch, "gatewayId") != gateway {
		return nil, errors.New("authenticated identity mismatch")
	}
	ns, err := a.pipeNamespace(ctx)
	if err != nil || ns != namespace {
		return nil, errors.New("unknown namespace")
	}
	var lifecycle, model, group, capabilities string
	if err = a.Management.QueryRowContext(ctx, "SELECT lifecycle,model,scope_id,capabilities_json FROM gateways WHERE gateway_id=?", gateway).Scan(&lifecycle, &model, &group, &capabilities); err != nil || lifecycle != "active" {
		return nil, errors.New("gateway inactive or unknown")
	}
	version := ps(batch, "schemaVersion")
	if version == "1.1" && !captureHasCapability(capabilities, CaptureContextCapability) {
		return nil, &PipelineIngressError{403, "CAPTURE_CAPABILITY_REQUIRED", "gateway signed capabilities do not include GW32_CAPTURE_CONTEXT_1"}
	}
	events := pl(batch, "events")
	if len(events) < 1 || len(events) > 200 {
		return nil, errors.New("batch requires 1..200 events")
	}
	decoded := make([][]byte, len(events))
	seen := map[string]bool{}
	total := 0
	for i, v := range events {
		e, ok := v.(map[string]any)
		if !ok || ps(e, "eventId") == "" || seen[ps(e, "eventId")] {
			return nil, errors.New("invalid or duplicate event identity within batch")
		}
		seen[ps(e, "eventId")] = true
		decoded[i], err = base64.StdEncoding.Strict().DecodeString(ps(e, "payloadBase64"))
		if err != nil || len(decoded[i]) > 65536 {
			return nil, errors.New("invalid or oversized base64 payload")
		}
		if err := ValidateCaptureEvent(version, e, decoded[i]); err != nil {
			return nil, err
		}
		total += len(decoded[i])
		if ps(e, "capturedAt") != "" {
			if _, err = time.Parse(time.RFC3339Nano, ps(e, "capturedAt")); err != nil {
				return nil, errors.New("invalid capturedAt")
			}
		}
	}
	if total > 262144 {
		return nil, errors.New("batch raw bytes exceed 256 KiB")
	}
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	var bundle string
	if err = tx.QueryRowContext(ctx, "SELECT bundle_id FROM pipeline_bundles WHERE namespace_id=? AND state='active'", ns).Scan(&bundle); err != nil {
		return nil, errors.New("no active processing policy")
	}
	var used int64
	if err = tx.QueryRowContext(ctx, "SELECT COALESCE((SELECT SUM(payload_bytes) FROM raw_events),0)+COALESCE((SELECT SUM(length(wire_payload)) FROM delivery_outbox),0)").Scan(&used); err != nil {
		return nil, err
	}
	limit := a.Config.MaxQueueBytes
	disk, diskErr := a.pipelineStorageStats()
	reservedBytes := int64(65536)
	items := []pipeMap{}
	now := pnow()
	protocolContexts := map[string]pipeMap{}
	for i, v := range events {
		e := v.(map[string]any)
		eventID := ps(e, "eventId")
		hash := pipeHash(e)
		if version == "1.1" {
			hash = pipeHash(pipeMap{"schemaVersion": version, "event": e})
		}
		key := pipeStable("event", []string{ns, gateway, eventID})
		var old string
		err = tx.QueryRowContext(ctx, "SELECT immutable_hash FROM event_receipts WHERE event_key=?", key).Scan(&old)
		if err == nil {
			if old == hash {
				items = append(items, pipeMap{"eventId": eventID, "status": "DUPLICATE"})
			} else {
				items = append(items, pipeMap{"eventId": eventID, "status": "REJECTED_PERMANENT", "code": "CONTENT_CONFLICT"})
			}
			continue
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return nil, err
		}
		meta := pipeMap{"rawEvent": e, "modelId": model, "groupId": group, "ingressSchemaVersion": version}
		captureIdentity := po(e, "capture")
		protocolKey := pipeHash(pipeMap{"kind": captureIdentity["kind"], "radio": captureIdentity["radio"], "connectionRef": captureIdentity["connectionRef"], "characteristicHandle": captureIdentity["characteristicHandle"], "addressType": captureIdentity["addressType"], "address": captureIdentity["address"]})
		protocolContext, cached := protocolContexts[protocolKey]
		if !cached {
			var contextErr error
			protocolContext, contextErr = a.freezeCaptureProtocolContext(ctx, gateway, e)
			if contextErr != nil {
				return nil, contextErr
			}
			protocolContexts[protocolKey] = protocolContext
		}
		estimatedBytes := int64(2*(len(decoded[i])+len(pipeJSON(meta))+len(pipeJSON(protocolContext))) + 4096)
		capacityCode := pipelineStorageCheck(disk, reservedBytes+estimatedBytes, limit)
		if used+int64(len(decoded[i])) > limit {
			capacityCode = "QUEUE_FULL"
		}
		if diskErr != nil {
			capacityCode = "DISK_STATUS_UNAVAILABLE"
		}
		if capacityCode != "" {
			items = append(items, pipeMap{"eventId": eventID, "status": "REJECTED_RETRYABLE", "code": capacityCode, "retryAfterMs": 1000})
			continue
		}
		_, err = tx.ExecContext(ctx, "INSERT INTO event_receipts(event_key,namespace_id,gateway_id,event_id,immutable_hash,bundle_id,accepted_at_ms,custody_state,dedup_until_ms,raw_retention_until_ms) VALUES(?,?,?,?,?,?,?,'accepted_raw',?,?)", key, ns, gateway, eventID, hash, bundle, now, now+a.Config.DedupRetention.Milliseconds(), now+a.Config.RawRetention.Milliseconds())
		if err != nil {
			return nil, err
		}
		var captured any
		if ps(e, "capturedAt") != "" {
			t, _ := time.Parse(time.RFC3339Nano, ps(e, "capturedAt"))
			captured = t.UnixMilli()
		}
		tq := map[string]string{"SYNCED": "trusted", "ESTIMATED": "estimated", "UNSYNCED": "unknown"}[ps(e, "timeQuality")]
		if tq == "" {
			return nil, errors.New("invalid time quality")
		}
		_, err = tx.ExecContext(ctx, "INSERT INTO raw_events(event_key,payload,payload_bytes,source_device_id,captured_at_ms,time_quality,protocol_hint,metadata_json) VALUES(?,?,?,?,?,?,?,?)", key, decoded[i], len(decoded[i]), ps(e, "sourceDeviceId"), captured, tq, ps(e, "protocolHint"), pipeJSON(meta))
		if err != nil {
			return nil, err
		}
		_, err = tx.ExecContext(ctx, "INSERT INTO processing_jobs(processing_id,event_key,revision,bundle_id,state,next_attempt_at_ms,created_at_ms,protocol_context_json) VALUES(?,?,1,?,'pending',?,?,?)", pipeStable("processing", []any{key, 1}), key, bundle, now, now, pipeJSON(protocolContext))
		if err != nil {
			return nil, err
		}
		used += int64(len(decoded[i]))
		reservedBytes += estimatedBytes
		items = append(items, pipeMap{"eventId": eventID, "status": "ACCEPTED"})
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	return pipeMap{"schemaVersion": version, "namespaceId": ns, "gatewayId": gateway, "batchId": ps(batch, "batchId"), "receiverId": a.Config.InstanceID, "durability": "RAW_PERSISTED", "items": items, "traceId": pipeID("trace")}, nil
}

type pipeJob struct {
	ID, Key, Bundle, Namespace, Gateway, Event, Meta, Manifest, ProtocolContext string
	Revision, Lease, Accepted                                                   int64
	Raw                                                                         []byte
}

func (a *App) ProcessOne(ctx context.Context) (bool, error) {
	now := pnow()
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	var j pipeJob
	err = tx.QueryRowContext(ctx, `SELECT j.processing_id,j.event_key,j.bundle_id,j.revision,j.lease_token,e.namespace_id,e.gateway_id,e.event_id,e.accepted_at_ms,r.payload,r.metadata_json,b.manifest_json,COALESCE(j.protocol_context_json,'{}') FROM processing_jobs j JOIN event_receipts e ON e.event_key=j.event_key JOIN raw_events r ON r.event_key=j.event_key JOIN pipeline_bundles b ON b.bundle_id=j.bundle_id WHERE ((j.state IN ('pending','retry_wait') AND j.next_attempt_at_ms<=?) OR (j.state='leased' AND j.lease_until_ms<?)) AND (j.reprocess_id IS NULL OR EXISTS(SELECT 1 FROM reprocess_jobs rp WHERE rp.reprocess_id=j.reprocess_id AND rp.state IN ('pending','running') AND COALESCE(json_extract(rp.cursor_json,'$.nextReleaseAtMs'),0)<=?)) ORDER BY j.created_at_ms,j.processing_id LIMIT 1`, now, now, now).Scan(&j.ID, &j.Key, &j.Bundle, &j.Revision, &j.Lease, &j.Namespace, &j.Gateway, &j.Event, &j.Accepted, &j.Raw, &j.Meta, &j.Manifest, &j.ProtocolContext)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	_, err = tx.ExecContext(ctx, "UPDATE reprocess_jobs SET state='running',cursor_json=json_set(cursor_json,'$.nextReleaseAtMs',?+CAST((999+COALESCE(json_extract(cursor_json,'$.maxEventsPerSecond'),1))/COALESCE(json_extract(cursor_json,'$.maxEventsPerSecond'),1) AS INTEGER)) WHERE reprocess_id=(SELECT reprocess_id FROM processing_jobs WHERE processing_id=?)", now, j.ID)
	if err != nil {
		return false, err
	}
	j.Lease++
	_, err = tx.ExecContext(ctx, "UPDATE processing_jobs SET state='leased',lease_owner=?,lease_token=?,lease_until_ms=?,attempts=attempts+1 WHERE processing_id=?", "pipeline", j.Lease, now+30000, j.ID)
	if err != nil {
		return false, err
	}
	if err = tx.Commit(); err != nil {
		return false, err
	}
	rule, records, parseErr := evaluatePipeline(j)
	tx, err = a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return true, err
	}
	defer tx.Rollback()
	var epoch int64
	var state string
	if err = tx.QueryRowContext(ctx, "SELECT lease_token,state FROM processing_jobs WHERE processing_id=?", j.ID).Scan(&epoch, &state); err != nil {
		return true, err
	}
	if epoch != j.Lease || state != "leased" {
		return true, nil
	}
	if ps(po(po(pm(j.Meta), "rawEvent"), "capture"), "kind") == "GATT_LINK_DOWN" {
		_, err = tx.ExecContext(ctx, "UPDATE processing_jobs SET state='filtered',mode=NULL,plan_json=?,output_count=0,error_code=NULL,completed_at_ms=?,lease_owner=NULL,lease_until_ms=NULL WHERE processing_id=? AND lease_token=?", pipeJSON(pipeMap{"consumer": "capture_control", "kind": "GATT_LINK_DOWN"}), pnow(), j.ID, j.Lease)
		if err == nil {
			_, err = tx.ExecContext(ctx, "UPDATE event_receipts SET custody_state='processed' WHERE event_key=? AND latest_revision=?", j.Key, j.Revision)
		}
		if err == nil {
			err = tx.Commit()
		}
		return true, err
	}
	if parseErr != nil {
		code := "PARSER_ERROR"
		var pe *ParserFailure
		if errors.As(parseErr, &pe) {
			code = pe.Code
		}
		_, err = tx.ExecContext(ctx, "UPDATE processing_jobs SET state='quarantined',mode='quarantine',error_code=?,error_detail_json=?,completed_at_ms=?,lease_owner=NULL,lease_until_ms=NULL WHERE processing_id=? AND lease_token=?", code, pipeJSON(pipeMap{"message": parseErr.Error()}), pnow(), j.ID, j.Lease)
		if err == nil {
			_, err = tx.ExecContext(ctx, "UPDATE event_receipts SET custody_state='quarantined' WHERE event_key=? AND latest_revision=?", j.Key, j.Revision)
		}
		if err == nil {
			err = tx.Commit()
		}
		return true, err
	}
	targets := po(pm(j.Manifest), "targets")
	meta := pm(j.Meta)
	rawEvent := po(meta, "rawEvent")
	replacements := map[string][]string{}
	if j.Revision > 1 {
		var mode string
		_ = tx.QueryRowContext(ctx, "SELECT COALESCE(json_extract(r.cursor_json,'$.correctionMode'),'UPSERT_RECORD') FROM reprocess_jobs r JOIN processing_jobs j ON j.reprocess_id=r.reprocess_id WHERE j.processing_id=?", j.ID).Scan(&mode)
		rows, e := tx.QueryContext(ctx, "SELECT r.output_id,r.record_key FROM processing_results r JOIN processing_jobs p ON p.processing_id=r.processing_id WHERE p.event_key=? AND p.revision=? ORDER BY r.record_key", j.Key, j.Revision-1)
		if e != nil {
			return true, e
		}
		for rows.Next() {
			var out, key string
			if e = rows.Scan(&out, &key); e != nil {
				rows.Close()
				return true, e
			}
			if mode == "REPLACE_EVENT_SET" {
				key = "*"
			}
			replacements[key] = append(replacements[key], out)
		}
		rows.Close()
	}
	// Compute actual frozen wire sizes before reserving all outputs atomically.
	increments := map[string]int64{}
	wireByID := map[string]pipeMap{}
	for _, rec := range records {
		for _, tv := range pl(rule, "targetVersions") {
			t := tv.(map[string]any)
			replaced := replacements[rec.Key]
			if len(replacements["*"]) > 0 {
				replaced = replacements["*"]
			}
			mappedRecord := rec
			if ps(rule, "action") == "DECODE" {
				target := po(targets, ps(t, "targetId")+"@"+ps(t, "version"))
				mapped, mapErr := MapOutputData(pm(pipeJSON(rec.Data)), target)
				if mapErr != nil {
					_, err = tx.ExecContext(ctx, "UPDATE processing_jobs SET state='quarantined',mode='quarantine',error_code='MAPPING_FAILED',error_detail_json=?,completed_at_ms=?,lease_owner=NULL,lease_until_ms=NULL WHERE processing_id=? AND lease_token=?", pipeJSON(pipeMap{"message": mapErr.Error(), "targetId": ps(t, "targetId")}), pnow(), j.ID, j.Lease)
					if err == nil {
						_, err = tx.ExecContext(ctx, "UPDATE event_receipts SET custody_state='quarantined' WHERE event_key=? AND latest_revision=?", j.Key, j.Revision)
					}
					if err == nil {
						err = tx.Commit()
					}
					return true, err
				}
				mappedRecord.Data = mapped
			}
			wire := pipelineWire(j, rule, mappedRecord, t, rawEvent, replaced)
			wireByID[ps(wire, "deliveryId")] = wire
			increments[ps(t, "targetId")] += int64(len(pipeJSON(wire)))
		}
	}
	quotaError := ""
	var used int64
	if err = tx.QueryRowContext(ctx, "SELECT COALESCE((SELECT SUM(payload_bytes) FROM raw_events),0)+COALESCE((SELECT SUM(length(wire_payload)) FROM delivery_outbox),0)").Scan(&used); err != nil {
		return true, err
	}
	totalNew := int64(0)
	for _, n := range increments {
		totalNew += n
	}
	if used+totalNew > a.Config.MaxQueueBytes {
		quotaError = "QUEUE_FULL"
	}
	disk, diskErr := a.pipelineStorageStats()
	if diskErr != nil {
		quotaError = "DISK_STATUS_UNAVAILABLE"
	} else if code := pipelineStorageCheck(disk, 2*totalNew+int64(len(records))*4096+int64(len(wireByID))*4096+32768, a.Config.MaxQueueBytes); code != "" {
		quotaError = code
	}
	for _, tv := range pl(rule, "targetVersions") {
		t := tv.(map[string]any)
		tc := po(targets, ps(t, "targetId")+"@"+ps(t, "version"))
		var queued int64
		if err = tx.QueryRowContext(ctx, "SELECT COALESCE(SUM(length(wire_payload)),0) FROM delivery_outbox WHERE target_id=? AND state NOT IN ('delivered','terminated')", ps(t, "targetId")).Scan(&queued); err != nil {
			return true, err
		}
		if queued+increments[ps(t, "targetId")] > pn(tc, "maxQueueBytes") {
			quotaError = "TARGET_QUEUE_FULL"
		}
	}
	if quotaError != "" {
		_, err = tx.ExecContext(ctx, "UPDATE processing_jobs SET state='retry_wait',error_code=?,next_attempt_at_ms=?,lease_owner=NULL,lease_until_ms=NULL WHERE processing_id=? AND lease_token=?", quotaError, pnow()+1000, j.ID, j.Lease)
		if err == nil {
			err = tx.Commit()
		}
		return true, err
	}
	for _, rec := range records {
		outputID := pipeStable("output", []any{j.Key, j.Revision, rec.Key})
		result := pipeMap{"mode": ps(rule, "action"), "data": rec.Data, "quality": rec.Quality}
		_, err = tx.ExecContext(ctx, "INSERT INTO processing_results(output_id,processing_id,record_key,parser_id,parser_version,schema_version,result_json,content_hash,quality_json,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?)", outputID, j.ID, rec.Key, ps(rule, "parserId"), ps(rule, "parserVersion"), ps(rule, "schemaVersion"), pipeJSON(result), pipeHash(result), pipeJSON(rec.Quality), pnow())
		if err != nil {
			return true, err
		}
		for _, tv := range pl(rule, "targetVersions") {
			t := tv.(map[string]any)
			target := po(targets, ps(t, "targetId")+"@"+ps(t, "version"))
			deliveryID := pipeStable("delivery", []string{outputID, ps(t, "targetId"), ps(t, "version"), ps(t, "mappingVersion")})
			wire := wireByID[deliveryID]
			required := 0
			if pb(t, "required") {
				required = 1
			}
			_, err = tx.ExecContext(ctx, "INSERT INTO delivery_outbox(delivery_id,output_id,target_id,target_config_version,mapping_version,required,protocol,destination_json,wire_payload,content_hash,state,next_attempt_at_ms,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,'pending',?,?)", deliveryID, outputID, ps(t, "targetId"), ps(t, "version"), ps(t, "mappingVersion"), required, strings.ToLower(ps(target, "protocol")), pipeJSON(target), []byte(pipeJSON(wire)), pipeHash(wire), pnow(), pnow())
			if err != nil {
				return true, err
			}
		}
	}
	_, err = tx.ExecContext(ctx, "UPDATE processing_jobs SET state='completed',mode=?,plan_json=?,output_count=?,completed_at_ms=?,lease_owner=NULL,lease_until_ms=NULL WHERE processing_id=? AND lease_token=?", strings.ToLower(ps(rule, "action")), pipeJSON(rule), len(records), pnow(), j.ID, j.Lease)
	if err == nil {
		_, err = tx.ExecContext(ctx, "UPDATE event_receipts SET custody_state='processed' WHERE event_key=? AND latest_revision=?", j.Key, j.Revision)
	}
	if err == nil {
		err = tx.Commit()
	}
	return true, err
}

func pipelineWire(j pipeJob, rule pipeMap, rec ParsedRecord, t, rawEvent pipeMap, replaced []string) pipeMap {
	outputID := pipeStable("output", []any{j.Key, j.Revision, rec.Key})
	deliveryID := pipeStable("delivery", []string{outputID, ps(t, "targetId"), ps(t, "version"), ps(t, "mappingVersion")})
	wire := pipeMap{"deliveryId": deliveryID, "outputId": outputID, "namespaceId": j.Namespace, "gatewayId": j.Gateway, "sourceEventId": j.Event, "processingRevision": j.Revision, "recordKey": rec.Key, "targetId": ps(t, "targetId"), "targetVersion": ps(t, "version"), "schemaVersion": ps(rule, "schemaVersion"), "mappingVersion": ps(t, "mappingVersion"), "mode": ps(rule, "action"), "correction": j.Revision > 1, "receivedAt": pts(j.Accepted), "timeQuality": ps(rawEvent, "timeQuality"), "quality": rec.Quality}
	if capture, ok := rawEvent["capture"].(map[string]any); ok {
		wire["capture"] = pm(pipeJSON(capture))
		wire["sourceSchemaVersion"] = "1.1"
	}
	if len(replaced) > 0 {
		wire["replacesOutputIds"] = replaced
	}
	if ps(rawEvent, "capturedAt") != "" {
		wire["capturedAt"] = rawEvent["capturedAt"]
	}
	if ps(rule, "action") == "PASSTHROUGH" {
		wire["rawBase64"] = base64.StdEncoding.EncodeToString(j.Raw)
	} else {
		wire["data"] = rec.Data
	}
	return wire
}

func evaluatePipeline(j pipeJob) (pipeMap, []ParsedRecord, error) {
	manifest := pm(j.Manifest)
	meta := pm(j.Meta)
	if j.ProtocolContext != "" {
		meta["protocolContext"] = pm(j.ProtocolContext)
	}
	raw := po(meta, "rawEvent")
	matches := []pipeMap{}
	packetType := "unknown"
	hint := ps(raw, "protocolHint")
	if strings.Contains(hint, "sensor") || hint == "demo-v1" {
		packetType = "sensor"
	}
	if strings.Contains(hint, "ble-ad") {
		packetType = "advertisement"
	}
	if kind := ps(po(raw, "capture"), "kind"); kind == "GATT_NOTIFY" || kind == "GATT_INDICATE" {
		packetType = "gatt_value"
	}
	for _, rv := range pl(manifest, "rules") {
		rule, ok := rv.(map[string]any)
		if !ok {
			continue
		}
		m := po(rule, "match")
		if !pcontains(m["modelIds"], ps(meta, "modelId")) || !pcontains(m["groupIds"], ps(meta, "groupId")) || !pcontains(m["protocolVersions"], hint) || !pcontains(m["packetTypes"], packetType) {
			continue
		}
		if !protocolRuleMatch(m, manifest, meta, j.Raw) {
			continue
		}
		matches = append(matches, rule)
	}
	if len(matches) == 0 {
		return nil, nil, &ParserFailure{"ROUTE_UNMATCHED", "no routing rule matched the captured identity and protocol"}
	}
	sort.Slice(matches, func(i, j int) bool { return pn(matches[i], "priority") > pn(matches[j], "priority") })
	if len(matches) > 1 && pn(matches[0], "priority") == pn(matches[1], "priority") {
		return nil, nil, &ParserFailure{"ROUTE_CONFLICT", "matching rules share the highest priority"}
	}
	rule := matches[0]
	switch ps(rule, "action") {
	case "QUARANTINE":
		return rule, nil, &ParserFailure{"ROUTE_QUARANTINE", "route explicitly quarantines this event"}
	case "PASSTHROUGH":
		if ps(rule, "schemaVersion") == "" {
			rule["schemaVersion"] = "raw-1"
		}
		return rule, []ParsedRecord{{Key: "0", Data: pipeMap{}, Quality: []string{}}}, nil
	case "DECODE":
		records, err := parseFrozenProtocol(rule, manifest, meta, j.Raw)
		annotateServiceUUIDSource(records, j.Event)
		return rule, records, err
	default:
		return rule, nil, &ParserFailure{"ROUTE_INVALID", "unknown action"}
	}
}

func (a *App) ReclaimPipelinePayloads(ctx context.Context) error {
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := pnow()
	// Only delivered originals past their promised raw-retention window are eligible.
	rows, err := tx.QueryContext(ctx, `SELECT e.event_key FROM event_receipts e WHERE e.raw_retention_until_ms<? AND e.raw_purged_at_ms IS NULL AND NOT EXISTS(SELECT 1 FROM raw_events original WHERE original.event_key=e.event_key AND json_type(original.metadata_json,'$.rawEvent.capture') IS NOT NULL AND NOT EXISTS(SELECT 1 FROM capture_consumptions consumed WHERE consumed.event_key=e.event_key)) AND NOT EXISTS(SELECT 1 FROM processing_jobs j WHERE j.event_key=e.event_key AND j.state NOT IN ('completed','filtered','terminated')) AND NOT EXISTS(SELECT 1 FROM delivery_outbox d JOIN processing_results r ON r.output_id=d.output_id JOIN processing_jobs j ON j.processing_id=r.processing_id WHERE j.event_key=e.event_key AND d.state NOT IN ('delivered','terminated')) LIMIT 100`, now)
	if err != nil {
		return err
	}
	keys := []string{}
	for rows.Next() {
		var k string
		if err = rows.Scan(&k); err != nil {
			rows.Close()
			return err
		}
		keys = append(keys, k)
	}
	if err = rows.Close(); err != nil {
		return err
	}
	for _, k := range keys {
		_, err = tx.ExecContext(ctx, "UPDATE delivery_outbox SET wire_payload=NULL,payload_purged_at_ms=? WHERE output_id IN (SELECT r.output_id FROM processing_results r JOIN processing_jobs j ON j.processing_id=r.processing_id WHERE j.event_key=?)", now, k)
		if err != nil {
			return err
		}
		_, err = tx.ExecContext(ctx, "UPDATE processing_results SET result_json=NULL,purged_at_ms=? WHERE processing_id IN (SELECT processing_id FROM processing_jobs WHERE event_key=?)", now, k)
		if err != nil {
			return err
		}
		if _, err = tx.ExecContext(ctx, "DELETE FROM raw_events WHERE event_key=?", k); err != nil {
			return err
		}
		if _, err = tx.ExecContext(ctx, "UPDATE event_receipts SET raw_purged_at_ms=? WHERE event_key=?", now, k); err != nil {
			return err
		}
	}
	if _, err = tx.ExecContext(ctx, "DELETE FROM delivery_batches WHERE batch_id IN (SELECT batch_id FROM delivery_batches WHERE expires_at_ms<? LIMIT 10000)", now); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, "DELETE FROM reprocess_jobs WHERE reprocess_id IN (SELECT reprocess_id FROM reprocess_jobs WHERE state='preview' AND created_at_ms<? LIMIT 1000)", now-15*60000); err != nil {
		return err
	}
	return tx.Commit()
}

// Keep metadata until the entire dedup/backup replay contract has expired; this
// first release never silently erases identity/ACK anchors to regain disk space.
