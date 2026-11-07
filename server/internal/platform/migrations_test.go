package platform

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
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

func migrationLineEndingDigests(t *testing.T, asset string) (lf, crlf string) {
	t.Helper()
	raw, e := Assets.ReadFile("assets/" + asset)
	if e != nil {
		t.Fatal(e)
	}
	unix := strings.ReplaceAll(string(raw), "\r\n", "\n")
	if !strings.Contains(unix, "\n") {
		t.Fatal("fixture needs multiple SQL lines")
	}
	lfSum := sha256.Sum256([]byte(unix))
	crlfSum := sha256.Sum256([]byte(strings.ReplaceAll(unix, "\n", "\r\n")))
	lf, crlf = hex.EncodeToString(lfSum[:]), hex.EncodeToString(crlfSum[:])
	if lf == crlf {
		t.Fatal("line-ending test did not change the digest")
	}
	return lf, crlf
}

func TestMigrationsUpgradeV1AcrossLineEndingsPreservesHistoricalChecksum(t *testing.T) {
	for _, schema := range []string{"management.sql", "pipeline.sql"} {
		t.Run(schema, func(t *testing.T) {
			path, db, original := migrationV1Fixture(t, schema)
			lf, crlf := migrationLineEndingDigests(t, schema)
			other := lf
			if original == lf {
				other = crlf
			}
			if other == original {
				t.Fatal("fixture did not select another checkout style")
			}
			if _, e := db.Exec("UPDATE schema_migrations SET checksum=? WHERE version=1", other); e != nil {
				t.Fatal(e)
			}
			db.Close()
			upgraded, e := openDB(path, schema)
			if e != nil {
				t.Fatal("equivalent SQL from the other operating system was rejected", e)
			}
			var saved string
			var versions int
			if e = upgraded.QueryRow("SELECT checksum FROM schema_migrations WHERE version=1").Scan(&saved); e != nil || saved != other {
				t.Fatal("historical migration checksum was rewritten", saved, other, e)
			}
			if e = upgraded.QueryRow("SELECT count(*) FROM schema_migrations WHERE version IN (1,2)").Scan(&versions); e != nil || versions != 2 {
				t.Fatal("migration 2 was not applied", versions, e)
			}
			v2LF, _ := migrationLineEndingDigests(t, strings.TrimSuffix(schema, ".sql")+".v2.sql")
			if e = upgraded.QueryRow("SELECT checksum FROM schema_migrations WHERE version=2").Scan(&saved); e != nil || saved != v2LF {
				t.Fatal("new migration did not use the canonical LF digest", saved, v2LF, e)
			}
			upgraded.Close()
			again, e := openDB(path, schema)
			if e != nil {
				t.Fatal("second open rejected preserved historical digest", e)
			}
			defer again.Close()
			if e = again.QueryRow("SELECT checksum FROM schema_migrations WHERE version=1").Scan(&saved); e != nil || saved != other {
				t.Fatal("historical digest changed on reopen", saved, e)
			}
		})
	}
}

func TestMigrationsReopenV2AcrossLineEndingsWithoutRewritingChecksum(t *testing.T) {
	for _, schema := range []string{"management.sql", "pipeline.sql"} {
		t.Run(schema, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "v2-state.db")
			db, e := openDB(path, schema)
			if e != nil {
				t.Fatal(e)
			}
			asset := strings.TrimSuffix(schema, ".sql") + ".v2.sql"
			lf, crlf := migrationLineEndingDigests(t, asset)
			var current string
			if e = db.QueryRow("SELECT checksum FROM schema_migrations WHERE version=2").Scan(&current); e != nil {
				t.Fatal(e)
			}
			other := lf
			if current == lf {
				other = crlf
			}
			if current == other {
				t.Fatal("fixture did not change the v2 digest")
			}
			if _, e = db.Exec("UPDATE schema_migrations SET checksum=? WHERE version=2", other); e != nil {
				t.Fatal(e)
			}
			db.Close()
			reopened, e := openDB(path, schema)
			if e != nil {
				t.Fatal("equivalent migration 2 line endings rejected", e)
			}
			defer reopened.Close()
			var saved string
			if e = reopened.QueryRow("SELECT checksum FROM schema_migrations WHERE version=2").Scan(&saved); e != nil || saved != other {
				t.Fatal("v2 historical digest was rewritten", saved, other, e)
			}
		})
	}
}

func TestMigrationsLineEndingCompatibilityDoesNotNormalizeOtherChanges(t *testing.T) {
	for _, schema := range []string{"management.sql", "pipeline.sql"} {
		for _, version := range []int{1, 2} {
			t.Run(fmt.Sprintf("%s/version%d", schema, version), func(t *testing.T) {
				path := filepath.Join(t.TempDir(), "modified-state.db")
				db, e := openDB(path, schema)
				if e != nil {
					t.Fatal(e)
				}
				asset := schema
				if version == 2 {
					asset = strings.TrimSuffix(schema, ".sql") + ".v2.sql"
				}
				raw, e := Assets.ReadFile("assets/" + asset)
				if e != nil {
					t.Fatal(e)
				}
				changed := sha256.Sum256(append(raw, []byte("\n-- unauthorized migration content change\n")...))
				if _, e = db.Exec("UPDATE schema_migrations SET checksum=? WHERE version=?", hex.EncodeToString(changed[:]), version); e != nil {
					t.Fatal(e)
				}
				db.Close()
				opened, e := openDB(path, schema)
				if e == nil {
					opened.Close()
					t.Fatal("non-line-ending content change was treated as equivalent")
				}
				if !strings.Contains(e.Error(), "checksum") {
					t.Fatal(e)
				}
			})
		}
	}
}
