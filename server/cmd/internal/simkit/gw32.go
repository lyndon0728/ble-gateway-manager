package simkit

// This file simulates the ESP-to-server protocol only. It does not own UART
// custody, controller execution or physical dual-radio deduplication.
import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

const GW32ServiceUUID = "12345678-1234-5678-9abc-def012345678"
const GW32CharacteristicUUID = "12345678-1234-5678-9abc-def012345679"
const GW32PeerAddress = "AABBCCDDEEFF"

func (g *Gateway) initializeGW32() error {
	rows, e := g.DB.Query("PRAGMA table_info(spool)")
	if e != nil {
		return e
	}
	hasVersion := false
	for rows.Next() {
		var cid, notnull, pk int
		var name, typ string
		var def any
		if e = rows.Scan(&cid, &name, &typ, &notnull, &def, &pk); e != nil {
			rows.Close()
			return e
		}
		if name == "schema_version" {
			hasVersion = true
		}
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		return e
	}
	if !hasVersion {
		if _, e = g.DB.Exec("ALTER TABLE spool ADD COLUMN schema_version TEXT NOT NULL DEFAULT '1.0'"); e != nil {
			return e
		}
	}
	_, e = g.DB.Exec(`CREATE TABLE IF NOT EXISTS gw32_radios(radio TEXT PRIMARY KEY,boot_id TEXT NOT NULL,next_connection INTEGER NOT NULL);
 CREATE TABLE IF NOT EXISTS gw32_connections(connection_ref TEXT PRIMARY KEY,radio TEXT NOT NULL,address_type TEXT NOT NULL,address TEXT NOT NULL,state TEXT NOT NULL,security TEXT NOT NULL,att_mtu INTEGER NOT NULL,connection_number INTEGER NOT NULL);
 CREATE TABLE IF NOT EXISTS gw32_subscriptions(connection_ref TEXT NOT NULL,service_uuid TEXT NOT NULL,characteristic_uuid TEXT NOT NULL,value_handle INTEGER NOT NULL,ccc_handle INTEGER NOT NULL,mode TEXT NOT NULL,active INTEGER NOT NULL,PRIMARY KEY(connection_ref,service_uuid,characteristic_uuid));
 CREATE TABLE IF NOT EXISTS gw32_values(connection_ref TEXT NOT NULL,service_uuid TEXT NOT NULL,characteristic_uuid TEXT NOT NULL,value_base64 TEXT NOT NULL,PRIMARY KEY(connection_ref,service_uuid,characteristic_uuid));
 CREATE TABLE IF NOT EXISTS gw32_observations(event_id TEXT PRIMARY KEY,address_type TEXT NOT NULL,address TEXT NOT NULL,captured_ms INTEGER NOT NULL,expires_ms INTEGER NOT NULL);
 CREATE TABLE IF NOT EXISTS gw32_network_trials(task_id TEXT PRIMARY KEY,configuration_json TEXT NOT NULL,effective_json TEXT NOT NULL,desired_revision INTEGER NOT NULL,deadline_ms INTEGER NOT NULL,probe_ids_json TEXT NOT NULL,state TEXT NOT NULL);`)
	if e != nil {
		return e
	}
	for _, radio := range []string{"NRF54", "ESP32"} {
		if _, e = g.DB.Exec("INSERT OR IGNORE INTO gw32_radios VALUES(?,?,0)", radio, ID("")); e != nil {
			return e
		}
	}
	// A host process restart models an ESP restart. Uncommitted network trials
	// roll back; committed event payloads keep their original IDs and versions.
	trialRows, e := g.DB.Query("SELECT task_id FROM gw32_network_trials WHERE state='PENDING'")
	if e != nil {
		return e
	}
	var trials []string
	for trialRows.Next() {
		var id string
		if e = trialRows.Scan(&id); e != nil {
			trialRows.Close()
			return e
		}
		trials = append(trials, id)
	}
	e = trialRows.Err()
	trialRows.Close()
	if e != nil {
		return e
	}
	for _, task := range trials {
		tx, e := g.DB.Begin()
		if e != nil {
			return e
		}
		if _, e = tx.Exec("UPDATE gw32_network_trials SET state='ROLLED_BACK' WHERE task_id=?", task); e == nil {
			var revision int64
			e = tx.QueryRow("SELECT revision FROM identity WHERE singleton=1").Scan(&revision)
			if e == nil {
				e = finishGW32(tx, task, "FAILED", "NONE", map[string]any{"simulated": true, "rolledBack": true, "reportedConfigurationRevision": revision}, "NETWORK_TRIAL_RESTART_ROLLBACK")
			}
		}
		if e == nil {
			e = tx.Commit()
		} else {
			_ = tx.Rollback()
		}
		if e != nil {
			return e
		}
	}
	if g.Profile == "gw32-profile-1" {
		if e = g.LinkDownAll(context.Background(), "ESP_RESTART"); e != nil {
			return e
		}
		if _, e = g.DB.Exec("UPDATE gw32_radios SET boot_id=?,next_connection=0 WHERE radio='ESP32'", ID("")); e != nil {
			return e
		}
		if _, e = g.DB.Exec("UPDATE identity SET boot_id=? WHERE singleton=1", ID("")); e != nil {
			return e
		}
		_, e = g.DB.Exec("DELETE FROM gw32_observations")
		if e != nil {
			return e
		}
	}
	return nil
}

