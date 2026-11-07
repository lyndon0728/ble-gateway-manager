package platform

import (
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"sort"
	"strings"

	"github.com/go-chi/chi/v5"
)

// RegisterFrontendAPI exposes durable read models for the operator console.
// Commands continue to use the existing authorized, idempotent task API.
func (a *App) RegisterFrontendAPI(r chi.Router) {
	r.Get("/api/v1/gateways/{gatewayId}/configuration/effective", a.frontendConfiguration)
	r.Get("/api/v1/tasks/{taskId}/request", a.frontendTaskRequest)
	r.Get("/api/v1/tasks/{taskId}/results", a.frontendTaskResults)
	r.Get("/api/v1/outputs/{outputId}", a.frontendOutput)
	r.Get("/api/v1/deliveries/{deliveryId}/payload", a.frontendDeliveryPayload)
	r.Get("/api/v1/gateways/{gatewayId}/events/{eventId}/deliveries", a.frontendEventDeliveries)
	r.Get("/api/v1/artifacts", a.frontendArtifacts)
	r.Get("/api/v1/reprocess-jobs", a.frontendReprocessJobs)
	r.Get("/api/v1/operations", a.frontendOperations)
	r.Get("/api/v1/ble-peers/{peerId}", a.frontendPeer)
	r.Get("/api/v1/auto-connect-rules/{ruleId}", a.frontendAutoRule)
	r.Get("/api/v1/gatt-subscriptions", a.frontendSubscriptions)
	r.Get("/api/v1/auto-connect-triggers", a.frontendTriggers)
}

func frontendRedact(v any) any {
	switch x := v.(type) {
	case map[string]any:
		out := map[string]any{}
		for k, value := range x {
			key := strings.ToLower(strings.NewReplacer("_", "", "-", "").Replace(k))
			switch key {
			case "password", "passwd", "psk", "passphrase", "token", "accesstoken", "refreshtoken", "authorization", "secret", "secretvalue", "privatekey", "valuebase64", "payloadbase64", "readbackbase64":
				out[k] = "[REDACTED]"
			default:
				out[k] = frontendRedact(value)
			}
		}
		return out
	case []any:
		out := make([]any, len(x))
		for i, value := range x {
			out[i] = frontendRedact(value)
		}
		return out
	default:
		return v
	}
}

