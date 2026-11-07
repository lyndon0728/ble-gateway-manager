// gatewayctl is an offline administration utility. Secrets are read from or
// created in restricted files and are never printed to the terminal.
package main

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/simkit"
	"github.com/lyndon0728/ble-gateway-manager/server/internal/platform"
	"os"
	"path/filepath"
	"strings"
)

func main() {
	if e := run(os.Args[1:]); e != nil {
		fmt.Fprintln(os.Stderr, "gatewayctl:", e)
		os.Exit(1)
	}
}
func run(args []string) error {
	if len(args) == 0 {
		return errors.New("commands: init, provision-gateway, token, keygen, sign, backup, restore, reconcile-revisions")
	}
	fs := flag.NewFlagSet(args[0], flag.ContinueOnError)
	dir := fs.String("data-dir", "data", "server data directory")
	namespace := fs.String("namespace", "ns_local", "stable installation namespace")
	out := fs.String("out", "", "output file; existing files are not overwritten")
	switch args[0] {
	case "init":
		name := fs.String("admin", "admin", "initial administrator login")
		password := fs.String("password-file", "", "existing password file, or new random password will be written here")
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *password == "" {
			return errors.New("--password-file required")
		}
		app, e := platform.New(platform.Config{DataDir: *dir, AllowInsecureDev: true, Role: "all", NamespaceID: *namespace})
		if e != nil {
			return e
		}
		defer app.Close()
		var initialized int
		if e = app.Management.QueryRow("SELECT count(*) FROM instance_settings").Scan(&initialized); e != nil {
			return e
		}
		if initialized > 0 {
			fmt.Println("Instance already initialized; existing credentials retained.")
			return nil
		}
		if e = ensureToken(*password); e != nil {
			return e
		}
		secret, e := simkit.Secret(*password)
		if e != nil {
			return e
		}
		if e = app.Bootstrap(context.Background(), *name, secret); e != nil {
			return e
		}
		fmt.Println("Initialized server data; administrator secret remains in the password file.")
		return nil
	case "provision-gateway":
		id := fs.String("gateway", "gw_demo", "reference gateway ID")
		token := fs.String("token-file", "", "existing/new device token file")
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *token == "" {
			return errors.New("--token-file required")
		}
		if e := ensureToken(*token); e != nil {
			return e
		}
		secret, e := simkit.Secret(*token)
		if e != nil {
			return e
		}
		app, e := platform.New(platform.Config{DataDir: *dir, AllowInsecureDev: true, Role: "all", NamespaceID: *namespace})
		if e != nil {
			return e
		}
		defer app.Close()
		if e = app.ProvisionGateway(context.Background(), *id, secret); e != nil {
			return e
		}
		fmt.Println("Provisioned gateway credential; this does not certify physical hardware compatibility.")
		return nil
	case "token":
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *out == "" {
			return errors.New("--out required")
		}
		return newToken(*out)
	case "keygen":
		id := fs.String("key-id", "vendor-1", "signing key ID")
		public := fs.String("public-out", "", "trust-map JSON output")
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *out == "" || *public == "" {
			return errors.New("--out (private key) and --public-out required")
		}
		pub, priv, e := ed25519.GenerateKey(rand.Reader)
		if e != nil {
			return e
		}
		if e = writeNew(*out, []byte(base64.StdEncoding.EncodeToString(priv)+"\n")); e != nil {
			return e
		}
		b, _ := json.MarshalIndent(map[string]string{*id: base64.StdEncoding.EncodeToString(pub)}, "", "  ")
		return writeNew(*public, append(b, '\n'))
	case "sign":
		manifest := fs.String("manifest", "", "exact manifest UTF-8 file")
		file := fs.String("file", "", "artifact file whose SHA256 must match manifest")
		private := fs.String("private-key", "", "base64 Ed25519 private key file")
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *manifest == "" || *file == "" || *private == "" || *out == "" {
			return errors.New("--manifest --file --private-key --out required")
		}
		raw, e := os.ReadFile(*manifest)
		if e != nil {
			return e
		}
		var m map[string]any
		if e = json.Unmarshal(raw, &m); e != nil {
			return e
		}
		content, e := os.ReadFile(*file)
		if e != nil {
			return e
		}
		digest := sha256.Sum256(content)
		if simkit.Str(m, "sha256") != hex.EncodeToString(digest[:]) {
			return errors.New("manifest sha256 does not match file")
		}
		privText, e := simkit.Secret(*private)
		if e != nil {
			return e
		}
		priv, e := base64.StdEncoding.DecodeString(privText)
		if e != nil || len(priv) != ed25519.PrivateKeySize {
			return errors.New("invalid Ed25519 private key")
		}
		return writeNew(*out, []byte(base64.StdEncoding.EncodeToString(ed25519.Sign(priv, raw))+"\n"))
	case "backup":
		key := fs.String("key-file", "", "separate backup encryption key file, generated if absent")
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *out == "" || *key == "" {
			return errors.New("--out and --key-file required")
		}
		return backup(*dir, *out, *key)
	case "restore":
		key := fs.String("key-file", "", "backup encryption key file")
		archive := fs.String("archive", "", "encrypted backup archive")
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *archive == "" || *key == "" {
			return errors.New("--archive and --key-file required")
		}
		return restore(*dir, *archive, *key)
	case "reconcile-revisions":
		file := fs.String("file", "", "complete merged customer revision high-water JSON")
		confirm := fs.Bool("confirm-complete", false, "confirm all customer receivers contributed their maximum revisions")
		if e := fs.Parse(args[1:]); e != nil {
			return e
		}
		if *file == "" || !*confirm {
			return errors.New("--file and --confirm-complete required")
		}
		return reconcileRevisions(*dir, *namespace, *file)
	default:
		return fmt.Errorf("unknown command %q", args[0])
	}
}
func ensureToken(path string) error {
	if _, e := os.Stat(path); e == nil {
		return nil
	} else if !os.IsNotExist(e) {
		return e
	}
	return newToken(path)
}
func newToken(path string) error {
	b := make([]byte, 32)
	if _, e := rand.Read(b); e != nil {
		return e
	}
	return writeNew(path, []byte(base64.RawURLEncoding.EncodeToString(b)+"\n"))
}
func writeNew(path string, b []byte) error {
	if path == "" {
		return errors.New("empty output path")
	}
	if e := os.MkdirAll(filepath.Dir(path), 0700); e != nil {
		return e
	}
	f, e := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if e != nil {
		return e
	}
	_, e = f.Write(b)
	if e == nil {
		e = f.Sync()
	}
	return errors.Join(e, f.Close())
}
func inside(path, dir string) bool {
	rel, e := filepath.Rel(dir, path)
	return e == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(os.PathSeparator)) && !filepath.IsAbs(rel)
}
