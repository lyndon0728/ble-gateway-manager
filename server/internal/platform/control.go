package platform

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

type ctlQuery interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

func (a *App) RegisterControl(r chi.Router) {
	r.Get("/api/v1/gateways", a.listGateways)
	r.Get("/api/v1/gateways/{gatewayId}", a.getGateway)
	r.Patch("/api/v1/gateways/{gatewayId}", a.patchGateway)
	r.Get("/api/v1/gateways/{gatewayId}/configuration", a.getConfiguration)
	r.Put("/api/v1/gateways/{gatewayId}/configuration", a.putConfiguration)
	r.Post("/api/v1/tasks", a.createTask)
	r.Get("/api/v1/tasks", a.listTasks)
	r.Get("/api/v1/tasks/{taskId}", a.getTask)
	r.Post("/api/v1/tasks/{taskId}/cancellation", a.cancelTask)
	r.Post("/api/v1/gateway-ingress/results", a.ingressResults)
	r.Post("/api/v1/gateway-ingress/status", a.ingressStatus)
	r.Get("/api/v1/gateway-ingress/commands", a.ingressCommands)
	r.Post("/api/v1/artifacts", a.uploadArtifact)
	r.Get("/api/v1/artifacts/{artifactId}", a.getArtifact)
	r.Get("/api/v1/artifacts/{artifactId}/content", a.downloadArtifact)
	r.Get("/api/v1/artifacts/{artifactId}/verification", a.artifactVerification)
	r.Post("/api/v1/gateway-imports", a.importGateways)
	r.Post("/api/v1/maintenance/jobs", a.createMaintenance)
	r.Get("/api/v1/operations/{operationId}", a.getOperation)
	r.Get("/api/v1/operations/{operationId}/content", a.downloadDiagnostic)
}

