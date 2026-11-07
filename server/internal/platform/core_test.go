package platform

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type apiHarness struct {
	t       *testing.T
	a       *App
	handler http.Handler
	cookie  *http.Cookie
	csrf    string
}

func newHarness(t *testing.T) *apiHarness {
	t.Helper()
	a, e := New(Config{DataDir: t.TempDir(), AllowInsecureDev: true})
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { a.Close() })
	if e = a.Bootstrap(context.Background(), "admin", "a-strong-test-password"); e != nil {
		t.Fatal(e)
	}
	if e = a.ensurePipelineDefault(context.Background()); e != nil {
		t.Fatal(e)
	}
	h := &apiHarness{t: t, a: a, handler: a.Handler()}
	r := h.do("POST", "/api/v1/auth/sessions", map[string]any{"name": "admin", "password": "a-strong-test-password"}, nil)
	h.expect(r, 200, "Session")
	h.cookie = r.Result().Cookies()[0]
	h.csrf = Str(h.object(r), "csrfToken")
	return h
}
func (h *apiHarness) do(method, path string, body any, headers map[string]string) *httptest.ResponseRecorder {
	h.t.Helper()
	var data []byte
	if body != nil {
		data, _ = json.Marshal(body)
	}
	r := httptest.NewRequest(method, path, bytes.NewReader(data))
	r.Header.Set("Content-Type", "application/json")
	if h.cookie != nil {
		r.AddCookie(h.cookie)
		r.Header.Set("X-CSRF-Token", h.csrf)
	}
	for k, v := range headers {
		r.Header.Set(k, v)
	}
	w := httptest.NewRecorder()
	h.handler.ServeHTTP(w, r)
	return w
}
func (h *apiHarness) object(r *httptest.ResponseRecorder) map[string]any {
	h.t.Helper()
	var m map[string]any
	if e := json.Unmarshal(r.Body.Bytes(), &m); e != nil {
		h.t.Fatalf("invalid JSON %s: %v", r.Body.String(), e)
	}
	return m
}
func (h *apiHarness) expect(r *httptest.ResponseRecorder, status int, schema string) {
	h.t.Helper()
	if r.Code != status {
		h.t.Fatalf("status %d expected %d: %s", r.Code, status, r.Body.String())
	}
	if schema != "" {
		if e := h.a.Validate(schema, h.object(r)); e != nil {
			h.t.Fatalf("%s response: %v; %s", schema, e, r.Body.String())
		}
	}
}
func TestCoreAPISchemasAndCSRF(t *testing.T) {
	h := newHarness(t)
	for path, schema := range map[string]string{"/health/live": "Health", "/health/ready": "Health", "/api/v1/auth/me": "Principal", "/api/v1/principals": "PrincipalPage", "/api/v1/resource-scopes": "ResourceScopePage", "/api/v1/audit-events": "AuditEventPage", "/api/v1/system/status": "SystemStatus", "/api/v1/gateways": "GatewayPage", "/api/v1/tasks": "TaskPage", "/api/v1/parsers": "ParserPage", "/api/v1/rule-bundles": "RuleBundlePage", "/api/v1/targets": "TargetPage", "/api/v1/deliveries": "DeliveryPage", "/api/v1/events": "EventViewPage", "/api/v1/quarantine": "QuarantineItemPage"} {
		t.Run(path, func(t *testing.T) {
			local := *h
			local.t = t
			local.expect(local.do("GET", path, nil, nil), 200, schema)
		})
	}
	h.expect(h.do("DELETE", "/api/v1/auth/sessions/current", nil, map[string]string{"X-CSRF-Token": "bad"}), 403, "ErrorResponse")
	h.expect(h.do("GET", "/api/v1/auth/me", nil, nil), 200, "Principal")
	h.expect(h.do("DELETE", "/api/v1/auth/sessions/current", nil, nil), 204, "")
	h.expect(h.do("GET", "/api/v1/auth/me", nil, nil), 401, "ErrorResponse")
}
func TestCorePrincipalTokensAndLastAdmin(t *testing.T) {
	h := newHarness(t)
	admin := h.object(h.do("GET", "/api/v1/auth/me", nil, nil))
	id := Str(admin, "principalId")
	h.expect(h.do("PATCH", "/api/v1/principals/"+id, map[string]any{"state": "DISABLED"}, map[string]string{"If-Match": "\"1\""}), 409, "ErrorResponse")
	request := map[string]any{"type": "APPLICATION", "name": "client", "roleBindings": []any{map[string]any{"role": "DEVELOPER", "scopeId": "scope_default_group"}}}
	headers := map[string]string{"Idempotency-Key": "create-app"}
	r := h.do("POST", "/api/v1/principals", request, headers)
	h.expect(r, 201, "Principal")
	pid := Str(h.object(r), "principalId")
	same := h.do("POST", "/api/v1/principals", request, headers)
	h.expect(same, 201, "Principal")
	if Str(h.object(same), "principalId") != pid {
		t.Fatal("idempotency changed principal")
	}
	request["name"] = "different"
	h.expect(h.do("POST", "/api/v1/principals", request, headers), 409, "ErrorResponse")
	issued := h.do("POST", "/api/v1/principals/"+pid+"/credentials", map[string]any{"label": "integration", "expiresAt": Timestamp(Now() + 3600000)}, nil)
	h.expect(issued, 201, "CredentialIssued")
	token := Str(h.object(issued), "token")
	cid := Str(h.object(issued), "credentialId")
	h.expect(h.do("GET", "/api/v1/principals/"+pid+"/credentials", nil, nil), 200, "CredentialMetadataPage")
	h.expect(h.do("GET", "/api/v1/auth/me", nil, map[string]string{"Authorization": "Bearer " + token}), 200, "Principal")
	h.expect(h.do("GET", "/api/v1/principals", nil, map[string]string{"Authorization": "Bearer " + token}), 403, "ErrorResponse")
	h.expect(h.do("DELETE", "/api/v1/principals/"+pid+"/credentials/"+cid, nil, nil), 204, "")
	h.expect(h.do("GET", "/api/v1/auth/me", nil, map[string]string{"Authorization": "Bearer " + token}), 401, "ErrorResponse")
	var responses string
	rows, e := h.a.Management.Query("SELECT COALESCE(response_json,'') FROM api_idempotency")
	if e != nil {
		t.Fatal(e)
	}
	defer rows.Close()
	for rows.Next() {
		if e = rows.Scan(&responses); e != nil {
			t.Fatal(e)
		}
		if strings.Contains(responses, token) {
			t.Fatal("token persisted in replay cache")
		}
	}
}
func TestCoreScopeIsolationAndArchive(t *testing.T) {
	h := newHarness(t)
	a := h.a
	p := &Principal{Bindings: []Binding{{"DEVICE_ADMIN", "scope_default_group"}, {"AUDITOR", "scope_instance"}}}
	if !a.Can(p, "gateway:configure", "scope_default_group") {
		t.Fatal("expected permitted scope")
	}
	_, e := a.Management.Exec("INSERT INTO resource_scopes VALUES('scope_other','group','scope_default_project','Other','active',1,?)", Now())
	if e != nil {
		t.Fatal(e)
	}
	if a.Can(p, "gateway:configure", "scope_other") {
		t.Fatal("combined unrelated role scopes into elevated permission")
	}
	r := h.do("POST", "/api/v1/resource-scopes", map[string]any{"kind": "GROUP", "parentScopeId": "scope_default_project", "name": "Empty"}, map[string]string{"Idempotency-Key": "empty"})
	h.expect(r, 201, "ResourceScope")
	sid := Str(h.object(r), "scopeId")
	h.expect(h.do("PATCH", "/api/v1/resource-scopes/"+sid, map[string]any{"state": "ARCHIVED"}, map[string]string{"If-Match": "\"1\""}), 200, "ResourceScope")
	h.expect(h.do("PATCH", "/api/v1/resource-scopes/scope_default_project", map[string]any{"state": "ARCHIVED"}, map[string]string{"If-Match": "\"1\""}), 409, "ErrorResponse")
	h.expect(h.do("PATCH", "/api/v1/resource-scopes/"+sid, map[string]any{"state": "ACTIVE"}, map[string]string{"If-Match": "\"1\""}), 412, "ErrorResponse")
}
func TestCoreDurabilityAndSecretProtection(t *testing.T) {
	h := newHarness(t)
	a := h.a
	var sync, foreign int
	if e := a.Pipeline.QueryRow("PRAGMA synchronous").Scan(&sync); e != nil || sync != 2 {
		t.Fatalf("synchronous=%d %v", sync, e)
	}
	if e := a.Pipeline.QueryRow("PRAGMA foreign_keys").Scan(&foreign); e != nil || foreign != 1 {
		t.Fatalf("foreign_keys=%d %v", foreign, e)
	}
	secret := "secret-used-only-in-test"
	ref, e := a.SecretPut(secret)
	if e != nil {
		t.Fatal(e)
	}
	raw, e := os.ReadFile(filepath.Join(a.Config.DataDir, "secrets", ref))
	if e != nil {
		t.Fatal(e)
	}
	if bytes.Contains(raw, []byte(secret)) {
		t.Fatal("plaintext secret")
	}
	if got, e := a.SecretGet(ref); e != nil || got != secret {
		t.Fatal("secret round trip", e)
	}
	raw[len(raw)-1] ^= 1
	if e = os.WriteFile(filepath.Join(a.Config.DataDir, "secrets", ref), raw, 0600); e != nil {
		t.Fatal(e)
	}
	if _, e = a.SecretGet(ref); e == nil {
		t.Fatal("accepted corrupted ciphertext")
	}
	if e = a.Bootstrap(context.Background(), "replace", "another-strong-password"); e == nil {
		t.Fatal("reset initialized instance")
	}
	cfg := a.Config
	if e = a.Close(); e != nil {
		t.Fatal(e)
	}
	db, e := sql.Open("sqlite", filepath.Join(cfg.DataDir, "management.db"))
	if e != nil {
		t.Fatal(e)
	}
	_, e = db.Exec("UPDATE schema_migrations SET checksum='tampered'")
	db.Close()
	if e != nil {
		t.Fatal(e)
	}
	if b, e := New(cfg); e == nil {
		b.Close()
		t.Fatal("accepted mutated migration")
	}
}
func TestCoreSnapshotPagination(t *testing.T) {
	h := newHarness(t)
	a := h.a
	ctx := context.Background()
	p, _ := a.principal(ctx, Str(h.object(h.do("GET", "/api/v1/auth/me", nil, nil)), "principalId"))
	page := func(path string, items []any) *httptest.ResponseRecorder {
		r := httptest.NewRequest("GET", path, nil)
		r = r.WithContext(context.WithValue(ctx, authKey{}, &authInfo{Principal: p}))
		w := httptest.NewRecorder()
		a.Page(w, r, items)
		return w
	}
	items := []any{map[string]any{"gatewayId": "a"}, map[string]any{"gatewayId": "b"}, map[string]any{"gatewayId": "c"}}
	first := page("/test?limit=1", items)
	cursor := Str(h.object(first), "nextCursor")
	changed := []any{map[string]any{"gatewayId": "new"}, items[1], items[2]}
	second := page("/test?limit=1&cursor="+cursor, changed)
	if Str(List(h.object(second), "items")[0].(map[string]any), "gatewayId") != "b" {
		t.Fatal("membership shifted")
	}
	h.expect(page("/different?cursor="+cursor, changed), 400, "ErrorResponse")
	for id, v := range a.pages {
		v.Expires = time.Now().Add(-time.Second)
		a.pages[id] = v
	}
	h.expect(page("/test?cursor="+cursor, changed), 400, "ErrorResponse")
}
