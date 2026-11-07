package platform

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
)

type gw32Connection struct {
	Radio, AddressType, Address, State, Security string
	MTU                                          int64
}

func gw32Radio(params map[string]any) string {
	if r := Str(params, "radio"); r != "" {
		return r
	}
	return "NRF54"
}
func (a *App) gw32Connection(ctx context.Context, tx *sql.Tx, gateway, ref string) (gw32Connection, error) {
	var c gw32Connection
	e := tx.QueryRowContext(ctx, "SELECT radio,address_type,peer_address,state,security_state,att_mtu FROM gatt_connections WHERE namespace_id=? AND gateway_id=? AND connection_ref=?", a.Config.NamespaceID, gateway, ref).Scan(&c.Radio, &c.AddressType, &c.Address, &c.State, &c.Security, &c.MTU)
	return c, e
}
func gw32GattTask(op string) bool { return strings.HasPrefix(op, "GATT_") }
func gw32ConnectionTask(op string, params map[string]any) bool {
	return op == "GATT_CONNECT" || (op == "GATT_READ" || op == "GATT_WRITE") && Str(params, "connectionRef") == ""
}

func (a *App) validateGW32Task(ctx context.Context, tx *sql.Tx, g map[string]any, op string, params map[string]any) error {
	if op == "DIAGNOSTICS" {
		if mode, present := params["wifiScan"]; present {
			if Str(params, "profile") != "NETWORK" || mode != "OFF" && mode != "PASSIVE" && mode != "ACTIVE" {
				return Problem(422, "INVALID_DIAGNOSTIC", "wifiScan requires NETWORK and OFF/PASSIVE/ACTIVE")
			}
			if e := gw32Profile(g); e != nil {
				return e
			}
			if e := gw32Require(g, "NETWORK_WIFI_STA"); e != nil {
				return e
			}
		}
		return nil
	}
	if !gw32GattTask(op) {
		return nil
	}
	extended := op != "GATT_READ" && op != "GATT_WRITE" || params["radio"] != nil || params["connectionRef"] != nil || params["autoTrigger"] != nil
	if !gw32Model(g) && !extended {
		return nil
	}
	if e := gw32Profile(g); e != nil {
		return e
	}
	radio := gw32Radio(params)
	if radio != "NRF54" && radio != "ESP32" {
		return Problem(422, "INVALID_RADIO", "radio must be NRF54 or ESP32")
	}
	if e := gw32Require(g, radio+"_BLE_GATT_CENTRAL"); e != nil {
		return e
	}
	if !gw32Range(params["timeoutMs"], 100, 30000) {
		return Problem(422, "INVALID_TIMEOUT", "GATT timeout must be 100..30000 ms")
	}
	if op == "GATT_SUBSCRIBE" || op == "GATT_UNSUBSCRIBE" {
		if e := gw32Require(g, radio+"_BLE_GATT_NOTIFY", "GW32_CAPTURE_CONTEXT_1"); e != nil {
			return e
		}
	}
	if auto := Obj(params, "autoTrigger"); len(auto) > 0 {
		if op != "GATT_CONNECT" || radio != "NRF54" {
			return Problem(422, "INVALID_AUTO_TRIGGER", "automatic connections require NRF54")
		}
		if e := gw32Require(g, "GW32_CAPTURE_CONTEXT_1", "NRF54_BLE_GATT_SERVICE_DISCOVERY"); e != nil {
			return e
		}
	}
	if op == "GATT_CONNECT" || op == "GATT_READ" || op == "GATT_WRITE" {
		if gw32Peer(Str(params, "deviceAddress")) == "" || Str(params, "addressType") != "PUBLIC" && Str(params, "addressType") != "RANDOM" {
			return Problem(422, "INVALID_PEER", "GATT requires a concrete peer address and address type")
		}
	}
	if op == "GATT_CONNECT" && Str(params, "security") != "UNENCRYPTED" && Str(params, "security") != "BONDED_ENCRYPTED" {
		return Problem(422, "INVALID_SECURITY", "unknown requested BLE security")
	}
	for _, key := range []string{"serviceUuid", "characteristicUuid"} {
		if v, ok := params[key]; ok {
			s, ok := v.(string)
			if !ok || gw32CanonicalUUID(s) == "" {
				return Problem(422, "INVALID_UUID", "invalid GATT service or characteristic UUID")
			}
		}
	}
	var connection gw32Connection
	if ref := Str(params, "connectionRef"); ref != "" {
		if !gw32ConnectionPattern.MatchString(ref) {
			return Problem(422, "INVALID_CONNECTION_REF", "connectionRef must be 32 lowercase hex characters")
		}
		var e error
		connection, e = a.gw32Connection(ctx, tx, Str(g, "gatewayId"), ref)
		if e != nil && !errors.Is(e, sql.ErrNoRows) {
			return e
		}
		if e != nil || connection.State != "active" || connection.Radio != radio {
			return Problem(409, "STALE_CONNECTION", "connection is not registered as active on this gateway and radio")
		}
		if op == "GATT_READ" || op == "GATT_WRITE" {
			if connection.Address != gw32Peer(Str(params, "deviceAddress")) || connection.AddressType != Str(params, "addressType") {
				return Problem(409, "CONNECTION_PEER_MISMATCH", "connection peer does not match request")
			}
		}
	} else if op == "GATT_DISCONNECT" || op == "GATT_SUBSCRIBE" || op == "GATT_UNSUBSCRIBE" {
		return Problem(422, "CONNECTION_REQUIRED", "this GATT action requires an active connectionRef")
	}
	if gw32ConnectionTask(op, params) {
		var count int
		if e := tx.QueryRowContext(ctx, "SELECT (SELECT count(*) FROM gatt_connections WHERE namespace_id=? AND gateway_id=? AND radio=? AND state IN ('active','unknown'))+(SELECT count(*) FROM connection_claims WHERE namespace_id=? AND gateway_id=? AND radio=? AND state IN ('attempting','active','unknown'))", a.Config.NamespaceID, Str(g, "gatewayId"), radio, a.Config.NamespaceID, Str(g, "gatewayId"), radio).Scan(&count); e != nil {
			return e
		}
		if count > 0 {
			return Problem(409, "RADIO_CONNECTION_BUSY", "radio already has a connection or unresolved attempt")
		}
	}
	if op == "GATT_WRITE" {
		if Str(params, "writeMode") != "WITH_RESPONSE" {
			return Problem(422, "WRITE_MODE_UNSUPPORTED", "GW32 baseline requires WITH_RESPONSE")
		}
		if Str(params, "verification") == "APPLICATION_REPLY" {
			return Problem(422, "APPLICATION_REPLY_UNSUPPORTED", "no installed GW32 application-reply adapter")
		}
		max := 512
		if connection.MTU > 0 && connection.MTU-3 < int64(max) {
			max = int(connection.MTU - 3)
		}
		if _, e := gw32Decode(params["valueBase64"], 1, max); e != nil {
			return Problem(422, "GATT_VALUE_LENGTH", "write length must fit the negotiated ATT MTU and 512-byte limit")
		}
	}
	if op == "GATT_SUBSCRIBE" {
		var n int
		e := tx.QueryRowContext(ctx, "SELECT count(*) FROM gatt_subscriptions WHERE namespace_id=? AND gateway_id=? AND connection_ref=? AND state='active' AND NOT(service_uuid=? AND characteristic_uuid=?)", a.Config.NamespaceID, Str(g, "gatewayId"), Str(params, "connectionRef"), gw32CanonicalUUID(Str(params, "serviceUuid")), gw32CanonicalUUID(Str(params, "characteristicUuid"))).Scan(&n)
		if e != nil {
			return e
		}
		if n >= 4 {
			return Problem(409, "SUBSCRIPTION_LIMIT", "GW32 baseline supports at most four subscriptions per connection")
		}
	}
	return nil
}

