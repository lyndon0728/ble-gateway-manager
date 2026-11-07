package platform

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

func scopeView(id, kind, parent, name, state string, revision int64) map[string]any {
	m := map[string]any{"scopeId": id, "kind": strings.ToUpper(kind), "name": name, "state": strings.ToUpper(state), "revision": revision}
	if parent != "" {
		m["parentScopeId"] = parent
	}
	return m
}
func (a *App) listScopes(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "scope:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT scope_id,kind,COALESCE(parent_scope_id,''),name,status,version FROM resource_scopes WHERE (?='' OR parent_scope_id=?) AND (?='' OR kind=?) ORDER BY created_at_ms,scope_id LIMIT 10001", r.URL.Query().Get("parentScopeId"), r.URL.Query().Get("parentScopeId"), strings.ToLower(r.URL.Query().Get("kind")), strings.ToLower(r.URL.Query().Get("kind")))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	candidates := []map[string]any{}
	for rows.Next() {
		var id, kind, parent, name, state string
		var revision int64
		if e = rows.Scan(&id, &kind, &parent, &name, &state, &revision); e != nil {
			RespondError(w, r, e)
			return
		}
		candidates = append(candidates, scopeView(id, kind, parent, name, state, revision))
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	// Authorization reads the same database. Release the cursor before asking
	// for another connection, including when the configured pool has size one.
	if !frontendCandidatesComplete(w, r, len(candidates)) {
		return
	}
	global := canQuery(r.Context(), a.Management, p, "scope:read", "scope_instance")
	items := []any{}
	for _, candidate := range candidates {
		if global || canQuery(r.Context(), a.Management, p, "scope:read", Str(candidate, "scopeId")) {
			items = append(items, candidate)
		}
	}
	a.Page(w, r, items)
}
func (a *App) createScope(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "scope:write")
	if !ok {
		return
	}
	m, ok := a.Decode(w, r, "ResourceScopeCreate")
	if !ok {
		return
	}
	parent := Str(m, "parentScopeId")
	if !a.Can(p, "scope:write", parent) {
		Fail(w, r, 403, "FORBIDDEN", "parent scope is not writable")
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "scope.create", r.Header.Get("Idempotency-Key"), m, func(tx *sql.Tx) (int, string, any, error) {
		var kind, state string
		if e := tx.QueryRow("SELECT kind,status FROM resource_scopes WHERE scope_id=?", parent).Scan(&kind, &state); e != nil {
			return 0, "", nil, e
		}
		expected := map[string]string{"PROJECT": "instance", "GROUP": "project"}[Str(m, "kind")]
		if kind != expected || state != "active" {
			return 0, "", nil, Problem(422, "INVALID_PARENT", "PROJECT requires active INSTANCE parent; GROUP requires active PROJECT parent")
		}
		var n int
		if e := tx.QueryRow("SELECT count(*) FROM resource_scopes WHERE parent_scope_id=? AND name=?", parent, Str(m, "name")).Scan(&n); e != nil {
			return 0, "", nil, e
		}
		if n > 0 {
			return 0, "", nil, Problem(409, "NAME_EXISTS", "scope name exists under this parent")
		}
		id := ID("scope")
		if _, e := tx.Exec("INSERT INTO resource_scopes VALUES(?,?,?,?,'active',1,?)", id, strings.ToLower(Str(m, "kind")), parent, Str(m, "name"), Now()); e != nil {
			return 0, "", nil, e
		}
		if e := a.Audit(r.Context(), tx, p.ID, "scope.create", "scope", id, map[string]any{"parentScopeId": parent}); e != nil {
			return 0, "", nil, e
		}
		return 201, id, scopeView(id, Str(m, "kind"), parent, Str(m, "name"), "active", 1), nil
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(1))
	JSON(w, status, v)
}
func (a *App) patchScope(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "scope:write")
	if !ok {
		return
	}
	m, ok := a.Decode(w, r, "ResourceScopePatch")
	if !ok {
		return
	}
	id := chi.URLParam(r, "scopeId")
	if !a.Can(p, "scope:write", id) && !a.Can(p, "scope:write", "scope_instance") {
		Fail(w, r, 404, "NOT_FOUND", "scope not found")
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	var kind, parent, name, state string
	var revision int64
	e = tx.QueryRow("SELECT kind,COALESCE(parent_scope_id,''),name,status,version FROM resource_scopes WHERE scope_id=?", id).Scan(&kind, &parent, &name, &state, &revision)
	if e != nil {
		Fail(w, r, 404, "NOT_FOUND", "scope not found")
		return
	}
	if e = Match(r, revision); e != nil {
		RespondError(w, r, e)
		return
	}
	if v := Str(m, "name"); v != "" {
		name = v
	}
	if v := Str(m, "state"); v != "" {
		state = strings.ToLower(v)
	}
	if state == "archived" {
		if kind == "instance" {
			Fail(w, r, 409, "INSTANCE_REQUIRED", "instance scope cannot be archived")
			return
		}
		var n int
		e = tx.QueryRow("SELECT (SELECT count(*) FROM resource_scopes WHERE parent_scope_id=? AND status='active')+(SELECT count(*) FROM gateways WHERE scope_id=?)+(SELECT count(*) FROM role_bindings WHERE scope_id=?)", id, id, id).Scan(&n)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		if n > 0 {
			Fail(w, r, 409, "SCOPE_IN_USE", "move gateways and resolve children and bindings before archiving")
			return
		}
	} else if parent != "" {
		var status string
		if tx.QueryRow("SELECT status FROM resource_scopes WHERE scope_id=?", parent).Scan(&status) != nil || status != "active" {
			Fail(w, r, 409, "PARENT_ARCHIVED", "activate parent scope first")
			return
		}
	}
	var duplicates int
	if e = tx.QueryRow("SELECT count(*) FROM resource_scopes WHERE parent_scope_id=? AND name=? AND scope_id<>?", parent, name, id).Scan(&duplicates); e != nil {
		RespondError(w, r, e)
		return
	}
	if duplicates > 0 {
		Fail(w, r, 409, "NAME_EXISTS", "scope name exists under parent")
		return
	}
	_, e = tx.Exec("UPDATE resource_scopes SET name=?,status=?,version=version+1 WHERE scope_id=?", name, state, id)
	if e == nil {
		e = a.Audit(r.Context(), tx, p.ID, "scope.update", "scope", id, map[string]any{"name": name, "state": state})
	}
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(revision+1))
	JSON(w, 200, scopeView(id, kind, parent, name, state, revision+1))
}

