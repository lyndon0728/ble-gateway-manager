package platform

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/gofrs/flock"
	"github.com/santhosh-tekuri/jsonschema/v6"
	_ "modernc.org/sqlite"
)

//go:embed assets/*
var Assets embed.FS

const Version = "0.1.0"

type Config struct {
	DataDir, ListenAddr, Role, NamespaceID, InstanceID, NodeID, PublicURL string
	TLSCertFile, TLSKeyFile, ClientCAFile                                 string
	AllowInsecureDev                                                      bool
	BrokerURL, BrokerUser, BrokerPassword, BrokerCAFile                   string
	ArtifactPublicKeys                                                    map[string]string
	AllowedTargetHosts                                                    []string
	MaxQueueBytes                                                         int64
	RawRetention, DedupRetention, WorkerInterval, HTTPTimeout             time.Duration
	PipelineURL                                                           string
}

func ConfigFromEnv() (Config, error) {
	c := Config{DataDir: env("GW_DATA_DIR", "data"), ListenAddr: env("GW_LISTEN", "127.0.0.1:8080"), Role: env("GW_ROLE", "all"), NamespaceID: env("GW_NAMESPACE", "ns_local"), InstanceID: os.Getenv("GW_INSTANCE_ID"), NodeID: env("GW_NODE_ID", "data_local"), PublicURL: os.Getenv("GW_PUBLIC_URL"), TLSCertFile: os.Getenv("GW_TLS_CERT"), TLSKeyFile: os.Getenv("GW_TLS_KEY"), ClientCAFile: os.Getenv("GW_CLIENT_CA"), AllowInsecureDev: os.Getenv("GW_ALLOW_INSECURE_DEV") == "true", BrokerURL: os.Getenv("GW_BROKER_URL"), BrokerUser: os.Getenv("GW_BROKER_USER"), BrokerPassword: os.Getenv("GW_BROKER_PASSWORD"), BrokerCAFile: os.Getenv("GW_BROKER_CA"), PipelineURL: os.Getenv("GW_PIPELINE_URL"), MaxQueueBytes: 1 << 30, RawRetention: 24 * time.Hour, DedupRetention: 7 * 24 * time.Hour, WorkerInterval: 200 * time.Millisecond, HTTPTimeout: 10 * time.Second, ArtifactPublicKeys: map[string]string{}}
	if v := os.Getenv("GW_MAX_QUEUE_BYTES"); v != "" {
		n, e := strconv.ParseInt(v, 10, 64)
		if e != nil {
			return c, e
		}
		c.MaxQueueBytes = n
	}
	if v := os.Getenv("GW_ALLOWED_TARGET_HOSTS"); v != "" {
		c.AllowedTargetHosts = strings.Split(v, ",")
	}
	if v := os.Getenv("GW_ARTIFACT_PUBLIC_KEYS"); v != "" {
		if err := json.Unmarshal([]byte(v), &c.ArtifactPublicKeys); err != nil {
			return c, err
		}
	}
	return c, ValidateConfig(c)
}
func env(k, d string) string {
	if s := os.Getenv(k); s != "" {
		return s
	}
	return d
}
func ValidateConfig(c Config) error {
	if c.DataDir == "" {
		return errors.New("data directory required")
	}
	if c.Role != "all" && c.Role != "manager" && c.Role != "pipeline" {
		return errors.New("role must be all, manager or pipeline")
	}
	if !c.AllowInsecureDev && (c.TLSCertFile == "" || c.TLSKeyFile == "") {
		return errors.New("TLS certificate and key required; loopback development requires explicit GW_ALLOW_INSECURE_DEV=true")
	}
	if c.MaxQueueBytes <= 0 {
		return errors.New("positive queue quota required")
	}
	return nil
}

type App struct {
	Management, Pipeline *sql.DB
	Config               Config
	Logger               *slog.Logger
	HTTPClient           *http.Client
	Publish              func(context.Context, string, any) error
	schemas              map[string]*jsonschema.Schema
	master               []byte
	mu                   sync.Mutex
	pageMu               sync.Mutex
	pages                map[string]pageSnapshot
	loginSlots           chan struct{}
	loginMu              sync.Mutex
	loginBuckets         map[string]loginBucket
	globalLogins         loginBucket
	runtimeLock          *flock.Flock
}
type loginBucket struct {
	At    time.Time
	Count int
}