func (a *App) reserveGW32Connection(ctx context.Context, tx *sql.Tx, g map[string]any, id, op string, params map[string]any) error {
	if !gw32Model(g) || !gw32ConnectionTask(op, params) {
		return nil
	}
	peer, addressType := gw32Peer(Str(params, "deviceAddress")), Str(params, "addressType")
	var state string
	var cooldown int64
	e := tx.QueryRowContext(ctx, "SELECT state,cooldown_until_ms FROM connection_claims WHERE namespace_id=? AND address_type=? AND peer_address=?", a.Config.NamespaceID, addressType, peer).Scan(&state, &cooldown)
	if e != nil && !errors.Is(e, sql.ErrNoRows) {
		return e
	}
	if e == nil && (state != "cooldown" || cooldown > Now()) {
		return Problem(409, "PEER_ALREADY_CLAIMED", "peer has an outstanding connection, uncertain attempt, or active cooldown")
	}
	_, e = tx.ExecContext(ctx, "INSERT INTO connection_claims(namespace_id,address_type,peer_address,gateway_id,radio,task_id,state,cooldown_until_ms,updated_at_ms) VALUES(?,?,?,?,?,?,'attempting',?,?) ON CONFLICT(namespace_id,address_type,peer_address) DO UPDATE SET gateway_id=excluded.gateway_id,radio=excluded.radio,task_id=excluded.task_id,connection_ref=NULL,state='attempting',cooldown_until_ms=excluded.cooldown_until_ms,updated_at_ms=excluded.updated_at_ms", a.Config.NamespaceID, addressType, peer, Str(g, "gatewayId"), gw32Radio(params), id, Now()+30000, Now())
	return e
}