func (a *App) listAudit(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "audit:read")
	if !ok {
		return
	}
	from, to := int64(0), Now()
	for key, dest := range map[string]*int64{"from": &from, "to": &to} {
		if v := r.URL.Query().Get(key); v != "" {
			t, e := time.Parse(time.RFC3339Nano, v)
			if e != nil {
				Fail(w, r, 400, "INVALID_TIME", "from/to must be RFC3339 timestamps")
				return
			}
			*dest = t.UnixMilli()
		}
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT audit_id,actor_id,action,object_type,object_id,outcome,correlation_id,detail_json,occurred_at_ms FROM audit_events WHERE occurred_at_ms BETWEEN ? AND ? ORDER BY occurred_at_ms,audit_id LIMIT 10001", from, to)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	candidates := []map[string]any{}
	for rows.Next() {
		var id, actor, action, typ, resource, outcome, trace, details string
		var at int64
		if e = rows.Scan(&id, &actor, &action, &typ, &resource, &outcome, &trace, &details, &at); e != nil {
			RespondError(w, r, e)
			return
		}
		var changes map[string]any
		_ = json.Unmarshal([]byte(details), &changes)
		outcome = strings.ToUpper(outcome)
		if outcome == "SUCCEEDED" {
			outcome = "SUCCESS"
		}
		candidates = append(candidates, map[string]any{"auditId": id, "actorId": actor, "action": action, "resourceType": typ, "resourceId": resource, "outcome": outcome, "traceId": trace, "at": Timestamp(at), "changes": changes})
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	if !frontendCandidatesComplete(w, r, len(candidates)) {
		return
	}
	global := canQuery(r.Context(), a.Management, p, "audit:read", "scope_instance")
	items := []any{}
	for _, candidate := range candidates {
		allowed := global
		resource := Str(candidate, "resourceId")
		if !allowed && Str(candidate, "resourceType") == "gateway" {
			var scope string
			if a.Management.QueryRowContext(r.Context(), "SELECT scope_id FROM gateways WHERE gateway_id=?", resource).Scan(&scope) == nil {
				allowed = canQuery(r.Context(), a.Management, p, "audit:read", scope)
			}
		} else if !allowed && Str(candidate, "resourceType") == "scope" {
			allowed = canQuery(r.Context(), a.Management, p, "audit:read", resource)
		}
		if allowed {
			items = append(items, candidate)
		}
	}
	a.Page(w, r, items)
}
func (a *App) systemStatus(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "system:read")
	if !ok {
		return
	}
	if !a.Can(p, "system:read", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "instance scope required for infrastructure statistics")
		return
	}
	components := []any{}
	ready := true
	for name, db := range map[string]*sql.DB{"management": a.Management, "pipeline": a.Pipeline} {
		state := "READY"
		if db.PingContext(r.Context()) != nil {
			state = "NOT_READY"
			ready = false
		}
		components = append(components, map[string]any{"name": name, "state": state})
	}
	var count int
	_ = a.Management.QueryRowContext(r.Context(), "SELECT count(*) FROM instance_settings").Scan(&count)
	if count != 1 {
		ready = false
		components = append(components, map[string]any{"name": "bootstrap", "state": "NOT_READY"})
	}
	nodes := []any{}
	var bundle string
	var revision int64
	if e := a.Pipeline.QueryRowContext(r.Context(), "SELECT bundle_id,activation_epoch FROM pipeline_bundles WHERE state='active' AND namespace_id=?", a.Config.NamespaceID).Scan(&bundle, &revision); e == nil {
		nodes = append(nodes, map[string]any{"dataNodeId": a.Config.NodeID, "activeBundleId": bundle, "activationRevision": revision, "activationEtag": fmt.Sprintf("\"activation-%d\"", revision)})
	}
	var bytes, oldest int64
	_ = a.Pipeline.QueryRowContext(r.Context(), "SELECT COALESCE(sum(length(wire_payload)),0)+(SELECT COALESCE(sum(payload_bytes),0) FROM raw_events),COALESCE(min(created_at_ms),0) FROM delivery_outbox WHERE state NOT IN ('delivered','terminated')").Scan(&bytes, &oldest)
	age := int64(0)
	if oldest > 0 && oldest < Now() {
		age = (Now() - oldest) / 1000
	}
	free, e := diskFree(a.Config.DataDir)
	if e != nil {
		ready = false
		components = append(components, map[string]any{"name": "disk", "state": "NOT_READY", "code": "DISK_STATS_UNAVAILABLE"})
	}
	storage, storageErr := a.pipelineStorageStats()
	if storageErr != nil {
		ready = false
		components = append(components, map[string]any{"name": "data-capacity", "state": "NOT_READY", "code": "DISK_STATS_UNAVAILABLE"})
	} else if code := pipelineStorageCheck(storage, 65536, a.Config.MaxQueueBytes); code != "" {
		ready = false
		components = append(components, map[string]any{"name": "data-capacity", "state": "DEGRADED", "code": code})
	}
	JSON(w, 200, map[string]any{"instanceId": a.Config.InstanceID, "softwareVersion": Version, "ready": ready, "components": components, "dataNodes": nodes, "queueBytes": bytes, "oldestPendingAgeSeconds": age, "diskFreeBytes": free, "storageBytes": storage.Stored, "storageLimitBytes": a.Config.MaxQueueBytes, "diskReserveBytes": pipelineDiskReserve})
}
