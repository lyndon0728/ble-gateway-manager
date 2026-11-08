package platform

import (
	"context"
	"strings"
	"testing"
)

func TestLocalDefaultAdministratorBootstrapIsExplicit(t *testing.T) {
	a, err := New(Config{DataDir: t.TempDir(), AllowInsecureDev: true})
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	if a.Bootstrap(context.Background(), "admin", "123456") == nil {
		t.Fatal("ordinary bootstrap accepted weak password")
	}
	a.Config.AllowInsecureDev = false
	if a.BootstrapLocalDefaultAdmin(context.Background()) == nil {
		t.Fatal("local default accepted outside development mode")
	}
	a.Config.AllowInsecureDev = true
	if err = a.BootstrapLocalDefaultAdmin(context.Background()); err != nil {
		t.Fatal(err)
	}
	var name, verifier string
	if err = a.Management.QueryRow("SELECT p.name,c.verifier FROM principals p JOIN credentials c ON c.principal_id=p.principal_id WHERE c.kind='password' AND c.status='active'").Scan(&name, &verifier); err != nil {
		t.Fatal(err)
	}
	if name != "admin" || !strings.HasPrefix(verifier, "argon2id-v1:") || !passwordValid("123456", verifier) {
		t.Fatal("local administrator was not stored as an Argon2 hash")
	}
	if a.BootstrapLocalDefaultAdmin(context.Background()) == nil {
		t.Fatal("bootstrap overwrote initialized instance")
	}
}

func TestAdministratorPasswordResetRevokesOnlyTargetSessions(t *testing.T) {
	h := newHarness(t)
	adminID := Str(h.object(h.do("GET", "/api/v1/auth/me", nil, nil)), "principalId")
	created := h.do("POST", "/api/v1/principals", map[string]any{"type": "USER", "name": "other_operator", "password": "other-user-strong-password", "roleBindings": []any{map[string]any{"role": "OPERATOR", "scopeId": "scope_default_group"}}}, map[string]string{"Idempotency-Key": ID("idem")})
	h.expect(created, 201, "Principal")
	otherID := Str(h.object(created), "principalId")
	var otherHash string
	if err := h.a.Management.QueryRow("SELECT verifier FROM credentials WHERE principal_id=? AND kind='password' AND status='active'", otherID).Scan(&otherHash); err != nil {
		t.Fatal(err)
	}
	if err := ResetAdministratorPassword(context.Background(), h.a.Management, h.a.Config.NamespaceID, "admin", "123456", true); err != nil {
		t.Fatal(err)
	}
	h.expect(h.do("GET", "/api/v1/auth/me", nil, nil), 401, "ErrorResponse")
	var oldActive, epoch int
	if err := h.a.Management.QueryRow("SELECT count(*) FROM credentials WHERE principal_id=? AND kind='session' AND status='active'", adminID).Scan(&oldActive); err != nil {
		t.Fatal(err)
	}
	if oldActive != 0 {
		t.Fatal("old browser session remains active")
	}
	if err := h.a.Management.QueryRow("SELECT auth_epoch FROM principals WHERE principal_id=?", adminID).Scan(&epoch); err != nil || epoch != 2 {
		t.Fatalf("auth epoch not incremented: %v", err)
	}
	h.expect(h.do("POST", "/api/v1/auth/sessions", map[string]any{"name": "admin", "password": "a-strong-test-password"}, nil), 401, "ErrorResponse")
	h.expect(h.do("POST", "/api/v1/auth/sessions", map[string]any{"name": "admin", "password": "123456"}, nil), 200, "Session")
	h.expect(h.do("POST", "/api/v1/auth/sessions", map[string]any{"name": "other_operator", "password": "other-user-strong-password"}, nil), 200, "Session")
	var afterHash, details string
	if err := h.a.Management.QueryRow("SELECT verifier FROM credentials WHERE principal_id=? AND kind='password' AND status='active'", otherID).Scan(&afterHash); err != nil {
		t.Fatal(err)
	}
	if afterHash != otherHash {
		t.Fatal("another user's credential changed")
	}
	if err := h.a.Management.QueryRow("SELECT detail_json FROM audit_events WHERE action='administrator.password.reset'").Scan(&details); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(details, "123456") || strings.Contains(details, "a-strong-test-password") {
		t.Fatal("password appeared in audit evidence")
	}
	if ResetAdministratorPassword(context.Background(), h.a.Management, h.a.Config.NamespaceID, "other_operator", "123456", true) == nil {
		t.Fatal("local-default reset accepted another user")
	}
	if ResetAdministratorPassword(context.Background(), h.a.Management, h.a.Config.NamespaceID, "other_operator", "new-strong-password", false) == nil {
		t.Fatal("non-administrator reset was accepted")
	}
	if ResetAdministratorPassword(context.Background(), h.a.Management, h.a.Config.NamespaceID, "admin", "123456", false) == nil {
		t.Fatal("ordinary reset accepted weak password")
	}
}