type gw32BatchNovelKey struct{}

func (a *App) gw32BatchContext(ctx context.Context, tx *sql.Tx, batch map[string]any) (context.Context, error) {
	novel := map[string]bool{}
	for _, raw := range List(batch, "results") {
		result := raw.(map[string]any)
		var n int
		if e := tx.QueryRowContext(ctx, "SELECT count(*) FROM command_results WHERE result_id=?", Str(result, "resultId")).Scan(&n); e != nil {
			return ctx, e
		}
		if n == 0 {
			novel[Str(result, "resultId")] = true
		}
	}
	return context.WithValue(ctx, gw32BatchNovelKey{}, novel), nil
}

func (a *App) validateGW32Result(ctx context.Context, tx *sql.Tx, gateway string, command, result map[string]any) (string, error) {
	g, e := a.gatewayView(ctx, tx, gateway)
	if e != nil {
		return "", e
	}
	if !gw32Model(g) {
		return "", nil
	}
	params, details := Obj(command, "parameters"), Obj(result, "details")
	op, state := Str(command, "type"), Str(result, "state")
	if op == "CONFIGURE" && Obj(params, "configuration")["network"] != nil {
		probe := Obj(details, "networkProbe")
		if state == "EXECUTING" && len(probe) > 0 {
			if Str(result, "verification") != "NONE" || Num(probe, "candidateRevision") != Num(params, "desiredRevision") || Str(probe, "interface") != "ETHERNET" && Str(probe, "interface") != "WIFI_STA" {
				return "NETWORK_PROBE_INVALID", nil
			}
			for _, k := range []string{"linkReady", "ipReady", "dnsReady", "tlsReady"} {
				if v, ok := probe[k].(bool); !ok || !v {
					return "NETWORK_PROBE_INVALID", nil
				}
			}
		}
		if state == "SUCCEEDED" {
			return a.validateGW32NetworkProof(ctx, tx, gateway, command)
		}
		return "", nil
	}
	if op == "DIAGNOSTICS" && state == "SUCCEEDED" && params["wifiScan"] != nil && params["wifiScan"] != "OFF" {
		if Str(result, "verification") != "VERIFIED" {
			return "DIAGNOSTICS_NOT_VERIFIED", nil
		}
		aps, ok := details["accessPoints"].([]any)
		if !ok || len(aps) > 32 {
			return "WIFI_SCAN_RESULT_INVALID", nil
		}
		for _, raw := range aps {
			ap, ok := raw.(map[string]any)
			if !ok || len([]byte(Str(ap, "ssid"))) > 32 || gw32Peer(Str(ap, "bssid")) == "" || !gw32Range(ap["channel"], 1, 14) || !gw32Range(ap["rssiDbm"], -127, 20) || Str(ap, "authentication") == "" {
				return "WIFI_SCAN_RESULT_INVALID", nil
			}
		}
		return "", nil
	}
	if !gw32GattTask(op) || state != "SUCCEEDED" {
		return "", nil
	}
	if Str(result, "verification") != "VERIFIED" || Str(details, "radio") != gw32Radio(params) {
		return "GATT_NOT_VERIFIED", nil
	}
	ref := Str(params, "connectionRef")
	if ref != "" && Str(details, "connectionRef") != ref {
		return "CONNECTION_IDENTITY_MISMATCH", nil
	}
	if op == "GATT_CONNECT" {
		ref = Str(details, "connectionRef")
		if !gw32ConnectionPattern.MatchString(ref) || Str(details, "connectionState") != "CONNECTED" || !gw32Range(details["attMtu"], 23, 517) || gw32Peer(Str(details, "deviceAddress")) != gw32Peer(Str(params, "deviceAddress")) || Str(details, "addressType") != Str(params, "addressType") {
			return "CONNECTION_NOT_VERIFIED", nil
		}
		if old, err := a.gw32Connection(ctx, tx, gateway, ref); err == nil {
			if old.Radio != gw32Radio(params) || old.Address != "" && (old.Address != gw32Peer(Str(params, "deviceAddress")) || old.AddressType != Str(params, "addressType")) {
				return "CONNECTION_REF_CONFLICT", nil
			}
		} else if !errors.Is(err, sql.ErrNoRows) {
			return "", err
		}
		security := Str(details, "securityState")
		if security != "UNENCRYPTED" && security != "ENCRYPTED" && security != "AUTHENTICATED_ENCRYPTED" || Str(params, "security") == "BONDED_ENCRYPTED" && security == "UNENCRYPTED" {
			return "CONNECTION_SECURITY_MISMATCH", nil
		}
		if boot, id := Str(details, "controllerBootId"), Num(details, "connectionId"); boot != "" || details["connectionId"] != nil {
			b, e := hex.DecodeString(boot)
			if e != nil || len(b) != 16 || !gw32Range(details["connectionId"], 1, 4294967295) {
				return "CONNECTION_EVIDENCE_INVALID", nil
			}
			raw := append([]byte("gw32-conn-v1"), byte(0))
			if gw32Radio(params) == "ESP32" {
				raw[len(raw)-1] = 1
			}
			raw = append(raw, b...)
			raw = binary.LittleEndian.AppendUint32(raw, uint32(id))
			hash := sha256.Sum256(raw)
			if hex.EncodeToString(hash[:16]) != ref {
				return "CONNECTION_REF_MISMATCH", nil
			}
		}
		if auto := Obj(params, "autoTrigger"); len(auto) > 0 {
			if Str(details, "ruleId") != Str(auto, "ruleId") || Str(details, "advEventId") != Str(auto, "advEventId") || gw32CanonicalUUID(Str(details, "serviceUuid")) != Str(auto, "serviceUuid") {
				return "SERVICE_DISCOVERY_NOT_VERIFIED", nil
			}
			if char := Str(auto, "characteristicUuid"); char != "" && (gw32CanonicalUUID(Str(details, "characteristicUuid")) != char || !gw32Range(details["characteristicHandle"], 1, 65535)) {
				return "CHARACTERISTIC_DISCOVERY_NOT_VERIFIED", nil
			}
		}
		return "", nil
	}
	if op == "GATT_DISCONNECT" {
		if Str(details, "connectionState") != "CLOSED" {
			return "DISCONNECT_NOT_VERIFIED", nil
		}
		return "", nil
	}
	if gw32CanonicalUUID(Str(details, "serviceUuid")) != gw32CanonicalUUID(Str(params, "serviceUuid")) || gw32CanonicalUUID(Str(details, "characteristicUuid")) != gw32CanonicalUUID(Str(params, "characteristicUuid")) {
		return "GATT_CHARACTERISTIC_MISMATCH", nil
	}
	if op == "GATT_SUBSCRIBE" {
		if details["cccEnabled"] != true || Str(details, "mode") != Str(params, "mode") || !gw32Range(details["characteristicHandle"], 1, 65535) || !gw32Range(details["cccHandle"], 1, 65535) {
			return "SUBSCRIPTION_NOT_VERIFIED", nil
		}
		return "", nil
	}
	if op == "GATT_UNSUBSCRIBE" {
		if details["cccEnabled"] != false {
			return "UNSUBSCRIBE_NOT_VERIFIED", nil
		}
		return "", nil
	}
	if op == "GATT_READ" || op == "GATT_WRITE" {
		if gw32Peer(Str(details, "deviceAddress")) != gw32Peer(Str(params, "deviceAddress")) || Str(details, "addressType") != Str(params, "addressType") {
			return "GATT_PEER_MISMATCH", nil
		}
		if op == "GATT_READ" {
			if _, e := gw32Decode(details["valueBase64"], 0, 512); e != nil {
				return "GATT_READ_INVALID", nil
			}
		} else {
			if details["attResponse"] != true {
				return "ATT_WRITE_NOT_CONFIRMED", nil
			}
			if Str(params, "verification") == "READBACK" {
				readback, e := gw32Decode(details["readbackBase64"], 1, 512)
				value, _ := gw32Decode(params["valueBase64"], 1, 512)
				if e != nil || string(readback) != string(value) {
					return "WRITE_READBACK_MISMATCH", nil
				}
			}
		}
		if ref == "" && Str(details, "connectionState") != "CLOSED" {
			return "TEMPORARY_CONNECTION_NOT_CLOSED", nil
		}
	}
	return "", nil
}