func GW32CaptureFixtures(at time.Time) []map[string]any {
	// The first record represents an already finalized ESP merge. Providing two
	// observedBy values is a fixture, not implementation of the ESP merge rule.
	return []map[string]any{
		{"timeQuality": "SYNCED", "capturedAt": at.Add(-100 * time.Millisecond).UTC().Format(time.RFC3339Nano), "sourceDeviceId": "ble:public:" + GW32PeerAddress, "protocolHint": "ble-ad-v1", "payloadBase64": "AgEG", "capture": map[string]any{"radio": "NRF54", "kind": "ADV", "addressType": "PUBLIC", "address": GW32PeerAddress, "legacy": true, "connectable": true, "observedBy": []string{"NRF54", "ESP32"}, "rssiDbm": -50, "primaryPhy": "LE_1M"}},
		{"timeQuality": "SYNCED", "capturedAt": at.UTC().Format(time.RFC3339Nano), "sourceDeviceId": "ble:public:" + GW32PeerAddress, "protocolHint": "ble-ad-v1", "payloadBase64": base64.StdEncoding.EncodeToString([]byte{0x11, 0x07, 0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12}), "capture": map[string]any{"radio": "ESP32", "kind": "SCAN_RSP", "addressType": "PUBLIC", "address": GW32PeerAddress, "legacy": true, "rssiDbm": -57, "primaryPhy": "LE_1M"}},
	}
}
func (g *Gateway) EnqueueGW32(ctx context.Context, count int) error {
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	for range count {
		for _, event := range GW32CaptureFixtures(time.Now()) {
			id, e := insertGW32Event(tx, event)
			if e != nil {
				return e
			}
			capture := object(event, "capture")
			if Str(capture, "kind") == "ADV" {
				at, _ := time.Parse(time.RFC3339Nano, Str(event, "capturedAt"))
				if _, e = tx.Exec("INSERT INTO gw32_observations VALUES(?,?,?,?,?)", id, Str(capture, "addressType"), Str(capture, "address"), at.UnixMilli(), at.Add(30*time.Second).UnixMilli()); e != nil {
					return e
				}
			}
		}
	}
	return tx.Commit()
}
func insertGW32Event(tx *sql.Tx, event map[string]any) (string, error) {
	var seq int64
	var boot string
	if e := tx.QueryRow("SELECT boot_id,sequence FROM identity WHERE singleton=1").Scan(&boot, &seq); e != nil {
		return "", e
	}
	seq++
	id := fmt.Sprintf("%s-%d", boot, seq)
	event["eventId"] = id
	event["sequence"] = seq
	raw, _ := json.Marshal(event)
	if _, e := tx.Exec("INSERT INTO spool(event_id,event_json,schema_version) VALUES(?,?,'1.1')", id, string(raw)); e != nil {
		return "", e
	}
	_, e := tx.Exec("UPDATE identity SET sequence=? WHERE singleton=1", seq)
	return id, e
}
func object(m map[string]any, k string) map[string]any {
	o, _ := m[k].(map[string]any)
	if o == nil {
		return map[string]any{}
	}
	return o
}
func encodeJSON(v any) string { b, _ := json.Marshal(v); return string(b) }
func finishGW32(tx *sql.Tx, task, state, verification string, details map[string]any, code string) error {
	if _, e := tx.Exec("UPDATE journal SET state=? WHERE task_id=?", state, task); e != nil {
		return e
	}
	var seq int
	if e := tx.QueryRow("SELECT COALESCE(MAX(sequence),-1)+1 FROM results WHERE task_id=?", task).Scan(&seq); e != nil {
		return e
	}
	return putResult(tx, task, seq, state, verification, details, code)
}
func ConnectionReference(radio, bootHex string, number uint32) (string, error) {
	boot, e := hex.DecodeString(bootHex)
	if e != nil || len(boot) != 16 {
		return "", errors.New("controller boot ID must be 16 bytes")
	}
	var radioByte byte
	switch radio {
	case "NRF54":
		radioByte = 0
	case "ESP32":
		radioByte = 1
	default:
		return "", errors.New("unknown radio")
	}
	b := append([]byte("gw32-conn-v1"), radioByte)
	b = append(b, boot...)
	n := make([]byte, 4)
	binary.LittleEndian.PutUint32(n, number)
	b = append(b, n...)
	digest := sha256.Sum256(b)
	return hex.EncodeToString(digest[:16]), nil
}

