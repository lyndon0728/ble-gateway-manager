package platform

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

func processState(s string) string {
	switch s {
	case "leased":
		return "PROCESSING"
	case "completed":
		return "SUCCEEDED"
	default:
		return strings.ToUpper(s)
	}
}
func (a *App) eventView(ctx context.Context, key string, includeRaw bool) (pipeMap, error) {
	var ns, gw, event, bundle, state string
	var at, rev int64
	var purged sql.NullInt64
	err := a.Pipeline.QueryRowContext(ctx, "SELECT namespace_id,gateway_id,event_id,bundle_id,custody_state,accepted_at_ms,latest_revision,raw_purged_at_ms FROM event_receipts WHERE event_key=?", key).Scan(&ns, &gw, &event, &bundle, &state, &at, &rev, &purged)
	if err != nil {
		return nil, err
	}
	if state == "processing" {
		state = "accepted_raw"
	}
	m := pipeMap{"namespaceId": ns, "gatewayId": gw, "eventId": event, "bundleId": bundle, "state": strings.ToUpper(state), "acceptedAt": pts(at), "latestProcessingRevision": rev, "rawAvailable": !purged.Valid, "processing": []pipeMap{}}
	rows, err := a.Pipeline.QueryContext(ctx, "SELECT processing_id,revision,state,bundle_id,COALESCE(error_code,'') FROM processing_jobs WHERE event_key=? ORDER BY revision", key)
	if err != nil {
		return nil, err
	}
	jobs := []pipeMap{}
	ids := []string{}
	for rows.Next() {
		var id, s, b, code string
		var revision int64
		if err = rows.Scan(&id, &revision, &s, &b, &code); err != nil {
			rows.Close()
			return nil, err
		}
		j := pipeMap{"processingRevision": revision, "state": processState(s), "bundleId": b, "outputIds": []string{}}
		if revision == rev && s == "filtered" {
			m["state"] = "FILTERED"
		}
		if code != "" {
			j["errorCode"] = code
		}
		jobs = append(jobs, j)
		ids = append(ids, id)
	}
	rows.Close()
	for i, id := range ids {
		r, e := a.Pipeline.QueryContext(ctx, "SELECT output_id FROM processing_results WHERE processing_id=? ORDER BY record_key", id)
		if e != nil {
			return nil, e
		}
		out := []string{}
		for r.Next() {
			var oid string
			_ = r.Scan(&oid)
			out = append(out, oid)
		}
		r.Close()
		jobs[i]["outputIds"] = out
	}
	m["processing"] = jobs
	if includeRaw && !purged.Valid {
		var raw string
		if err = a.Pipeline.QueryRowContext(ctx, "SELECT metadata_json FROM raw_events WHERE event_key=?", key).Scan(&raw); err != nil {
			return nil, err
		}
		m["raw"] = po(pm(raw), "rawEvent")
	}
	return m, nil
}
func (a *App) pipelineEvent(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:read")
	if !ok {
		return
	}
	gw, event := chi.URLParam(r, "gatewayId"), chi.URLParam(r, "eventId")
	if !a.CanGateway(p, "data:read", gw) {
		pfail(w, r, 404, "NOT_FOUND", "event unavailable")
		return
	}
	raw := r.URL.Query().Get("includeRaw") == "true"
	if raw && !a.CanGateway(p, "data:raw", gw) {
		pfail(w, r, 403, "FORBIDDEN", "raw-data permission required")
		return
	}
	v, err := a.eventView(r.Context(), pipeStable("event", []string{a.Config.NamespaceID, gw, event}), raw)
	if err != nil {
		pfail(w, r, 404, "NOT_FOUND", "event unavailable")
		return
	}
	pjson(w, 200, v)
}
func pipeRange(r *http.Request) (int64, int64, error) {
	from, to := int64(0), pnow()+1
	var err error
	if v := r.URL.Query().Get("from"); v != "" {
		var t time.Time
		t, err = time.Parse(time.RFC3339Nano, v)
		if err != nil {
			return 0, 0, err
		}
		from = t.UnixMilli()
	}
	if v := r.URL.Query().Get("to"); v != "" {
		var t time.Time
		t, err = time.Parse(time.RFC3339Nano, v)
		if err != nil {
			return 0, 0, err
		}
		to = t.UnixMilli()
	}
	if from >= to {
		return 0, 0, errors.New("from must precede to")
	}
	return from, to, nil
}
func (a *App) pipelineEvents(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:read")
	if !ok {
		return
	}
	c, ok := a.readPipeCursor(w, r, p, "events", "event_receipts", a.Pipeline)
	if !ok {
		return
	}
	from, to, err := pipeRange(r)
	if err != nil {
		pfail(w, r, 400, "INVALID_RANGE", err.Error())
		return
	}
	gw := r.URL.Query().Get("gatewayId")
	if gw != "" && !a.CanGateway(p, "data:read", gw) {
		pfail(w, r, 404, "NOT_FOUND", "gateway unavailable")
		return
	}
	rows, err := a.Pipeline.QueryContext(r.Context(), "SELECT event_key,gateway_id FROM event_receipts WHERE namespace_id=? AND event_key>? AND rowid<=? AND accepted_at_ms>=? AND accepted_at_ms<? AND (?='' OR gateway_id=?) ORDER BY event_key LIMIT 2001", a.Config.NamespaceID, c.After, c.Upper, from, to, gw, gw)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "events unavailable")
		return
	}
	type entry struct{ key, gw string }
	entries := []entry{}
	for rows.Next() {
		var e entry
		_ = rows.Scan(&e.key, &e.gw)
		entries = append(entries, e)
	}
	rows.Close()
	items := []pipeMap{}
	last := ""
	more := false
	for i, e := range entries {
		last = e.key
		if !a.CanGateway(p, "data:read", e.gw) {
			continue
		}
		v, err := a.eventView(r.Context(), e.key, false)
		if err != nil {
			pfail(w, r, 503, "STORE_UNAVAILABLE", "event unavailable")
			return
		}
		items = append(items, v)
		if len(items) >= pipeLimit(r) {
			more = i < len(entries)-1
			break
		}
	}
	if len(entries) == 2001 {
		more = true
	}
	if !more {
		last = ""
	}
	pjson(w, 200, a.cursorPage(items, last, c))
}
func (a *App) pipelineQuarantine(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:read")
	if !ok {
		return
	}
	c, ok := a.readPipeCursor(w, r, p, "quarantine", "processing_jobs", a.Pipeline)
	if !ok {
		return
	}
	code := r.URL.Query().Get("errorCode")
	rows, err := a.Pipeline.QueryContext(r.Context(), "SELECT j.processing_id,e.namespace_id,e.gateway_id,e.event_id,j.revision,j.error_code,j.bundle_id,j.created_at_ms,e.raw_purged_at_ms FROM processing_jobs j JOIN event_receipts e ON e.event_key=j.event_key WHERE j.state='quarantined' AND j.processing_id>? AND j.rowid<=? AND (?='' OR j.error_code=?) ORDER BY j.processing_id LIMIT 2001", c.After, c.Upper, code, code)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "quarantine unavailable")
		return
	}
	items := []pipeMap{}
	ids := []string{}
	for rows.Next() {
		var id, ns, gw, event, errorCode, bundle string
		var rev, at int64
		var purged sql.NullInt64
		if err = rows.Scan(&id, &ns, &gw, &event, &rev, &errorCode, &bundle, &at, &purged); err != nil {
			break
		}
		items = append(items, pipeMap{"namespaceId": ns, "gatewayId": gw, "eventId": event, "processingRevision": rev, "errorCode": errorCode, "bundleId": bundle, "createdAt": pts(at), "rawAvailable": !purged.Valid})
		ids = append(ids, id)
	}
	rows.Close()
	visible := []pipeMap{}
	next := ""
	for i, v := range items {
		next = ids[i]
		if !a.CanGateway(p, "data:read", ps(v, "gatewayId")) {
			continue
		}
		visible = append(visible, v)
		if len(visible) >= pipeLimit(r) {
			if i == len(items)-1 {
				next = ""
			}
			break
		}
		if i == len(items)-1 {
			next = ""
		}
	}
	if len(items) == 2001 && next == "" {
		next = ids[len(ids)-1]
	}
	pjson(w, 200, a.cursorPage(visible, next, c))
}

