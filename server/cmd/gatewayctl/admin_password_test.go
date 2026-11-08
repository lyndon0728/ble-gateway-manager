package main

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/lyndon0728/ble-gateway-manager/server/internal/platform"
)

func TestLocalDefaultInitPreservesExistingCredentials(t *testing.T) {
	base := t.TempDir()
	data := filepath.Join(base, "data")
	passwordFile := filepath.Join(base, "secrets", "admin-password")
	args := []string{"init", "--data-dir", data, "--password-file", passwordFile, "--local-default-admin"}
	if err := run(args); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(passwordFile)
	if err != nil || string(b) != "123456\n" {
		t.Fatal("local default password file was not created")
	}
	app, err := platform.New(platform.Config{DataDir: data, AllowInsecureDev: true, NamespaceID: "ns_local"})
	if err != nil {
		t.Fatal(err)
	}
	if err = platform.ResetAdministratorPassword(context.Background(), app.Management, "ns_local", "admin", "custom-password-kept-on-restart", false); err != nil {
		t.Fatal(err)
	}
	var before string
	if err = app.Management.QueryRow("SELECT verifier FROM credentials WHERE kind='password' AND status='active'").Scan(&before); err != nil {
		t.Fatal(err)
	}
	app.Close()
	if err = os.WriteFile(passwordFile, []byte("custom-password-kept-on-restart\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err = run(args); err != nil {
		t.Fatal(err)
	}
	b, err = os.ReadFile(passwordFile)
	if err != nil || string(b) != "custom-password-kept-on-restart\n" {
		t.Fatal("restart overwrote the password file")
	}
	app, err = platform.New(platform.Config{DataDir: data, AllowInsecureDev: true, NamespaceID: "ns_local"})
	if err != nil {
		t.Fatal(err)
	}
	defer app.Close()
	var after string
	if err = app.Management.QueryRow("SELECT verifier FROM credentials WHERE kind='password' AND status='active'").Scan(&after); err != nil {
		t.Fatal(err)
	}
	if before != after {
		t.Fatal("restart changed the stored administrator password")
	}
}

func TestLocalDefaultInitRespectsCustomInputAndOrdinaryInit(t *testing.T) {
	for _, local := range []bool{false, true} {
		base := t.TempDir()
		passwordFile := filepath.Join(base, "password")
		original := []byte("preexisting-strong-password\n")
		if err := os.WriteFile(passwordFile, original, 0600); err != nil {
			t.Fatal(err)
		}
		args := []string{"init", "--data-dir", filepath.Join(base, "data"), "--password-file", passwordFile}
		if local {
			args = append(args, "--local-default-admin")
		}
		if err := run(args); err != nil {
			t.Fatal(err)
		}
		b, err := os.ReadFile(passwordFile)
		if err != nil || string(b) != string(original) {
			t.Fatal("custom input was overwritten")
		}
	}
	base := t.TempDir()
	passwordFile := filepath.Join(base, "random-password")
	if err := run([]string{"init", "--data-dir", filepath.Join(base, "data"), "--password-file", passwordFile}); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(passwordFile)
	if err != nil || len(b) < 32 || string(b) == "123456\n" {
		t.Fatal("ordinary initializer no longer generates a random password")
	}
}

func TestOfflineAdministratorResetRejectsRunningInstance(t *testing.T) {
	base := t.TempDir()
	data := filepath.Join(base, "data")
	passwordFile := filepath.Join(base, "secrets", "admin-password")
	if err := run([]string{"init", "--data-dir", data, "--password-file", passwordFile}); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(passwordFile)
	if err != nil {
		t.Fatal(err)
	}
	app, err := platform.New(platform.Config{DataDir: data, AllowInsecureDev: true, NamespaceID: "ns_local"})
	if err != nil {
		t.Fatal(err)
	}
	reset := []string{"reset-admin-password", "--data-dir", data, "--password-file", passwordFile, "--local-default-admin"}
	if run(reset) == nil {
		t.Fatal("reset was permitted while a runtime owns the shared lock")
	}
	after, err := os.ReadFile(passwordFile)
	if err != nil || string(after) != string(before) {
		t.Fatal("rejected reset changed the password file")
	}
	app.Close()
	if err = run(reset); err != nil {
		t.Fatal(err)
	}
	after, err = os.ReadFile(passwordFile)
	if err != nil || string(after) != "123456\n" {
		t.Fatal("offline reset did not synchronize password file")
	}
	if err = os.WriteFile(passwordFile, []byte("new-ordinary-admin-password\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err = run([]string{"reset-admin-password", "--data-dir", data, "--password-file", passwordFile}); err != nil {
		t.Fatal(err)
	}
	if err = run([]string{"reset-admin-password", "--data-dir", data, "--password-file", filepath.Join(data, "master.key"), "--local-default-admin"}); err == nil {
		t.Fatal("reset overwrote a data-directory file")
	}
}