func New(c Config) (*App, error) {
	if c.DataDir == "" {
		c.DataDir = "data"
	}
	if c.Role == "" {
		c.Role = "all"
	}
	if c.NamespaceID == "" {
		c.NamespaceID = "ns_local"
	}
	if c.NodeID == "" {
		c.NodeID = "data_local"
	}
	if c.MaxQueueBytes == 0 {
		c.MaxQueueBytes = 1 << 30
	}
	if c.RawRetention == 0 {
		c.RawRetention = 24 * time.Hour
	}
	if c.DedupRetention == 0 {
		c.DedupRetention = 7 * 24 * time.Hour
	}
	if c.WorkerInterval == 0 {
		c.WorkerInterval = 200 * time.Millisecond
	}
	if c.HTTPTimeout == 0 {
		c.HTTPTimeout = 10 * time.Second
	}
	if err := os.MkdirAll(c.DataDir, 0700); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(filepath.Join(c.DataDir, "tmp"), 0700); err != nil {
		return nil, err
	}
	lock := flock.New(filepath.Join(c.DataDir, "runtime.lock"))
	locked, err := lock.TryRLock()
	if err != nil {
		return nil, err
	}
	if !locked {
		return nil, errors.New("offline maintenance owns data directory")
	}
	ready := false
	defer func() {
		if !ready {
			_ = lock.Unlock()
		}
	}()
	a := &App{Config: c, Logger: slog.Default(), schemas: map[string]*jsonschema.Schema{}, loginBuckets: map[string]loginBucket{}, pages: map[string]pageSnapshot{}, loginSlots: make(chan struct{}, 8)}
	a.runtimeLock = lock
	a.HTTPClient = &http.Client{Timeout: c.HTTPTimeout, CheckRedirect: func(req *http.Request, via []*http.Request) error { return http.ErrUseLastResponse }}
	a.Publish = func(context.Context, string, any) error { return errors.New("MQTT broker is not connected") }
	keyPath := filepath.Join(c.DataDir, "master.key")
	key, e := os.ReadFile(keyPath)
	if os.IsNotExist(e) {
		if _, x := os.Stat(filepath.Join(c.DataDir, "management.db")); x == nil {
			return nil, errors.New("existing database without master.key; restore the matching key")
		}
		key = make([]byte, 32)
		if _, e = rand.Read(key); e == nil {
			e = writeExclusive(keyPath, key, 0600)
		}
	}
	if e != nil || len(key) != 32 {
		return nil, fmt.Errorf("master key unavailable or invalid: %w", e)
	}
	a.master = key
	if a.Management, e = openDB(filepath.Join(c.DataDir, "management.db"), "management.sql"); e != nil {
		return nil, e
	}
	if a.Pipeline, e = openDB(filepath.Join(c.DataDir, "pipeline.db"), "pipeline.sql"); e != nil {
		a.Management.Close()
		return nil, e
	}
	var id, settings string
	e = a.Management.QueryRow("SELECT instance_id,settings_json FROM instance_settings WHERE singleton=1").Scan(&id, &settings)
	if e == nil {
		if c.InstanceID != "" && c.InstanceID != id {
			a.Close()
			return nil, errors.New("instance ID does not match database")
		}
		a.Config.InstanceID = id
		var saved map[string]any
		if json.Unmarshal([]byte(settings), &saved) != nil || Str(saved, "namespaceId") == "" {
			a.Close()
			return nil, errors.New("invalid stored namespace")
		}
		if a.Config.NamespaceID != Str(saved, "namespaceId") {
			a.Close()
			return nil, errors.New("namespace does not match database; use original GW_NAMESPACE")
		}
	} else if !errors.Is(e, sql.ErrNoRows) {
		a.Close()
		return nil, e
	} else if a.Config.InstanceID == "" {
		a.Config.InstanceID = ID("inst")
	}
	raw, _ := Assets.ReadFile("assets/openapi.json")
	var doc map[string]any
	if e = json.Unmarshal(raw, &doc); e != nil {
		a.Close()
		return nil, e
	}
	compiler := jsonschema.NewCompiler()
	compiler.AssertFormat()
	if e = compiler.AddResource("https://local.invalid/openapi.json", doc); e != nil {
		a.Close()
		return nil, e
	}
	for name := range doc["components"].(map[string]any)["schemas"].(map[string]any) {
		sch, err := compiler.Compile("https://local.invalid/openapi.json#/components/schemas/" + name)
		if err != nil {
			a.Close()
			return nil, err
		}
		a.schemas[name] = sch
	}
	ready = true
	return a, nil
}

