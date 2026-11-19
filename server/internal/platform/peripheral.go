package platform

import (
	"context"
	"crypto/rand"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

func (a *App) putPeripheralClient(w http.ResponseWriter, r *http.Request) {
	p, id, ok := a.networkAdmin(w, r, "gateway:configure")
	if !ok {
		return
	}
	b, ok := a.Decode(w, r, "PeripheralClientAuthorization")
	if !ok {
		return
	}
	client := chi.URLParam(r, "clientId")
	if !artifactIDPattern.MatchString(client) {
		Fail(w, r, 422, "INVALID_CLIENT", "invalid client identity")
		return
	}
	status, result, e := a.DoIdempotent(r.Context(), p.ID, "peripheral.client.authorize", r.Header.Get("Idempotency-Key"), map[string]any{"gatewayId": id, "clientId": client, "authorization": b}, func(tx *sql.Tx) (int, string, any, error) {
		enabled := 0
		if b["enabled"] == true {
			enabled = 1
		}
		_, e := tx.ExecContext(r.Context(), "INSERT INTO peripheral_clients(gateway_id,client_id,routes_json,enabled,updated_at_ms) VALUES(?,?,?,?,?) ON CONFLICT(gateway_id,client_id) DO UPDATE SET routes_json=excluded.routes_json,enabled=excluded.enabled,updated_at_ms=excluded.updated_at_ms", id, client, encode(b["routes"]), enabled, Now())
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "peripheral.client.authorize", "gateway", id, map[string]any{"clientId": client, "enabled": b["enabled"], "routes": b["routes"]})
		}
		return 200, client, map[string]any{"gatewayId": id, "clientId": client, "enabled": b["enabled"], "routes": b["routes"]}, e
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, result)
}
func peripheralAllowed(ctx context.Context, q ctlQuery, gateway, client, route string) bool {
	var raw string
	var enabled int
	if e := q.QueryRowContext(ctx, "SELECT routes_json,enabled FROM peripheral_clients WHERE gateway_id=? AND client_id=?", gateway, client).Scan(&raw, &enabled); e != nil || enabled != 1 {
		return false
	}
	for _, x := range List(map[string]any{"routes": parseList(raw)}, "routes") {
		if x == route {
			return true
		}
	}
	return false
}
func parseList(s string) []any { var m []any; _ = json.Unmarshal([]byte(s), &m); return m }

// Stored business data remains valid after a controller reboot, but it cannot
// establish a live session on a boot that is no longer reported by the device.
func peripheralCurrentBoot(ctx context.Context, q ctlQuery, gateway, radio, boot string) (bool, error) {
	if boot == "" {
		return false, nil
	}
	var raw string
	e := q.QueryRowContext(ctx, "SELECT report_json FROM gateway_runtime_capabilities WHERE gateway_id=?", gateway).Scan(&raw)
	if errors.Is(e, sql.ErrNoRows) {
		return false, nil
	}
	if e != nil {
		return false, e
	}
	key := "nrfBootId"
	if radio == "ESP32" {
		key = "espBootId"
	} else if radio != "NRF54" {
		return false, nil
	}
	return Str(pm(raw), key) == boot, nil
}

func (a *App) peripheralBatch(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 128*1024)
	b, ok := a.Decode(w, r, "PeripheralEventBatch")
	if !ok || !a.integrationIdentity(w, r, id, b) {
		return
	}
	result, e := a.AcceptPeripheralBatch(r.Context(), id, b)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, result)
}

