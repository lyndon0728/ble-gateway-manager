package platform

import (
	"archive/zip"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/go-chi/chi/v5"
)

func operationView(id, kind, state string, created, finished int64, result map[string]any) map[string]any {
	progress := 0.0
	if state == "succeeded" || state == "failed" || state == "cancelled" {
		progress = 1
	}
	api := strings.ToUpper(state)
	if state == "pending" {
		api = "QUEUED"
	}
	return map[string]any{"operationId": id, "kind": strings.ToUpper(kind), "state": api, "createdAt": Timestamp(created), "progress": progress, "resourceIds": []any{}, "result": result}
}

func (a *App) createMaintenance(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "system:maintain")
	if !ok {
		return
	}
	if !a.Can(p, "system:maintain", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "instance maintenance permission required")
		return
	}
	body, ok := a.Decode(w, r, "MaintenanceRequest")
	if !ok {
		return
	}
	if Str(body, "kind") == "BACKUP" {
		Fail(w, r, 409, "MAINTENANCE_OFFLINE_REQUIRED", "stop the service and run gatewayctl backup for a coherent cross-database snapshot")
		return
	}
	if raw, _ := body["includeRawPayload"].(bool); raw {
		Fail(w, r, 422, "RAW_DIAGNOSTIC_UNSUPPORTED", "diagnostic exports contain metadata only; use scoped ingress record export for payloads")
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "maintenance.create", r.Header.Get("Idempotency-Key"), body, func(tx *sql.Tx) (int, string, any, error) {
		id, now := ID("job"), Now()
		result, ref, digest, e := a.writeDiagnostic(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, e
		}
		_, e = tx.ExecContext(r.Context(), "INSERT INTO maintenance_jobs(job_id,kind,created_by,request_json,state,checkpoint_json,result_ref,manifest_hash,created_at_ms,finished_at_ms) VALUES(?,'diagnostic',?,?,'succeeded',?,?,?,?,?)", id, p.ID, encode(body), encode(result), ref, digest, now, Now())
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "diagnostic.export", "operation", id, map[string]any{"includeRawPayload": false, "sha256": digest})
		}
		if e != nil {
			_ = os.Remove(filepath.Join(a.Config.DataDir, "diagnostics", ref))
			return 0, "", nil, e
		}
		return 202, id, operationView(id, "diagnostic", "succeeded", now, Now(), result), nil
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}
func (a *App) writeDiagnostic(ctx context.Context, tx *sql.Tx, id string) (map[string]any, string, string, error) {
	data := map[string]any{"softwareVersion": Version, "generatedAt": Timestamp(Now()), "instanceId": a.Config.InstanceID, "namespaceId": a.Config.NamespaceID, "role": a.Config.Role, "containsRawPayload": false, "containsCredentials": false}
	management := map[string]any{}
	for _, table := range []string{"gateways", "command_tasks", "command_results", "command_outbox", "artifacts", "audit_events"} {
		var count int64
		if e := tx.QueryRowContext(ctx, "SELECT count(*) FROM "+table).Scan(&count); e != nil {
			return nil, "", "", e
		}
		management[table] = count
	}
	pipeline := map[string]any{}
	for _, table := range []string{"event_receipts", "raw_events", "processing_jobs", "processing_results", "delivery_outbox"} {
		var count int64
		if e := a.Pipeline.QueryRowContext(ctx, "SELECT count(*) FROM "+table).Scan(&count); e == nil {
			pipeline[table] = count
		}
	}
	data["managementCounts"] = management
	data["pipelineCounts"] = pipeline
	dir := filepath.Join(a.Config.DataDir, "diagnostics")
	if e := os.MkdirAll(dir, 0700); e != nil {
		return nil, "", "", e
	}
	ref := id + ".zip"
	path := filepath.Join(dir, ref)
	f, e := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if e != nil {
		return nil, "", "", e
	}
	h := sha256.New()
	z := zip.NewWriter(io.MultiWriter(f, h))
	entry, e := z.Create("diagnostic.json")
	if e == nil {
		_, e = entry.Write([]byte(encode(data)))
	}
	e = errors.Join(e, z.Close())
	if e == nil {
		e = f.Sync()
	}
	e = errors.Join(e, f.Close())
	if e != nil {
		_ = os.Remove(path)
		return nil, "", "", e
	}
	digest := hex.EncodeToString(h.Sum(nil))
	return map[string]any{"downloadPath": "/api/v1/operations/" + id + "/content", "sha256": digest, "containsRawPayload": false}, ref, digest, nil
}
func (a *App) getOperation(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "operation:read")
	if !ok {
		return
	}
	id := chi.URLParam(r, "operationId")
	var kind, state, owner, raw string
	var created int64
	var finished sql.NullInt64
	e := a.Management.QueryRowContext(r.Context(), "SELECT kind,state,created_by,checkpoint_json,created_at_ms,finished_at_ms FROM maintenance_jobs WHERE job_id=?", id).Scan(&kind, &state, &owner, &raw, &created, &finished)
	if errors.Is(e, sql.ErrNoRows) {
		var response string
		e = a.Management.QueryRowContext(r.Context(), "SELECT principal_id,response_json FROM api_idempotency WHERE (operation_id=? OR resource_id=?) AND state='committed' AND resource_type='pipeline' LIMIT 1", id, id).Scan(&owner, &response)
		if e == nil {
			if owner != p.ID && !a.Can(p, "system:maintain", "scope_instance") {
				Fail(w, r, 403, "FORBIDDEN", "operation owner permission required")
				return
			}
			var v map[string]any
			if json.Unmarshal([]byte(response), &v) == nil {
				JSON(w, 200, v)
				return
			}
		}
	}
	if errors.Is(e, sql.ErrNoRows) {
		Fail(w, r, 404, "OPERATION_NOT_FOUND", "operation not found")
		return
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if owner != p.ID && !a.Can(p, "system:maintain", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "operation owner permission required")
		return
	}
	result := map[string]any{}
	_ = json.Unmarshal([]byte(raw), &result)
	JSON(w, 200, operationView(id, kind, state, created, finished.Int64, result))
}
func (a *App) downloadDiagnostic(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "system:maintain")
	if !ok {
		return
	}
	if !a.Can(p, "system:maintain", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "instance maintenance permission required")
		return
	}
	var ref, digest string
	e := a.Management.QueryRowContext(r.Context(), "SELECT result_ref,manifest_hash FROM maintenance_jobs WHERE job_id=? AND kind='diagnostic' AND state='succeeded'", chi.URLParam(r, "operationId")).Scan(&ref, &digest)
	if e != nil || filepath.Base(ref) != ref {
		Fail(w, r, 404, "DIAGNOSTIC_NOT_FOUND", "diagnostic not found")
		return
	}
	f, e := os.Open(filepath.Join(a.Config.DataDir, "diagnostics", ref))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer f.Close()
	st, e := f.Stat()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("X-Content-SHA256", digest)
	w.Header().Set("Content-Disposition", `attachment; filename="`+ref+`"`)
	http.ServeContent(w, r, ref, st.ModTime(), f)
}