func (a *App) validateGW32NetworkProof(ctx context.Context, tx *sql.Tx, gateway string, command map[string]any) (string, error) {
	params := Obj(command, "parameters")
	var base int64
	if e := tx.QueryRowContext(ctx, "SELECT COALESCE(base_revision,0) FROM gateway_configs WHERE gateway_id=? AND revision=?", gateway, Num(params, "desiredRevision")).Scan(&base); e != nil {
		return "", e
	}
	before, e := a.gw32Snapshot(ctx, tx, gateway, base)
	if e != nil {
		return "", e
	}
	after := gw32ApplyConfig(before, Obj(params, "configuration"))
	next, old := Obj(after, "network"), Obj(before, "network")
	required := map[string]bool{}
	for i, v := range List(next, "uplinkOrder") {
		name := "ethernet"
		if v == "WIFI_STA" {
			name = "wifiSta"
		}
		if i == 0 || Hash(Obj(next, name)) != Hash(Obj(old, name)) {
			required[v.(string)] = true
		}
	}
	rows, e := tx.QueryContext(ctx, "SELECT result_id,result_json FROM command_results WHERE task_id=? AND stage='executing'", Str(command, "taskId"))
	if e != nil {
		return "", e
	}
	defer rows.Close()
	novel, _ := ctx.Value(gw32BatchNovelKey{}).(map[string]bool)
	for rows.Next() {
		var id, raw string
		if e = rows.Scan(&id, &raw); e != nil {
			return "", e
		}
		if novel[id] {
			continue
		}
		var v map[string]any
		if json.Unmarshal([]byte(raw), &v) != nil {
			continue
		}
		probe := Obj(Obj(v, "details"), "networkProbe")
		if Num(probe, "candidateRevision") == Num(params, "desiredRevision") && Str(v, "verification") == "NONE" && probe["linkReady"] == true && probe["ipReady"] == true && probe["dnsReady"] == true && probe["tlsReady"] == true {
			delete(required, Str(probe, "interface"))
		}
	}
	if e = rows.Err(); e != nil {
		return "", e
	}
	if len(required) > 0 {
		return "NETWORK_PROBES_NOT_PERSISTED", nil
	}
	return "", nil
}

