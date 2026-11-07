package platform

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"time"
)

type captureObservation struct {
	Key, Namespace, Gateway, Event string
	Captured, Accepted             int64
	Raw                            map[string]any
	Payload                        []byte
}

func (o captureObservation) capture() map[string]any { return Obj(o.Raw, "capture") }
func (o captureObservation) fresh(now, maxAge int64) bool {
	return Str(o.Raw, "timeQuality") == "SYNCED" && o.Captured > 0 && o.Captured <= now && o.Captured+maxAge > now
}
func scanObservation(rows *sql.Rows) (captureObservation, error) {
	var o captureObservation
	var captured sql.NullInt64
	var meta string
	e := rows.Scan(&o.Key, &o.Namespace, &o.Gateway, &o.Event, &captured, &o.Accepted, &o.Payload, &meta)
	o.Captured = captured.Int64
	o.Raw = Obj(pm(meta), "rawEvent")
	return o, e
}

const observationColumns = "e.event_key,e.namespace_id,e.gateway_id,e.event_id,r.captured_at_ms,e.accepted_at_ms,r.payload,r.metadata_json"

// Cross-database handoff: management effects commit first. Pipeline consumption
// follows. A crash between commits re-evaluates the event; unique triggers and
// connection tombstones make that replay harmless.
func (a *App) ProcessCaptureBatch(ctx context.Context, limit int) (int, error) {
	if limit < 1 || limit > 256 {
		limit = 64
	}
	rows, e := a.Pipeline.QueryContext(ctx, "SELECT "+observationColumns+" FROM event_receipts e JOIN raw_events r ON r.event_key=e.event_key LEFT JOIN capture_consumptions c ON c.event_key=e.event_key WHERE e.namespace_id=? AND c.event_key IS NULL AND json_extract(r.metadata_json,'$.ingressSchemaVersion')='1.1' AND json_type(r.metadata_json,'$.rawEvent.capture')='object' ORDER BY e.accepted_at_ms,e.event_key LIMIT ?", a.Config.NamespaceID, limit)
	if e != nil {
		return 0, e
	}
	observations := []captureObservation{}
	for rows.Next() {
		o, e := scanObservation(rows)
		if e != nil {
			rows.Close()
			return 0, e
		}
		observations = append(observations, o)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		return 0, e
	}
	if len(observations) == 0 {
		return 0, nil
	}
	type outcome struct{ key, state, code string }
	completed := []outcome{}
	for _, o := range observations {
		state, code, e := a.consumeCapture(ctx, o)
		if e != nil {
			return 0, e
		}
		completed = append(completed, outcome{o.Key, state, code})
	}
	tx, e := a.Pipeline.BeginTx(ctx, nil)
	if e != nil {
		return 0, e
	}
	defer tx.Rollback()
	for _, v := range completed {
		if _, e = tx.ExecContext(ctx, "INSERT OR IGNORE INTO capture_consumptions VALUES(?,?,?,?)", v.key, Now(), v.state, v.code); e != nil {
			return 0, e
		}
	}
	if e = tx.Commit(); e != nil {
		return 0, e
	}
	return len(completed), nil
}
func (a *App) StartAutoConnect(ctx context.Context) {
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			for i := 0; i < 8; i++ {
				n, e := a.ProcessCaptureBatch(ctx, 64)
				if e != nil {
					if ctx.Err() == nil {
						a.Logger.Warn("capture control worker", "error", e)
					}
					break
				}
				if n < 64 {
					break
				}
			}
		}
	}
}
func (a *App) consumeCapture(ctx context.Context, o captureObservation) (string, string, error) {
	kind := Str(o.capture(), "kind")
	if kind == "GATT_LINK_DOWN" {
		return a.consumeLinkDown(ctx, o)
	}
	if kind != "ADV" && kind != "SCAN_RSP" {
		return "observed", "", nil
	}
	if Str(o.Raw, "protocolHint") != "ble-ad-v1" {
		return "ignored", "PROTOCOL_NOT_BLE_AD", nil
	}
	if !o.fresh(Now(), 30000) {
		return "ignored", "STALE_OBSERVATION", nil
	}
	if _, e := ExtractServiceUUIDs(o.Payload); e != nil {
		var p *ParserFailure
		code := "BLE_AD_INVALID"
		if errors.As(e, &p) {
			code = p.Code
		}
		return "invalid", code, nil
	}
	candidates := []captureObservation{}
	if kind == "ADV" {
		if Str(o.capture(), "radio") == "NRF54" && o.capture()["connectable"] == true {
			candidates = append(candidates, o)
		}
	} else {
		var e error
		candidates, e = a.relatedObservations(ctx, o, "ADV")
		if e != nil {
			return "", "", e
		}
	}
	for _, adv := range candidates {
		if Str(adv.capture(), "radio") != "NRF54" || adv.capture()["connectable"] != true {
			continue
		}
		created, e := a.evaluateAutoConnect(ctx, adv)
		if e != nil {
			return "", "", e
		}
		if created {
			return "task_created", "", nil
		}
	}
	return "evaluated", "", nil
}
func sameCapturePeer(a, b captureObservation) bool {
	x, y := a.capture(), b.capture()
	if a.Gateway != b.Gateway || a.Namespace != b.Namespace || Str(x, "address") == "" || Str(x, "address") != Str(y, "address") || Str(x, "addressType") != Str(y, "addressType") || x["legacy"] != y["legacy"] {
		return false
	}
	if x["legacy"] == false {
		xs, xok := x["sid"]
		ys, yok := y["sid"]
		if !xok || !yok || xs != ys {
			return false
		}
	}
	d := a.Captured - b.Captured
	if d < 0 {
		d = -d
	}
	return d <= 500 && Str(a.Raw, "timeQuality") == "SYNCED" && Str(b.Raw, "timeQuality") == "SYNCED"
}
func (a *App) relatedObservations(ctx context.Context, base captureObservation, kind string) ([]captureObservation, error) {
	c := base.capture()
	if Str(c, "address") == "" {
		return nil, nil
	}
	rows, e := a.Pipeline.QueryContext(ctx, "SELECT "+observationColumns+" FROM event_receipts e JOIN raw_events r ON r.event_key=e.event_key WHERE e.namespace_id=? AND e.gateway_id=? AND r.captured_at_ms BETWEEN ? AND ? AND json_extract(r.metadata_json,'$.rawEvent.capture.address')=? AND json_extract(r.metadata_json,'$.rawEvent.capture.kind')=? AND json_extract(r.metadata_json,'$.ingressSchemaVersion')='1.1' AND r.protocol_hint='ble-ad-v1' ORDER BY ABS(r.captured_at_ms-?),e.accepted_at_ms,e.event_key LIMIT 256", base.Namespace, base.Gateway, base.Captured-500, base.Captured+500, Str(c, "address"), kind, base.Captured)
	if e != nil {
		return nil, e
	}
	defer rows.Close()
	out := []captureObservation{}
	for rows.Next() {
		o, e := scanObservation(rows)
		if e != nil {
			return nil, e
		}
		if sameCapturePeer(base, o) && o.fresh(Now(), 30000) {
			out = append(out, o)
		}
	}
	return out, rows.Err()
}

