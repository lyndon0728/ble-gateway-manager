package platform

import (
	"context"
	"database/sql"
	"errors"
	"net/http"
	"strconv"
)

var snapshotGroups = []string{"network", "nrf54Scan", "nrf54Advertising", "esp32Scan", "esp32Advertising", "upload"}

func (a *App) deviceConfigurationSnapshot(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if ok {
		a.configurationSnapshot(w, r, id)
	}
}
func (a *App) adminConfigurationSnapshot(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.networkAdmin(w, r, "gateway:read")
	if ok {
		a.configurationSnapshot(w, r, id)
	}
}
func (a *App) configurationSnapshot(w http.ResponseWriter, r *http.Request, id string) {
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	if source := r.URL.Query().Get("snapshotId"); source != "" {
		var raw string
		if e = tx.QueryRowContext(r.Context(), "SELECT snapshot_json FROM gateway_configuration_snapshot_history WHERE gateway_id=? AND snapshot_id=?", id, source).Scan(&raw); e != nil || pm(raw)["complete"] != true {
			Fail(w, r, 404, "SNAPSHOT_NOT_FOUND", "complete snapshot unavailable for this gateway")
			return
		}
		JSON(w, 200, pm(raw))
		return
	}
	result, e := a.buildConfigurationSnapshot(r.Context(), tx, id)
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, result)
}
func snapshotPatchGroups(p map[string]any) map[string]any {
	out := map[string]any{}
	if p["network"] != nil {
		out["network"] = p["network"]
	}
	if p["upload"] != nil {
		out["upload"] = p["upload"]
	}
	for _, radio := range []string{"nrf54", "esp32"} {
		v := Obj(Obj(p, "radios"), radio)
		scan := map[string]any{}
		for _, k := range []string{"scan", "filters", "beaconRules", "beaconFilterV2"} {
			if x, ok := v[k]; ok {
				scan[k] = x
			}
		}
		if len(scan) > 0 {
			out[radio+"Scan"] = scan
		}
		if v["advertising"] != nil {
			out[radio+"Advertising"] = v["advertising"]
		}
	}
	return out
}
func snapshotCompleteGroup(group string, v map[string]any) bool {
	has := func(m map[string]any, keys ...string) bool {
		for _, k := range keys {
			if _, ok := m[k]; !ok {
				return false
			}
		}
		return true
	}
	switch group {
	case "upload":
		return has(v, "batchMaxRecords", "flushIntervalMs") && ValidateGatewayConfiguration(map[string]any{"upload": v}) == nil
	case "network":
		if !has(v, "uplinkOrder", "ethernet", "wifiSta") || gw32NetworkShape(v) != nil {
			return false
		}
		for _, key := range []string{"ethernet", "wifiSta"} {
			m := Obj(v, key)
			if !has(m, "enabled", "ipv4") {
				return false
			}
			if key == "wifiSta" && m["enabled"] == true && !has(m, "ssid", "security", "credentialRef") {
				return false
			}
		}
		return true
	case "nrf54Advertising", "esp32Advertising":
		return has(v, "enabled", "mode", "intervalMs", "advDataBase64", "scanResponseBase64") && gw32AdvertisingShape(v) == nil
	default:
		if !has(v, "scan", "filters", "beaconRules", "beaconFilterV2") || !has(Obj(v, "scan"), "enabled", "active", "intervalMs", "windowMs") || !has(Obj(v, "filters"), "minRssi", "serviceUuids", "manufacturerIds") {
			return false
		}
		_, e := CompileScanProfileV2(v, 1)
		return e == nil
	}
}
func (a *App) buildConfigurationSnapshot(ctx context.Context, tx *sql.Tx, id string) (map[string]any, error) {
	rows, e := tx.QueryContext(ctx, `SELECT c.revision,c.reported_json,c.verified_at_ms,t.task_id FROM gateway_configs c JOIN command_tasks t ON t.config_id=c.config_id AND t.state='succeeded' WHERE c.gateway_id=? AND c.state='verified' AND c.reported_json IS NOT NULL AND c.verified_at_ms IS NOT NULL ORDER BY c.revision`, id)
	if e != nil {
		return nil, e
	}
	groups := map[string]any{}
	values := map[string]any{}
	for rows.Next() {
		var revision, at int64
		var raw, task string
		if e = rows.Scan(&revision, &raw, &at, &task); e != nil {
			rows.Close()
			return nil, e
		}
		for group, patch := range snapshotPatchGroups(frontendCanonicalPatch(pm(raw), true)) {
			merged := gw32Merge(Obj(values, group), patch.(map[string]any))
			values[group] = merged
			if snapshotCompleteGroup(group, merged) {
				groups[group] = map[string]any{"revision": revision, "value": merged, "source": map[string]any{"kind": "VERIFIED_TASK", "id": task}, "verifiedAt": Timestamp(at)}
			} else {
				delete(groups, group)
			}
		}
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		return nil, e
	}
	missing := []any{}
	for _, k := range snapshotGroups {
		if groups[k] == nil {
			missing = append(missing, k)
		}
	}
	blocked := []any{}
	if len(missing) > 0 {
		blocked = append(blocked, "MISSING_BASELINE")
	}
	var count int
	if e = tx.QueryRowContext(ctx, "SELECT count(*) FROM command_tasks WHERE gateway_id=? AND state='unknown'", id).Scan(&count); e != nil {
		return nil, e
	}
	if count > 0 {
		blocked = append(blocked, "UNKNOWN_TASK")
	}
	if e = tx.QueryRowContext(ctx, "SELECT count(*) FROM command_tasks WHERE gateway_id=? AND operation='CONFIGURE' AND state IN ('queued','sent','device_accepted','executing','applied')", id).Scan(&count); e != nil {
		return nil, e
	}
	if count > 0 {
		blocked = append(blocked, "PENDING_CONFIGURATION")
	}
	var local string
	e = tx.QueryRowContext(ctx, "SELECT state_json FROM gateway_network_states WHERE gateway_id=?", id).Scan(&local)
	if e != nil && !errors.Is(e, sql.ErrNoRows) {
		return nil, e
	}
	if pm(local)["localOverride"] == true {
		blocked = append(blocked, "LOCAL_OVERRIDE")
	}
	content := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": id, "complete": len(missing) == 0 && len(blocked) == 0, "groups": groups, "missingGroups": missing, "blockedReasons": blocked}
	hash := networkSnapshotID(content)
	var oldHash, oldJSON, generation string
	e = tx.QueryRowContext(ctx, "SELECT content_hash,snapshot_json,generation FROM gateway_configuration_snapshots WHERE gateway_id=?", id).Scan(&oldHash, &oldJSON, &generation)
	if e != nil && !errors.Is(e, sql.ErrNoRows) {
		return nil, e
	}
	if e == nil && oldHash == hash {
		return pm(oldJSON), nil
	}
	n := uint64(0)
	if generation != "" {
		n, e = networkCounter(generation)
		if e != nil || n == ^uint64(0) {
			return nil, Problem(409, "SNAPSHOT_GENERATION_EXHAUSTED", "snapshot generation invalid")
		}
	}
	content["generation"] = strconv.FormatUint(n+1, 10)
	content["snapshotId"] = ID("snapshot")
	content["digest"] = networkSnapshotID(content)
	_, e = tx.ExecContext(ctx, "INSERT INTO gateway_configuration_snapshots(gateway_id,generation,content_hash,snapshot_json) VALUES(?,?,?,?) ON CONFLICT(gateway_id) DO UPDATE SET generation=excluded.generation,content_hash=excluded.content_hash,snapshot_json=excluded.snapshot_json", id, content["generation"], hash, encode(content))
	if e == nil {
		_, e = tx.ExecContext(ctx, "INSERT INTO gateway_configuration_snapshot_history(snapshot_id,gateway_id,snapshot_json) VALUES(?,?,?)", content["snapshotId"], id, encode(content))
	}
	return content, e
}
