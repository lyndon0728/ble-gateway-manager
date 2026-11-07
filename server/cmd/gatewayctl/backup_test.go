package main

import (
	"bytes"
	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/runtimeguard"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func TestEncryptedFramesRejectDamageAndTruncation(t *testing.T) {
	key := bytes.Repeat([]byte{42}, 32)
	plain := bytes.Repeat([]byte("persistent-data"), 12000)
	var out bytes.Buffer
	enc, e := newEncryptWriter(&out, key)
	if e != nil {
		t.Fatal(e)
	}
	if _, e = enc.Write(plain); e != nil {
		t.Fatal(e)
	}
	if e = enc.Close(); e != nil {
		t.Fatal(e)
	}
	var restored bytes.Buffer
	if e = decrypt(bytes.NewReader(out.Bytes()), &restored, key); e != nil || !bytes.Equal(plain, restored.Bytes()) {
		t.Fatalf("round trip failed: %v", e)
	}
	for _, raw := range [][]byte{out.Bytes()[:out.Len()-20], append([]byte(nil), out.Bytes()...)} {
		raw[len(raw)/2] ^= 1
		if decrypt(bytes.NewReader(raw), io.Discard, key) == nil {
			t.Fatal("accepted damaged backup")
		}
	}
}
func TestOfflineBackupRestoreAndRuntimeExclusion(t *testing.T) {
	base := t.TempDir()
	data := filepath.Join(base, "data")
	if e := os.MkdirAll(data, 0700); e != nil {
		t.Fatal(e)
	}
	for _, name := range []string{"master.key", "management.db", "pipeline.db"} {
		if e := os.WriteFile(filepath.Join(data, name), []byte(name), 0600); e != nil {
			t.Fatal(e)
		}
	}
	archive := filepath.Join(base, "backup.bgb")
	key := filepath.Join(base, "backup.key")
	release, e := runtimeguard.Acquire(data, false)
	if e != nil {
		t.Fatal(e)
	}
	if backup(data, archive, key) == nil {
		t.Fatal("backed up a running server")
	}
	release()
	if e = backup(data, archive, key); e != nil {
		t.Fatal(e)
	}
	target := filepath.Join(base, "restored")
	if e = restore(target, archive, key); e != nil {
		t.Fatal(e)
	}
	b, e := os.ReadFile(filepath.Join(target, "master.key"))
	if e != nil || string(b) != "master.key" {
		t.Fatalf("restored key mismatch: %v", e)
	}
	if restore(target, archive, key) == nil {
		t.Fatal("overwrote nonempty restore destination")
	}
}
