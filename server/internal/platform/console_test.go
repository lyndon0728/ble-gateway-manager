package platform

import (
	"io/fs"
	"net/http/httptest"
	"os"
	"regexp"
	"strings"
	"testing"
)

func TestFrontendEmbeddedRoutesAndSecurity(t *testing.T) {
	// Vite emits helper chunks whose names start with an underscore. Go's
	// default directory embedding skips these unless the all: prefix is used.
	if err := fs.WalkDir(os.DirFS("assets/ui"), ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !entry.IsDir() {
			if _, err := Assets.ReadFile("assets/ui/" + name); err != nil {
				return err
			}
		}
		return nil
	}); err != nil {
		t.Fatalf("frontend build asset not embedded: %v", err)
	}
	h := newHarness(t)
	h.cookie = nil
	root := h.do("GET", "/", nil, nil)
	if root.Code != 307 || root.Header().Get("Location") != "/ui/" {
		t.Fatal("root must enter the console")
	}
	var index string
	for _, route := range []string{"/ui/", "/ui/login", "/ui/gateways/gw32_sim/configuration", "/ui/tasks/task_missing"} {
		r := h.do("GET", route, nil, nil)
		if r.Code != 200 || !strings.Contains(r.Header().Get("Content-Type"), "text/html") || !strings.Contains(r.Body.String(), `id="app"`) {
			t.Fatalf("SPA route unavailable: %s status %d", route, r.Code)
		}
		if r.Header().Get("Cache-Control") != "no-store" || !strings.Contains(r.Header().Get("Content-Security-Policy"), "script-src 'self';") {
			t.Fatal("HTML must be fresh and inline JavaScript prohibited")
		}
		index = r.Body.String()
	}
	asset := regexp.MustCompile(`src="(/ui/assets/[^"]+\.js)"`).FindStringSubmatch(index)
	if len(asset) != 2 {
		t.Fatal("built entry asset missing from HTML")
	}
	r := h.do("GET", asset[1], nil, nil)
	if r.Code != 200 || !strings.Contains(r.Header().Get("Content-Type"), "javascript") || !strings.Contains(r.Header().Get("Cache-Control"), "immutable") {
		t.Fatal("versioned asset unavailable or incorrectly cached")
	}
	for _, route := range []string{"/ui/assets/missing.js", "/ui/missing.css", "/ui/../openapi.json", "/ui/%2e%2e/openapi.json"} {
		if h.do("GET", route, nil, nil).Code != 404 {
			t.Fatalf("missing/path traversal must not fall back to HTML: %s", route)
		}
	}
	if h.do("GET", "/api/v1/gateways", nil, nil).Code != 401 {
		t.Fatal("public SPA shell must not make data public")
	}
	h.a.Config.Role = "pipeline"
	if h.do("GET", "/ui/", nil, nil).Code != 404 {
		t.Fatal("pipeline listener must not expose management UI")
	}
}

func TestFrontendSessionRestoresCSRFWithoutExposingCookie(t *testing.T) {
	h := newHarness(t)
	r := h.do("GET", "/api/v1/auth/me", nil, nil)
	if r.Header().Get("X-CSRF-Token") != h.csrf || strings.Contains(r.Body.String(), h.cookie.Value) {
		t.Fatal("session restoration must return only the CSRF proof")
	}
	h.csrf = r.Header().Get("X-CSRF-Token")
	h.expect(h.do("DELETE", "/api/v1/auth/sessions/current", nil, nil), 204, "")
	request := httptest.NewRequest("GET", "/api/v1/auth/me", nil)
	response := httptest.NewRecorder()
	h.handler.ServeHTTP(response, request)
	if response.Code != 401 || response.Header().Get("X-CSRF-Token") != "" {
		t.Fatal("anonymous caller must not get a CSRF token")
	}
}

func TestFrontendAuditNormalizesLegacyPipelineSuccess(t *testing.T) {
	h := newHarness(t)
	actor := Str(h.object(h.do("GET", "/api/v1/auth/me", nil, nil)), "principalId")
	_, err := h.a.Management.Exec("INSERT INTO audit_events VALUES(?,?,?,?,?,?,?,?,?,?)", "audit_frontend_contract", actor, "target.create", "pipeline", "target_contract", "succeeded", "trace_contract", "{}", Now(), Now()+86400000)
	if err != nil {
		t.Fatal(err)
	}
	response := h.do("GET", "/api/v1/audit-events", nil, nil)
	h.expect(response, 200, "AuditEventPage")
	if !strings.Contains(response.Body.String(), `"outcome":"SUCCESS"`) || strings.Contains(response.Body.String(), `"outcome":"SUCCEEDED"`) {
		t.Fatal("legacy audit result must conform to API enum")
	}
}