func openDB(path, schemaName string) (*sql.DB, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	uriPath := filepath.ToSlash(abs)
	if !strings.HasPrefix(uriPath, "/") {
		uriPath = "/" + uriPath
	}
	u := url.URL{Scheme: "file", Path: uriPath}
	q := u.Query()
	q.Add("_pragma", "foreign_keys(1)")
	q.Add("_pragma", "busy_timeout(5000)")
	q.Add("_pragma", "synchronous(FULL)")
	q.Set("_txlock", "immediate")
	u.RawQuery = q.Encode()
	db, err := sql.Open("sqlite", u.String())
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(8)
	db.SetMaxIdleConns(8)
	fail := func(e error) (*sql.DB, error) { db.Close(); return nil, e }
	if _, err = db.Exec("PRAGMA journal_mode=WAL"); err != nil {
		return fail(err)
	}
	raw, err := Assets.ReadFile("assets/" + schemaName)
	if err != nil {
		return fail(err)
	}
	digest := sha256.Sum256(raw)
	checksum := hex.EncodeToString(digest[:])
	tx, err := db.BeginTx(context.Background(), nil)
	if err != nil {
		return fail(err)
	}
	defer tx.Rollback()
	var exists int
	if err = tx.QueryRow("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='schema_migrations'").Scan(&exists); err != nil {
		return fail(err)
	}
	if exists == 0 {
		lines := strings.Split(string(raw), "\n")
		out := make([]string, 0, len(lines))
		for _, l := range lines {
			t := strings.ToUpper(strings.TrimSpace(l))
			if strings.HasPrefix(t, "PRAGMA ") || t == "BEGIN IMMEDIATE;" || t == "COMMIT;" {
				continue
			}
			out = append(out, l)
		}
		if _, err = tx.Exec(strings.Join(out, "\n")); err != nil {
			return fail(err)
		}
		if _, err = tx.Exec("INSERT INTO schema_migrations(version,checksum,applied_at_ms,app_version) VALUES(1,?,?,?)", checksum, Now(), Version); err != nil {
			return fail(err)
		}
	} else {
		var old string
		if err = tx.QueryRow("SELECT checksum FROM schema_migrations WHERE version=1").Scan(&old); err != nil {
			return fail(err)
		}
		if old != checksum {
			return fail(errors.New("migration checksum differs; refusing unsafe schema change"))
		}
	}
	if err = tx.Commit(); err != nil {
		return fail(err)
	}
	return db, nil
}
func (a *App) Close() error {
	a.closeDeliveryClients()
	var e error
	if a.Pipeline != nil {
		e = a.Pipeline.Close()
	}
	if a.Management != nil {
		e = errors.Join(e, a.Management.Close())
	}
	if a.runtimeLock != nil {
		e = errors.Join(e, a.runtimeLock.Unlock())
	}
	return e
}
func (a *App) Run(ctx context.Context) {
	var workers sync.WaitGroup
	if a.Config.Role != "pipeline" {
		workers.Add(1)
		go func() { defer workers.Done(); a.StartControl(ctx) }()
	}
	if a.Config.Role != "manager" {
		workers.Add(1)
		go func() { defer workers.Done(); a.StartPipeline(ctx) }()
	}
	<-ctx.Done()
	workers.Wait()
}
func (a *App) Handler() http.Handler {
	r := chi.NewRouter()
	r.Use(requestTrace, middleware.Recoverer, a.roleMiddleware, a.authMiddleware)
	r.Get("/health/live", func(w http.ResponseWriter, r *http.Request) { JSON(w, 200, map[string]any{"status": "ok"}) })
	r.Get("/health/ready", func(w http.ResponseWriter, r *http.Request) {
		if a.Management.PingContext(r.Context()) != nil || a.Pipeline.PingContext(r.Context()) != nil {
			Fail(w, r, 503, "NOT_READY", "database unavailable")
			return
		}
		var initialized, active int
		if a.Management.QueryRowContext(r.Context(), "SELECT count(*) FROM instance_settings").Scan(&initialized) != nil || initialized != 1 || a.Pipeline.QueryRowContext(r.Context(), "SELECT count(*) FROM pipeline_bundles WHERE namespace_id=? AND state='active'", a.Config.NamespaceID).Scan(&active) != nil || active != 1 {
			JSON(w, 503, map[string]any{"status": "not_ready"})
			return
		}
		JSON(w, 200, map[string]any{"status": "ok"})
	})
	r.Get("/openapi.json", func(w http.ResponseWriter, r *http.Request) {
		if _, ok := a.Require(w, r, ""); !ok {
			return
		}
		b, _ := Assets.ReadFile("assets/openapi.json")
		w.Header().Set("Content-Type", "application/json")
		w.Write(b)
	})
	a.RegisterCore(r)
	a.RegisterControl(r)
	a.RegisterPipeline(r)
	r.Get("/", a.console)
	return r
}
func requestTrace(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		trace := ID("trace")
		w.Header().Set("X-Trace-Id", trace)
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), middleware.RequestIDKey, trace)))
	})
}
func (a *App) Decode(w http.ResponseWriter, r *http.Request, schema string) (map[string]any, bool) {
	defer r.Body.Close()
	r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
	dec := json.NewDecoder(r.Body)
	var obj map[string]any
	if err := dec.Decode(&obj); err != nil || obj == nil {
		Fail(w, r, 400, "INVALID_JSON", "expected a JSON object within the size limit")
		return nil, false
	}
	if err := dec.Decode(new(any)); err != io.EOF {
		Fail(w, r, 400, "INVALID_JSON", "only one JSON value is allowed")
		return nil, false
	}
	sch := a.schemas[schema]
	if sch == nil {
		Fail(w, r, 500, "SCHEMA_MISSING", "server schema is unavailable")
		return nil, false
	}
	if err := sch.Validate(obj); err != nil {
		Fail(w, r, 422, "VALIDATION_FAILED", "request does not match "+schema+"; see /openapi.json")
		return nil, false
	}
	return obj, true
}
func (a *App) Validate(schema string, v any) error {
	b, e := json.Marshal(v)
	if e != nil {
		return e
	}
	var obj any
	if e = json.Unmarshal(b, &obj); e != nil {
		return e
	}
	s := a.schemas[schema]
	if s == nil {
		return errors.New("unknown schema")
	}
	return s.Validate(obj)
}
func JSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	if status != 204 {
		_ = json.NewEncoder(w).Encode(v)
	}
}
func Fail(w http.ResponseWriter, r *http.Request, status int, code, msg string) {
	if status == 429 || status == 503 {
		w.Header().Set("Retry-After", "2")
	}
	trace := middleware.GetReqID(r.Context())
	if trace == "" {
		trace = ID("trace")
	}
	w.Header().Set("X-Trace-Id", trace)
	JSON(w, status, map[string]any{"error": map[string]any{"code": code, "message": msg, "retryable": status == 429 || status == 503, "traceId": trace}})
}