type autoCandidateRule struct {
	ID, Owner, Scope, Address, AddressType string
	Revision                               int64
	Definition                             map[string]any
}

func (a *App) evaluateAutoConnect(ctx context.Context, adv captureObservation) (bool, error) {
	if !adv.fresh(Now(), 30000) {
		return false, nil
	}
	capture := adv.capture()
	addr, typ := Str(capture, "address"), Str(capture, "addressType")
	if !registeredAddress(addr, typ) {
		return false, nil
	}
	uuids, e := ExtractServiceUUIDs(adv.Payload)
	if e != nil {
		return false, nil
	}
	rows, e := a.Management.QueryContext(ctx, "SELECT a.rule_id,a.revision,a.rule_json,a.created_by,p.scope_id,p.device_address,p.address_type FROM auto_connect_rules a JOIN ble_peers p ON p.peer_id=a.peer_id WHERE a.enabled=1 AND p.state='active' AND p.namespace_id=? AND p.device_address=? AND p.address_type=? ORDER BY a.priority DESC,a.rule_id LIMIT 1000", adv.Namespace, addr, typ)
	if e != nil {
		return false, e
	}
	rules := []autoCandidateRule{}
	for rows.Next() {
		var r autoCandidateRule
		var raw string
		if e = rows.Scan(&r.ID, &r.Revision, &raw, &r.Owner, &r.Scope, &r.Address, &r.AddressType); e != nil {
			rows.Close()
			return false, e
		}
		r.Definition = pm(raw)
		rules = append(rules, r)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		return false, e
	}
	if len(rules) == 0 {
		return false, nil
	}
	responses, e := a.relatedObservations(ctx, adv, "SCAN_RSP")
	if e != nil {
		return false, e
	}
	for _, r := range rules {
		req := r.Definition
		if !containsAuto(req["gatewayIds"], adv.Gateway) || !adv.fresh(Now(), Num(req, "maxAgeMs")) {
			continue
		}
		if v, ok := req["minRssi"]; ok {
			rss, exists := capture["rssiDbm"]
			if !exists {
				continue
			}
			min, _ := mappingNumber(v)
			actual, _ := mappingNumber(rss)
			if actual < min {
				continue
			}
		}
		var matched *ServiceUUID
		source := adv
		match := func(values []ServiceUUID) *ServiceUUID {
			for _, u := range values {
				if u.UUID == Str(req, "serviceUuid") && (Num(req, "wireBits") == 0 || int64(u.WireBits) == Num(req, "wireBits")) {
					v := u
					return &v
				}
			}
			return nil
		}
		matched = match(uuids)
		if matched == nil {
			for _, rsp := range responses {
				if !rsp.fresh(Now(), Num(req, "maxAgeMs")) {
					continue
				}
				us, e := ExtractServiceUUIDs(rsp.Payload)
				if e != nil {
					continue
				}
				if m := match(us); m != nil {
					matched = m
					source = rsp
					break
				}
			}
		}
		if matched == nil {
			continue
		}
		p, e := a.principal(ctx, r.Owner)
		if errors.Is(e, sql.ErrNoRows) {
			continue
		}
		if e != nil {
			return false, e
		}
		if p.State != "ACTIVE" || !a.Can(p, "gatt:manage", r.Scope) || !a.CanGateway(p, "gatt:execute", adv.Gateway) || !a.CanGateway(p, "task:create", adv.Gateway) {
			continue
		}
		return a.createAutomaticTask(ctx, r, adv, source, *matched)
	}
	return false, nil
}
func containsAuto(v any, want string) bool {
	a, ok := v.([]any)
	if !ok {
		return false
	}
	for _, v := range a {
		if v == want {
			return true
		}
	}
	return false
}
func (a *App) createAutomaticTask(ctx context.Context, rule autoCandidateRule, adv, source captureObservation, uuid ServiceUUID) (bool, error) {
	maxAge := Num(rule.Definition, "maxAgeMs")
	expires := adv.Captured + maxAge
	if x := source.Captured + maxAge; x < expires {
		expires = x
	}
	remaining := expires - Now()
	if remaining < 100 {
		return false, nil
	}
	if remaining > 30000 {
		remaining = 30000
	}
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return false, e
	}
	defer tx.Rollback()
	var current int64
	var enabled int
	var peerState string
	e = tx.QueryRowContext(ctx, "SELECT a.revision,a.enabled,p.state FROM auto_connect_rules a JOIN ble_peers p ON p.peer_id=a.peer_id WHERE a.rule_id=?", rule.ID).Scan(&current, &enabled, &peerState)
	if e != nil {
		return false, e
	}
	if current != rule.Revision || enabled != 1 || peerState != "active" {
		return false, nil
	}
	// Re-read authorization inside the same write transaction as task creation.
	// Disabling an owner, moving a gateway, or revoking a binding must also stop
	// a candidate which was evaluated just before that administration change.
	principal, e := a.principalWithQuery(ctx, tx, rule.Owner)
	if errors.Is(e, sql.ErrNoRows) {
		return false, nil
	}
	if e != nil {
		return false, e
	}
	var gatewayScope string
	if e = tx.QueryRowContext(ctx, "SELECT scope_id FROM gateways WHERE gateway_id=?", adv.Gateway).Scan(&gatewayScope); e != nil {
		return false, e
	}
	if principal.State != "ACTIVE" || !canQuery(ctx, tx, principal, "gatt:manage", rule.Scope) || !canQuery(ctx, tx, principal, "gatt:execute", gatewayScope) || !canQuery(ctx, tx, principal, "task:create", gatewayScope) {
		return false, nil
	}
	var n int
	e = tx.QueryRowContext(ctx, "SELECT count(*) FROM auto_connect_triggers WHERE namespace_id=? AND gateway_id=? AND rule_id=? AND rule_revision=? AND adv_event_id=? AND service_uuid=?", adv.Namespace, adv.Gateway, rule.ID, rule.Revision, adv.Event, uuid.UUID).Scan(&n)
	if e != nil {
		return false, e
	}
	if n > 0 {
		return false, nil
	}
	var caps string
	if e = tx.QueryRowContext(ctx, "SELECT capabilities_json FROM gateways WHERE gateway_id=?", adv.Gateway).Scan(&caps); e != nil {
		return false, e
	}
	var capabilities []string
	_ = json.Unmarshal([]byte(caps), &capabilities)
	have := map[string]bool{}
	for _, c := range capabilities {
		have[c] = true
	}
	for _, c := range []string{"GW32_CONTROL_PROFILE_1", "GW32_CAPTURE_CONTEXT_1", "NRF54_BLE_GATT_CENTRAL", "NRF54_BLE_GATT_SERVICE_DISCOVERY"} {
		if !have[c] {
			return false, nil
		}
	}
	key := pipeStable("auto", []any{adv.Namespace, adv.Gateway, rule.ID, rule.Revision, adv.Event, uuid.UUID})
	trigger := map[string]any{"advEventId": adv.Event, "uuidEventId": source.Event, "ruleId": rule.ID, "ruleRevision": rule.Revision, "serviceUuid": uuid.UUID, "wireBits": uuid.WireBits, "maxAgeMs": maxAge}
	if c := Str(rule.Definition, "characteristicUuid"); c != "" {
		trigger["characteristicUuid"] = c
	}
	request := map[string]any{"gatewayId": adv.Gateway, "type": "GATT_CONNECT", "parameters": map[string]any{"radio": "NRF54", "deviceAddress": rule.Address, "addressType": rule.AddressType, "security": Str(rule.Definition, "security"), "timeoutMs": remaining, "autoTrigger": trigger}, "expiresAt": Timestamp(expires)}
	_, task, _, e := a.newTask(ctx, tx, rule.Owner, key, request)
	if e != nil {
		var api *APIError
		if errors.As(e, &api) && (api.Status == 409 || api.Status == 422 || api.Code == "INVALID_EXPIRY") {
			return false, nil
		}
		return false, e
	}
	_, e = tx.ExecContext(ctx, "INSERT INTO auto_connect_triggers VALUES(?,?,?,?,?,?,?,?,?,?)", key, adv.Namespace, adv.Gateway, rule.ID, rule.Revision, adv.Event, source.Event, uuid.UUID, task, Now())
	if e != nil {
		return false, e
	}
	_, e = tx.ExecContext(ctx, "UPDATE connection_claims SET cooldown_until_ms=MAX(cooldown_until_ms,?) WHERE task_id=?", Now()+Num(rule.Definition, "cooldownMs"), task)
	if e != nil {
		return false, e
	}
	if e = a.Audit(ctx, tx, rule.Owner, "auto-connect.trigger", "gateway", adv.Gateway, map[string]any{"taskId": task, "ruleId": rule.ID, "advEventId": adv.Event, "uuidEventId": source.Event}); e != nil {
		return false, e
	}
	if e = tx.Commit(); e != nil {
		return false, e
	}
	return true, nil
}
func (a *App) consumeLinkDown(ctx context.Context, o captureObservation) (string, string, error) {
	c := o.capture()
	ref, radio := Str(c, "connectionRef"), Str(c, "radio")
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return "", "", e
	}
	defer tx.Rollback()
	var knownRadio string
	e = tx.QueryRowContext(ctx, "SELECT radio FROM gatt_connections WHERE namespace_id=? AND gateway_id=? AND connection_ref=?", o.Namespace, o.Gateway, ref).Scan(&knownRadio)
	if e == nil && knownRadio != radio {
		return "invalid", "CONNECTION_RADIO_MISMATCH", nil
	}
	if e != nil && !errors.Is(e, sql.ErrNoRows) {
		return "", "", e
	}
	now := Now()
	_, e = tx.ExecContext(ctx, `INSERT INTO gatt_connections(namespace_id,gateway_id,connection_ref,radio,state,closed_event_key,details_json,created_at_ms,updated_at_ms,closed_at_ms) VALUES(?,?,?,?,'closed',?,?,?,?,?) ON CONFLICT(namespace_id,gateway_id,connection_ref) DO UPDATE SET state='closed',closed_event_key=COALESCE(gatt_connections.closed_event_key,excluded.closed_event_key),updated_at_ms=excluded.updated_at_ms,closed_at_ms=COALESCE(gatt_connections.closed_at_ms,excluded.closed_at_ms)`, o.Namespace, o.Gateway, ref, radio, o.Key, encode(map[string]any{"linkReason": Str(c, "linkReason"), "eventId": o.Event}), now, now, now)
	if e != nil {
		return "", "", e
	}
	_, e = tx.ExecContext(ctx, "UPDATE gatt_subscriptions SET state='closed',updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND connection_ref=?", now, o.Namespace, o.Gateway, ref)
	if e != nil {
		return "", "", e
	}
	_, e = tx.ExecContext(ctx, "UPDATE connection_claims SET state='cooldown',cooldown_until_ms=MAX(cooldown_until_ms,?),updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND connection_ref=?", now+30000, now, o.Namespace, o.Gateway, ref)
	if e != nil {
		return "", "", e
	}
	if e = a.Audit(ctx, tx, o.Gateway, "gatt.link-down", "gateway", o.Gateway, map[string]any{"connectionRef": ref, "reason": Str(c, "linkReason"), "sourceEventId": o.Event}); e != nil {
		return "", "", e
	}
	if e = tx.Commit(); e != nil {
		return "", "", e
	}
	return "connection_closed", "", nil
}

// Explicitly resolving UNKNOWN requires new device evidence, never a timeout
// alone. Normal capture processing intentionally does not clear such claims.