func (a *App) applyGW32Result(ctx context.Context, tx *sql.Tx, command, result map[string]any, state string) error {
	gateway, task, op := Str(command, "gatewayId"), Str(command, "taskId"), Str(command, "type")
	params, details := Obj(command, "parameters"), Obj(result, "details")
	g, e := a.gatewayView(ctx, tx, gateway)
	if e != nil || !gw32Model(g) {
		return e
	}
	if op == "REBOOT" && (state == "succeeded" || state == "unknown") {
		target := "closed"
		if state == "unknown" {
			target = "unknown"
		}
		return a.gw32InvalidateGateway(ctx, tx, gateway, target)
	}
	if !gw32GattTask(op) {
		return nil
	}
	ref := Str(params, "connectionRef")
	if state == "unknown" {
		if _, e = tx.ExecContext(ctx, "UPDATE connection_claims SET state='unknown',updated_at_ms=? WHERE namespace_id=? AND (task_id=? OR (gateway_id=? AND connection_ref=?))", Now(), a.Config.NamespaceID, task, gateway, ref); e != nil {
			return e
		}
		if ref != "" {
			_, e = tx.ExecContext(ctx, "UPDATE gatt_connections SET state='unknown',updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND connection_ref=? AND state='active'", Now(), a.Config.NamespaceID, gateway, ref)
		}
		return e
	}
	if state == "succeeded" && op == "GATT_CONNECT" {
		ref = Str(details, "connectionRef")
		existing, e := a.gw32Connection(ctx, tx, gateway, ref)
		if e != nil && !errors.Is(e, sql.ErrNoRows) {
			return e
		}
		if e == nil && existing.State == "closed" {
			return a.releaseGW32Claim(ctx, tx, task, "")
		}
		if e == nil && existing.Radio != gw32Radio(params) {
			return Problem(409, "CONNECTION_IDENTITY_CONFLICT", "connectionRef cannot change radio")
		}
		_, e = tx.ExecContext(ctx, "INSERT INTO gatt_connections(namespace_id,gateway_id,connection_ref,radio,address_type,peer_address,security_state,att_mtu,state,opened_task_id,details_json,created_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,'active',?,?,?,?) ON CONFLICT(namespace_id,gateway_id,connection_ref) DO UPDATE SET state='active',security_state=excluded.security_state,att_mtu=excluded.att_mtu,details_json=excluded.details_json,updated_at_ms=excluded.updated_at_ms WHERE gatt_connections.state<>'closed'", a.Config.NamespaceID, gateway, ref, gw32Radio(params), Str(params, "addressType"), gw32Peer(Str(params, "deviceAddress")), Str(details, "securityState"), Num(details, "attMtu"), task, encode(details), Now(), Now())
		if e != nil {
			return e
		}
		_, e = tx.ExecContext(ctx, "UPDATE connection_claims SET state='active',connection_ref=?,updated_at_ms=? WHERE namespace_id=? AND task_id=?", ref, Now(), a.Config.NamespaceID, task)
		return e
	}
	if state == "succeeded" && op == "GATT_DISCONNECT" {
		if _, e = tx.ExecContext(ctx, "UPDATE gatt_connections SET state='closed',closed_at_ms=?,updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND connection_ref=?", Now(), Now(), a.Config.NamespaceID, gateway, ref); e != nil {
			return e
		}
		if _, e = tx.ExecContext(ctx, "UPDATE gatt_subscriptions SET state='closed',updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND connection_ref=?", Now(), a.Config.NamespaceID, gateway, ref); e != nil {
			return e
		}
		return a.releaseGW32Claim(ctx, tx, "", gateway+"/"+ref)
	}
	if state == "succeeded" && (op == "GATT_SUBSCRIBE" || op == "GATT_UNSUBSCRIBE") {
		if op == "GATT_UNSUBSCRIBE" {
			_, e = tx.ExecContext(ctx, "UPDATE gatt_subscriptions SET state='closed',updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND connection_ref=? AND service_uuid=? AND characteristic_uuid=?", Now(), a.Config.NamespaceID, gateway, ref, gw32CanonicalUUID(Str(params, "serviceUuid")), gw32CanonicalUUID(Str(params, "characteristicUuid")))
			return e
		}
		connection, e := a.gw32Connection(ctx, tx, gateway, ref)
		if e != nil {
			return e
		}
		subscriptionState := "active"
		if connection.State != "active" {
			subscriptionState = "closed"
		}
		_, e = tx.ExecContext(ctx, "INSERT INTO gatt_subscriptions(namespace_id,gateway_id,connection_ref,service_uuid,characteristic_uuid,characteristic_handle,mode,state,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(namespace_id,gateway_id,connection_ref,service_uuid,characteristic_uuid) DO UPDATE SET characteristic_handle=excluded.characteristic_handle,mode=excluded.mode,state=excluded.state,updated_at_ms=excluded.updated_at_ms", a.Config.NamespaceID, gateway, ref, gw32CanonicalUUID(Str(params, "serviceUuid")), gw32CanonicalUUID(Str(params, "characteristicUuid")), Num(details, "characteristicHandle"), Str(params, "mode"), subscriptionState, Now())
		return e
	}
	if gw32ConnectionTask(op, params) && (state == "succeeded" || state == "failed" || state == "expired" || state == "cancelled") {
		knownClosed := state == "succeeded" || Str(details, "connectionState") == "CLOSED"
		switch Str(result, "errorCode") {
		case "UNSUPPORTED", "BUSY", "STALE_OBSERVATION", "BAD_PARAM", "PEER_REJECTED":
			knownClosed = true
		}
		if knownClosed {
			return a.releaseGW32Claim(ctx, tx, task, "")
		}
		_, e = tx.ExecContext(ctx, "UPDATE connection_claims SET state='unknown',updated_at_ms=? WHERE namespace_id=? AND task_id=?", Now(), a.Config.NamespaceID, task)
		return e
	}
	return nil
}

