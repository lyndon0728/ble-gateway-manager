package platform

import (
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"strconv"
	"strings"

	"github.com/go-chi/chi/v5"
)

func (a *App) RegisterAutoConnect(r chi.Router) {
	r.Get("/api/v1/ble-peers", a.listBLEPeers)
	r.Post("/api/v1/ble-peers", a.createBLEPeer)
	r.Patch("/api/v1/ble-peers/{peerId}", a.patchBLEPeer)
	r.Get("/api/v1/auto-connect-rules", a.listAutoRules)
	r.Post("/api/v1/auto-connect-rules", a.createAutoRule)
	r.Patch("/api/v1/auto-connect-rules/{ruleId}", a.patchAutoRule)
	r.Get("/api/v1/gatt-connections", a.listGATTConnections)
}
func peerView(id, ns, addr, typ, scope, name, state string, revision, created int64) map[string]any {
	return map[string]any{"peerId": id, "namespaceId": ns, "deviceAddress": addr, "addressType": typ, "scopeId": scope, "displayName": name, "state": strings.ToUpper(state), "revision": revision, "createdAt": Timestamp(created)}
}
func (a *App) listBLEPeers(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT peer_id,namespace_id,device_address,address_type,scope_id,display_name,state,revision,created_at_ms FROM ble_peers ORDER BY created_at_ms,peer_id LIMIT 10001")
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var id, ns, addr, typ, scope, name, state string
		var rev, at int64
		if e = rows.Scan(&id, &ns, &addr, &typ, &scope, &name, &state, &rev, &at); e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, peerView(id, ns, addr, typ, scope, name, state, rev, at))
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	visible := []any{}
	for _, item := range items {
		if a.Can(p, "gatt:read", Str(item.(map[string]any), "scopeId")) {
			visible = append(visible, item)
		}
	}
	a.Page(w, r, visible)
}
func registeredAddress(addr, typ string) bool {
	if len(addr) != 12 || strings.ToUpper(addr) != addr {
		return false
	}
	n, e := strconv.ParseUint(addr[:2], 16, 8)
	if e != nil {
		return false
	}
	if _, e = strconv.ParseUint(addr, 16, 48); e != nil {
		return false
	}
	if addr == "000000000000" || addr == "FFFFFFFFFFFF" {
		return false
	}
	return typ == "PUBLIC" || (typ == "RANDOM" && n&0xc0 == 0xc0)
}
func (a *App) createBLEPeer(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:manage")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "BLEPeerCreate")
	if !ok {
		return
	}
	scope := Str(req, "scopeId")
	if !a.Can(p, "gatt:manage", scope) {
		Fail(w, r, 403, "FORBIDDEN", "peer scope is not writable")
		return
	}
	addr, typ := Str(req, "deviceAddress"), Str(req, "addressType")
	if !registeredAddress(addr, typ) {
		Fail(w, r, 422, "ADDRESS_NOT_STATIC", "automatic peers require a nonzero public or random-static address")
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "ble-peer.create", r.Header.Get("Idempotency-Key"), req, func(tx *sql.Tx) (int, string, any, error) {
		var n int
		if e := tx.QueryRow("SELECT count(*) FROM ble_peers WHERE namespace_id=? AND address_type=? AND device_address=?", a.Config.NamespaceID, typ, addr).Scan(&n); e != nil {
			return 0, "", nil, e
		}
		if n > 0 {
			return 0, "", nil, Problem(409, "PEER_EXISTS", "peer address already registered")
		}
		id, now := ID("peer"), Now()
		_, e := tx.Exec("INSERT INTO ble_peers VALUES(?,?,?,?,?,?,'active',1,?,?,?)", id, a.Config.NamespaceID, typ, addr, scope, Str(req, "displayName"), p.ID, now, now)
		if e != nil {
			return 0, "", nil, e
		}
		e = a.Audit(r.Context(), tx, p.ID, "ble-peer.create", "scope", scope, map[string]any{"peerId": id})
		return 201, id, peerView(id, a.Config.NamespaceID, addr, typ, scope, Str(req, "displayName"), "active", 1, now), e
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(1))
	JSON(w, status, v)
}
func (a *App) patchBLEPeer(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:manage")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "BLEPeerPatch")
	if !ok {
		return
	}
	id := chi.URLParam(r, "peerId")
	var ns, typ, addr, scope, name, state string
	var rev, created int64
	e := a.Management.QueryRowContext(r.Context(), "SELECT namespace_id,address_type,device_address,scope_id,display_name,state,revision,created_at_ms FROM ble_peers WHERE peer_id=?", id).Scan(&ns, &typ, &addr, &scope, &name, &state, &rev, &created)
	if e != nil || !a.Can(p, "gatt:manage", scope) {
		Fail(w, r, 404, "NOT_FOUND", "peer unavailable")
		return
	}
	if e = Match(r, rev); e != nil {
		RespondError(w, r, e)
		return
	}
	if v, ok := req["displayName"]; ok {
		name = v.(string)
	}
	if v := Str(req, "state"); v != "" {
		state = strings.ToLower(v)
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	result, e := tx.ExecContext(r.Context(), "UPDATE ble_peers SET display_name=?,state=?,revision=revision+1,updated_at_ms=? WHERE peer_id=? AND revision=?", name, state, Now(), id, rev)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	n, e := result.RowsAffected()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if n != 1 {
		Fail(w, r, 412, "PRECONDITION_FAILED", "peer revision changed")
		return
	}
	if e == nil {
		e = a.Audit(r.Context(), tx, p.ID, "ble-peer.update", "scope", scope, map[string]any{"peerId": id, "state": state})
	}
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(rev+1))
	JSON(w, 200, peerView(id, ns, addr, typ, scope, name, state, rev+1, created))
}

func autoRuleView(id string, rev, created int64, req map[string]any) map[string]any {
	m := map[string]any{}
	for k, v := range req {
		m[k] = v
	}
	m["ruleId"] = id
	m["revision"] = rev
	m["createdAt"] = Timestamp(created)
	return m
}
func (a *App) authorizeAutoRule(r *http.Request, p *Principal, req map[string]any) error {
	var scope, state string
	e := a.Management.QueryRowContext(r.Context(), "SELECT scope_id,state FROM ble_peers WHERE peer_id=? AND namespace_id=?", Str(req, "peerId"), a.Config.NamespaceID).Scan(&scope, &state)
	if e != nil || !a.Can(p, "gatt:manage", scope) {
		return Problem(404, "PEER_NOT_FOUND", "registered peer unavailable")
	}
	if state != "active" && req["enabled"] == true {
		return Problem(409, "PEER_DISABLED", "enable peer before configuring automation")
	}
	enabled, _ := req["enabled"].(bool)
	for _, v := range List(req, "gatewayIds") {
		g := v.(string)
		if !a.CanGateway(p, "gatt:execute", g) {
			return Problem(404, "GATEWAY_NOT_FOUND", "gateway unavailable")
		}
		if enabled {
			var caps, model, life string
			if e := a.Management.QueryRowContext(r.Context(), "SELECT capabilities_json,model,lifecycle FROM gateways WHERE gateway_id=?", g).Scan(&caps, &model, &life); e != nil {
				return e
			}
			if life != "active" || !(model == "GW32I33V2" || strings.HasPrefix(model, "GW32I33V2-")) {
				return Problem(422, "AUTO_CONNECT_UNSUPPORTED", "active GW32 gateway required")
			}
			var capabilities []string
			if json.Unmarshal([]byte(caps), &capabilities) != nil {
				return errors.New("invalid gateway capabilities")
			}
			have := map[string]bool{}
			for _, c := range capabilities {
				have[c] = true
			}
			for _, c := range []string{"GW32_CONTROL_PROFILE_1", "GW32_CAPTURE_CONTEXT_1", "GATT_CONNECT", "NRF54_BLE_GATT_CENTRAL", "NRF54_BLE_GATT_SERVICE_DISCOVERY"} {
				if !have[c] {
					return Problem(422, "AUTO_CONNECT_UNSUPPORTED", "gateway lacks "+c)
				}
			}
		}
	}
	return nil
}
func defaultAutoRule(m map[string]any) map[string]any {
	if _, ok := m["maxAgeMs"]; !ok {
		m["maxAgeMs"] = int64(5000)
	}
	if _, ok := m["cooldownMs"]; !ok {
		m["cooldownMs"] = int64(30000)
	}
	return m
}
func (a *App) listAutoRules(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT a.rule_id,a.revision,a.created_at_ms,a.rule_json,p.scope_id FROM auto_connect_rules a JOIN ble_peers p ON p.peer_id=a.peer_id ORDER BY a.created_at_ms,a.rule_id LIMIT 10001")
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	type candidate struct {
		scope string
		value map[string]any
	}
	candidates := []candidate{}
	for rows.Next() {
		var id, raw, scope string
		var rev, at int64
		if e = rows.Scan(&id, &rev, &at, &raw, &scope); e != nil {
			RespondError(w, r, e)
			return
		}
		candidates = append(candidates, candidate{scope, autoRuleView(id, rev, at, pm(raw))})
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	for _, c := range candidates {
		if !a.Can(p, "gatt:read", c.scope) {
			continue
		}
		m := c.value
		visible := true
		for _, g := range List(m, "gatewayIds") {
			if !a.CanGateway(p, "gatt:read", g.(string)) {
				visible = false
			}
		}
		if visible {
			items = append(items, m)
		}
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	a.Page(w, r, items)
}
func (a *App) createAutoRule(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:manage")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "AutoConnectRuleCreate")
	if !ok {
		return
	}
	req = defaultAutoRule(req)
	if e := a.authorizeAutoRule(r, p, req); e != nil {
		RespondError(w, r, e)
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "auto-rule.create", r.Header.Get("Idempotency-Key"), req, func(tx *sql.Tx) (int, string, any, error) {
		id, now := ID("auto"), Now()
		enabled, _ := req["enabled"].(bool)
		_, e := tx.Exec("INSERT INTO auto_connect_rules VALUES(?,?,1,?,?,?,?,?,?,?)", id, Str(req, "peerId"), Str(req, "name"), enabled, Num(req, "priority"), encode(req), p.ID, now, now)
		if e != nil {
			return 0, "", nil, e
		}
		if e = a.Audit(r.Context(), tx, p.ID, "auto-rule.create", "rule", id, map[string]any{"enabled": enabled}); e != nil {
			return 0, "", nil, e
		}
		return 201, id, autoRuleView(id, 1, now, req), nil
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(1))
	JSON(w, status, v)
}
func (a *App) patchAutoRule(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:manage")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "AutoConnectRulePatch")
	if !ok {
		return
	}
	id := chi.URLParam(r, "ruleId")
	var raw, scope string
	var rev, at int64
	e := a.Management.QueryRowContext(r.Context(), "SELECT a.rule_json,a.revision,a.created_at_ms,p.scope_id FROM auto_connect_rules a JOIN ble_peers p ON p.peer_id=a.peer_id WHERE a.rule_id=?", id).Scan(&raw, &rev, &at, &scope)
	if e != nil || !a.Can(p, "gatt:manage", scope) {
		Fail(w, r, 404, "NOT_FOUND", "rule unavailable")
		return
	}
	m := pm(raw)
	for k, v := range req {
		m[k] = v
	}
	if e = a.authorizeAutoRule(r, p, m); e != nil {
		RespondError(w, r, e)
		return
	}
	if e = Match(r, rev); e != nil {
		RespondError(w, r, e)
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	enabled, _ := m["enabled"].(bool)
	result, e := tx.Exec("UPDATE auto_connect_rules SET name=?,enabled=?,priority=?,rule_json=?,created_by=?,revision=revision+1,updated_at_ms=? WHERE rule_id=? AND revision=?", Str(m, "name"), enabled, Num(m, "priority"), encode(m), p.ID, Now(), id, rev)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	n, _ := result.RowsAffected()
	if n != 1 {
		Fail(w, r, 412, "PRECONDITION_FAILED", "rule revision changed")
		return
	}
	if e = a.Audit(r.Context(), tx, p.ID, "auto-rule.update", "rule", id, map[string]any{"revision": rev + 1, "enabled": enabled}); e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(rev+1))
	JSON(w, 200, autoRuleView(id, rev+1, at, m))
}
func (a *App) listGATTConnections(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	gw := r.URL.Query().Get("gatewayId")
	rows, e := a.Management.QueryContext(r.Context(), "SELECT gateway_id,connection_ref,radio,address_type,peer_address,security_state,att_mtu,state,created_at_ms,updated_at_ms FROM gatt_connections WHERE namespace_id=? AND (?='' OR gateway_id=?) ORDER BY created_at_ms,connection_ref LIMIT 10001", a.Config.NamespaceID, gw, gw)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var gateway, ref, radio, typ, addr, security, state string
		var mtu, created, updated int64
		if e = rows.Scan(&gateway, &ref, &radio, &typ, &addr, &security, &mtu, &state, &created, &updated); e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, map[string]any{"namespaceId": a.Config.NamespaceID, "gatewayId": gateway, "connectionRef": ref, "radio": radio, "addressType": typ, "deviceAddress": addr, "securityState": security, "attMtu": mtu, "state": strings.ToUpper(state), "createdAt": Timestamp(created), "updatedAt": Timestamp(updated)})
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	visible := []any{}
	for _, item := range items {
		if a.CanGateway(p, "gatt:read", Str(item.(map[string]any), "gatewayId")) {
			visible = append(visible, item)
		}
	}
	a.Page(w, r, visible)
}
