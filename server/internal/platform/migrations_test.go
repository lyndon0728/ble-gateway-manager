package platform

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"net/url"
	"path/filepath"
	"strings"
	"testing"
)

func migrationRawDB(t *testing.T, path string) *sql.DB {
	t.Helper()
	p := filepath.ToSlash(path)
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	u := url.URL{Scheme: "file", Path: p}
	db, e := sql.Open("sqlite", u.String())
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { db.Close() })
	return db
}
func migrationV1Fixture(t *testing.T, schema string) (string, *sql.DB, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), strings.TrimSuffix(schema, ".sql")+".db")
	db := migrationRawDB(t, path)
	raw, e := Assets.ReadFile("assets/" + schema)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = db.Exec(string(raw)); e != nil {
		t.Fatal(e)
	}
	digest := sha256.Sum256(raw)
	checksum := hex.EncodeToString(digest[:])
	if _, e = db.Exec("INSERT INTO schema_migrations VALUES(1,?,?,'0.1.0')", checksum, Now()); e != nil {
		t.Fatal(e)
	}
	return path, db, checksum
}

func TestMigrationsUpgradeV1PreservesManagementAndRawCustody(t *testing.T) {
	for _, schema := range []string{"management.sql", "pipeline.sql"} {
		t.Run(schema, func(t *testing.T) {
			path, db, checksum := migrationV1Fixture(t, schema)
			now := Now()
			if schema == "management.sql" {
				if _, e := db.Exec("INSERT INTO resource_scopes(scope_id,kind,name,created_at_ms) VALUES('original_scope','instance','Original scope',?)", now); e != nil {
					t.Fatal(e)
				}
				if _, e := db.Exec("INSERT INTO principals(principal_id,kind,name,status,created_at_ms,updated_at_ms) VALUES('original_owner','human','original-owner','active',?,?)", now, now); e != nil {
					t.Fatal(e)
				}
				if _, e := db.Exec("INSERT INTO gateways(gateway_id,serial_number,scope_id,model,hardware_version,lifecycle,created_at_ms) VALUES('original_gateway','original_serial','original_scope','LEGACY_GATEWAY','1','active',?)", now); e != nil {
					t.Fatal(e)
				}
			} else {
				if _, e := db.Exec("INSERT INTO pipeline_bundles VALUES('original_bundle','original_namespace',1,'digest','{}','active',?,?)", now, now); e != nil {
					t.Fatal(e)
				}
				if _, e := db.Exec("INSERT INTO event_receipts(event_key,namespace_id,gateway_id,event_id,immutable_hash,bundle_id,accepted_at_ms,custody_state,dedup_until_ms,raw_retention_until_ms) VALUES('original_key','original_namespace','original_gateway','original_event','original_hash','original_bundle',?,'accepted_raw',?,?)", now, now+60000, now+60000); e != nil {
					t.Fatal(e)
				}
				if _, e := db.Exec("INSERT INTO raw_events(event_key,payload,payload_bytes,time_quality,metadata_json) VALUES('original_key',?,4,'unknown','{\"original\":true}')", []byte{0, 1, 2, 255}); e != nil {
					t.Fatal(e)
				}
			}
			db.Close()
			upgraded, e := openDB(path, schema)
			if e != nil {
				t.Fatal(e)
			}
			defer upgraded.Close()
			var v1, v2 string
			if e = upgraded.QueryRow("SELECT checksum FROM schema_migrations WHERE version=1").Scan(&v1); e != nil || v1 != checksum {
				t.Fatal("migration1 history changed", v1, e)
			}
			if e = upgraded.QueryRow("SELECT checksum FROM schema_migrations WHERE version=2").Scan(&v2); e != nil || len(v2) != 64 {
				t.Fatal(v2, e)
			}
			if schema == "management.sql" {
				var model string
				if e = upgraded.QueryRow("SELECT model FROM gateways WHERE gateway_id='original_gateway'").Scan(&model); e != nil || model != "LEGACY_GATEWAY" {
					t.Fatal("v1 gateway lost", model, e)
				}
				var n int
				if e = upgraded.QueryRow("SELECT count(*) FROM ble_peers").Scan(&n); e != nil || n != 0 {
					t.Fatal(n, e)
				}
			} else {
				var payload []byte
				var hash string
				if e = upgraded.QueryRow("SELECT r.payload,e.immutable_hash FROM raw_events r JOIN event_receipts e ON e.event_key=r.event_key WHERE e.event_key='original_key'").Scan(&payload, &hash); e != nil || hex.EncodeToString(payload) != "000102ff" || hash != "original_hash" {
					t.Fatal("custody original changed", payload, hash, e)
				}
				var n int
				if e = upgraded.QueryRow("SELECT count(*) FROM capture_consumptions").Scan(&n); e != nil || n != 0 {
					t.Fatal(n, e)
				}
			}
			upgraded.Close()
			again, e := openDB(path, schema)
			if e != nil {
				t.Fatal("upgrade is not idempotent", e)
			}
			again.Close()
		})
	}
}

func TestMigrationsRejectModifiedChecksumsAndFutureVersions(t *testing.T) {
	for _, schema := range []string{"management.sql", "pipeline.sql"} {
		for _, mode := range []string{"v1-checksum", "v2-checksum", "future"} {
			t.Run(schema+"/"+mode, func(t *testing.T) {
				path := filepath.Join(t.TempDir(), "state.db")
				db, e := openDB(path, schema)
				if e != nil {
					t.Fatal(e)
				}
				switch mode {
				case "v1-checksum":
					_, e = db.Exec("UPDATE schema_migrations SET checksum='altered' WHERE version=1")
				case "v2-checksum":
					_, e = db.Exec("UPDATE schema_migrations SET checksum='altered' WHERE version=2")
				case "future":
					_, e = db.Exec("INSERT INTO schema_migrations VALUES(3,'future',?,'9.0.0')", Now())
				}
				if e != nil {
					t.Fatal(e)
				}
				db.Close()
				opened, e := openDB(path, schema)
				if e == nil {
					opened.Close()
					t.Fatal("unsafe schema accepted")
				}
				if !strings.Contains(e.Error(), map[string]string{"v1-checksum": "checksum", "v2-checksum": "checksum", "future": "newer"}[mode]) {
					t.Fatal(e)
				}
			})
		}
	}
}

func TestMigrationsRollbackAllExtensionDDLOnFailure(t *testing.T) {
	path, db, _ := migrationV1Fixture(t, "management.sql")
	if _, e := db.Exec("CREATE TABLE gatt_connections(conflicting_column TEXT)"); e != nil {
		t.Fatal(e)
	}
	db.Close()
	opened, e := openDB(path, "management.sql")
	if e == nil {
		opened.Close()
		t.Fatal("conflicting extension schema accepted")
	}
	verify := migrationRawDB(t, path)
	var count int
	if e = verify.QueryRow("SELECT count(*) FROM schema_migrations WHERE version=2").Scan(&count); e != nil || count != 0 {
		t.Fatal("failed migration marked applied", count, e)
	}
	if e = verify.QueryRow("SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('ble_peers','auto_connect_rules','auto_connect_triggers')").Scan(&count); e != nil || count != 0 {
		t.Fatal("partial migration DDL leaked", count, e)
	}
}
