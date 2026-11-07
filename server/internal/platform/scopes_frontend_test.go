package platform

import (
	"context"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
	"time"
)

// A single connection makes retaining a cursor during authorization fail
// deterministically instead of depending on production request concurrency.
func scopeReadSingleConnection(t *testing.T, a *App, actor, path string, handler http.HandlerFunc) *httptest.ResponseRecorder {
	t.Helper()
	p, e := a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	ctx, cancel := context.WithTimeout(t.Context(), 2*time.Second)
	defer cancel()
	r := httptest.NewRequest(http.MethodGet, path, nil)
	r = r.WithContext(context.WithValue(ctx, authKey{}, &authInfo{Principal: p}))
	w := httptest.NewRecorder()
	done := make(chan struct{})
	go func() { handler(w, r); close(done) }()
	select {
	case <-done:
		return w
	case <-ctx.Done():
		// Release the artificial pool bound so a regression cannot strand test cleanup.
		a.Management.SetMaxOpenConns(8)
		<-done
		t.Fatal("list handler failed to release its cursor before authorization")
		return w
	}
}

func TestScopeFrontendSingleConnectionAndVisibility(t *testing.T) {
	a, admin := controlFixture(t)
	frontendSQL(t, a, "INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,status,created_at_ms) VALUES('scope_hidden','group','scope_default_project','Hidden','active',?),('scope_archived','group','scope_default_project','Archived','archived',?)", Now(), Now())
	auditor := autoScopedPrincipal(t, a, "scope_auditor", "auditor", "scope_default_group")
	a.Management.SetMaxOpenConns(1)
	w := scopeReadSingleConnection(t, a, auditor, "/api/v1/resource-scopes", a.listScopes)
	if w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	m := pm(w.Body.String())
	if e := a.Validate("ResourceScopePage", m); e != nil {
		t.Fatal(e)
	}
	items := List(m, "items")
	if len(items) != 1 || Str(items[0].(map[string]any), "scopeId") != "scope_default_group" {
		t.Fatal("scope visibility changed", m)
	}
	seen := map[string]bool{}
	cursor := ""
	for {
		path := "/api/v1/resource-scopes?parentScopeId=scope_default_project&limit=1"
		if cursor != "" {
			path += "&cursor=" + url.QueryEscape(cursor)
		}
		w = scopeReadSingleConnection(t, a, admin, path, a.listScopes)
		if w.Code != 200 {
			t.Fatal(w.Code, w.Body.String())
		}
		m = pm(w.Body.String())
		for _, raw := range List(m, "items") {
			id := Str(raw.(map[string]any), "scopeId")
			if seen[id] {
				t.Fatal("duplicate scope across cursor pages")
			}
			seen[id] = true
		}
		cursor = Str(m, "nextCursor")
		if cursor == "" {
			break
		}
	}
	if len(seen) != 3 || !seen["scope_archived"] || !seen["scope_hidden"] {
		t.Fatal("instance reader lost archived scopes or parent filter", seen)
	}
}

func TestAuditFrontendSingleConnectionAndVisibility(t *testing.T) {
	a, admin := controlFixture(t)
	frontendSQL(t, a, "INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('scope_hidden','group','scope_default_project','Hidden',?)", Now())
	frontendSQL(t, a, "UPDATE gateways SET scope_id='scope_hidden' WHERE gateway_id='gw_other'")
	auditor := autoScopedPrincipal(t, a, "audit_scoped", "auditor", "scope_default_group")
	for _, item := range []struct{ id, kind, resource string }{{"audit_visible_gateway", "gateway", "gw_test"}, {"audit_hidden_gateway", "gateway", "gw_other"}, {"audit_visible_scope", "scope", "scope_default_group"}, {"audit_hidden_scope", "scope", "scope_hidden"}, {"audit_global_only", "principal", admin}} {
		frontendSQL(t, a, "INSERT INTO audit_events VALUES(?,?,'test.read',?,?,'succeeded','trace_test','{}',?,?)", item.id, admin, item.kind, item.resource, Now(), Now()+60000)
	}
	a.Management.SetMaxOpenConns(1)
	w := scopeReadSingleConnection(t, a, auditor, "/api/v1/audit-events", a.listAudit)
	if w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	m := pm(w.Body.String())
	if e := a.Validate("AuditEventPage", m); e != nil {
		t.Fatal(e)
	}
	items := List(m, "items")
	if len(items) != 2 {
		t.Fatal("audit scope leaked", m)
	}
	for _, raw := range items {
		item := raw.(map[string]any)
		if Str(item, "outcome") != "SUCCESS" || (Str(item, "auditId") != "audit_visible_gateway" && Str(item, "auditId") != "audit_visible_scope") {
			t.Fatal("audit visibility or historical outcome normalization changed", item)
		}
	}
	w = scopeReadSingleConnection(t, a, admin, "/api/v1/audit-events", a.listAudit)
	if w.Code != 200 || len(List(pm(w.Body.String()), "items")) != 6 {
		t.Fatal("instance audit reader lost global records", w.Code, w.Body.String())
	}
}

func TestScopeAuditFrontendRejectTruncatedCandidateSets(t *testing.T) {
	a, admin := controlFixture(t)
	auditor := autoScopedPrincipal(t, a, "broad_auditor", "auditor", "scope_default_group")
	frontendSQL(t, a, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10001) INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) SELECT 'scope_many_'||x,'group','scope_default_project','Many '||x,? FROM n", Now())
	frontendSQL(t, a, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<10001) INSERT INTO audit_events SELECT 'audit_many_'||x,?,'test.broad','principal',?,'success','trace_broad','{}',?,? FROM n", admin, admin, Now(), Now()+60000)
	a.Management.SetMaxOpenConns(1)
	for _, tc := range []struct {
		path string
		h    http.HandlerFunc
	}{{"/api/v1/resource-scopes", a.listScopes}, {"/api/v1/audit-events", a.listAudit}} {
		w := scopeReadSingleConnection(t, a, auditor, tc.path, tc.h)
		if w.Code != 422 || Str(Obj(pm(w.Body.String()), "error"), "code") != "QUERY_TOO_BROAD" {
			t.Fatal("filtered candidates silently truncated", tc.path, w.Code, w.Body.String())
		}
	}
	w := scopeReadSingleConnection(t, a, auditor, "/api/v1/resource-scopes?kind=INSTANCE", a.listScopes)
	if w.Code != 200 || len(List(pm(w.Body.String()), "items")) != 0 {
		t.Fatal("server filter did not narrow before limit", w.Code, w.Body.String())
	}
}