func (a *App) releaseGW32Claim(ctx context.Context, tx *sql.Tx, task, identity string) error {
	if task != "" {
		_, e := tx.ExecContext(ctx, "UPDATE connection_claims SET state='cooldown',cooldown_until_ms=MAX(cooldown_until_ms,?),updated_at_ms=? WHERE namespace_id=? AND task_id=?", Now()+30000, Now(), a.Config.NamespaceID, task)
		return e
	}
	parts := strings.SplitN(identity, "/", 2)
	if len(parts) != 2 {
		return errors.New("invalid connection identity")
	}
	_, e := tx.ExecContext(ctx, "UPDATE connection_claims SET state='cooldown',cooldown_until_ms=MAX(cooldown_until_ms,?),updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND connection_ref=?", Now()+30000, Now(), a.Config.NamespaceID, parts[0], parts[1])
	return e
}
func (a *App) gw32InvalidateGateway(ctx context.Context, tx *sql.Tx, gateway, state string) error {
	var closed any
	if state == "closed" {
		closed = Now()
	}
	if _, e := tx.ExecContext(ctx, "UPDATE gatt_connections SET state=?,closed_at_ms=?,updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND state<>'closed'", state, closed, Now(), a.Config.NamespaceID, gateway); e != nil {
		return e
	}
	claimState := "unknown"
	if state == "closed" {
		claimState = "cooldown"
		if _, e := tx.ExecContext(ctx, "UPDATE gatt_subscriptions SET state='closed',updated_at_ms=? WHERE namespace_id=? AND gateway_id=?", Now(), a.Config.NamespaceID, gateway); e != nil {
			return e
		}
	}
	_, e := tx.ExecContext(ctx, "UPDATE connection_claims SET state=?,cooldown_until_ms=MAX(cooldown_until_ms,?),updated_at_ms=? WHERE namespace_id=? AND gateway_id=? AND state<>'cooldown'", claimState, Now()+30000, Now(), a.Config.NamespaceID, gateway)
	return e
}
func (a *App) reconcileGW32Expiries(ctx context.Context, tx *sql.Tx) error {
	if _, e := tx.ExecContext(ctx, "UPDATE connection_claims SET state='unknown',updated_at_ms=? WHERE task_id IN(SELECT task_id FROM command_tasks WHERE state='unknown') AND state='attempting'", Now()); e != nil {
		return e
	}
	if _, e := tx.ExecContext(ctx, "UPDATE connection_claims SET state='cooldown',cooldown_until_ms=MAX(cooldown_until_ms,?),updated_at_ms=? WHERE task_id IN(SELECT task_id FROM command_tasks WHERE state IN ('expired','cancelled')) AND state='attempting'", Now()+30000, Now()); e != nil {
		return e
	}
	if _, e := tx.ExecContext(ctx, "UPDATE gatt_connections SET state='unknown',updated_at_ms=? WHERE state='active' AND EXISTS(SELECT 1 FROM command_tasks t WHERE t.gateway_id=gatt_connections.gateway_id AND t.state='unknown' AND (t.operation='REBOOT' OR json_extract(t.request_json,'$.parameters.connectionRef')=gatt_connections.connection_ref))", Now()); e != nil {
		return e
	}
	_, e := tx.ExecContext(ctx, "UPDATE connection_claims SET state='unknown',updated_at_ms=? WHERE state='active' AND EXISTS(SELECT 1 FROM gatt_connections c WHERE c.namespace_id=connection_claims.namespace_id AND c.gateway_id=connection_claims.gateway_id AND c.connection_ref=connection_claims.connection_ref AND c.state='unknown')", Now())
	return e
}
