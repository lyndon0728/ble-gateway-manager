package platform

import (
	"context"
	"database/sql"
	"net/http"
)

func (a *App) aiModel(ctx context.Context, q ctlQuery, id string) (map[string]any, error) {
	var raw string
	e := q.QueryRowContext(ctx, "SELECT model_json FROM ai_model_versions WHERE model_id=?", id).Scan(&raw)
	if e != nil {
		return nil, aiNotFound(e, "MODEL_NOT_FOUND")
	}
	return aiMap(raw), nil
}
func (a *App) aiModelView(p *Principal, m map[string]any) map[string]any {
	out := aiMap(encode(m))
	if !a.Can(p, "document:read", Str(m, "scopeId")) {
		content := Obj(out, "content")
		content["sources"] = []any{}
		content["tests"] = []any{}
		content["issues"] = []any{}
		spec := Obj(content, "parserSpec")
		delete(spec, "description")
		for _, raw := range List(spec, "fields") {
			field, _ := raw.(map[string]any)
			delete(field, "description")
			delete(field, "evidence")
		}
		delete(out, "documentIds")
		// Model template definitions are intended for reuse, original evidence is not.
		out["evidenceRestricted"] = true
	}
	return out
}
func (a *App) aiListModels(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT model_json FROM ai_model_versions ORDER BY created_at_ms DESC,model_id LIMIT 10001")
	if e != nil {
		RespondError(w, r, e)
		return
	}
	candidates := []map[string]any{}
	for rows.Next() {
		var raw string
		if e = rows.Scan(&raw); e != nil {
			break
		}
		candidates = append(candidates, aiMap(raw))
	}
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if len(candidates) > 10000 {
		Fail(w, r, 422, "QUERY_TOO_BROAD", "too many model templates")
		return
	}
	items := []any{}
	for _, m := range candidates {
		if a.Can(p, "profile:read", Str(m, "scopeId")) {
			items = append(items, a.aiModelView(p, m))
		}
	}
	a.Page(w, r, items)
}
func (a *App) aiGetModel(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:read")
	if !ok {
		return
	}
	m, e := a.aiModel(r.Context(), a.Management, aiParam(r, "modelId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !a.Can(p, "profile:read", Str(m, "scopeId")) {
		Fail(w, r, 403, "FORBIDDEN", "template scope is not accessible")
		return
	}
	JSON(w, 200, a.aiModelView(p, m))
}
func aiPlanSteps(model, source map[string]any) []any {
	content := Obj(model, "content")
	steps := []any{}
	common := map[string]any{"modelId": model["modelId"], "modelVersion": model["version"]}
	if len(Obj(model, "parserRef")) > 0 {
		p := aiMap(encode(common))
		p["parserRef"] = model["parserRef"]
		p["match"] = map[string]any{"peerIds": []any{}, "protocolBindingIds": []any{}}
		if connection := Obj(content, "connectionSpec"); len(connection) > 0 {
			p["match"] = map[string]any{"peerIds": []any{}, "protocolBindingIds": []any{}, "serviceUuids": []string{gw32CanonicalUUID(Str(connection, "serviceUuid"))}, "characteristicUuids": []string{gw32CanonicalUUID(Str(connection, "characteristicUuid"))}}
		}
		steps = append(steps, map[string]any{"kind": "PARSER_BINDING", "destination": "/ble-peers", "label": "选择真实对端并创建不可变协议绑定", "parameters": p})
		steps = append(steps, map[string]any{"kind": "RULE_BUNDLE_DRAFT", "destination": "/rule-bundles", "label": "创建规则包草稿并验证；激活单独确认", "parameters": p})
	}
	if connection := Obj(content, "connectionSpec"); len(connection) > 0 {
		p := aiMap(encode(common))
		p["connectionSpec"] = connection
		steps = append(steps, map[string]any{"kind": "GATT_DRAFT", "destination": "/gatt-connections", "label": "选择网关与真实连接，带入操作参数；创建任务单独确认", "parameters": p})
		p2 := aiMap(encode(p))
		p2["enabled"] = false
		steps = append(steps, map[string]any{"kind": "AUTO_CONNECT_DRAFT", "destination": "/auto-connect-rules", "label": "选择实际对端和授权网关，创建关闭的自动连接规则", "parameters": p2})
	}
	if mapping := Obj(content, "mappingSpec"); len(mapping) > 0 {
		p := aiMap(encode(common))
		p["mapping"] = mapping
		p["mappingVersion"] = "ai-" + Str(model, "modelId") + "-" + Str(model, "version")
		destination := "/targets"
		if id := Str(source, "targetId"); id != "" {
			destination = "/targets/" + id
		}
		steps = append(steps, map[string]any{"kind": "TARGET_MAPPING_DRAFT", "destination": destination, "label": "将映射带入新的目标版本，核对地址、认证与应用确认", "parameters": p})
	}
	return steps
}
func (a *App) aiCreatePlan(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:read")
	if !ok {
		return
	}
	body, ok := aiDecode(w, r, "modelId", "sourceContext")
	if !ok {
		return
	}
	model, e := a.aiModel(r.Context(), a.Management, Str(body, "modelId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !a.Can(p, "profile:read", Str(model, "scopeId")) {
		Fail(w, r, 403, "FORBIDDEN", "template scope is not accessible")
		return
	}
	source := Obj(body, "sourceContext")
	if len(encode(source)) > 4096 {
		Fail(w, r, 422, "INVALID_CONTEXT", "source context too large")
		return
	}
	// Client context never creates authorization. Actual target/peer APIs check it
	// again when the user submits a concrete mutation.
	if id := Str(source, "targetId"); id != "" && !artifactIDPattern.MatchString(id) {
		Fail(w, r, 422, "INVALID_CONTEXT", "invalid target identifier")
		return
	}
	a.aiMutation(w, r, p, "ai.plan.create", body, func(tx *sql.Tx) (int, string, any, error) {
		id := ID("aiplan")
		plan := map[string]any{"planId": id, "modelId": model["modelId"], "modelVersion": model["version"], "scopeId": model["scopeId"], "sourceContext": source, "state": "PREPARED", "steps": aiPlanSteps(model, source), "createdAt": Timestamp(Now()), "message": "This plan only prepares parameters. Device tasks, bindings and rule activation require their normal explicit actions."}
		_, e := tx.Exec("INSERT INTO ai_apply_plans(plan_id,owner_id,model_id,plan_json,created_at_ms) VALUES(?,?,?,?,?)", id, p.ID, Str(model, "modelId"), encode(plan), Now())
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "ai.plan.create", "ai_apply_plan", id, map[string]any{"modelId": model["modelId"]})
		}
		return 201, id, plan, e
	})
}
func (a *App) aiPlan(ctx context.Context, q ctlQuery, id string) (map[string]any, string, error) {
	var raw, owner string
	e := q.QueryRowContext(ctx, "SELECT plan_json,owner_id FROM ai_apply_plans WHERE plan_id=?", id).Scan(&raw, &owner)
	if e != nil {
		return nil, "", aiNotFound(e, "APPLY_PLAN_NOT_FOUND")
	}
	return aiMap(raw), owner, nil
}
func (a *App) aiGetPlan(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:read")
	if !ok {
		return
	}
	plan, owner, e := a.aiPlan(r.Context(), a.Management, aiParam(r, "planId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if owner != p.ID || !a.Can(p, "profile:read", Str(plan, "scopeId")) {
		Fail(w, r, 403, "FORBIDDEN", "apply plan belongs to another principal or scope")
		return
	}
	JSON(w, 200, plan)
}
func (a *App) aiConfirmPlan(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:read")
	if !ok {
		return
	}
	body, ok := aiDecode(w, r, "confirmed")
	if !ok {
		return
	}
	if !aiBool(body, "confirmed") {
		Fail(w, r, 422, "CONFIRMATION_REQUIRED", "confirm the parameter preparation plan")
		return
	}
	id := aiParam(r, "planId")
	a.aiMutation(w, r, p, "ai.plan.confirm:"+id, body, func(tx *sql.Tx) (int, string, any, error) {
		plan, owner, e := a.aiPlan(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, e
		}
		if owner != p.ID || !canQuery(r.Context(), tx, p, "profile:read", Str(plan, "scopeId")) {
			return 0, "", nil, Problem(403, "FORBIDDEN", "apply plan scope is not accessible")
		}
		plan["state"] = "READY_TO_APPLY"
		plan["confirmedAt"] = Timestamp(Now())
		if _, e = tx.Exec("UPDATE ai_apply_plans SET plan_json=? WHERE plan_id=?", encode(plan), id); e != nil {
			return 0, "", nil, e
		}
		e = a.Audit(r.Context(), tx, p.ID, "ai.plan.confirm", "ai_apply_plan", id, map[string]any{"modelId": plan["modelId"], "physicalActions": 0})
		return 200, id, plan, e
	})
}