func (a *App) gatewayView(ctx context.Context, q ctlQuery, id string) (map[string]any, error) {
	var sn, name, scope, model, hw, state, caps string
	var fw sql.NullString
	var seen sql.NullInt64
	var revision int64
	e := q.QueryRowContext(ctx, "SELECT serial_number,display_name,scope_id,model,hardware_version,firmware_version,lifecycle,capabilities_json,last_seen_at_ms,version FROM gateways WHERE gateway_id=?", id).Scan(&sn, &name, &scope, &model, &hw, &fw, &state, &caps, &seen, &revision)
	if errors.Is(e, sql.ErrNoRows) {
		return nil, Problem(404, "GATEWAY_NOT_FOUND", "gateway not found")
	}
	if e != nil {
		return nil, e
	}
	capabilities := []any{}
	_ = json.Unmarshal([]byte(caps), &capabilities)
	m := map[string]any{"gatewayId": id, "namespaceId": a.Config.NamespaceID, "sn": sn, "displayName": name, "groupId": scope, "model": model, "hardwareVersion": hw, "state": strings.ToUpper(state), "capabilities": capabilities, "online": seen.Valid && Now()-seen.Int64 < 90000, "revision": revision}
	if fw.Valid && fw.String != "" {
		m["firmwareVersion"] = fw.String
	}
	if seen.Valid {
		m["lastSeenAt"] = Timestamp(seen.Int64)
	}
	return m, nil
}
func (a *App) listGateways(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gateway:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT gateway_id FROM gateways ORDER BY gateway_id")
	if e != nil {
		RespondError(w, r, e)
		return
	}
	ids := []string{}
	for rows.Next() {
		var id string
		if rows.Scan(&id) == nil {
			ids = append(ids, id)
		}
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for _, id := range ids {
		if !a.CanGateway(p, "gateway:read", id) {
			continue
		}
		m, e := a.gatewayView(r.Context(), a.Management, id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		if group := r.URL.Query().Get("groupId"); group != "" && group != Str(m, "groupId") {
			continue
		}
		if state := r.URL.Query().Get("state"); state != "" && state != Str(m, "state") {
			continue
		}
		items = append(items, m)
	}
	a.Page(w, r, items)
}
func (a *App) getGateway(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gateway:read")
	if !ok {
		return
	}
	id := chi.URLParam(r, "gatewayId")
	if !a.CanGateway(p, "gateway:read", id) {
		Fail(w, r, 403, "FORBIDDEN", "gateway scope denied")
		return
	}
	m, e := a.gatewayView(r.Context(), a.Management, id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(Num(m, "revision")))
	JSON(w, 200, m)
}
func (a *App) patchGateway(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gateway:write")
	if !ok {
		return
	}
	id := chi.URLParam(r, "gatewayId")
	if !a.CanGateway(p, "gateway:write", id) {
		Fail(w, r, 403, "FORBIDDEN", "gateway scope denied")
		return
	}
	body, ok := a.Decode(w, r, "GatewayPatch")
	if !ok {
		return
	}
	if s := Str(body, "groupId"); s != "" && !a.Can(p, "gateway:write", s) {
		Fail(w, r, 403, "FORBIDDEN", "destination group denied")
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	m, e := a.gatewayView(r.Context(), tx, id)
	if e == nil {
		e = Match(r, Num(m, "revision"))
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if Str(m, "state") == "RETIRED" && Str(body, "state") != "" && Str(body, "state") != "RETIRED" {
		Fail(w, r, 409, "GATEWAY_RETIRED", "retired gateway cannot be reactivated")
		return
	}
	if scope := Str(body, "groupId"); scope != "" {
		var kind, state string
		if e := tx.QueryRowContext(r.Context(), "SELECT kind,status FROM resource_scopes WHERE scope_id=?", scope).Scan(&kind, &state); e != nil || kind != "group" || state != "active" {
			Fail(w, r, 422, "INVALID_GROUP", "an active group is required")
			return
		}
	}
	for _, k := range []string{"displayName", "groupId", "state"} {
		if v, exists := body[k]; exists {
			m[k] = v
		}
	}
	if Str(m, "state") == "RETIRED" {
		_, e = tx.ExecContext(r.Context(), "UPDATE credentials SET status='revoked',revoked_at_ms=? WHERE gateway_id=? AND status='active'", Now(), id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
	}
	_, e = tx.ExecContext(r.Context(), "UPDATE gateways SET display_name=?,scope_id=?,lifecycle=?,version=version+1 WHERE gateway_id=?", Str(m, "displayName"), Str(m, "groupId"), strings.ToLower(Str(m, "state")), id)
	if e == nil {
		e = a.Audit(r.Context(), tx, p.ID, "gateway.update", "gateway", id, body)
	}
	if e == nil {
		m, e = a.gatewayView(r.Context(), tx, id)
	}
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(Num(m, "revision")))
	JSON(w, 200, m)
}

func (a *App) configurationView(ctx context.Context, q ctlQuery, id string) (map[string]any, error) {
	var desired, reported string
	var revision int64
	e := q.QueryRowContext(ctx, "SELECT revision,desired_json FROM gateway_configs WHERE gateway_id=? ORDER BY revision DESC LIMIT 1", id).Scan(&revision, &desired)
	if errors.Is(e, sql.ErrNoRows) {
		return nil, Problem(404, "CONFIGURATION_NOT_FOUND", "gateway configuration not found")
	}
	if e != nil {
		return nil, e
	}
	var rep int64
	e = q.QueryRowContext(ctx, "SELECT revision,reported_json FROM gateway_configs WHERE gateway_id=? AND state='verified' AND reported_json IS NOT NULL ORDER BY revision DESC LIMIT 1", id).Scan(&rep, &reported)
	if e != nil && !errors.Is(e, sql.ErrNoRows) {
		return nil, e
	}
	dm, rm := map[string]any{}, map[string]any{}
	_ = json.Unmarshal([]byte(desired), &dm)
	_ = json.Unmarshal([]byte(reported), &rm)
	m := map[string]any{"gatewayId": id, "desiredRevision": revision, "reportedRevision": rep, "desired": dm, "reported": rm}
	var task string
	if q.QueryRowContext(ctx, "SELECT task_id FROM command_tasks WHERE gateway_id=? AND operation='CONFIGURE' ORDER BY created_at_ms DESC LIMIT 1", id).Scan(&task) == nil {
		m["lastTaskId"] = task
	}
	return m, nil
}
func (a *App) getConfiguration(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gateway:read")
	if !ok {
		return
	}
	id := chi.URLParam(r, "gatewayId")
	if !a.CanGateway(p, "gateway:read", id) {
		Fail(w, r, 403, "FORBIDDEN", "gateway scope denied")
		return
	}
	m, e := a.configurationView(r.Context(), a.Management, id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(Num(m, "desiredRevision")))
	JSON(w, 200, m)
}
func (a *App) putConfiguration(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gateway:configure")
	if !ok {
		return
	}
	id := chi.URLParam(r, "gatewayId")
	if !a.CanGateway(p, "gateway:configure", id) || !a.CanGateway(p, "task:create", id) {
		Fail(w, r, 403, "FORBIDDEN", "configuration scope denied")
		return
	}
	body, ok := a.Decode(w, r, "ConfigurationUpdate")
	if !ok {
		return
	}
	req := map[string]any{"gatewayId": id, "type": "CONFIGURE", "parameters": map[string]any{"configuration": Obj(body, "configuration"), "expectedReportedRevision": Num(body, "expectedReportedRevision")}, "expiresAt": Str(body, "expiresAt")}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "configuration.put", r.Header.Get("Idempotency-Key"), map[string]any{"request": req, "ifMatch": r.Header.Get("If-Match")}, func(tx *sql.Tx) (int, string, any, error) {
		m, e := a.configurationView(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, e
		}
		if e = Match(r, Num(m, "desiredRevision")); e != nil {
			return 0, "", nil, e
		}
		return a.newTask(r.Context(), tx, p.ID, r.Header.Get("Idempotency-Key"), req)
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}

var taskPermission = map[string]string{"CONFIGURE": "gateway:configure", "REBOOT": "gateway:reboot", "DIAGNOSTICS": "diagnostics:run", "FIRMWARE_UPGRADE": "firmware:deploy", "GATT_READ": "gatt:execute", "GATT_WRITE": "gatt:execute"}

func (a *App) createTask(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "task:create")
	if !ok {
		return
	}
	body, ok := a.Decode(w, r, "TaskRequest")
	if !ok {
		return
	}
	id := Str(body, "gatewayId")
	if !a.CanGateway(p, "task:create", id) || !a.CanGateway(p, taskPermission[Str(body, "type")], id) {
		Fail(w, r, 403, "FORBIDDEN", "task scope or operation denied")
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "task.create", r.Header.Get("Idempotency-Key"), body, func(tx *sql.Tx) (int, string, any, error) {
		return a.newTask(r.Context(), tx, p.ID, r.Header.Get("Idempotency-Key"), body)
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}
func (a *App) newTask(ctx context.Context, tx *sql.Tx, actor, key string, req map[string]any) (int, string, any, error) {
	gateway, op := Str(req, "gatewayId"), Str(req, "type")
	params := map[string]any{}
	_ = json.Unmarshal([]byte(encode(Obj(req, "parameters"))), &params)
	t, e := time.Parse(time.RFC3339Nano, Str(req, "expiresAt"))
	if e != nil || t.UnixMilli() <= Now() || t.After(time.Now().Add(7*24*time.Hour)) {
		return 0, "", nil, Problem(400, "INVALID_EXPIRY", "expiresAt must be in the next seven days")
	}
	g, e := a.gatewayView(ctx, tx, gateway)
	if e != nil {
		return 0, "", nil, e
	}
	if Str(g, "state") != "ACTIVE" {
		return 0, "", nil, Problem(409, "GATEWAY_INACTIVE", "gateway is not active")
	}
	supported := false
	for _, x := range List(g, "capabilities") {
		if x == op {
			supported = true
		}
	}
	if !supported {
		return 0, "", nil, Problem(422, "CAPABILITY_UNSUPPORTED", "signed gateway capability does not support this operation")
	}
	var active int
	e = tx.QueryRowContext(ctx, "SELECT count(*) FROM command_tasks WHERE gateway_id=? AND state IN ('queued','sent','device_accepted','executing','applied','unknown')", gateway).Scan(&active)
	if e != nil {
		return 0, "", nil, e
	}
	if active > 0 {
		return 0, "", nil, Problem(409, "TASK_IN_PROGRESS", "resolve the outstanding gateway task before creating another")
	}
	var configID, artifactID any
	risk := "idempotent"
	if op == "CONFIGURE" {
		if e = ValidateGatewayConfiguration(Obj(params, "configuration")); e != nil {
			return 0, "", nil, e
		}
		c, e := a.configurationView(ctx, tx, gateway)
		if e != nil {
			return 0, "", nil, e
		}
		if Num(c, "reportedRevision") != Num(params, "expectedReportedRevision") {
			return 0, "", nil, Problem(409, "CONFIGURATION_CONFLICT", "reported revision changed")
		}
		rev := Num(c, "desiredRevision") + 1
		configID = ID("cfg")
		_, e = tx.ExecContext(ctx, "INSERT INTO gateway_configs(config_id,gateway_id,revision,base_revision,desired_json,content_hash,state,created_by,created_at_ms) VALUES(?,?,?,?,?,?,'desired',?,?)", configID, gateway, rev, Num(c, "reportedRevision"), encode(Obj(params, "configuration")), Hash(Obj(params, "configuration")), actor, Now())
		if e != nil {
			return 0, "", nil, e
		}
		params["desiredRevision"] = rev
		risk = "readback"
	}
	if op == "REBOOT" || op == "GATT_WRITE" {
		risk = "nonrepeatable"
	}
	if op == "FIRMWARE_UPGRADE" {
		artifactID = Str(params, "artifactId")
		if e = a.checkFirmware(ctx, tx, Str(params, "artifactId"), g, Str(params, "expectedFirmwareVersion")); e != nil {
			return 0, "", nil, e
		}
		risk = "readback"
	}
	id, now := ID("task"), Now()
	command := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": gateway, "taskId": id, "type": op, "parameters": params, "createdAt": Timestamp(now), "expiresAt": Str(req, "expiresAt"), "requestDigest": Hash(req)}
	_, e = tx.ExecContext(ctx, "INSERT INTO command_tasks(task_id,gateway_id,created_by,operation,idempotency_key,request_hash,request_json,config_id,artifact_id,state,risk_class,expires_at_ms,created_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,'queued',?,?,?,?)", id, gateway, actor, op, key, Str(command, "requestDigest"), encode(command), configID, artifactID, risk, t.UnixMilli(), now, now)
	if e != nil {
		return 0, "", nil, e
	}
	_, e = tx.ExecContext(ctx, "INSERT INTO command_outbox(message_id,task_id,kind,payload_json,state,next_attempt_at_ms,created_at_ms) VALUES(?,?,'command',?,'pending',?,?)", ID("msg"), id, encode(command), now, now)
	if e == nil {
		e = a.Audit(ctx, tx, actor, "task.create", "task", id, map[string]any{"gatewayId": gateway, "type": op})
	}
	if e != nil {
		return 0, "", nil, e
	}
	v, e := a.taskView(ctx, tx, id)
	return 202, id, v, e
}

// Firmware accepts this versioned, deliberately small configuration surface.
// Unknown keys fail closed so an incompatible setting never appears applied.
func ValidateGatewayConfiguration(c map[string]any) error {
	allowed := map[string]map[string][2]int64{"scan": {"intervalMs": {10, 10240}, "windowMs": {10, 10240}}, "upload": {"batchMaxRecords": {1, 1000}, "flushIntervalMs": {100, 60000}}, "filters": {"minRssi": {-127, 20}}}
	for section, v := range c {
		fields, ok := allowed[section]
		if !ok {
			return Problem(422, "CONFIGURATION_UNSUPPORTED", "unknown configuration section: "+section)
		}
		m, ok := v.(map[string]any)
		if !ok {
			return Problem(422, "INVALID_CONFIGURATION", "configuration section must be object")
		}
		for k, x := range m {
			if section == "scan" && (k == "enabled" || k == "active") {
				if _, ok = x.(bool); !ok {
					return Problem(422, "INVALID_CONFIGURATION", "scan boolean required")
				}
				continue
			}
			if section == "filters" && (k == "serviceUuids" || k == "manufacturerIds") {
				list, ok := x.([]any)
				if !ok || len(list) > 128 {
					return Problem(422, "INVALID_CONFIGURATION", "filter array length must be 0..128")
				}
				for _, item := range list {
					if k == "serviceUuids" {
						s, ok := item.(string)
						if !ok || len(s) < 4 || len(s) > 36 {
							return Problem(422, "INVALID_CONFIGURATION", "invalid service UUID")
						}
					} else {
						n, ok := item.(float64)
						if !ok || n < 0 || n > 65535 || n != float64(int64(n)) {
							return Problem(422, "INVALID_CONFIGURATION", "invalid manufacturer identifier")
						}
					}
				}
				continue
			}
			bounds, ok := fields[k]
			if !ok {
				return Problem(422, "CONFIGURATION_UNSUPPORTED", "unknown configuration field: "+section+"."+k)
			}
			n, ok := x.(float64)
			if !ok {
				if integer, yes := x.(int64); yes {
					n = float64(integer)
					ok = true
				}
			}
			if !ok || n != float64(int64(n)) || int64(n) < bounds[0] || int64(n) > bounds[1] {
				return Problem(422, "INVALID_CONFIGURATION", "configuration value out of range: "+section+"."+k)
			}
		}
		if section == "scan" {
			i, w := Num(m, "intervalMs"), Num(m, "windowMs")
			if w > 0 && i > 0 && w > i {
				return Problem(422, "INVALID_CONFIGURATION", "scan window must not exceed interval")
			}
		}
	}
	return nil
}

func (a *App) taskView(ctx context.Context, q ctlQuery, id string) (map[string]any, error) {
	var gateway, op, state, digest, command string
	var created, expires, rev int64
	e := q.QueryRowContext(ctx, "SELECT gateway_id,operation,state,request_hash,request_json,created_at_ms,expires_at_ms,version FROM command_tasks WHERE task_id=?", id).Scan(&gateway, &op, &state, &digest, &command, &created, &expires, &rev)
	if errors.Is(e, sql.ErrNoRows) {
		return nil, Problem(404, "TASK_NOT_FOUND", "task not found")
	}
	if e != nil {
		return nil, e
	}
	stage := "NONE"
	apiState := strings.ToUpper(state)
	switch state {
	case "accepted", "queued":
		apiState = "QUEUED"
	case "sent":
		apiState = "DISPATCHED"
	case "device_accepted":
		apiState = "ACCEPTED"
		stage = "DEVICE_ACCEPTED"
	case "executing":
		stage = "EXECUTING"
	case "applied":
		apiState = "EXECUTING"
		stage = "APPLIED"
	case "verified":
		apiState = "SUCCEEDED"
		stage = "VERIFIED"
	case "succeeded":
		stage = "VERIFIED"
	}
	m := map[string]any{"taskId": id, "gatewayId": gateway, "type": op, "state": apiState, "executionStage": stage, "requestDigest": digest, "createdAt": Timestamp(created), "expiresAt": Timestamp(expires), "revision": rev}
	var result string
	if q.QueryRowContext(ctx, "SELECT result_json FROM command_results WHERE task_id=? ORDER BY CASE WHEN stage=? THEN 0 ELSE 1 END,sequence DESC LIMIT 1", id, state).Scan(&result) == nil {
		var v map[string]any
		if json.Unmarshal([]byte(result), &v) == nil {
			m["result"] = v
			if state == "succeeded" {
				if Str(v, "verification") == "VERIFIED" {
					m["executionStage"] = "VERIFIED"
				} else {
					m["executionStage"] = "APPLIED"
				}
			}
			if s := Str(v, "errorCode"); s != "" {
				m["errorCode"] = s
			}
		}
	}
	return m, nil
}
func (a *App) taskAuthorized(w http.ResponseWriter, r *http.Request, permission string) (*Principal, string, bool) {
	p, ok := a.Require(w, r, permission)
	if !ok {
		return nil, "", false
	}
	id := chi.URLParam(r, "taskId")
	var g string
	if a.Management.QueryRowContext(r.Context(), "SELECT gateway_id FROM command_tasks WHERE task_id=?", id).Scan(&g) != nil {
		Fail(w, r, 404, "TASK_NOT_FOUND", "task not found")
		return nil, "", false
	}
	if !a.CanGateway(p, permission, g) {
		Fail(w, r, 403, "FORBIDDEN", "task scope denied")
		return nil, "", false
	}
	return p, id, true
}
func (a *App) getTask(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.taskAuthorized(w, r, "task:read")
	if !ok {
		return
	}
	m, e := a.taskView(r.Context(), a.Management, id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(Num(m, "revision")))
	JSON(w, 200, m)
}
func (a *App) listTasks(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "task:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT task_id,gateway_id FROM command_tasks ORDER BY created_at_ms DESC,task_id")
	if e != nil {
		RespondError(w, r, e)
		return
	}
	type item struct{ id, gateway string }
	ids := []item{}
	for rows.Next() {
		var i item
		if rows.Scan(&i.id, &i.gateway) == nil {
			ids = append(ids, i)
		}
	}
	rows.Close()
	out := []any{}
	for _, i := range ids {
		if !a.CanGateway(p, "task:read", i.gateway) {
			continue
		}
		if q := r.URL.Query().Get("gatewayId"); q != "" && q != i.gateway {
			continue
		}
		m, e := a.taskView(r.Context(), a.Management, i.id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		if q := r.URL.Query().Get("state"); q != "" && q != Str(m, "state") {
			continue
		}
		out = append(out, m)
	}
	a.Page(w, r, out)
}
func (a *App) cancelTask(w http.ResponseWriter, r *http.Request) {
	p, id, ok := a.taskAuthorized(w, r, "task:cancel")
	if !ok {
		return
	}
	body, ok := a.Decode(w, r, "CancelRequest")
	if !ok {
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "task.cancel", r.Header.Get("Idempotency-Key"), map[string]any{"taskId": id, "body": body}, func(tx *sql.Tx) (int, string, any, error) {
		var state string
		var attempts int
		if e := tx.QueryRowContext(r.Context(), "SELECT t.state,o.attempts FROM command_tasks t JOIN command_outbox o ON o.task_id=t.task_id AND o.kind='command' WHERE t.task_id=?", id).Scan(&state, &attempts); e != nil {
			return 0, "", nil, e
		}
		if state != "cancelled" && (state != "queued" || attempts > 0) {
			return 0, "", nil, Problem(409, "TASK_ALREADY_DISPATCHED", "a dispatched physical action cannot be safely cancelled")
		}
		_, e := tx.ExecContext(r.Context(), "UPDATE command_tasks SET state='cancelled',version=version+1,updated_at_ms=? WHERE task_id=?", Now(), id)
		if e == nil {
			_, e = tx.ExecContext(r.Context(), "UPDATE command_outbox SET state='cancelled' WHERE task_id=? AND kind='command'", id)
		}
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "task.cancel", "task", id, body)
		}
		if e != nil {
			return 0, "", nil, e
		}
		v, e := a.taskView(r.Context(), tx, id)
		return 202, id, v, e
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}

func ctlNamespace(a *App, m map[string]any, gateway string) error {
	if Str(m, "namespaceId") != a.Config.NamespaceID || Str(m, "gatewayId") != gateway {
		return Problem(403, "GATEWAY_IDENTITY_MISMATCH", "payload identity must match authenticated gateway and namespace")
	}
	return nil
}
func (a *App) ingressResults(w http.ResponseWriter, r *http.Request) {
	g, ok := a.Gateway(r)
	if !ok {
		Fail(w, r, 401, "GATEWAY_AUTH_REQUIRED", "valid gateway credential required")
		return
	}
	m, ok := a.Decode(w, r, "TaskResultBatch")
	if !ok {
		return
	}
	v, e := a.AcceptTaskResults(r.Context(), g, m)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, v)
}
func (a *App) ingressStatus(w http.ResponseWriter, r *http.Request) {
	g, ok := a.Gateway(r)
	if !ok {
		Fail(w, r, 401, "GATEWAY_AUTH_REQUIRED", "valid gateway credential required")
		return
	}
	m, ok := a.Decode(w, r, "GatewayStatus")
	if !ok {
		return
	}
	if e := a.RecordGatewayStatus(r.Context(), g, m); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"accepted": true, "serverTime": Timestamp(Now())})
}
func (a *App) RecordGatewayStatus(ctx context.Context, g string, m map[string]any) error {
	if e := a.Validate("GatewayStatus", m); e != nil {
		return Problem(400, "INVALID_STATUS", e.Error())
	}
	if e := ctlNamespace(a, m, g); e != nil {
		return e
	}
	res, e := a.Management.ExecContext(ctx, "UPDATE gateways SET last_seen_at_ms=?,firmware_version=?,health_json=? WHERE gateway_id=? AND lifecycle='active'", Now(), Str(m, "firmwareVersion"), encode(m), g)
	if e != nil {
		return e
	}
	n, _ := res.RowsAffected()
	if n != 1 {
		return Problem(403, "GATEWAY_INACTIVE", "gateway inactive")
	}
	return nil
}

func (a *App) AcceptTaskResults(ctx context.Context, gateway string, batch map[string]any) (map[string]any, error) {
	if e := a.Validate("TaskResultBatch", batch); e != nil {
		return nil, Problem(400, "INVALID_RESULT", e.Error())
	}
	if e := ctlNamespace(a, batch, gateway); e != nil {
		return nil, e
	}
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return nil, e
	}
	defer tx.Rollback()
	var life string
	if e = tx.QueryRowContext(ctx, "SELECT lifecycle FROM gateways WHERE gateway_id=?", gateway).Scan(&life); e != nil || life != "active" {
		return nil, Problem(403, "GATEWAY_INACTIVE", "gateway inactive")
	}
	items := []any{}
	for _, raw := range List(batch, "results") {
		result := raw.(map[string]any)
		item, e := a.acceptTaskResult(ctx, tx, gateway, result)
		if e != nil {
			return nil, e
		}
		items = append(items, item)
	}
	ack := a.resultAck(gateway, items)
	if e = tx.Commit(); e != nil {
		return nil, e
	}
	return ack, nil
}
func (a *App) resultAck(g string, items []any) map[string]any {
	return map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": g, "receiverId": a.Config.InstanceID, "durability": "RESULT_PERSISTED", "items": items}
}
func (a *App) acceptTaskResult(ctx context.Context, tx *sql.Tx, g string, v map[string]any) (map[string]any, error) {
	task, resultID, sequence := Str(v, "taskId"), Str(v, "resultId"), Num(v, "resultSequence")
	item := map[string]any{"taskId": task, "resultId": resultID, "status": "ACCEPTED"}
	reject := func(code string) (map[string]any, error) {
		item["status"] = "REJECTED_PERMANENT"
		item["code"] = code
		return item, nil
	}
	var gateway, operation, current, request string
	var configID sql.NullString
	e := tx.QueryRowContext(ctx, "SELECT gateway_id,operation,state,request_json,config_id FROM command_tasks WHERE task_id=?", task).Scan(&gateway, &operation, &current, &request, &configID)
	if errors.Is(e, sql.ErrNoRows) || gateway != g {
		return reject("TASK_SOURCE_MISMATCH")
	}
	if e != nil {
		return nil, e
	}
	digest := Hash(v)
	var oldID, oldHash string
	e = tx.QueryRowContext(ctx, "SELECT result_id,content_hash FROM command_results WHERE result_id=? OR (task_id=? AND sequence=?) LIMIT 1", resultID, task, sequence).Scan(&oldID, &oldHash)
	if e == nil {
		if oldID != resultID || oldHash != digest {
			return reject("RESULT_CONFLICT")
		}
		item["status"] = "DUPLICATE"
		_, e = tx.ExecContext(ctx, "UPDATE command_outbox SET state='pending',next_attempt_at_ms=? WHERE result_id=?", Now(), resultID)
		return item, e
	}
	if !errors.Is(e, sql.ErrNoRows) {
		return nil, e
	}
	var command map[string]any
	_ = json.Unmarshal([]byte(request), &command)
	params := Obj(command, "parameters")
	state := strings.ToLower(Str(v, "state"))
	if state == "accepted" {
		state = "device_accepted"
	}
	details := Obj(v, "details")
	if state == "succeeded" {
		if operation == "REBOOT" && (Str(v, "verification") != "VERIFIED" || Str(details, "bootId") == "") {
			return reject("REBOOT_NOT_VERIFIED")
		}
		if operation == "CONFIGURE" {
			if Str(v, "verification") != "VERIFIED" || Num(details, "reportedConfigurationRevision") != Num(params, "desiredRevision") || Hash(Obj(details, "configuration")) != Hash(Obj(params, "configuration")) {
				return reject("CONFIGURATION_NOT_VERIFIED")
			}
		}
		if operation == "FIRMWARE_UPGRADE" && (Str(v, "verification") != "VERIFIED" || Str(details, "firmwareVersion") != Str(params, "expectedFirmwareVersion")) {
			return reject("FIRMWARE_NOT_VERIFIED")
		}
		if operation == "GATT_WRITE" && Str(params, "verification") != "NONE" && Str(v, "verification") != "VERIFIED" {
			return reject("WRITE_NOT_VERIFIED")
		}
	}
	var maxSequence sql.NullInt64
	e = tx.QueryRowContext(ctx, "SELECT max(sequence) FROM command_results WHERE task_id=?", task).Scan(&maxSequence)
	if e != nil {
		return nil, e
	}
	if (!maxSequence.Valid || sequence > maxSequence.Int64) && (state == "device_accepted" || state == "executing") {
		ranks := map[string]int{"queued": 0, "sent": 0, "device_accepted": 1, "executing": 2, "applied": 3, "unknown": 3}
		if ranks[state] < ranks[current] {
			return reject("RESULT_STATE_REGRESSION")
		}
	}
	_, e = tx.ExecContext(ctx, "INSERT INTO command_results(result_id,task_id,gateway_id,sequence,stage,content_hash,result_json,received_at_ms,dedup_until_ms) VALUES(?,?,?,?,?,?,?,?,?)", resultID, task, g, sequence, state, digest, encode(v), Now(), Now()+int64(a.Config.DedupRetention/time.Millisecond))
	if e != nil {
		return nil, e
	}
	terminal := current == "succeeded" || current == "failed" || current == "cancelled" || current == "expired"
	if (!maxSequence.Valid || sequence > maxSequence.Int64) && !terminal {
		projection := state
		if state == "executing" && Str(v, "verification") == "APPLIED" {
			projection = "applied"
		}
		_, e = tx.ExecContext(ctx, "UPDATE command_tasks SET state=?,version=version+1,updated_at_ms=? WHERE task_id=?", projection, Now(), task)
		if e != nil {
			return nil, e
		}
		if configID.Valid && state == "succeeded" {
			_, e = tx.ExecContext(ctx, "UPDATE gateway_configs SET state='verified',reported_json=?,verified_at_ms=? WHERE config_id=?", encode(Obj(details, "configuration")), Now(), configID.String)
			if e != nil {
				return nil, e
			}
		}
	}
	_, e = tx.ExecContext(ctx, "UPDATE command_outbox SET state='done' WHERE task_id=? AND kind='command'", task)
	if e != nil {
		return nil, e
	}
	_, e = tx.ExecContext(ctx, "INSERT INTO command_outbox(message_id,task_id,result_id,kind,payload_json,state,next_attempt_at_ms,created_at_ms) VALUES(?,?,?,'result_ack',?,'pending',?,?)", ID("msg"), task, resultID, encode(a.resultAck(g, []any{item})), Now(), Now())
	return item, e
}

func (a *App) ingressCommands(w http.ResponseWriter, r *http.Request) {
	g, ok := a.Gateway(r)
	if !ok {
		Fail(w, r, 401, "GATEWAY_AUTH_REQUIRED", "valid gateway credential required")
		return
	}
	commands, e := a.PullCommands(r.Context(), g)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"commands": commands})
}
func (a *App) PullCommands(ctx context.Context, g string) ([]any, error) {
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return nil, e
	}
	defer tx.Rollback()
	now := Now()
	rows, e := tx.QueryContext(ctx, "SELECT o.message_id,o.payload_json FROM command_outbox o JOIN command_tasks t ON t.task_id=o.task_id JOIN gateways g ON g.gateway_id=t.gateway_id WHERE t.gateway_id=? AND g.lifecycle='active' AND t.expires_at_ms>? AND t.state IN ('queued','sent') AND o.kind='command' AND ((o.state IN ('pending','awaiting_receipt') AND o.next_attempt_at_ms<=?) OR (o.state='leased' AND o.lease_until_ms<=?)) ORDER BY o.created_at_ms LIMIT 16", g, now, now, now)
	if e != nil {
		return nil, e
	}
	type entry struct{ id, raw string }
	entries := []entry{}
	for rows.Next() {
		var v entry
		if e = rows.Scan(&v.id, &v.raw); e != nil {
			rows.Close()
			return nil, e
		}
		entries = append(entries, v)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		return nil, e
	}
	out := []any{}
	for _, v := range entries {
		var m map[string]any
		if e = json.Unmarshal([]byte(v.raw), &m); e != nil {
			return nil, e
		}
		_, e = tx.ExecContext(ctx, "UPDATE command_outbox SET state='awaiting_receipt',attempts=attempts+1,next_attempt_at_ms=?,lease_token=lease_token+1,lease_owner=NULL,lease_until_ms=NULL WHERE message_id=?", now+10000, v.id)
		if e != nil {
			return nil, e
		}
		_, e = tx.ExecContext(ctx, "UPDATE command_tasks SET state='sent',version=version+1,updated_at_ms=? WHERE task_id=? AND state='queued'", now, Str(m, "taskId"))
		if e != nil {
			return nil, e
		}
		out = append(out, m)
	}
	if e = tx.Commit(); e != nil {
		return nil, e
	}
	return out, nil
}