type APIError struct {
	Status        int
	Code, Message string
}

func (e *APIError) Error() string                    { return e.Message }
func Problem(status int, code, message string) error { return &APIError{status, code, message} }
func RespondError(w http.ResponseWriter, r *http.Request, err error) {
	var p *APIError
	if errors.As(err, &p) {
		Fail(w, r, p.Status, p.Code, p.Message)
	} else {
		Fail(w, r, 500, "INTERNAL_ERROR", "operation failed; see server diagnostics")
	}
}
func ID(prefix string) string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return prefix + "_" + hex.EncodeToString(b)
}
func Now() int64                { return time.Now().UTC().UnixMilli() }
func Timestamp(ms int64) string { return time.UnixMilli(ms).UTC().Format(time.RFC3339Nano) }
func Hash(v any) string {
	b, _ := json.Marshal(v)
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}
func Str(m map[string]any, k string) string { s, _ := m[k].(string); return s }
func Num(m map[string]any, k string) int64 {
	switch n := m[k].(type) {
	case float64:
		return int64(n)
	case int:
		return int64(n)
	case int64:
		return n
	case json.Number:
		x, _ := n.Int64()
		return x
	}
	return 0
}
func Obj(m map[string]any, k string) map[string]any {
	o, _ := m[k].(map[string]any)
	if o == nil {
		return map[string]any{}
	}
	return o
}
func List(m map[string]any, k string) []any {
	a, _ := m[k].([]any)
	if a == nil {
		return []any{}
	}
	return a
}
func encode(v any) string { b, _ := json.Marshal(v); return string(b) }
func writeExclusive(path string, b []byte, mode os.FileMode) error {
	f, e := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, mode)
	if e != nil {
		return e
	}
	_, e = f.Write(b)
	if e == nil {
		e = f.Sync()
	}
	return errors.Join(e, f.Close())
}
func (a *App) SecretPut(value string) (string, error) {
	if len(value) > 65536 {
		return "", errors.New("secret too large")
	}
	block, e := aes.NewCipher(a.master)
	if e != nil {
		return "", e
	}
	g, e := cipher.NewGCM(block)
	if e != nil {
		return "", e
	}
	id := ID("sec")
	nonce := make([]byte, g.NonceSize())
	if _, e = rand.Read(nonce); e != nil {
		return "", e
	}
	sealed := g.Seal(nonce, nonce, []byte(value), []byte(id))
	dir := filepath.Join(a.Config.DataDir, "secrets")
	if e = os.MkdirAll(dir, 0700); e != nil {
		return "", e
	}
	return id, writeExclusive(filepath.Join(dir, id), sealed, 0600)
}
func (a *App) SecretGet(ref string) (string, error) {
	if !strings.HasPrefix(ref, "sec_") || strings.ContainsAny(ref, "/\\.") {
		return "", errors.New("invalid secret reference")
	}
	sealed, e := os.ReadFile(filepath.Join(a.Config.DataDir, "secrets", ref))
	if e != nil {
		return "", e
	}
	block, e := aes.NewCipher(a.master)
	if e != nil {
		return "", e
	}
	g, e := cipher.NewGCM(block)
	if e != nil || len(sealed) < g.NonceSize() {
		return "", errors.New("invalid encrypted secret")
	}
	b, e := g.Open(nil, sealed[:g.NonceSize()], sealed[g.NonceSize():], []byte(ref))
	return string(b), e
}
func (a *App) Audit(ctx context.Context, tx *sql.Tx, actor, action, objType, objID string, details any) error {
	q := "INSERT INTO audit_events VALUES(?,?,?,?,?,?,?,?,?,?)"
	args := []any{ID("audit"), actor, action, objType, objID, "success", ID("corr"), encode(details), Now(), Now() + int64(90*24*time.Hour/time.Millisecond)}
	if tx != nil {
		_, e := tx.ExecContext(ctx, q, args...)
		return e
	}
	_, e := a.Management.ExecContext(ctx, q, args...)
	return e
}
func (a *App) DoIdempotent(ctx context.Context, pID, op, key string, request any, fn func(*sql.Tx) (int, string, any, error)) (int, any, error) {
	if key == "" || len(key) > 128 {
		return 0, nil, Problem(400, "IDEMPOTENCY_KEY_REQUIRED", "Idempotency-Key of 1-128 bytes required")
	}
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return 0, nil, e
	}
	defer tx.Rollback()
	mac := hmac.New(sha256.New, a.master)
	mac.Write([]byte(encode(request)))
	digest := hex.EncodeToString(mac.Sum(nil))
	var old, response string
	var status int
	e = tx.QueryRowContext(ctx, "SELECT request_hash,http_status,response_json FROM api_idempotency WHERE principal_id=? AND operation=? AND idempotency_key=? AND state='committed'", pID, op, key).Scan(&old, &status, &response)
	if e == nil {
		if old != digest {
			return 0, nil, Problem(409, "IDEMPOTENCY_CONFLICT", "key already used for a different request")
		}
		var v any
		e = json.Unmarshal([]byte(response), &v)
		return status, v, e
	}
	if !errors.Is(e, sql.ErrNoRows) {
		return 0, nil, e
	}
	status, id, v, e := fn(tx)
	if e != nil {
		return 0, nil, e
	}
	_, e = tx.ExecContext(ctx, "INSERT INTO api_idempotency(operation_id,principal_id,operation,idempotency_key,request_hash,state,resource_type,resource_id,http_status,response_json,created_at_ms,expires_at_ms) VALUES(?,?,?,?,?,'committed',?,?,?,?,?,?)", ID("op"), pID, op, key, digest, op, id, status, encode(v), Now(), Now()+int64(a.Config.DedupRetention/time.Millisecond))
	if e != nil {
		return 0, nil, e
	}
	if e = tx.Commit(); e != nil {
		return 0, nil, e
	}
	return status, v, nil
}
func ETag(version int64) string { return fmt.Sprintf("\"%d\"", version) }
func Match(r *http.Request, version int64) error {
	h := r.Header.Get("If-Match")
	if h == "" {
		return Problem(428, "PRECONDITION_REQUIRED", "If-Match required")
	}
	if h != ETag(version) {
		return Problem(412, "PRECONDITION_FAILED", "resource revision changed")
	}
	return nil
}