// Preview stages a validated, immutable rule snapshot without changing ingress routing.
func (a *App) stageReplayBundle(ctx context.Context, id string) error {
	var raw, hash, state string
	var ver int64
	if err := a.Management.QueryRowContext(ctx, "SELECT manifest_json,content_hash,state,version FROM rule_bundles WHERE bundle_id=?", id).Scan(&raw, &hash, &state, &ver); err != nil {
		return err
	}
	if state != "validated" && state != "released" {
		return errors.New("reprocess bundle must be validated")
	}
	if err := verifyFrozenProtocolRuntime(pm(raw)); err != nil {
		return err
	}
	if hash != pipeHash(pm(raw)) {
		return errors.New("bundle digest mismatch")
	}
	_, err := a.Pipeline.ExecContext(ctx, "INSERT OR IGNORE INTO pipeline_bundles(bundle_id,namespace_id,activation_epoch,content_hash,manifest_json,state,stored_at_ms) VALUES(?,?,?,?,?,'staged',?)", id, a.Config.NamespaceID, 1000000000000+ver, hash, raw, pnow())
	return err
}
func (a *App) pipelinePreview(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:reprocess")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "ReprocessPreviewRequest")
	if !ok {
		return
	}
	selector := po(req, "selector")
	for _, id := range pl(selector, "gatewayIds") {
		if !a.CanGateway(p, "data:reprocess", id.(string)) {
			pfail(w, r, 404, "NOT_FOUND", "gateway unavailable")
			return
		}
	}
	from, e1 := time.Parse(time.RFC3339Nano, ps(selector, "from"))
	to, e2 := time.Parse(time.RFC3339Nano, ps(selector, "to"))
	if e1 != nil || e2 != nil || !from.Before(to) || to.Sub(from) > 31*24*time.Hour {
		pfail(w, r, 422, "INVALID_RANGE", "preview range must be positive and at most 31 days")
		return
	}
	if err := a.stageReplayBundle(r.Context(), ps(req, "bundleId")); err != nil {
		pfail(w, r, 422, "BUNDLE_UNAVAILABLE", err.Error())
		return
	}
	eventIDs, errorCodes := pipeJSON(pl(selector, "eventIds")), pipeJSON(pl(selector, "errorCodes"))
	rows, err := a.Pipeline.QueryContext(r.Context(), "SELECT e.event_key,e.gateway_id,e.event_id,e.raw_purged_at_ms,COALESCE(j.error_code,'') FROM event_receipts e JOIN processing_jobs j ON j.event_key=e.event_key AND j.revision=e.latest_revision WHERE e.namespace_id=? AND e.accepted_at_ms>=? AND e.accepted_at_ms<? AND e.gateway_id IN (SELECT value FROM json_each(?)) AND (?='[]' OR e.event_id IN (SELECT value FROM json_each(?))) AND (?='[]' OR j.error_code IN (SELECT value FROM json_each(?))) ORDER BY e.event_key LIMIT 10001", a.Config.NamespaceID, from.UnixMilli(), to.UnixMilli(), pipeJSON(selector["gatewayIds"]), eventIDs, eventIDs, errorCodes, errorCodes)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	keys := []string{}
	missing := 0
	for rows.Next() {
		var key, gw, event, code string
		var purged sql.NullInt64
		_ = rows.Scan(&key, &gw, &event, &purged, &code)
		if !pcontains(selector["eventIds"], event) || !pcontains(selector["errorCodes"], code) {
			continue
		}
		keys = append(keys, key)
		if purged.Valid {
			missing++
		}
	}
	rows.Close()
	if len(keys) > 10000 {
		pfail(w, r, 422, "SELECTION_TOO_LARGE", "narrow the selection to at most 10000 events")
		return
	}
	now := pnow()
	contexts := pipeMap{}
	for _, key := range keys {
		var metadata, gateway string
		if e := a.Pipeline.QueryRowContext(r.Context(), "SELECT r.metadata_json,e.gateway_id FROM raw_events r JOIN event_receipts e ON e.event_key=r.event_key WHERE r.event_key=?", key).Scan(&metadata, &gateway); e != nil {
			continue
		}
		frozen, e := a.freezeCaptureProtocolContext(r.Context(), gateway, po(pm(metadata), "rawEvent"))
		if e != nil {
			pfail(w, r, 503, "STORE_UNAVAILABLE", e.Error())
			return
		}
		contexts[key] = frozen
	}
	selection := pipeMap{"eventKeys": keys, "selector": selector, "bundleId": req["bundleId"], "expiresAtMs": now + 15*60000, "protocolContexts": contexts}
	digest := pipeHash(selection)
	selection["selectionDigest"] = digest
	disk, diskErr := a.pipelineStorageStats()
	if diskErr != nil {
		pfail(w, r, 503, "DISK_STATUS_UNAVAILABLE", "cannot verify durable capacity")
		return
	}
	if code := pipelineStorageCheck(disk, 2*int64(len(pipeJSON(selection)))+65536, a.Config.MaxQueueBytes); code != "" {
		pfail(w, r, 503, code, "insufficient durable capacity for preview")
		return
	}
	id := pipeID("preview")
	_, err = a.Pipeline.ExecContext(r.Context(), "INSERT INTO reprocess_jobs(reprocess_id,bundle_id,requested_by,idempotency_key,request_hash,selection_json,reason,state,created_at_ms) VALUES(?,?,?,?,?,?,'preview','preview',?)", id, ps(req, "bundleId"), p.ID, id, pipeHash(req), pipeJSON(selection), now)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	samples := []pipeMap{}
	var manifest string
	_ = a.Pipeline.QueryRowContext(r.Context(), "SELECT manifest_json FROM pipeline_bundles WHERE bundle_id=?", ps(req, "bundleId")).Scan(&manifest)
	for _, key := range keys {
		if len(samples) >= int(pn(req, "maxSampleEvents")) {
			break
		}
		var raw []byte
		var meta string
		if a.Pipeline.QueryRowContext(r.Context(), "SELECT payload,metadata_json FROM raw_events WHERE event_key=?", key).Scan(&raw, &meta) != nil {
			continue
		}
		rule, out, e := evaluatePipeline(pipeJob{Raw: raw, Meta: meta, Manifest: manifest, ProtocolContext: pipeJSON(po(contexts, key))})
		sample := pipeMap{"eventKey": key, "action": ps(rule, "action"), "newRecordCount": len(out)}
		if e != nil {
			sample["error"] = e.Error()
		}
		samples = append(samples, sample)
	}
	pjson(w, 200, pipeMap{"previewId": id, "selectionDigest": digest, "matchedCount": len(keys), "missingRawCount": missing, "expiresAt": pts(now + 15*60000), "sampleDifferences": samples})
}
func (a *App) reprocessView(ctx context.Context, id string) (pipeMap, string, error) {
	var bundle, actor, state string
	var created int64
	if err := a.Pipeline.QueryRowContext(ctx, "SELECT bundle_id,requested_by,state,created_at_ms FROM reprocess_jobs WHERE reprocess_id=? AND state<>'preview'", id).Scan(&bundle, &actor, &state, &created); err != nil {
		return nil, "", err
	}
	var total, done, failed int64
	if err := a.Pipeline.QueryRowContext(ctx, "SELECT COUNT(*),COALESCE(SUM(state='completed'),0),COALESCE(SUM(state IN ('quarantined','terminated')),0) FROM processing_jobs WHERE reprocess_id=?", id).Scan(&total, &done, &failed); err != nil {
		return nil, "", err
	}
	apiState := map[string]string{"pending": "QUEUED", "running": "RUNNING", "paused": "PAUSED", "completed": "SUCCEEDED", "failed": "FAILED", "cancelled": "CANCELLED"}[state]
	if done+failed == total {
		apiState = "SUCCEEDED"
		if failed > 0 {
			apiState = "FAILED"
		}
	} else if done+failed > 0 {
		apiState = "RUNNING"
	}
	return pipeMap{"jobId": id, "bundleId": bundle, "state": apiState, "selectedCount": total, "processedCount": done, "failedCount": failed, "createdAt": pts(created)}, actor, nil
}
func (a *App) pipelineReprocess(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:reprocess")
	if !ok {
		return
	}
	floors, floorErr := a.reprocessRevisionFloors()
	if floorErr != nil {
		pfail(w, r, 409, "RESTORE_RECONCILIATION_REQUIRED", floorErr.Error())
		return
	}
	req, ok := a.Decode(w, r, "ReprocessCreate")
	if !ok {
		return
	}
	key := r.Header.Get("Idempotency-Key")
	if key == "" || len(key) > 128 {
		pfail(w, r, 400, "IDEMPOTENCY_REQUIRED", "Idempotency-Key required")
		return
	}
	idem := pipeStable("reprocess", []string{p.ID, key})
	ctx := r.Context()
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	defer tx.Rollback()
	var existing, oldHash string
	err = tx.QueryRowContext(ctx, "SELECT reprocess_id,request_hash FROM reprocess_jobs WHERE idempotency_key=?", idem).Scan(&existing, &oldHash)
	if err == nil {
		if oldHash != pipeHash(req) {
			pfail(w, r, 409, "IDEMPOTENCY_CONFLICT", "request differs")
			return
		}
		_ = tx.Rollback()
		v, _, e := a.reprocessView(ctx, existing)
		if e != nil {
			pfail(w, r, 503, "STORE_UNAVAILABLE", e.Error())
			return
		}
		pjson(w, 202, v)
		return
	}
	if !errors.Is(err, sql.ErrNoRows) {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	var actor, bundle, raw string
	if err = tx.QueryRowContext(ctx, "SELECT requested_by,bundle_id,selection_json FROM reprocess_jobs WHERE reprocess_id=? AND state='preview'", ps(req, "previewId")).Scan(&actor, &bundle, &raw); err != nil || actor != p.ID {
		pfail(w, r, 404, "NOT_FOUND", "preview unavailable")
		return
	}
	selection := pm(raw)
	if ps(selection, "selectionDigest") != ps(req, "selectionDigest") || pn(selection, "expiresAtMs") < pnow() {
		pfail(w, r, 409, "PREVIEW_STALE", "create a current preview")
		return
	}
	disk, diskErr := a.pipelineStorageStats()
	if diskErr != nil {
		pfail(w, r, 503, "DISK_STATUS_UNAVAILABLE", "cannot verify durable capacity")
		return
	}
	if code := pipelineStorageCheck(disk, 2*int64(len(raw))+int64(len(pl(selection, "eventKeys")))*4096+65536, a.Config.MaxQueueBytes); code != "" {
		pfail(w, r, 503, code, "insufficient durable capacity for reprocess jobs")
		return
	}
	for _, g := range pl(po(selection, "selector"), "gatewayIds") {
		if !a.CanGateway(p, "data:reprocess", g.(string)) {
			pfail(w, r, 404, "NOT_FOUND", "gateway unavailable")
			return
		}
	}
	id := pipeID("reprocess")
	now := pnow()
	_, err = tx.ExecContext(ctx, "INSERT INTO reprocess_jobs(reprocess_id,bundle_id,requested_by,idempotency_key,request_hash,selection_json,cursor_json,reason,state,created_at_ms) VALUES(?,?,?,?,?,?,?,?,'pending',?)", id, bundle, p.ID, idem, pipeHash(req), raw, pipeJSON(req), ps(req, "reason"), now)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	for i, k := range pl(selection, "eventKeys") {
		eventKey := k.(string)
		var rev int64
		var present int
		if err = tx.QueryRowContext(ctx, "SELECT latest_revision,EXISTS(SELECT 1 FROM raw_events WHERE event_key=?) FROM event_receipts WHERE event_key=?", eventKey, eventKey).Scan(&rev, &present); err != nil || present != 1 {
			pfail(w, r, 409, "RAW_UNAVAILABLE", "selection contains reclaimed raw data; create a narrower preview")
			return
		}
		if floor := floors[eventKey]; floor > rev {
			rev = floor
		}
		rev++
		if _, err = tx.ExecContext(ctx, "UPDATE event_receipts SET latest_revision=?,custody_state='accepted_raw' WHERE event_key=?", rev, eventKey); err != nil {
			pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
			return
		}
		protocolContext := po(po(selection, "protocolContexts"), eventKey)
		_, err = tx.ExecContext(ctx, "INSERT INTO processing_jobs(processing_id,event_key,revision,bundle_id,reprocess_id,state,next_attempt_at_ms,created_at_ms,protocol_context_json) VALUES(?,?,?,?,?,'pending',?,?,?)", pipeStable("processing", []any{eventKey, rev}), eventKey, rev, bundle, id, now+int64(i)*1000/pn(req, "maxEventsPerSecond"), now, pipeJSON(protocolContext))
		if err != nil {
			pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
			return
		}
	}
	if err = tx.Commit(); err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	v, _, err := a.reprocessView(ctx, id)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	w.Header().Set("Location", "/api/v1/reprocess-jobs/"+id)
	pjson(w, 202, v)
}

func (a *App) reprocessRevisionFloors() (map[string]int64, error) {
	if _, err := os.Stat(filepath.Join(a.Config.DataDir, "restore-reconciliation.required")); err == nil {
		return nil, errors.New("offline restore requires customer revision reconciliation before reprocessing")
	} else if !os.IsNotExist(err) {
		return nil, err
	}
	floors := map[string]int64{}
	f, err := os.Open(filepath.Join(a.Config.DataDir, "revision-highwater.json"))
	if os.IsNotExist(err) {
		return floors, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var doc struct {
		NamespaceID string `json:"namespaceId"`
		Complete    bool   `json:"complete"`
		Revisions   []struct {
			GatewayID string `json:"gatewayId"`
			EventID   string `json:"eventId"`
			Revision  int64  `json:"processingRevision"`
		} `json:"revisions"`
	}
	if stat, err := f.Stat(); err != nil {
		return nil, err
	} else if stat.Size() > 64<<20 {
		return nil, errors.New("revision highwater exceeds 64 MiB")
	}
	decoder := json.NewDecoder(io.LimitReader(f, (64<<20)+1))
	decoder.DisallowUnknownFields()
	if err = decoder.Decode(&doc); err != nil {
		return nil, err
	}
	var extra any
	if err = decoder.Decode(&extra); err != io.EOF {
		return nil, errors.New("revision highwater has trailing data")
	}
	if doc.NamespaceID != a.Config.NamespaceID || !doc.Complete {
		return nil, errors.New("revision highwater must be complete and match namespace")
	}
	for _, v := range doc.Revisions {
		if v.GatewayID == "" || v.EventID == "" || v.Revision < 1 || v.Revision >= 9007199254740991 {
			return nil, errors.New("invalid revision highwater entry")
		}
		key := pipeStable("event", []string{doc.NamespaceID, v.GatewayID, v.EventID})
		if v.Revision > floors[key] {
			floors[key] = v.Revision
		}
	}
	return floors, nil
}
func (a *App) pipelineReprocessGet(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:reprocess")
	if !ok {
		return
	}
	v, actor, err := a.reprocessView(r.Context(), chi.URLParam(r, "jobId"))
	if err != nil || actor != p.ID {
		pfail(w, r, 404, "NOT_FOUND", "reprocess job unavailable")
		return
	}
	pjson(w, 200, v)
}

func deliveryState(state string) string {
	if state == "leased" || state == "awaiting_ack" {
		return "IN_FLIGHT"
	}
	return strings.ToUpper(state)
}
func (a *App) deliveryView(ctx context.Context, id string) (pipeMap, error) {
	var out, ns, gw, event, record, target, ver, mapping, state, last, reason string
	var rev, attempts, next int64
	var confirmed sql.NullInt64
	err := a.Pipeline.QueryRowContext(ctx, "SELECT d.output_id,e.namespace_id,e.gateway_id,e.event_id,j.revision,r.record_key,d.target_id,d.target_config_version,d.mapping_version,d.state,d.attempts,d.next_attempt_at_ms,COALESCE(d.last_error,''),COALESCE(d.terminated_reason,''),d.delivered_at_ms FROM delivery_outbox d JOIN processing_results r ON r.output_id=d.output_id JOIN processing_jobs j ON j.processing_id=r.processing_id JOIN event_receipts e ON e.event_key=j.event_key WHERE d.delivery_id=?", id).Scan(&out, &ns, &gw, &event, &rev, &record, &target, &ver, &mapping, &state, &attempts, &next, &last, &reason, &confirmed)
	if err != nil {
		return nil, err
	}
	m := pipeMap{"deliveryId": id, "outputId": out, "namespaceId": ns, "gatewayId": gw, "sourceEventId": event, "processingRevision": rev, "recordKey": record, "targetId": target, "targetVersion": ver, "mappingVersion": mapping, "state": deliveryState(state), "attempts": attempts}
	if state != "delivered" && state != "terminated" {
		m["nextAttemptAt"] = pts(next)
	}
	if last != "" {
		m["lastErrorCode"] = last
	}
	if reason != "" {
		m["terminationReason"] = reason
	}
	if confirmed.Valid {
		m["confirmedAt"] = pts(confirmed.Int64)
	}
	return m, nil
}
func (a *App) pipelineDelivery(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "delivery:read")
	if !ok {
		return
	}
	m, err := a.deliveryView(r.Context(), chi.URLParam(r, "deliveryId"))
	if err != nil || !a.CanGateway(p, "delivery:read", ps(m, "gatewayId")) {
		pfail(w, r, 404, "NOT_FOUND", "delivery unavailable")
		return
	}
	pjson(w, 200, m)
}
func (a *App) pipelineDeliveries(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "delivery:read")
	if !ok {
		return
	}
	c, ok := a.readPipeCursor(w, r, p, "deliveries", "delivery_outbox", a.Pipeline)
	if !ok {
		return
	}
	target, state := r.URL.Query().Get("targetId"), r.URL.Query().Get("state")
	rows, err := a.Pipeline.QueryContext(r.Context(), "SELECT delivery_id FROM delivery_outbox WHERE delivery_id>? AND rowid<=? AND (?='' OR target_id=?) AND (?='' OR UPPER(state)=? OR (?='IN_FLIGHT' AND state IN ('leased','awaiting_ack'))) ORDER BY delivery_id LIMIT 2001", c.After, c.Upper, target, target, state, state, state)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	ids := []string{}
	for rows.Next() {
		var id string
		_ = rows.Scan(&id)
		ids = append(ids, id)
	}
	rows.Close()
	items := []pipeMap{}
	next := ""
	for i, id := range ids {
		next = id
		m, err := a.deliveryView(r.Context(), id)
		if err == nil && a.CanGateway(p, "delivery:read", ps(m, "gatewayId")) {
			items = append(items, m)
		}
		if len(items) >= pipeLimit(r) {
			if i == len(ids)-1 {
				next = ""
			}
			break
		}
		if i == len(ids)-1 {
			next = ""
		}
	}
	if len(ids) == 2001 && next == "" {
		next = ids[len(ids)-1]
	}
	pjson(w, 200, a.cursorPage(items, next, c))
}
func (a *App) pipelineDeliveryRetry(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "delivery:retry")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "RetryRequest")
	if !ok {
		return
	}
	id := chi.URLParam(r, "deliveryId")
	m, err := a.deliveryView(r.Context(), id)
	if err != nil || !a.CanGateway(p, "delivery:retry", ps(m, "gatewayId")) {
		pfail(w, r, 404, "NOT_FOUND", "delivery unavailable")
		return
	}
	a.pipeMutation(w, r, p, "delivery:retry:"+id, req, func(tx *sql.Tx) (int, pipeMap, error) {
		result, err := a.Pipeline.ExecContext(r.Context(), "UPDATE delivery_outbox SET state='retry_wait',next_attempt_at_ms=?,lease_owner=NULL,lease_until_ms=NULL,lease_token=lease_token+1 WHERE delivery_id=? AND state IN ('pending','retry_wait','awaiting_ack') AND wire_payload IS NOT NULL", pnow(), id)
		if err != nil {
			return 0, nil, err
		}
		n, _ := result.RowsAffected()
		if n == 0 {
			return 0, nil, errors.New("delivery is not retryable or payload unavailable")
		}
		v, err := a.deliveryView(r.Context(), id)
		return 202, v, err
	})
}
