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
	const latest = 3
	var maximum int
	if e := tx.QueryRow("SELECT COALESCE(MAX(version),0) FROM schema_migrations").Scan(&maximum); e != nil {
		return e
	}
	if maximum > latest {
		return errors.New("database schema is newer than this application; upgrade the program")
	}
	for version := 2; version <= latest; version++ {
		raw, e := extensionMigration(schemaName, version)
		if e != nil {
			return e
		}
		var saved string
		e = tx.QueryRow("SELECT checksum FROM schema_migrations WHERE version=?", version).Scan(&saved)
		if e == nil {
			if !migrationChecksumMatches(raw, saved) {
				return fmt.Errorf("migration %d checksum differs; refusing unsafe schema change", version)
			}
			continue
		}
		if !errors.Is(e, sql.ErrNoRows) {
			return e
		}
		if _, e = tx.Exec(string(raw)); e != nil {
			return fmt.Errorf("migration %d: %w", version, e)
		}
		if _, e = tx.Exec("INSERT INTO schema_migrations(version,checksum,applied_at_ms,app_version) VALUES(?,?,?,?)", version, migrationChecksum(raw), Now(), Version); e != nil {
			return e
		}
	}
	return nil
}

// Related v3 modules share one transaction and checksum. Once shipped these
// resources are immutable; later extensions must use a new migration version.
func extensionMigration(schemaName string, version int) ([]byte, error) {
	prefix := strings.TrimSuffix(schemaName, ".sql")
	if version == 2 {
		return Assets.ReadFile("assets/" + prefix + ".v2.sql")
	}
	if version != 3 || (prefix != "management" && prefix != "pipeline") {
		return nil, errors.New("unknown migration")
	}
	names := []string{"assets/" + prefix + ".protocol.v3.sql"}
	if prefix == "management" {
		names = append([]string{"assets/management.ai.v3.sql"}, names...)
	}
	var combined []byte
	for _, name := range names {
		raw, err := Assets.ReadFile(name)
		if err != nil {
			return nil, err
		}
		combined = append(combined, []byte("\n-- "+name+"\n")...)
		combined = append(combined, raw...)
		combined = append(combined, '\n')
	}
	return combined, nil
}
