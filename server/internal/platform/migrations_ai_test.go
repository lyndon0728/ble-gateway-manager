package platform

import (
	"database/sql"
	"strings"
	"testing"
)

func migrationV2Fixture(t *testing.T, schema string) (string, *sql.DB, string) {
	t.Helper()
	path, db, _ := migrationV1Fixture(t, schema)
	raw, err := extensionMigration(schema, 2)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec(string(raw)); err != nil {
		t.Fatal(err)
	}
	checksum := migrationChecksum(raw)
	if _, err = db.Exec("INSERT INTO schema_migrations VALUES(2,?,?,'0.3.0')", checksum, Now()); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec("CREATE TABLE upgrade_sentinel (value BLOB NOT NULL); INSERT INTO upgrade_sentinel VALUES(x'0001ff');"); err != nil {
		t.Fatal(err)
	}
	return path, db, checksum
}

func TestAIUpgradeFromV2PreservesDataAndMigrationHistory(t *testing.T) {
	for _, schema := range []string{"management.sql", "pipeline.sql"} {
		t.Run(schema, func(t *testing.T) {
			path, old, checksum := migrationV2Fixture(t, schema)
			old.Close()
			db, err := openDB(path, schema)
			if err != nil {
				t.Fatal(err)
			}
			var saved, sentinel string
			if err = db.QueryRow("SELECT checksum FROM schema_migrations WHERE version=2").Scan(&saved); err != nil || saved != checksum {
				t.Fatal("v2 migration history changed", err)
			}
			if err = db.QueryRow("SELECT hex(value) FROM upgrade_sentinel").Scan(&sentinel); err != nil || sentinel != "0001FF" {
				t.Fatal("pre-upgrade data changed", sentinel, err)
			}
			var latest int
			if err = db.QueryRow("SELECT max(version) FROM schema_migrations").Scan(&latest); err != nil || latest != 3 {
				t.Fatal("v3 not applied", latest, err)
			}
			if schema == "management.sql" {
				for _, table := range []string{"ai_providers", "ai_documents", "ai_jobs", "ai_drafts", "ai_model_versions", "protocol_parsers", "protocol_bindings", "gatt_discovery_snapshots"} {
					var count int
					if err = db.QueryRow("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?", table).Scan(&count); err != nil || count != 1 {
						t.Fatal("new table unavailable", table, err)
					}
				}
			} else {
				if _, err = db.Exec("SELECT protocol_context_json FROM processing_jobs LIMIT 0"); err != nil {
					t.Fatal("revision-specific capture snapshot missing", err)
				}
			}
			db.Close()
			db, err = openDB(path, schema)
			if err != nil {
				t.Fatal("v3 reopening not idempotent", err)
			}
			db.Close()
		})
	}
}

func TestAIMigrationRollsBackAllModulesOnConflict(t *testing.T) {
	path, db, _ := migrationV2Fixture(t, "management.sql")
	if _, err := db.Exec("CREATE TABLE protocol_parsers (conflicting_column TEXT)"); err != nil {
		t.Fatal(err)
	}
	db.Close()
	opened, err := openDB(path, "management.sql")
	if err == nil {
		opened.Close()
		t.Fatal("conflicting v3 table accepted")
	}
	if !strings.Contains(err.Error(), "migration 3") {
		t.Fatal(err)
	}
	verify := migrationRawDB(t, path)
	var count int
	if err = verify.QueryRow("SELECT count(*) FROM schema_migrations WHERE version=3").Scan(&count); err != nil || count != 0 {
		t.Fatal("failed migration recorded", err)
	}
	if err = verify.QueryRow("SELECT count(*) FROM sqlite_master WHERE name='ai_providers'").Scan(&count); err != nil || count != 0 {
		t.Fatal("partial AI schema escaped transaction", err)
	}
}
