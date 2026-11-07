package simkit

import (
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"time"
)

type Gateway struct {
	DB                   *sql.DB
	Namespace, GatewayID string
}

func OpenGateway(path, namespace, id string) (*Gateway, error) {
	db, e := Open(path)
	if e != nil {
		return nil, e
	}
	_, e = db.Exec(`CREATE TABLE IF NOT EXISTS identity(singleton INTEGER PRIMARY KEY CHECK(singleton=1),namespace TEXT NOT NULL,gateway_id TEXT NOT NULL,boot_id TEXT NOT NULL,sequence INTEGER NOT NULL,revision INTEGER NOT NULL,configuration TEXT NOT NULL);
 CREATE TABLE IF NOT EXISTS spool(event_id TEXT PRIMARY KEY,event_json TEXT NOT NULL,quarantined INTEGER NOT NULL DEFAULT 0);
 CREATE TABLE IF NOT EXISTS journal(task_id TEXT PRIMARY KEY,digest TEXT NOT NULL,command_json TEXT NOT NULL,state TEXT NOT NULL,execution_count INTEGER NOT NULL DEFAULT 0);
 CREATE TABLE IF NOT EXISTS results(result_id TEXT PRIMARY KEY,task_id TEXT NOT NULL,sequence INTEGER NOT NULL,result_json TEXT NOT NULL,acked INTEGER NOT NULL DEFAULT 0,UNIQUE(task_id,sequence));`)
	if e != nil {
		db.Close()
		return nil, e
	}
	_, e = db.Exec("INSERT OR IGNORE INTO identity VALUES(1,?,?,?,0,0,'{}')", namespace, id, ID("boot"))
	if e != nil {
		db.Close()
		return nil, e
	}
	var ns, gw string
	e = db.QueryRow("SELECT namespace,gateway_id FROM identity WHERE singleton=1").Scan(&ns, &gw)
	if e != nil || ns != namespace || gw != id {
		db.Close()
		return nil, errors.New("simulator database identity mismatch")
	}
	g := &Gateway{db, namespace, id}
	// Once execution intent is durable, a crash cannot prove physical completion.
	// Recover as UNKNOWN, never execute that task a second time.
	rows, e := db.Query("SELECT task_id FROM journal WHERE state='EXECUTING'")
	if e != nil {
		db.Close()
		return nil, e
	}
	var interrupted []string
	for rows.Next() {
		var task string
		if e = rows.Scan(&task); e != nil {
			rows.Close()
			db.Close()
			return nil, e
		}
		interrupted = append(interrupted, task)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		db.Close()
		return nil, e
	}
	for _, task := range interrupted {
		if e = g.finish(context.Background(), task, "UNKNOWN", "NONE", map[string]any{"reason": "interrupted execution; operator reconciliation required"}, "OUTCOME_UNKNOWN"); e != nil {
			db.Close()
			return nil, e
		}
	}
	rows, e = db.Query("SELECT command_json FROM journal WHERE state='ACCEPTED'")
	if e != nil {
		db.Close()
		return nil, e
	}
	var pending []map[string]any
	for rows.Next() {
		var raw string
		if e = rows.Scan(&raw); e != nil {
			rows.Close()
			db.Close()
			return nil, e
		}
		var cmd map[string]any
		if e = json.Unmarshal([]byte(raw), &cmd); e != nil {
			rows.Close()
			db.Close()
			return nil, e
		}
		pending = append(pending, cmd)
	}
	e = rows.Err()
	rows.Close()
	if e != nil {
		db.Close()
		return nil, e
	}
	for _, cmd := range pending {
		if e = g.Command(context.Background(), cmd); e != nil {
			db.Close()
			return nil, e
		}
	}
	return g, nil
}