func (g *Gateway) executeGW32(ctx context.Context, cmd map[string]any) (bool, error) {
	typ, task := Str(cmd, "type"), Str(cmd, "taskId")
	params := object(cmd, "parameters")
	if typ == "CONFIGURE" {
		configuration := object(params, "configuration")
		if len(object(configuration, "network")) > 0 {
			return true, g.beginNetworkTrial(ctx, cmd)
		}
		return true, g.configureRadio(ctx, cmd)
	}
	if typ == "DIAGNOSTICS" {
		details := map[string]any{"profile": Str(params, "profile"), "simulated": true, "implementation": "gw32-profile-1 HTTP simulator", "physicalMeasurements": false}
		var configuration string
		if e := g.DB.QueryRowContext(ctx, "SELECT configuration FROM identity WHERE singleton=1").Scan(&configuration); e != nil {
			return true, e
		}
		var effective map[string]any
		if e := json.Unmarshal([]byte(configuration), &effective); e != nil {
			return true, e
		}
		details["effective"] = effective
		if scan := Str(params, "wifiScan"); scan != "" && scan != "OFF" {
			details["accessPoints"] = []any{map[string]any{"ssid": "SIMULATED-NETWORK", "bssid": "020000000001", "channel": 6, "rssiDbm": -55, "authentication": "WPA2_PSK"}}
		}
		return true, g.finish(ctx, task, "SUCCEEDED", "VERIFIED", details, "")
	}
	if typ == "REBOOT" {
		if e := g.LinkDownAll(ctx, "ESP_RESTART"); e != nil {
			return true, e
		}
		tx, e := g.DB.BeginTx(ctx, nil)
		if e != nil {
			return true, e
		}
		defer tx.Rollback()
		if _, e = tx.Exec("UPDATE gw32_radios SET boot_id=?,next_connection=0 WHERE radio='ESP32'", ID("")); e != nil {
			return true, e
		}
		if _, e = tx.Exec("DELETE FROM gw32_observations"); e != nil {
			return true, e
		}
		if e = tx.Commit(); e != nil {
			return true, e
		}
		return false, nil
	}
	if !strings.HasPrefix(typ, "GATT_") {
		return false, nil
	}
	radio := Str(params, "radio")
	if radio != "NRF54" && radio != "ESP32" {
		return true, g.finish(ctx, task, "FAILED", "NONE", map[string]any{"simulated": true}, "UNSUPPORTED")
	}
	if (typ == "GATT_READ" || typ == "GATT_WRITE") && Str(params, "connectionRef") == "" {
		return true, g.temporaryGATT(ctx, cmd)
	}
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return true, e
	}
	defer tx.Rollback()
	details := map[string]any{"simulated": true, "radio": radio}
	fail := func(code string) (bool, error) {
		if e := finishGW32(tx, task, "FAILED", "NONE", details, code); e != nil {
			return true, e
		}
		return true, tx.Commit()
	}
	if typ == "GATT_CONNECT" {
		address, addressType := Str(params, "deviceAddress"), Str(params, "addressType")
		auto := object(params, "autoTrigger")
		if len(auto) > 0 {
			var at, expires int64
			var observedType, observedAddress string
			e = tx.QueryRow("SELECT address_type,address,captured_ms,expires_ms FROM gw32_observations WHERE event_id=?", Str(auto, "advEventId")).Scan(&observedType, &observedAddress, &at, &expires)
			maxAge := Int(auto, "maxAgeMs")
			if maxAge == 0 {
				maxAge = 5000
			}
			if e != nil || time.Now().UnixMilli() > expires || time.Now().UnixMilli()-at > maxAge || address != observedAddress || addressType != observedType || radio != "NRF54" {
				return fail("STALE_OBSERVATION")
			}
			if Str(auto, "serviceUuid") != GW32ServiceUUID && Str(auto, "serviceUuid") != "0000180f-0000-1000-8000-00805f9b34fb" {
				return fail("SERVICE_NOT_FOUND")
			}
			details["ruleId"] = Str(auto, "ruleId")
			details["advEventId"] = Str(auto, "advEventId")
			details["serviceUuid"] = Str(auto, "serviceUuid")
			if characteristic := Str(auto, "characteristicUuid"); characteristic != "" {
				if characteristic != GW32CharacteristicUUID && characteristic != "00002a19-0000-1000-8000-00805f9b34fb" {
					return fail("CHARACTERISTIC_NOT_FOUND")
				}
				details["characteristicUuid"] = characteristic
				details["characteristicHandle"] = 42
			}
		}
		var occupied int
		if e = tx.QueryRow("SELECT count(*) FROM gw32_connections WHERE state='CONNECTED' AND address_type=? AND address=?", addressType, address).Scan(&occupied); e != nil {
			return true, e
		}
		if occupied > 0 {
			return fail("BUSY")
		}
		var boot string
		var number int64
		if e = tx.QueryRow("SELECT boot_id,next_connection+1 FROM gw32_radios WHERE radio=?", radio).Scan(&boot, &number); e != nil {
			return true, e
		}
		if number > 0xffffffff {
			return fail("BUSY")
		}
		ref, e := ConnectionReference(radio, boot, uint32(number))
		if e != nil {
			return true, e
		}
		security := "UNENCRYPTED"
		if Str(params, "security") == "BONDED_ENCRYPTED" {
			security = "ENCRYPTED"
		}
		if _, e = tx.Exec("INSERT INTO gw32_connections VALUES(?,?,?,?,'CONNECTED',?,247,?)", ref, radio, addressType, address, security, number); e != nil {
			return true, e
		}
		if _, e = tx.Exec("UPDATE gw32_radios SET next_connection=? WHERE radio=?", number, radio); e != nil {
			return true, e
		}
		details["connectionRef"] = ref
		details["connectionState"] = "CONNECTED"
		details["deviceAddress"] = address
		details["addressType"] = addressType
		details["attMtu"] = 247
		details["securityState"] = security
	} else {
		ref := Str(params, "connectionRef")
		var storedRadio, addressType, address, state, security string
		e = tx.QueryRow("SELECT radio,address_type,address,state,security FROM gw32_connections WHERE connection_ref=?", ref).Scan(&storedRadio, &addressType, &address, &state, &security)
		if e != nil || storedRadio != radio || state != "CONNECTED" {
			return fail("STALE_CONNECTION")
		}
		details["connectionRef"] = ref
		service, characteristic := Str(params, "serviceUuid"), Str(params, "characteristicUuid")
		switch typ {
		case "GATT_DISCONNECT":
			details["connectionState"] = "CLOSED"
			if e = closeGW32Connection(tx, ref, "DISCONNECTED"); e != nil {
				return true, e
			}
		case "GATT_SUBSCRIBE":
			mode := Str(params, "mode")
			if mode != "NOTIFY" && mode != "INDICATE" {
				return fail("UNSUPPORTED")
			}
			if _, e = tx.Exec("INSERT INTO gw32_subscriptions VALUES(?,?,?,?,?,?,1) ON CONFLICT(connection_ref,service_uuid,characteristic_uuid) DO UPDATE SET mode=excluded.mode,active=1", ref, service, characteristic, 42, 43, mode); e != nil {
				return true, e
			}
			details["serviceUuid"] = service
			details["characteristicUuid"] = characteristic
			details["characteristicHandle"] = 42
			details["cccHandle"] = 43
			details["mode"] = mode
			details["cccEnabled"] = true
			kind := "GATT_NOTIFY"
			if mode == "INDICATE" {
				kind = "GATT_INDICATE"
			}
			for i := 0; i < g.NotificationsPerSubscription; i++ {
				event := map[string]any{"timeQuality": "SYNCED", "capturedAt": time.Now().UTC().Format(time.RFC3339Nano), "sourceDeviceId": "ble:" + strings.ToLower(addressType) + ":" + address, "protocolHint": "ble-gatt-value-v1", "payloadBase64": base64.StdEncoding.EncodeToString([]byte{0x10, 0x20, byte(i)}), "capture": map[string]any{"radio": radio, "kind": kind, "addressType": addressType, "address": address, "connectionRef": ref, "characteristicHandle": 42}}
				if _, e = insertGW32Event(tx, event); e != nil {
					return true, e
				}
			}
		case "GATT_UNSUBSCRIBE":
			if _, e = tx.Exec("UPDATE gw32_subscriptions SET active=0 WHERE connection_ref=? AND service_uuid=? AND characteristic_uuid=?", ref, service, characteristic); e != nil {
				return true, e
			}
			details["serviceUuid"] = service
			details["characteristicUuid"] = characteristic
			details["cccEnabled"] = false
		case "GATT_READ", "GATT_WRITE":
			details["deviceAddress"] = address
			details["addressType"] = addressType
			details["serviceUuid"] = service
			details["characteristicUuid"] = characteristic
			value := "ECAA"
			if typ == "GATT_WRITE" {
				value = Str(params, "valueBase64")
				decoded, e := base64.StdEncoding.Strict().DecodeString(value)
				if e != nil || len(decoded) > 512 {
					return fail("INVALID_VALUE")
				}
				if _, e = tx.Exec("INSERT INTO gw32_values VALUES(?,?,?,?) ON CONFLICT(connection_ref,service_uuid,characteristic_uuid) DO UPDATE SET value_base64=excluded.value_base64", ref, service, characteristic, value); e != nil {
					return true, e
				}
				details["attResponse"] = true
				details["readbackBase64"] = value
			} else {
				e = tx.QueryRow("SELECT value_base64 FROM gw32_values WHERE connection_ref=? AND service_uuid=? AND characteristic_uuid=?", ref, service, characteristic).Scan(&value)
				if e != nil && !errors.Is(e, sql.ErrNoRows) {
					return true, e
				}
				details["valueBase64"] = value
			}
		default:
			return fail("UNSUPPORTED")
		}
	}
	if e = finishGW32(tx, task, "SUCCEEDED", "VERIFIED", details, ""); e != nil {
		return true, e
	}
	return true, tx.Commit()
}

