package platform

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
)

func migrateExtensions(tx *sql.Tx, schemaName string) error {
	const latest = 2
	var maximum int
	if e := tx.QueryRow("SELECT COALESCE(MAX(version),0) FROM schema_migrations").Scan(&maximum); e != nil {
		return e
	}
	if maximum > latest {
		return errors.New("database schema is newer than this application; upgrade the program")
	}
	raw, e := Assets.ReadFile("assets/" + strings.TrimSuffix(schemaName, ".sql") + ".v2.sql")
	if e != nil {
		return e
	}
	digest := sha256.Sum256(raw)
	checksum := hex.EncodeToString(digest[:])
	var saved string
	e = tx.QueryRow("SELECT checksum FROM schema_migrations WHERE version=2").Scan(&saved)
	if e == nil {
		if saved != checksum {
			return errors.New("migration 2 checksum differs; refusing unsafe schema change")
		}
		return nil
	}
	if !errors.Is(e, sql.ErrNoRows) {
		return e
	}
	if _, e = tx.Exec(string(raw)); e != nil {
		return fmt.Errorf("migration 2: %w", e)
	}
	_, e = tx.Exec("INSERT INTO schema_migrations(version,checksum,applied_at_ms,app_version) VALUES(2,?,?,?)", checksum, Now(), Version)
	return e
}