func (a *App) expireCommands(ctx context.Context) error {
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	_, e = tx.ExecContext(ctx, "UPDATE command_tasks SET state=CASE WHEN state='queued' AND NOT EXISTS(SELECT 1 FROM command_outbox o WHERE o.task_id=command_tasks.task_id AND o.kind='command' AND o.attempts>0) THEN 'expired' ELSE 'unknown' END,version=version+1,updated_at_ms=? WHERE expires_at_ms<=? AND state IN ('queued','sent','device_accepted','executing','applied')", Now(), Now())
	if e == nil {
		_, e = tx.ExecContext(ctx, "UPDATE command_outbox SET state='expired' WHERE kind='command' AND state NOT IN ('done','cancelled','expired') AND task_id IN (SELECT task_id FROM command_tasks WHERE expires_at_ms<=?)", Now())
	}
	if e != nil {
		return e
	}
	return tx.Commit()
}

func (a *App) StartControl(ctx context.Context) {
	runBroker, e := a.prepareGatewayBroker()
	if e != nil {
		a.Logger.Error("gateway broker configuration", "error", e)
		runBroker = func(ctx context.Context) { <-ctx.Done() }
	}
	brokerDone := make(chan struct{})
	go func() { defer close(brokerDone); runBroker(ctx) }()
	interval := a.Config.WorkerInterval
	if interval <= 0 {
		interval = 200 * time.Millisecond
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			<-brokerDone
			return
		case <-ticker.C:
			if e := a.expireCommands(ctx); e != nil && !errors.Is(e, context.Canceled) {
				a.Logger.Error("expire commands", "error", e)
			}
			if a.Config.BrokerURL != "" {
				started := time.Now()
				for i := 0; i < 32 && ctx.Err() == nil && time.Since(started) < 250*time.Millisecond; i++ {
					var due int
					e := a.Management.QueryRowContext(ctx, "SELECT EXISTS(SELECT 1 FROM command_outbox WHERE (state IN ('pending','awaiting_receipt') AND next_attempt_at_ms<=?) OR (state='leased' AND lease_until_ms<=?))", Now(), Now()).Scan(&due)
					if e != nil || due == 0 {
						break
					}
					if e = a.dispatchControl(ctx); e != nil && !errors.Is(e, context.Canceled) {
						a.Logger.Warn("dispatch control", "error", e)
						break
					}
				}
			}
		}
	}
}

