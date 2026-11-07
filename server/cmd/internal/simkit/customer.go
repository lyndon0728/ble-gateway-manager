package simkit

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"time"
)

type Customer struct {
	DB                      *sql.DB
	TargetID, TargetVersion string
}

func OpenCustomer(path, target, version string) (*Customer, error) {
	db, err := Open(path)
	if err != nil {
		return nil, err
	}
	_, err = db.Exec(`CREATE TABLE IF NOT EXISTS deliveries(delivery_id TEXT PRIMARY KEY,content_hash TEXT NOT NULL,event_json TEXT NOT NULL,received_ms INTEGER NOT NULL);
 CREATE TABLE IF NOT EXISTS records(record_id TEXT PRIMARY KEY,output_id TEXT NOT NULL,revision INTEGER NOT NULL,event_json TEXT NOT NULL);
	CREATE TABLE IF NOT EXISTS source_revisions(source_key TEXT PRIMARY KEY,revision INTEGER NOT NULL);
 CREATE TABLE IF NOT EXISTS counters(name TEXT PRIMARY KEY,value INTEGER NOT NULL);`)
	if err != nil {
		db.Close()
		return nil, err
	}
	return &Customer{db, target, version}, nil
}

func (c *Customer) Receive(ctx context.Context, batch map[string]any) (map[string]any, error) {
	batchID := Str(batch, "batchId")
	events, ok := batch["events"].([]any)
	if batchID == "" || !ok || len(events) < 1 || len(events) > 200 {
		return nil, errors.New("invalid customer batch")
	}
	tx, err := c.DB.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	_, err = tx.ExecContext(ctx, `INSERT INTO counters(name,value) VALUES('requests',1) ON CONFLICT(name) DO UPDATE SET value=value+1`)
	if err != nil {
		return nil, err
	}
	items := make([]map[string]any, 0, len(events))
	for _, raw := range events {
		event, ok := raw.(map[string]any)
		if !ok {
			return nil, errors.New("invalid event")
		}
		id := Str(event, "deliveryId")
		output := Str(event, "outputId")
		rev := Int(event, "processingRevision")
		if id == "" || output == "" || rev < 1 || Str(event, "namespaceId") == "" || Str(event, "gatewayId") == "" || Str(event, "sourceEventId") == "" {
			return nil, errors.New("missing event identity")
		}
		if Str(event, "targetId") != c.TargetID || Str(event, "targetVersion") != c.TargetVersion {
			return nil, errors.New("target/version mismatch")
		}
		digest := Hash(event)
		var existing string
		err = tx.QueryRowContext(ctx, "SELECT content_hash FROM deliveries WHERE delivery_id=?", id).Scan(&existing)
		if err == nil {
			if existing != digest {
				items = append(items, map[string]any{"deliveryId": id, "status": "REJECTED", "code": "CONTENT_CONFLICT"})
			} else {
				items = append(items, map[string]any{"deliveryId": id, "status": "DUPLICATE"})
			}
			continue
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return nil, err
		}
		encoded, _ := json.Marshal(event)
		if _, err = tx.ExecContext(ctx, "INSERT INTO deliveries VALUES(?,?,?,?)", id, digest, string(encoded), time.Now().UnixMilli()); err != nil {
			return nil, err
		}
		sourceKey := Hash([]string{c.TargetID, Str(event, "namespaceId"), Str(event, "gatewayId"), Str(event, "sourceEventId")})
		var latest int64
		err = tx.QueryRowContext(ctx, "SELECT revision FROM source_revisions WHERE source_key=?", sourceKey).Scan(&latest)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return nil, err
		}
		if latest > rev {
			items = append(items, map[string]any{"deliveryId": id, "status": "ACCEPTED"})
			continue
		}
		if _, err = tx.ExecContext(ctx, `INSERT INTO source_revisions VALUES(?,?) ON CONFLICT(source_key) DO UPDATE SET revision=MAX(revision,excluded.revision)`, sourceKey, rev); err != nil {
			return nil, err
		}
		if replaces, ok := event["replacesOutputIds"].([]any); ok {
			for _, old := range replaces {
				oldID, ok := old.(string)
				if !ok {
					return nil, errors.New("invalid replacement output ID")
				}
				if _, err = tx.ExecContext(ctx, "DELETE FROM records WHERE output_id=? AND revision<?", oldID, rev); err != nil {
					return nil, err
				}
			}
		}
		// The stable business key deliberately excludes processingRevision. Older
		// late deliveries are acknowledged, but cannot overwrite a newer correction.
		key := Hash([]string{c.TargetID, Str(event, "namespaceId"), Str(event, "gatewayId"), Str(event, "sourceEventId"), Str(event, "recordKey")})
		if _, err = tx.ExecContext(ctx, `INSERT INTO records(record_id,output_id,revision,event_json) VALUES(?,?,?,?)
    ON CONFLICT(record_id) DO UPDATE SET output_id=excluded.output_id,revision=excluded.revision,event_json=excluded.event_json
    WHERE excluded.revision>records.revision`, key, output, rev, string(encoded)); err != nil {
			return nil, err
		}
		items = append(items, map[string]any{"deliveryId": id, "status": "ACCEPTED"})
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	return map[string]any{"batchId": batchID, "targetId": c.TargetID, "targetVersion": c.TargetVersion, "durability": "CUSTOMER_BUSINESS_PERSISTED", "items": items}, nil
}

func (c *Customer) Stats(ctx context.Context) (map[string]any, error) {
	r := map[string]any{}
	for name, q := range map[string]string{"deliveries": "SELECT count(*) FROM deliveries", "businessRecords": "SELECT count(*) FROM records", "requests": "SELECT COALESCE((SELECT value FROM counters WHERE name='requests'),0)"} {
		var n int64
		if err := c.DB.QueryRowContext(ctx, q).Scan(&n); err != nil {
			return nil, fmt.Errorf("%s: %w", name, err)
		}
		r[name] = n
	}
	return r, nil
}