func (g *Gateway) temporaryGATT(ctx context.Context, cmd map[string]any) error {
	params := object(cmd, "parameters")
	details := map[string]any{"simulated": true, "radio": Str(params, "radio"), "deviceAddress": Str(params, "deviceAddress"), "addressType": Str(params, "addressType"), "serviceUuid": Str(params, "serviceUuid"), "characteristicUuid": Str(params, "characteristicUuid"), "connectionState": "CLOSED"}
	value := "ECAA"
	if Str(cmd, "type") == "GATT_WRITE" {
		value = Str(params, "valueBase64")
		raw, e := base64.StdEncoding.Strict().DecodeString(value)
		if e != nil || len(raw) > 512 {
			return g.finish(ctx, Str(cmd, "taskId"), "FAILED", "NONE", details, "INVALID_VALUE")
		}
		details["attResponse"] = true
		details["readbackBase64"] = value
	} else {
		details["valueBase64"] = value
	}
	return g.finish(ctx, Str(cmd, "taskId"), "SUCCEEDED", "VERIFIED", details, "")
}
func closeGW32Connection(tx *sql.Tx, ref, reason string) error {
	var radio, addressType, address, state string
	if e := tx.QueryRow("SELECT radio,address_type,address,state FROM gw32_connections WHERE connection_ref=?", ref).Scan(&radio, &addressType, &address, &state); e != nil {
		return e
	}
	if state != "CONNECTED" {
		return nil
	}
	if _, e := tx.Exec("UPDATE gw32_connections SET state='CLOSED' WHERE connection_ref=?", ref); e != nil {
		return e
	}
	if _, e := tx.Exec("UPDATE gw32_subscriptions SET active=0 WHERE connection_ref=?", ref); e != nil {
		return e
	}
	_, e := insertGW32Event(tx, map[string]any{"timeQuality": "SYNCED", "capturedAt": time.Now().UTC().Format(time.RFC3339Nano), "sourceDeviceId": "ble:" + strings.ToLower(addressType) + ":" + address, "protocolHint": "ble-gatt-link-v1", "payloadBase64": "", "capture": map[string]any{"radio": radio, "kind": "GATT_LINK_DOWN", "connectionRef": ref, "linkReason": reason}})
	return e
}
func (g *Gateway) LinkDownAll(ctx context.Context, reason string) error {
	if reason != "DISCONNECTED" && reason != "NRF_RESTART" && reason != "ESP_RESTART" {
		return errors.New("invalid link-down reason")
	}
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	query := "SELECT connection_ref FROM gw32_connections WHERE state='CONNECTED'"
	if reason == "NRF_RESTART" {
		query += " AND radio='NRF54'"
	}
	rows, e := tx.Query(query)
	if e != nil {
		return e
	}
	var refs []string
	for rows.Next() {
		var ref string
		if e = rows.Scan(&ref); e != nil {
			rows.Close()
			return e
		}
		refs = append(refs, ref)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		return e
	}
	for _, ref := range refs {
		if e = closeGW32Connection(tx, ref, reason); e != nil {
			return e
		}
	}
	if reason == "NRF_RESTART" {
		if _, e = tx.Exec("UPDATE gw32_radios SET boot_id=?,next_connection=0 WHERE radio='NRF54'", ID("")); e != nil {
			return e
		}
		if _, e = tx.Exec("DELETE FROM gw32_observations"); e != nil {
			return e
		}
	}
	return tx.Commit()
}
func (g *Gateway) configureRadio(ctx context.Context, cmd map[string]any) error {
	params := object(cmd, "parameters")
	requested := object(params, "configuration")
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	var raw string
	var revision int64
	if e = tx.QueryRow("SELECT configuration,revision FROM identity WHERE singleton=1").Scan(&raw, &revision); e != nil {
		return e
	}
	desired := Int(params, "desiredRevision")
	if desired <= revision {
		return finishAndCommit(tx, Str(cmd, "taskId"), "FAILED", "NONE", map[string]any{"simulated": true}, "STALE_REVISION")
	}
	var current map[string]any
	_ = json.Unmarshal([]byte(raw), &current)
	merged := mergeObjects(current, requested)
	if _, e = tx.Exec("UPDATE identity SET configuration=?,revision=? WHERE singleton=1", encodeJSON(merged), desired); e != nil {
		return e
	}
	return finishAndCommit(tx, Str(cmd, "taskId"), "SUCCEEDED", "VERIFIED", map[string]any{"simulated": true, "configuration": requested, "reportedConfigurationRevision": desired, "effective": object(merged, "radios")}, "")
}
func mergeObjects(base, patch map[string]any) map[string]any {
	out := map[string]any{}
	for k, v := range base {
		out[k] = v
	}
	for k, v := range patch {
		if k == "ipv4" {
			out[k] = v
			continue
		}
		if p, ok := v.(map[string]any); ok {
			b, _ := out[k].(map[string]any)
			out[k] = mergeObjects(b, p)
		} else {
			out[k] = v
		}
	}
	return out
}
func finishAndCommit(tx *sql.Tx, task, state, verification string, details map[string]any, code string) error {
	if e := finishGW32(tx, task, state, verification, details, code); e != nil {
		return e
	}
	return tx.Commit()
}