func (a *App) dispatchControl(ctx context.Context) error {
	// The attempt is persisted before publishing: a crash can only cause a duplicate,
	// never a second task identity or an unsafe cancellation of a sent command.
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	var id, task, g, kind, raw string
	var token int64
	e = tx.QueryRowContext(ctx, "SELECT o.message_id,o.task_id,t.gateway_id,o.kind,o.payload_json,o.lease_token FROM command_outbox o JOIN command_tasks t ON t.task_id=o.task_id JOIN gateways g ON g.gateway_id=t.gateway_id WHERE g.lifecycle='active' AND ((o.state IN ('pending','awaiting_receipt') AND o.next_attempt_at_ms<=?) OR (o.state='leased' AND o.lease_until_ms<=?)) AND (o.kind='result_ack' OR (t.expires_at_ms>? AND t.state IN ('queued','sent'))) ORDER BY o.created_at_ms LIMIT 1", Now(), Now(), Now()).Scan(&id, &task, &g, &kind, &raw, &token)
	if errors.Is(e, sql.ErrNoRows) {
		return nil
	}
	if e != nil {
		return e
	}
	token++
	_, e = tx.ExecContext(ctx, "UPDATE command_outbox SET state='leased',attempts=attempts+1,lease_token=?,lease_owner=?,lease_until_ms=? WHERE message_id=?", token, a.Config.NodeID, Now()+30000, id)
	if e != nil {
		return e
	}
	if kind == "command" {
		_, e = tx.ExecContext(ctx, "UPDATE command_tasks SET state='sent',version=version+1,updated_at_ms=? WHERE task_id=? AND state='queued'", Now(), task)
		if e != nil {
			return e
		}
	}
	if e = tx.Commit(); e != nil {
		return e
	}
	var payload any
	if e = json.Unmarshal([]byte(raw), &payload); e != nil {
		return e
	}
	suffix := "commands"
	if kind == "result_ack" {
		suffix = "result-ack"
	}
	e = a.Publish(ctx, fmt.Sprintf("gw/v1/%s/%s/%s", a.Config.NamespaceID, g, suffix), payload)
	state := "awaiting_receipt"
	var last any
	if e != nil {
		state = "pending"
		last = e.Error()
	} else if kind == "result_ack" {
		state = "done"
	}
	_, updateErr := a.Management.ExecContext(ctx, "UPDATE command_outbox SET state=?,next_attempt_at_ms=?,last_error=?,lease_owner=NULL,lease_until_ms=NULL WHERE message_id=? AND lease_token=? AND state='leased'", state, Now()+10000, last, id, token)
	return updateErr
}
