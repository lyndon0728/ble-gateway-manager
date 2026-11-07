package main

import (
	"database/sql"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestRevisionReconciliationPreservesHigherFloor(t *testing.T) {
	dir := t.TempDir()
	db, e := sql.Open("sqlite", filepath.Join(dir, "management.db"))
	if e != nil {
		t.Fatal(e)
	}
	if _, e = db.Exec(`CREATE TABLE instance_settings(singleton INTEGER,settings_json TEXT);INSERT INTO instance_settings VALUES(1,'{"namespaceId":"ns_local"}')`); e != nil {
		t.Fatal(e)
	}
	db.Close()
	marker := filepath.Join(dir, "restore-reconciliation.required")
	if e = os.WriteFile(marker, []byte("required"), 0600); e != nil {
		t.Fatal(e)
	}
	existing := `{"namespaceId":"ns_local","complete":true,"revisions":[{"gatewayId":"gw_test","eventId":"boot_1-1","processingRevision":9}]}`
	if e = os.WriteFile(filepath.Join(dir, "revision-highwater.json"), []byte(existing), 0600); e != nil {
		t.Fatal(e)
	}
	input := filepath.Join(t.TempDir(), "merged.json")
	if e = os.WriteFile(input, []byte(`{"namespaceId":"ns_local","complete":true,"revisions":[{"gatewayId":"gw_test","eventId":"boot_1-1","processingRevision":7}]}`), 0600); e != nil {
		t.Fatal(e)
	}
	if reconcileRevisions(dir, "other", input) == nil {
		t.Fatal("accepted wrong installation namespace")
	}
	if _, e = os.Stat(marker); e != nil {
		t.Fatal("removed guard on failed import")
	}
	if e = reconcileRevisions(dir, "ns_local", input); e != nil {
		t.Fatal(e)
	}
	raw, e := os.ReadFile(filepath.Join(dir, "revision-highwater.json"))
	if e != nil {
		t.Fatal(e)
	}
	var doc revisionDocument
	if e = json.Unmarshal(raw, &doc); e != nil {
		t.Fatal(e)
	}
	if doc.Revisions[0].ProcessingRevision != 9 {
		t.Fatal("lowered an existing revision floor")
	}
	if _, e = os.Stat(marker); !os.IsNotExist(e) {
		t.Fatal("successful reconciliation did not remove guard")
	}
}