func ReferencePacket(sequence uint32) []byte {
	b := make([]byte, 18)
	copy(b, []byte{'B', 'G', 1, 1})
	binary.BigEndian.PutUint32(b[4:8], sequence)
	binary.BigEndian.PutUint16(b[8:10], 2500)
	binary.BigEndian.PutUint16(b[10:12], 5000)
	binary.BigEndian.PutUint16(b[12:14], 3000)
	crc := uint16(0xffff)
	for _, v := range b[:16] {
		crc ^= uint16(v) << 8
		for range 8 {
			if crc&0x8000 != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	binary.BigEndian.PutUint16(b[16:], crc)
	return b
}
func (g *Gateway) Enqueue(ctx context.Context, count int) error {
	if count < 0 || count > 100000 {
		return errors.New("count must be 0..100000")
	}
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	var seq int64
	var boot string
	if e = tx.QueryRow("SELECT boot_id,sequence FROM identity WHERE singleton=1").Scan(&boot, &seq); e != nil {
		return e
	}
	for range count {
		seq++
		event := map[string]any{"eventId": fmt.Sprintf("%s-%d", boot, seq), "timeQuality": "SYNCED", "capturedAt": time.Now().UTC().Format(time.RFC3339Nano), "sourceDeviceId": "reference_sensor_01", "sequence": seq, "protocolHint": "reference-sensor-v1", "payloadBase64": base64.StdEncoding.EncodeToString(ReferencePacket(uint32(seq)))}
		b, _ := json.Marshal(event)
		if _, e = tx.Exec("INSERT INTO spool(event_id,event_json) VALUES(?,?)", Str(event, "eventId"), string(b)); e != nil {
			return e
		}
	}
	if _, e = tx.Exec("UPDATE identity SET sequence=? WHERE singleton=1", seq); e != nil {
		return e
	}
	return tx.Commit()
}
func (g *Gateway) Batch(ctx context.Context) (map[string]any, error) {
	rows, e := g.DB.QueryContext(ctx, "SELECT event_json FROM spool WHERE quarantined=0 ORDER BY rowid LIMIT 200")
	if e != nil {
		return nil, e
	}
	defer rows.Close()
	events := []any{}
	for rows.Next() {
		var b string
		if e = rows.Scan(&b); e != nil {
			return nil, e
		}
		var event map[string]any
		if e = json.Unmarshal([]byte(b), &event); e != nil {
			return nil, e
		}
		events = append(events, event)
	}
	if e = rows.Err(); e != nil {
		return nil, e
	}
	return map[string]any{"schemaVersion": "1.0", "namespaceId": g.Namespace, "gatewayId": g.GatewayID, "batchId": ID("batch"), "events": events}, nil
}
func (g *Gateway) Ack(ctx context.Context, batch, ack map[string]any) error {
	if Str(ack, "namespaceId") != g.Namespace || Str(ack, "gatewayId") != g.GatewayID || Str(ack, "batchId") != Str(batch, "batchId") || Str(ack, "durability") != "RAW_PERSISTED" {
		return errors.New("invalid ingress ACK identity/durability")
	}
	allowed := map[string]bool{}
	for _, v := range batch["events"].([]any) {
		allowed[Str(v.(map[string]any), "eventId")] = true
	}
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	items, _ := ack["items"].([]any)
	for _, v := range items {
		item, ok := v.(map[string]any)
		if !ok {
			return errors.New("invalid ACK item")
		}
		id := Str(item, "eventId")
		if !allowed[id] {
			return errors.New("ACK references an unsent event")
		}
		switch Str(item, "status") {
		case "ACCEPTED", "DUPLICATE":
			_, e = tx.Exec("DELETE FROM spool WHERE event_id=?", id)
		case "REJECTED_PERMANENT":
			_, e = tx.Exec("UPDATE spool SET quarantined=1 WHERE event_id=?", id)
		}
		if e != nil {
			return e
		}
	}
	return tx.Commit()
}
func (g *Gateway) Command(ctx context.Context, cmd map[string]any) error {
	task := Str(cmd, "taskId")
	digest := Str(cmd, "requestDigest")
	if task == "" || digest == "" || Str(cmd, "namespaceId") != g.Namespace || Str(cmd, "gatewayId") != g.GatewayID {
		return errors.New("command identity invalid")
	}
	expires, e := time.Parse(time.RFC3339Nano, Str(cmd, "expiresAt"))
	if e != nil {
		return e
	}
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	var old, state string
	e = tx.QueryRow("SELECT digest,state FROM journal WHERE task_id=?", task).Scan(&old, &state)
	if e == nil {
		if old != digest {
			return errors.New("task ID reused with another digest")
		}
		_, e = tx.Exec("UPDATE results SET acked=0 WHERE task_id=?", task)
		if e != nil {
			return e
		}
		if e = tx.Commit(); e != nil {
			return e
		}
		if state == "ACCEPTED" {
			return g.execute(ctx, cmd, expires)
		}
		return nil
	}
	if !errors.Is(e, sql.ErrNoRows) {
		return e
	}
	b, _ := json.Marshal(cmd)
	if _, e = tx.Exec("INSERT INTO journal(task_id,digest,command_json,state) VALUES(?,?,?,'ACCEPTED')", task, digest, string(b)); e != nil {
		return e
	}
	if e = putResult(tx, task, 0, "ACCEPTED", "NONE", map[string]any{}, ""); e != nil {
		return e
	}
	if e = tx.Commit(); e != nil {
		return e
	}
	return g.execute(ctx, cmd, expires)
}
func (g *Gateway) execute(ctx context.Context, cmd map[string]any, expires time.Time) error {
	task := Str(cmd, "taskId")
	if !time.Now().Before(expires) {
		return g.finish(ctx, task, "EXPIRED", "NONE", map[string]any{}, "TTL_EXPIRED")
	}
	// Persist intent before any simulated side effect, in a separate commit.
	claimed, e := g.DB.ExecContext(ctx, "UPDATE journal SET state='EXECUTING',execution_count=execution_count+1 WHERE task_id=? AND state='ACCEPTED'", task)
	if e != nil {
		return e
	}
	if n, e := claimed.RowsAffected(); e != nil {
		return e
	} else if n == 0 {
		return nil
	}
	params, _ := cmd["parameters"].(map[string]any)
	details := map[string]any{}
	switch Str(cmd, "type") {
	case "CONFIGURE":
		revision := Int(params, "desiredRevision")
		if revision < 1 {
			return g.finish(ctx, task, "FAILED", "NONE", details, "INVALID_CONFIGURATION")
		}
		configuration := params["configuration"]
		if configuration == nil {
			configuration = map[string]any{}
		}
		raw, _ := json.Marshal(configuration)
		var current int64
		if e = g.DB.QueryRow("SELECT revision FROM identity WHERE singleton=1").Scan(&current); e != nil {
			return e
		}
		if revision < current {
			return g.finish(ctx, task, "FAILED", "NONE", details, "STALE_REVISION")
		}
		if _, has := params["expectedReportedRevision"]; has && Int(params, "expectedReportedRevision") != current {
			return g.finish(ctx, task, "FAILED", "NONE", details, "BASELINE_MISMATCH")
		}
		if _, e = g.DB.ExecContext(ctx, "UPDATE identity SET revision=?,configuration=? WHERE singleton=1", revision, string(raw)); e != nil {
			return e
		}
		details["configuration"] = configuration
		details["reportedConfigurationRevision"] = revision
	case "REBOOT":
		boot := ID("boot")
		if _, e = g.DB.ExecContext(ctx, "UPDATE identity SET boot_id=? WHERE singleton=1", boot); e != nil {
			return e
		}
		details["bootId"] = boot
		details["simulated"] = true
	case "DIAGNOSTICS":
		details["profile"] = "reference-http-simulator"
		details["simulated"] = true
		details["capabilities"] = []string{"CONFIGURE", "REBOOT", "DIAGNOSTICS"}
	default:
		return g.finish(ctx, task, "FAILED", "NONE", map[string]any{"simulated": true}, "UNSUPPORTED")
	}
	return g.finish(ctx, task, "SUCCEEDED", "VERIFIED", details, "")
}
func putResult(tx *sql.Tx, task string, seq int, state, verification string, details map[string]any, code string) error {
	result := map[string]any{"resultId": ID("result"), "taskId": task, "resultSequence": seq, "state": state, "observedAt": time.Now().UTC().Format(time.RFC3339Nano), "verification": verification, "details": details}
	if code != "" {
		result["errorCode"] = code
	}
	b, _ := json.Marshal(result)
	_, e := tx.Exec("INSERT INTO results(result_id,task_id,sequence,result_json) VALUES(?,?,?,?)", Str(result, "resultId"), task, seq, string(b))
	return e
}
func (g *Gateway) finish(ctx context.Context, task, state, verification string, details map[string]any, code string) error {
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	if _, e = tx.Exec("UPDATE journal SET state=? WHERE task_id=?", state, task); e != nil {
		return e
	}
	var seq int
	if e = tx.QueryRow("SELECT COALESCE(MAX(sequence),-1)+1 FROM results WHERE task_id=?", task).Scan(&seq); e != nil {
		return e
	}
	if e = putResult(tx, task, seq, state, verification, details, code); e != nil {
		return e
	}
	return tx.Commit()
}
func (g *Gateway) Results(ctx context.Context) (map[string]any, error) {
	rows, e := g.DB.QueryContext(ctx, "SELECT result_json FROM results WHERE acked=0 ORDER BY task_id,sequence LIMIT 200")
	if e != nil {
		return nil, e
	}
	defer rows.Close()
	results := []any{}
	for rows.Next() {
		var raw string
		if e = rows.Scan(&raw); e != nil {
			return nil, e
		}
		var v map[string]any
		if e = json.Unmarshal([]byte(raw), &v); e != nil {
			return nil, e
		}
		results = append(results, v)
	}
	return map[string]any{"schemaVersion": "1.0", "namespaceId": g.Namespace, "gatewayId": g.GatewayID, "results": results}, rows.Err()
}
func (g *Gateway) AckResults(ctx context.Context, batch, ack map[string]any) error {
	if Str(ack, "namespaceId") != g.Namespace || Str(ack, "gatewayId") != g.GatewayID || Str(ack, "durability") != "RESULT_PERSISTED" {
		return errors.New("invalid result ACK")
	}
	sent := map[string]string{}
	for _, v := range batch["results"].([]any) {
		m := v.(map[string]any)
		sent[Str(m, "resultId")] = Str(m, "taskId")
	}
	tx, e := g.DB.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	items, _ := ack["items"].([]any)
	for _, v := range items {
		m, ok := v.(map[string]any)
		if !ok {
			return errors.New("invalid result ACK item")
		}
		id := Str(m, "resultId")
		if sent[id] == "" || sent[id] != Str(m, "taskId") {
			return errors.New("ACK references unsent result")
		}
		if Str(m, "status") == "ACCEPTED" || Str(m, "status") == "DUPLICATE" {
			if _, e = tx.Exec("UPDATE results SET acked=1 WHERE result_id=?", id); e != nil {
				return e
			}
		}
	}
	return tx.Commit()
}
func (g *Gateway) Status(ctx context.Context) (map[string]any, error) {
	var revision, bytes int64
	if e := g.DB.QueryRowContext(ctx, "SELECT revision FROM identity WHERE singleton=1").Scan(&revision); e != nil {
		return nil, e
	}
	if e := g.DB.QueryRowContext(ctx, "SELECT COALESCE(SUM(length(event_json)),0) FROM spool").Scan(&bytes); e != nil {
		return nil, e
	}
	return map[string]any{"schemaVersion": "1.0", "namespaceId": g.Namespace, "gatewayId": g.GatewayID, "observedAt": time.Now().UTC().Format(time.RFC3339Nano), "firmwareVersion": "simulator-1.0.0", "reportedConfigurationRevision": revision, "spoolBytes": bytes, "timeQuality": "SYNCED", "health": "OK"}, nil
}