func (g *Gateway) beginNetworkTrial(ctx context.Context, cmd map[string]any) error {
	params := object(cmd, "parameters")
	requested := object(params, "configuration")
	task := Str(cmd, "taskId")
	desired := Int(params, "desiredRevision")
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	var old string
	var revision int64
	if e = tx.QueryRow("SELECT configuration,revision FROM identity WHERE singleton=1").Scan(&old, &revision); e != nil {
		return e
	}
	if desired <= revision {
		return finishAndCommit(tx, task, "FAILED", "NONE", map[string]any{"simulated": true}, "STALE_REVISION")
	}
	var current map[string]any
	_ = json.Unmarshal([]byte(old), &current)
	if len(object(current, "network")) == 0 {
		current["network"] = map[string]any{"uplinkOrder": []any{"ETHERNET"}, "ethernet": map[string]any{"enabled": true, "ipv4": map[string]any{"mode": "DHCP"}}}
	}
	effective := mergeObjects(current, requested)
	network := object(effective, "network")
	order, ok := network["uplinkOrder"].([]any)
	if !ok {
		if ss, ok := network["uplinkOrder"].([]string); ok {
			for _, s := range ss {
				order = append(order, s)
			}
		}
	}
	if len(order) == 0 {
		return finishAndCommit(tx, task, "FAILED", "NONE", map[string]any{"simulated": true}, "INVALID_CONFIGURATION")
	}
	deadline := time.Now().Add(120 * time.Second)
	expires, e := time.Parse(time.RFC3339Nano, Str(cmd, "expiresAt"))
	if e != nil {
		return e
	}
	if expires.Before(deadline) {
		deadline = expires
	}
	ids := []string{}
	for _, item := range order {
		iface, ok := item.(string)
		if !ok || (iface != "ETHERNET" && iface != "WIFI_STA") {
			return errors.New("invalid trial interface")
		}
		var seq int
		if e = tx.QueryRow("SELECT COALESCE(MAX(sequence),-1)+1 FROM results WHERE task_id=?", task).Scan(&seq); e != nil {
			return e
		}
		if e = putResult(tx, task, seq, "EXECUTING", "NONE", map[string]any{"simulated": true, "networkProbe": map[string]any{"interface": iface, "candidateRevision": desired, "linkReady": true, "ipReady": true, "dnsReady": true, "tlsReady": true}}, ""); e != nil {
			return e
		}
		var id string
		if e = tx.QueryRow("SELECT result_id FROM results WHERE task_id=? AND sequence=?", task, seq).Scan(&id); e != nil {
			return e
		}
		ids = append(ids, id)
	}
	if _, e = tx.Exec("INSERT INTO gw32_network_trials VALUES(?,?,?,?,?,?,'PENDING')", task, encodeJSON(requested), encodeJSON(effective), desired, deadline.UnixMilli(), encodeJSON(ids)); e != nil {
		return e
	}
	return tx.Commit()
}
func (g *Gateway) finalizeNetworkTrials(ctx context.Context) error {
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	rows, e := tx.Query("SELECT task_id,configuration_json,effective_json,desired_revision,deadline_ms,probe_ids_json FROM gw32_network_trials WHERE state='PENDING'")
	if e != nil {
		return e
	}
	type trial struct {
		task, requested, effective, ids string
		revision, deadline              int64
	}
	var pending []trial
	for rows.Next() {
		var t trial
		if e = rows.Scan(&t.task, &t.requested, &t.effective, &t.revision, &t.deadline, &t.ids); e != nil {
			rows.Close()
			return e
		}
		pending = append(pending, t)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		return e
	}
	for _, t := range pending {
		if time.Now().UnixMilli() > t.deadline {
			if _, e = tx.Exec("UPDATE gw32_network_trials SET state='ROLLED_BACK' WHERE task_id=?", t.task); e != nil {
				return e
			}
			if e = finishGW32(tx, t.task, "FAILED", "NONE", map[string]any{"simulated": true, "rolledBack": true}, "NETWORK_TRIAL_TIMEOUT"); e != nil {
				return e
			}
			continue
		}
		var ids []string
		if e = json.Unmarshal([]byte(t.ids), &ids); e != nil {
			return e
		}
		all := len(ids) > 0
		for _, id := range ids {
			var acked int
			if e = tx.QueryRow("SELECT acked FROM results WHERE result_id=? AND task_id=?", id, t.task).Scan(&acked); e != nil {
				return e
			}
			all = all && acked == 1
		}
		if !all {
			continue
		}
		if _, e = tx.Exec("UPDATE identity SET configuration=?,revision=? WHERE singleton=1", t.effective, t.revision); e != nil {
			return e
		}
		if _, e = tx.Exec("UPDATE gw32_network_trials SET state='COMMITTED' WHERE task_id=?", t.task); e != nil {
			return e
		}
		var request, effective map[string]any
		_ = json.Unmarshal([]byte(t.requested), &request)
		_ = json.Unmarshal([]byte(t.effective), &effective)
		if e = finishGW32(tx, t.task, "SUCCEEDED", "VERIFIED", map[string]any{"simulated": true, "configuration": request, "reportedConfigurationRevision": t.revision, "effective": object(effective, "network")}, ""); e != nil {
			return e
		}
	}
	return tx.Commit()
}