// HTTP and authenticated MQTT share the same commit-before-ack transaction.
func (a *App) AcceptPeripheralBatch(ctx context.Context, id string, b map[string]any) (map[string]any, error) {
	if e := a.Validate("PeripheralEventBatch", b); e != nil {
		return nil, Problem(422, "INVALID_BATCH", "invalid peripheral batch")
	}
	if Str(b, "namespaceId") != a.Config.NamespaceID || Str(b, "gatewayId") != id || len(encode(b)) > 128*1024 {
		return nil, Problem(403, "IDENTITY_MISMATCH", "invalid peripheral batch identity/size")
	}
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return nil, e
	}
	defer tx.Rollback()
	g, e := a.gatewayView(ctx, tx, id)
	if e != nil {
		return nil, e
	}
	if e = gw32Require(g, "PERIPHERAL_BRIDGE_1"); e != nil {
		return nil, e
	}
	items := []any{}
	var used int64
	if e = tx.QueryRowContext(ctx, "SELECT COALESCE(SUM(length(event_json)),0) FROM peripheral_events").Scan(&used); e != nil {
		return nil, e
	}
	seen := map[string]bool{}
	for _, raw := range List(b, "events") {
		event := raw.(map[string]any)
		eventID := Str(event, "eventId")
		item := map[string]any{"eventId": eventID, "status": "ACCEPTED"}
		reject := func(code string) { item["status"] = "REJECTED"; item["code"] = code }
		if seen[eventID] {
			return nil, Problem(422, "INVALID_BATCH", "event IDs must be unique within batch")
		}
		seen[eventID] = true
		if decoded, err := gw32Decode(event["payloadBase64"], 0, 2048); err != nil || base64.StdEncoding.EncodeToString(decoded) != Str(event, "payloadBase64") {
			reject("INVALID_EVENT")
		}
		if Str(event, "timeQuality") == "SYNCED" {
			t, err := time.Parse(time.RFC3339Nano, Str(event, "receivedAt"))
			if err != nil || !strings.HasSuffix(Str(event, "receivedAt"), "Z") || t.UnixMilli() > Now()+1000 {
				reject("INVALID_EVENT")
			}
		} else if _, present := event["receivedAt"]; present {
			reject("INVALID_EVENT")
		}
		if !peripheralAllowed(ctx, tx, id, Str(event, "clientId"), Str(event, "route")) {
			reject("NOT_AUTHORIZED")
		}
		digest := Hash(event)
		var old string
		if item["status"] == "ACCEPTED" {
			err := tx.QueryRowContext(ctx, "SELECT content_hash FROM peripheral_events WHERE namespace_id=? AND gateway_id=? AND event_id=?", a.Config.NamespaceID, id, eventID).Scan(&old)
			if err == nil {
				if old == digest {
					item["status"] = "DUPLICATE"
				} else {
					reject("EVENT_ID_CONFLICT")
				}
			} else if !errors.Is(err, sql.ErrNoRows) {
				return nil, err
			}
		}
		business := map[string]any{"clientId": event["clientId"], "direction": "UPLINK", "businessId": event["businessId"], "route": event["route"], "payloadBase64": event["payloadBase64"]}
		if v, ok := event["inReplyTo"]; ok {
			business["inReplyTo"] = v
		}
		businessHash := Hash(business)
		newBusiness := true
		if item["status"] == "ACCEPTED" {
			err := tx.QueryRowContext(ctx, "SELECT content_hash FROM peripheral_business WHERE namespace_id=? AND gateway_id=? AND client_id=? AND direction='UPLINK' AND business_id=?", a.Config.NamespaceID, id, event["clientId"], event["businessId"]).Scan(&old)
			if err == nil {
				newBusiness = false
				if old == businessHash {
					item["status"] = "DUPLICATE"
				} else {
					reject("BUSINESS_ID_CONFLICT")
				}
			} else if !errors.Is(err, sql.ErrNoRows) {
				return nil, err
			}
		}
		if item["status"] == "ACCEPTED" || item["status"] == "DUPLICATE" {
			// Existing events remain acknowledgeable at capacity. New observations
			// retain their device spool until the durable inbox has room.
			var existing int
			if e = tx.QueryRowContext(ctx, "SELECT count(*) FROM peripheral_events WHERE namespace_id=? AND gateway_id=? AND event_id=?", a.Config.NamespaceID, id, eventID).Scan(&existing); e != nil {
				return nil, e
			}
			if existing == 0 && used+int64(len(encode(event))) > a.Config.MaxQueueBytes {
				item["status"] = "RETRY"
				item["code"] = "CAPACITY_TEMPORARY"
				items = append(items, item)
				continue
			}
			if existing == 0 {
				used += int64(len(encode(event)))
			}
			_, e = tx.ExecContext(ctx, "INSERT OR IGNORE INTO peripheral_events(namespace_id,gateway_id,event_id,content_hash,event_json,received_at_ms) VALUES(?,?,?,?,?,?)", a.Config.NamespaceID, id, eventID, digest, encode(event), Now())
			if e != nil {
				return nil, e
			}
			if newBusiness {
				_, e = tx.ExecContext(ctx, "INSERT OR IGNORE INTO peripheral_business(namespace_id,gateway_id,client_id,direction,business_id,content_hash,event_id) VALUES(?,?,?,'UPLINK',?,?,?)", a.Config.NamespaceID, id, event["clientId"], event["businessId"], businessHash, eventID)
				if e != nil {
					return nil, e
				}
			}
			// A duplicate transport event never refreshes a connection lease.
			// A first delivery from an old offline spool is still persisted, but
			// only the device's current controller boot can authorize downlink.
			if item["status"] == "ACCEPTED" {
				current, err := peripheralCurrentBoot(ctx, tx, id, Str(event, "radio"), Str(event, "controllerBootId"))
				if err != nil {
					return nil, err
				}
				if current {
					_, e = tx.ExecContext(ctx, "INSERT INTO peripheral_connections(gateway_id,connection_ref,radio,client_id,controller_boot_id,state,received_at_ms) VALUES(?,?,?,?,?,'ACTIVE',?) ON CONFLICT(gateway_id,connection_ref) DO UPDATE SET received_at_ms=excluded.received_at_ms WHERE peripheral_connections.client_id=excluded.client_id AND peripheral_connections.controller_boot_id=excluded.controller_boot_id AND peripheral_connections.radio=excluded.radio", id, event["connectionRef"], event["radio"], event["clientId"], event["controllerBootId"], Now())
					if e != nil {
						return nil, e
					}
				}
			}
		}
		items = append(items, item)
	}
	if e = tx.Commit(); e != nil {
		return nil, e
	}
	return map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": id, "batchId": b["batchId"], "durability": "PERIPHERAL_PERSISTED", "items": items}, nil
}
func (a *App) getPeripheralEvents(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.networkAdmin(w, r, "gateway:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT event_json FROM peripheral_events WHERE namespace_id=? AND gateway_id=? ORDER BY received_at_ms DESC,event_id LIMIT 100", a.Config.NamespaceID, id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var raw string
		if e = rows.Scan(&raw); e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, pm(raw))
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"items": items})
}
func (a *App) validatePeripheralTask(ctx context.Context, tx *sql.Tx, g map[string]any, op string, p map[string]any) error {
	if e := gw32Require(g, "PERIPHERAL_BRIDGE_1", Str(p, "radio")+"_BLE_PERIPHERAL"); e != nil {
		return e
	}
	var radio, client, state, boot string
	var at int64
	e := tx.QueryRowContext(ctx, "SELECT radio,client_id,state,received_at_ms,controller_boot_id FROM peripheral_connections WHERE gateway_id=? AND connection_ref=?", Str(g, "gatewayId"), Str(p, "connectionRef")).Scan(&radio, &client, &state, &at, &boot)
	if e != nil || radio != Str(p, "radio") || client != Str(p, "clientId") || state != "ACTIVE" || Now()-at > 60000 {
		return Problem(409, "STALE_PERIPHERAL_SESSION", "no fresh authenticated association for this client and connection")
	}
	current, e := peripheralCurrentBoot(ctx, tx, Str(g, "gatewayId"), radio, boot)
	if e != nil {
		return e
	}
	if !current {
		return Problem(409, "STALE_PERIPHERAL_SESSION", "session controller boot is not currently reported by the device")
	}
	if op == "PERIPHERAL_NOTIFY" {
		if !peripheralAllowed(ctx, tx, Str(g, "gatewayId"), client, Str(p, "route")) {
			return Problem(403, "PERIPHERAL_ROUTE_DENIED", "client route not authorized")
		}
		if _, e = gw32Decode(p["payloadBase64"], 0, 2048); e != nil {
			return e
		}
		if !gw32Range(p["timeoutMs"], 100, 30000) {
			return Problem(422, "INVALID_TIMEOUT", "peripheral timeout must be 100..30000 ms")
		}
		b := make([]byte, 16)
		if _, e = rand.Read(b); e != nil {
			return e
		}
		p["businessId"] = hex.EncodeToString(b)
	}
	return nil
}