// Filtering a bounded candidate set must never turn a truncated query into a
// successful but incomplete page. The common Page implementation has the same cap.
func frontendCandidatesComplete(w http.ResponseWriter, r *http.Request, count int) bool {
	if count > 10000 {
		Fail(w, r, 422, "QUERY_TOO_BROAD", "narrow the query to at most 10000 candidate records")
		return false
	}
	return true
}
func frontendKnownPaths(v map[string]any, prefix string, out *[]string) {
	for k, value := range v {
		path := k
		if prefix != "" {
			path = prefix + "." + k
		}
		if child, ok := value.(map[string]any); ok && len(child) > 0 {
			frontendKnownPaths(child, path, out)
		} else {
			*out = append(*out, path)
		}
	}
}
func frontendCanonicalPatch(patch map[string]any, gw32 bool) map[string]any {
	out := pm(encode(patch))
	if !gw32 {
		return out
	}
	for _, key := range []string{"scan", "filters"} {
		if value, ok := out[key]; ok {
			radios := Obj(out, "radios")
			nrf := Obj(radios, "nrf54")
			nrf[key] = value
			radios["nrf54"] = nrf
			out["radios"] = radios
			delete(out, key)
		}
	}
	return out
}
func (a *App) frontendConfiguration(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gateway:read")
	if !ok {
		return
	}
	id := chi.URLParam(r, "gatewayId")
	if !a.CanGateway(p, "gateway:read", id) {
		Fail(w, r, 404, "NOT_FOUND", "gateway unavailable")
		return
	}
	g, e := a.gatewayView(r.Context(), a.Management, id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), `SELECT c.revision,c.reported_json,c.verified_at_ms,COALESCE((SELECT task_id FROM command_tasks t WHERE t.config_id=c.config_id ORDER BY created_at_ms DESC LIMIT 1),'') FROM gateway_configs c WHERE c.gateway_id=? AND c.state='verified' AND c.reported_json IS NOT NULL ORDER BY c.revision`, id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	configuration := map[string]any{}
	provenance := map[string]any{}
	revision := int64(0)
	for rows.Next() {
		var rev int64
		var raw, task string
		var verified sql.NullInt64
		if e = rows.Scan(&rev, &raw, &verified, &task); e != nil {
			RespondError(w, r, e)
			return
		}
		patch := frontendCanonicalPatch(pm(raw), gw32Model(g))
		configuration = gw32Merge(configuration, patch)
		revision = rev
		paths := []string{}
		frontendKnownPaths(patch, "", &paths)
		// Replacing an object such as ipv4 removes provenance for its former fields.
		for old := range provenance {
			parts := strings.Split(old, ".")
			cur := configuration
			exists := true
			for i, k := range parts {
				v, yes := cur[k]
				if !yes {
					exists = false
					break
				}
				if i < len(parts)-1 {
					cur, yes = v.(map[string]any)
					if !yes {
						exists = false
						break
					}
				}
			}
			if !exists {
				delete(provenance, old)
			}
		}
		for _, path := range paths {
			source := map[string]any{"path": path, "revision": rev, "source": "DEVICE_VERIFIED_PATCH"}
			if task != "" {
				source["taskId"] = task
			}
			if verified.Valid {
				source["verifiedAt"] = Timestamp(verified.Int64)
			}
			provenance[path] = source
		}
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	paths := []string{}
	frontendKnownPaths(configuration, "", &paths)
	sort.Strings(paths)
	sources := []any{}
	for _, path := range paths {
		if source, ok := provenance[path]; ok {
			sources = append(sources, source)
		}
	}
	JSON(w, 200, map[string]any{"gatewayId": id, "configuration": frontendRedact(configuration), "knownPaths": paths, "sources": sources, "reportedRevision": revision, "complete": false, "valueSemantics": "ACKNOWLEDGED_PATCH_NOT_RUNTIME_READBACK"})
}
func (a *App) frontendTaskRequest(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.taskAuthorized(w, r, "task:read")
	if !ok {
		return
	}
	var gw, raw, digest string
	e := a.Management.QueryRowContext(r.Context(), "SELECT gateway_id,request_json,request_hash FROM command_tasks WHERE task_id=?", id).Scan(&gw, &raw, &digest)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"taskId": id, "gatewayId": gw, "request": frontendRedact(pm(raw)), "requestDigest": digest, "redacted": true})
}
func (a *App) frontendTaskResults(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.taskAuthorized(w, r, "task:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT result_id,sequence,received_at_ms,result_json FROM command_results WHERE task_id=? ORDER BY sequence,result_id LIMIT 10001", id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var result string
		var raw sql.NullString
		var sequence, received int64
		if e = rows.Scan(&result, &sequence, &received, &raw); e != nil {
			RespondError(w, r, e)
			return
		}
		item := map[string]any{"resultId": result, "taskId": id, "sequence": sequence, "receivedAt": Timestamp(received), "payloadAvailable": raw.Valid, "redacted": true}
		if raw.Valid {
			item["result"] = frontendRedact(pm(raw.String))
		}
		items = append(items, item)
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	a.Page(w, r, items)
}

func (a *App) frontendOutput(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:read")
	if !ok {
		return
	}
	id := chi.URLParam(r, "outputId")
	var gw, event, record, schema, parser, version, digest string
	var raw sql.NullString
	var rev, at int64
	e := a.Pipeline.QueryRowContext(r.Context(), `SELECT e.gateway_id,e.event_id,j.revision,o.record_key,o.schema_version,COALESCE(o.parser_id,''),COALESCE(o.parser_version,''),o.content_hash,o.result_json,o.created_at_ms FROM processing_results o JOIN processing_jobs j ON j.processing_id=o.processing_id JOIN event_receipts e ON e.event_key=j.event_key WHERE o.output_id=? AND e.namespace_id=?`, id, a.Config.NamespaceID).Scan(&gw, &event, &rev, &record, &schema, &parser, &version, &digest, &raw, &at)
	if errors.Is(e, sql.ErrNoRows) || e == nil && !a.CanGateway(p, "data:read", gw) {
		Fail(w, r, 404, "NOT_FOUND", "output unavailable")
		return
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !a.CanGateway(p, "data:raw", gw) {
		Fail(w, r, 403, "FORBIDDEN", "raw-data permission required for persisted output")
		return
	}
	m := map[string]any{"outputId": id, "namespaceId": a.Config.NamespaceID, "gatewayId": gw, "sourceEventId": event, "processingRevision": rev, "recordKey": record, "schemaVersion": schema, "parserId": parser, "parserVersion": version, "contentDigest": digest, "createdAt": Timestamp(at), "payloadAvailable": raw.Valid}
	if raw.Valid {
		m["record"] = pm(raw.String)
	}
	JSON(w, 200, m)
}
func (a *App) frontendDeliveryPayload(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "delivery:read")
	if !ok {
		return
	}
	id := chi.URLParam(r, "deliveryId")
	summary, e := a.deliveryView(r.Context(), id)
	if e != nil || !a.CanGateway(p, "delivery:read", Str(summary, "gatewayId")) {
		Fail(w, r, 404, "NOT_FOUND", "delivery unavailable")
		return
	}
	if !a.CanGateway(p, "data:raw", Str(summary, "gatewayId")) {
		Fail(w, r, 403, "FORBIDDEN", "raw-data permission required for delivery payload")
		return
	}
	var payload []byte
	var digest string
	e = a.Pipeline.QueryRowContext(r.Context(), "SELECT wire_payload,content_hash FROM delivery_outbox WHERE delivery_id=?", id).Scan(&payload, &digest)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	m := map[string]any{"deliveryId": id, "gatewayId": Str(summary, "gatewayId"), "contentDigest": digest, "payloadAvailable": payload != nil}
	if payload != nil {
		var v any
		if e = json.Unmarshal(payload, &v); e != nil {
			RespondError(w, r, e)
			return
		}
		m["payload"] = v
	}
	JSON(w, 200, m)
}
func (a *App) frontendEventDeliveries(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "delivery:read")
	if !ok {
		return
	}
	gw, event := chi.URLParam(r, "gatewayId"), chi.URLParam(r, "eventId")
	if !a.CanGateway(p, "delivery:read", gw) {
		Fail(w, r, 404, "NOT_FOUND", "event unavailable")
		return
	}
	rows, e := a.Pipeline.QueryContext(r.Context(), `SELECT d.delivery_id FROM delivery_outbox d JOIN processing_results o ON o.output_id=d.output_id JOIN processing_jobs j ON j.processing_id=o.processing_id JOIN event_receipts e ON e.event_key=j.event_key WHERE e.namespace_id=? AND e.gateway_id=? AND e.event_id=? ORDER BY d.created_at_ms,d.delivery_id LIMIT 10001`, a.Config.NamespaceID, gw, event)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	ids := []string{}
	for rows.Next() {
		var id string
		if e = rows.Scan(&id); e != nil {
			rows.Close()
			RespondError(w, r, e)
			return
		}
		ids = append(ids, id)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for _, id := range ids {
		v, e := a.deliveryView(r.Context(), id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, v)
	}
	a.Page(w, r, items)
}

func (a *App) frontendArtifacts(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "artifact:read")
	if !ok {
		return
	}
	if !a.Can(p, "artifact:read", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "instance artifact permission required")
		return
	}
	kind := r.URL.Query().Get("kind")
	dbKind := strings.ToLower(kind)
	if kind == "PLATFORM_PACKAGE" {
		dbKind = "installer"
	}
	if kind != "" && kind != "DEVICE_MANIFEST" && kind != "FIRMWARE" && kind != "PLATFORM_PACKAGE" {
		Fail(w, r, 400, "INVALID_FILTER", "kind must be DEVICE_MANIFEST, FIRMWARE or PLATFORM_PACKAGE")
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT artifact_id,name,created_at_ms FROM artifacts WHERE kind IN ('device_manifest','firmware','installer') AND (?='' OR kind=?) ORDER BY created_at_ms DESC,artifact_id LIMIT 10001", dbKind, dbKind)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	type item struct {
		id, name string
		at       int64
	}
	list := []item{}
	for rows.Next() {
		var v item
		if e = rows.Scan(&v.id, &v.name, &v.at); e != nil {
			rows.Close()
			RespondError(w, r, e)
			return
		}
		list = append(list, v)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for _, v := range list {
		m, e := a.artifactView(r.Context(), a.Management, v.id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		m["name"] = v.name
		m["createdAt"] = Timestamp(v.at)
		items = append(items, m)
	}
	a.Page(w, r, items)
}
func (a *App) frontendReprocessJobs(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "data:reprocess")
	if !ok {
		return
	}
	rows, e := a.Pipeline.QueryContext(r.Context(), "SELECT reprocess_id FROM reprocess_jobs WHERE requested_by=? AND state<>'preview' ORDER BY created_at_ms DESC,reprocess_id LIMIT 10001", p.ID)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	ids := []string{}
	for rows.Next() {
		var id string
		if e = rows.Scan(&id); e != nil {
			rows.Close()
			RespondError(w, r, e)
			return
		}
		ids = append(ids, id)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !frontendCandidatesComplete(w, r, len(ids)) {
		return
	}
	items := []any{}
	for _, id := range ids {
		v, _, e := a.reprocessView(r.Context(), id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		if state := r.URL.Query().Get("state"); state != "" && state != Str(v, "state") {
			continue
		}
		items = append(items, v)
	}
	a.Page(w, r, items)
}
func (a *App) frontendOperations(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "operation:read")
	if !ok {
		return
	}
	global := a.Can(p, "system:maintain", "scope_instance")
	kind, state := r.URL.Query().Get("kind"), r.URL.Query().Get("state")
	rows, e := a.Management.QueryContext(r.Context(), `SELECT job_id,kind,state,created_at_ms,COALESCE(finished_at_ms,0),checkpoint_json FROM maintenance_jobs WHERE (? OR created_by=?) AND (?='' OR UPPER(kind)=?) AND (?='' OR (CASE state WHEN 'pending' THEN 'QUEUED' ELSE UPPER(state) END)=?) ORDER BY created_at_ms DESC,job_id LIMIT 10001`, global, p.ID, kind, kind, state, state)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for rows.Next() {
		var id, kind, state, raw string
		var at, finished int64
		if e = rows.Scan(&id, &kind, &state, &at, &finished, &raw); e != nil {
			rows.Close()
			RespondError(w, r, e)
			return
		}
		items = append(items, operationView(id, kind, state, at, finished, Obj(map[string]any{"v": frontendRedact(pm(raw))}, "v")))
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	rows, e = a.Management.QueryContext(r.Context(), `SELECT response_json FROM api_idempotency WHERE state='committed' AND resource_type='pipeline' AND (? OR principal_id=?) AND json_type(response_json,'$.operationId')='text' AND (?='' OR json_extract(response_json,'$.kind')=?) AND (?='' OR json_extract(response_json,'$.state')=?) ORDER BY created_at_ms DESC,operation_id LIMIT 10001`, global, p.ID, kind, kind, state, state)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	for rows.Next() {
		var raw string
		if e = rows.Scan(&raw); e != nil {
			rows.Close()
			RespondError(w, r, e)
			return
		}
		items = append(items, frontendRedact(pm(raw)))
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !frontendCandidatesComplete(w, r, len(items)) {
		return
	}
	sort.SliceStable(items, func(i, j int) bool {
		x, y := items[i].(map[string]any), items[j].(map[string]any)
		if Str(x, "createdAt") == Str(y, "createdAt") {
			return Str(x, "operationId") < Str(y, "operationId")
		}
		return Str(x, "createdAt") > Str(y, "createdAt")
	})
	a.Page(w, r, items)
}

func (a *App) frontendPeer(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	id := chi.URLParam(r, "peerId")
	var ns, addr, typ, scope, name, state string
	var rev, at int64
	e := a.Management.QueryRowContext(r.Context(), "SELECT namespace_id,device_address,address_type,scope_id,display_name,state,revision,created_at_ms FROM ble_peers WHERE peer_id=? AND namespace_id=?", id, a.Config.NamespaceID).Scan(&ns, &addr, &typ, &scope, &name, &state, &rev, &at)
	if e != nil || !a.Can(p, "gatt:read", scope) {
		Fail(w, r, 404, "NOT_FOUND", "peer unavailable")
		return
	}
	w.Header().Set("ETag", ETag(rev))
	JSON(w, 200, peerView(id, ns, addr, typ, scope, name, state, rev, at))
}
func (a *App) frontendAutoRuleView(r *http.Request, p *Principal, id string) (map[string]any, error) {
	var raw, scope string
	var rev, at int64
	e := a.Management.QueryRowContext(r.Context(), "SELECT a.rule_json,a.revision,a.created_at_ms,p.scope_id FROM auto_connect_rules a JOIN ble_peers p ON p.peer_id=a.peer_id WHERE a.rule_id=? AND p.namespace_id=?", id, a.Config.NamespaceID).Scan(&raw, &rev, &at, &scope)
	if e != nil || !a.Can(p, "gatt:read", scope) {
		return nil, Problem(404, "NOT_FOUND", "rule unavailable")
	}
	m := pm(raw)
	for _, g := range List(m, "gatewayIds") {
		if !a.CanGateway(p, "gatt:read", g.(string)) {
			return nil, Problem(404, "NOT_FOUND", "rule unavailable")
		}
	}
	return autoRuleView(id, rev, at, m), nil
}
func (a *App) frontendAutoRule(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	m, e := a.frontendAutoRuleView(r, p, chi.URLParam(r, "ruleId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(Num(m, "revision")))
	JSON(w, 200, m)
}
func (a *App) frontendSubscriptions(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	gw, ref := r.URL.Query().Get("gatewayId"), r.URL.Query().Get("connectionRef")
	if gw != "" && !a.CanGateway(p, "gatt:read", gw) {
		Fail(w, r, 404, "NOT_FOUND", "gateway unavailable")
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), `SELECT gateway_id,connection_ref,service_uuid,characteristic_uuid,characteristic_handle,mode,state,updated_at_ms FROM gatt_subscriptions WHERE namespace_id=? AND (?='' OR gateway_id=?) AND (?='' OR connection_ref=?) ORDER BY updated_at_ms, gateway_id,connection_ref,service_uuid,characteristic_uuid LIMIT 10001`, a.Config.NamespaceID, gw, gw, ref, ref)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for rows.Next() {
		var g, c, s, characteristic, mode, state string
		var handle, at int64
		if e = rows.Scan(&g, &c, &s, &characteristic, &handle, &mode, &state, &at); e != nil {
			rows.Close()
			RespondError(w, r, e)
			return
		}
		items = append(items, map[string]any{"subscriptionId": pipeStable("subscription", []string{a.Config.NamespaceID, g, c, s, characteristic}), "namespaceId": a.Config.NamespaceID, "gatewayId": g, "connectionRef": c, "serviceUuid": s, "characteristicUuid": characteristic, "characteristicHandle": handle, "mode": mode, "state": strings.ToUpper(state), "updatedAt": Timestamp(at)})
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !frontendCandidatesComplete(w, r, len(items)) {
		return
	}
	visible := []any{}
	for _, v := range items {
		if a.CanGateway(p, "gatt:read", Str(v.(map[string]any), "gatewayId")) {
			visible = append(visible, v)
		}
	}
	a.Page(w, r, visible)
}
func (a *App) frontendTriggers(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	gw, rule := r.URL.Query().Get("gatewayId"), r.URL.Query().Get("ruleId")
	if gw != "" && !a.CanGateway(p, "gatt:read", gw) {
		Fail(w, r, 404, "NOT_FOUND", "gateway unavailable")
		return
	}
	if rule != "" {
		if _, e := a.frontendAutoRuleView(r, p, rule); e != nil {
			RespondError(w, r, e)
			return
		}
	}
	rows, e := a.Management.QueryContext(r.Context(), `SELECT trigger_id,gateway_id,rule_id,rule_revision,adv_event_id,uuid_event_id,service_uuid,task_id,created_at_ms FROM auto_connect_triggers WHERE namespace_id=? AND (?='' OR gateway_id=?) AND (?='' OR rule_id=?) ORDER BY created_at_ms DESC,trigger_id LIMIT 10001`, a.Config.NamespaceID, gw, gw, rule, rule)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for rows.Next() {
		var id, g, rid, adv, uuid, service, task string
		var rev, at int64
		if e = rows.Scan(&id, &g, &rid, &rev, &adv, &uuid, &service, &task, &at); e != nil {
			rows.Close()
			RespondError(w, r, e)
			return
		}
		items = append(items, map[string]any{"triggerId": id, "namespaceId": a.Config.NamespaceID, "gatewayId": g, "ruleId": rid, "ruleRevision": rev, "advEventId": adv, "uuidEventId": uuid, "serviceUuid": service, "taskId": task, "createdAt": Timestamp(at)})
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !frontendCandidatesComplete(w, r, len(items)) {
		return
	}
	visible := []any{}
	for _, v := range items {
		m := v.(map[string]any)
		if !a.CanGateway(p, "gatt:read", Str(m, "gatewayId")) {
			continue
		}
		if _, e := a.frontendAutoRuleView(r, p, Str(m, "ruleId")); e != nil {
			continue
		}
		visible = append(visible, m)
	}
	a.Page(w, r, visible)
}
