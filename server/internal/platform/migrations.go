package platform

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
)

// Older Windows builds embedded CRLF while Linux builds embedded LF. Accept
// only those two byte-equivalent forms, preserving recorded migration history.
// All newly applied migrations use the canonical LF digest.
func migrationChecksum(raw []byte) string {
	raw = bytes.ReplaceAll(raw, []byte("\r\n"), []byte("\n"))
	digest := sha256.Sum256(raw)
	return hex.EncodeToString(digest[:])
}
func migrationChecksumMatches(raw []byte, saved string) bool {
	if saved == migrationChecksum(raw) {
		return true
	}
	lf := bytes.ReplaceAll(raw, []byte("\r\n"), []byte("\n"))
	digest := sha256.Sum256(bytes.ReplaceAll(lf, []byte("\n"), []byte("\r\n")))
	return saved == hex.EncodeToString(digest[:])
}

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
	checksum := migrationChecksum(raw)
	var saved string
	e = tx.QueryRow("SELECT checksum FROM schema_migrations WHERE version=2").Scan(&saved)
	if e == nil {
		if !migrationChecksumMatches(raw, saved) {
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
