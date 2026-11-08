package platform

import (
	"context"
	"database/sql"
	"net/http"
)

// connectionRef includes controller boot identity and a non-reused connection
// counter (gateway-protocol.md). Conflicting mappings are retained and rejected,
// never resolved by selecting the newest active connection.
func (a *App) rememberProtocolDiscovery(ctx context.Context, tx *sql.Tx, command, result map[string]any) error {
	params, details := Obj(command, "parameters"), Obj(result, "details")
	if Str(result, "verification") != "VERIFIED" {
		return nil
	}
	gateway, ref := Str(command, "gatewayId"), Str(params, "connectionRef")
	if ref == "" {
		ref = Str(details, "connectionRef")
	}
	service, char := gw32CanonicalUUID(Str(params, "serviceUuid")), gw32CanonicalUUID(Str(params, "characteristicUuid"))
	if Str(command, "type") == "GATT_CONNECT" {
		auto := Obj(params, "autoTrigger")
		if len(auto) == 0 {
			return nil
		}
		service, char = gw32CanonicalUUID(Str(auto, "serviceUuid")), gw32CanonicalUUID(Str(auto, "characteristicUuid"))
	}
	if service == "" || char == "" || !gw32Range(details["characteristicHandle"], 1, 65535) || ref == "" {
		return nil
	}
	connection, e := a.gw32Connection(ctx, tx, gateway, ref)
	if e != nil {
		return e
	}
	if connection.Address == "" || connection.Radio != gw32Radio(params) {
		return nil
	}
	handle := Num(details, "characteristicHandle")
	id := pipeStable("discovery", []any{a.Config.NamespaceID, gateway, connection.Radio, ref, handle, service, char})
	_, e = tx.ExecContext(ctx, "INSERT OR IGNORE INTO gatt_discovery_snapshots(snapshot_id,namespace_id,gateway_id,radio,connection_ref,address_type,device_address,characteristic_handle,service_uuid,characteristic_uuid,source_task_id,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", id, a.Config.NamespaceID, gateway, connection.Radio, ref, connection.AddressType, connection.Address, handle, service, char, Str(command, "taskId"), Now())
	return e
}

func (a *App) freezeCaptureProtocolContext(ctx context.Context, gateway string, event pipeMap) (pipeMap, error) {
	capture := po(event, "capture")
	kind := ps(capture, "kind")
	if kind != "GATT_NOTIFY" && kind != "GATT_INDICATE" {
		return pipeMap{}, nil
	}
	identity := pipeMap{"gatewayId": gateway, "radio": capture["radio"], "connectionRef": capture["connectionRef"], "characteristicHandle": capture["characteristicHandle"], "address": capture["address"], "addressType": capture["addressType"]}
	rows, e := a.Management.QueryContext(ctx, "SELECT snapshot_id,address_type,device_address,service_uuid,characteristic_uuid,source_task_id,created_at_ms FROM gatt_discovery_snapshots WHERE namespace_id=? AND gateway_id=? AND radio=? AND connection_ref=? AND characteristic_handle=? ORDER BY snapshot_id LIMIT 3", a.Config.NamespaceID, gateway, ps(capture, "radio"), ps(capture, "connectionRef"), pn(capture, "characteristicHandle"))
	if e != nil {
		return nil, e
	}
	defer rows.Close()
	found := []pipeMap{}
	for rows.Next() {
		var id, typ, addr, service, char, task string
		var at int64
		if e = rows.Scan(&id, &typ, &addr, &service, &char, &task, &at); e != nil {
			return nil, e
		}
		found = append(found, pipeMap{"snapshotId": id, "addressType": typ, "address": addr, "serviceUuid": service, "characteristicUuid": char, "sourceTaskId": task, "createdAt": Timestamp(at)})
	}
	if e = rows.Err(); e != nil {
		return nil, e
	}
	identity["state"] = "MISSING"
	if len(found) > 1 {
		identity["state"] = "AMBIGUOUS"
		return identity, nil
	}
	if len(found) == 1 {
		if ps(found[0], "address") != ps(capture, "address") || ps(found[0], "addressType") != ps(capture, "addressType") {
			identity["state"] = "IDENTITY_MISMATCH"
			return identity, nil
		}
		for k, v := range found[0] {
			identity[k] = v
		}
		identity["state"] = "VERIFIED"
	}
	return identity, nil
}
func (a *App) protocolDiscoverySnapshots(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:read")
	if !ok {
		return
	}
	gateway := r.URL.Query().Get("gatewayId")
	if gateway == "" || !a.CanGateway(p, "gatt:read", gateway) {
		Fail(w, r, 404, "NOT_FOUND", "select an authorized gateway")
		return
	}
	ref := r.URL.Query().Get("connectionRef")
	rows, e := a.Management.QueryContext(r.Context(), "SELECT snapshot_id,radio,connection_ref,address_type,device_address,characteristic_handle,service_uuid,characteristic_uuid,source_task_id,created_at_ms FROM gatt_discovery_snapshots WHERE gateway_id=? AND namespace_id=? AND (?='' OR connection_ref=?) ORDER BY created_at_ms,snapshot_id LIMIT 10001", gateway, a.Config.NamespaceID, ref, ref)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var id, radio, connection, typ, addr, service, char, task string
		var handle, at int64
		if e = rows.Scan(&id, &radio, &connection, &typ, &addr, &handle, &service, &char, &task, &at); e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, pipeMap{"snapshotId": id, "gatewayId": gateway, "radio": radio, "connectionRef": connection, "addressType": typ, "deviceAddress": addr, "characteristicHandle": handle, "serviceUuid": service, "characteristicUuid": char, "sourceTaskId": task, "createdAt": Timestamp(at)})
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	if len(items) > 10000 {
		Fail(w, r, 422, "QUERY_TOO_BROAD", "filter by connectionRef")
		return
	}
	a.Page(w, r, items)
}

func verifyFrozenProtocolRuntime(manifest pipeMap) error {
	for _, pv := range po(manifest, "protocolParsers") {
		p, ok := pv.(map[string]any)
		if !ok {
			return protocolBad("PARSER_DEPENDENCY_INVALID", "invalid frozen parser")
		}
		spec := po(p, "spec")
		if ps(p, "compilerVersion") != ProtocolCompilerVersion || ps(p, "implementationDigest") != protocolDigest(spec) {
			return protocolBad("PARSER_RUNTIME_MISMATCH", "worker does not support frozen protocol implementation")
		}
		if e := ValidateProtocolSpec(spec); e != nil {
			return e
		}
	}
	return nil
}
