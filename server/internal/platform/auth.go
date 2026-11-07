package platform

import (
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
	"golang.org/x/crypto/argon2"
)

type Binding struct {
	Role    string `json:"role"`
	ScopeID string `json:"scopeId"`
}
type Principal struct {
	ID, Type, Name, State string
	Revision              int64
	Bindings              []Binding
	Scopes                []string
}
type authKey struct{}
type authInfo struct {
	Principal           *Principal
	Token, CredentialID string
	Cookie              bool
}

var roleToDB = map[string]string{"ADMIN": "instance_admin", "DEVICE_ADMIN": "device_admin", "OPERATOR": "operator", "DEVELOPER": "developer", "AUDITOR": "auditor"}
var dbToRole = map[string]string{"instance_admin": "ADMIN", "device_admin": "DEVICE_ADMIN", "operator": "OPERATOR", "developer": "DEVELOPER", "auditor": "AUDITOR"}
var rolePermissions = map[string][]string{
	"ADMIN":        {"*"},
	"DEVICE_ADMIN": {"scope:read", "gateway:read", "gateway:write", "gateway:configure", "gateway:reboot", "diagnostics:run", "gatt:read", "gatt:manage", "gatt:execute", "firmware:deploy", "task:read", "task:create", "task:cancel", "artifact:read", "artifact:write", "operation:read", "system:read", "parser:read", "target:read"},
	"OPERATOR":     {"scope:read", "gateway:read", "gatt:read", "gateway:configure", "gateway:reboot", "diagnostics:run", "task:read", "task:create", "task:cancel", "data:read", "delivery:read", "delivery:retry", "operation:read", "system:read", "artifact:read"},
	"DEVELOPER":    {"scope:read", "gateway:read", "gatt:read", "task:read", "data:read", "data:raw", "data:reprocess", "delivery:read", "delivery:retry", "parser:read", "parser:test", "rule:read", "target:read", "operation:read"},
	"AUDITOR":      {"scope:read", "gateway:read", "gatt:read", "task:read", "audit:read", "operation:read", "system:read", "rule:read", "target:read", "parser:read"},
}

func TokenHash(token string) string {
	h := sha256.Sum256([]byte(token))
	return hex.EncodeToString(h[:])
}
func randomToken() string {
	b := make([]byte, 32)
	_, _ = rand.Read(b)
	return base64.RawURLEncoding.EncodeToString(b)
}
func passwordHash(password string) (string, error) {
	salt := make([]byte, 16)
	if _, e := rand.Read(salt); e != nil {
		return "", e
	}
	key := argon2.IDKey([]byte(password), salt, 2, 32*1024, 1, 32)
	return "argon2id-v1:" + base64.RawStdEncoding.EncodeToString(salt) + ":" + base64.RawStdEncoding.EncodeToString(key), nil
}
func passwordValid(password, hash string) bool {
	parts := strings.Split(hash, ":")
	if len(parts) != 3 || parts[0] != "argon2id-v1" {
		return false
	}
	salt, e := base64.RawStdEncoding.DecodeString(parts[1])
	if e != nil || len(salt) != 16 {
		return false
	}
	want, e := base64.RawStdEncoding.DecodeString(parts[2])
	if e != nil || len(want) != 32 {
		return false
	}
	got := argon2.IDKey([]byte(password), salt, 2, 32*1024, 1, 32)
	return subtle.ConstantTimeCompare(got, want) == 1
}
func (a *App) csrf(token string) string {
	mac := hmac.New(sha256.New, a.master)
	mac.Write([]byte("csrf:" + token))
	return base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}
func (p *Principal) View() map[string]any {
	return map[string]any{"principalId": p.ID, "type": p.Type, "name": p.Name, "roleBindings": p.Bindings, "scopes": p.Scopes, "state": p.State, "revision": p.Revision}
}
func (a *App) principal(ctx context.Context, id string) (*Principal, error) {
	return a.principalWithQuery(ctx, a.Management, id)
}

type authQuery interface {
	ctlQuery
	QueryContext(context.Context, string, ...any) (*sql.Rows, error)
}

func (a *App) principalWithQuery(ctx context.Context, q authQuery, id string) (*Principal, error) {
	p := &Principal{ID: id, Bindings: []Binding{}, Scopes: []string{}}
	var kind, state string
	e := q.QueryRowContext(ctx, "SELECT kind,name,status,auth_epoch FROM principals WHERE principal_id=?", id).Scan(&kind, &p.Name, &state, &p.Revision)
	if e != nil {
		return nil, e
	}
	p.Type = map[string]string{"human": "USER", "application": "APPLICATION", "service": "SERVICE"}[kind]
	p.State = strings.ToUpper(state)
	rows, e := q.QueryContext(ctx, "SELECT role,scope_id FROM role_bindings WHERE principal_id=? ORDER BY role,scope_id", id)
	if e != nil {
		return nil, e
	}
	defer rows.Close()
	seen := map[string]bool{}
	for rows.Next() {
		var role, scope string
		if e = rows.Scan(&role, &scope); e != nil {
			return nil, e
		}
		role = dbToRole[role]
		p.Bindings = append(p.Bindings, Binding{role, scope})
		for _, perm := range rolePermissions[role] {
			if !seen[perm] {
				p.Scopes = append(p.Scopes, perm)
				seen[perm] = true
			}
		}
	}
	return p, rows.Err()
}
func (a *App) authMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Cache-Control", "no-store")
		token := ""
		cookie := false
		if h := r.Header.Get("Authorization"); strings.HasPrefix(h, "Bearer ") {
			token = strings.TrimSpace(strings.TrimPrefix(h, "Bearer "))
		} else if c, e := r.Cookie("gw_session"); e == nil {
			token = c.Value
			cookie = true
		}
		if token != "" && len(token) <= 512 {
			var cid, pid, kind, meta string
			var expires sql.NullInt64
			e := a.Management.QueryRowContext(r.Context(), "SELECT credential_id,principal_id,kind,COALESCE(key_ref,''),expires_at_ms FROM credentials WHERE verifier=? AND principal_id IS NOT NULL AND status='active' AND not_before_ms<=? AND (expires_at_ms IS NULL OR expires_at_ms>?)", TokenHash(token), Now(), Now()).Scan(&cid, &pid, &kind, &meta, &expires)
			if e == nil && ((cookie && kind == "session") || (!cookie && kind == "api_key")) {
				p, e := a.principal(r.Context(), pid)
				if e == nil && p.State == "ACTIVE" {
					valid := true
					if cookie {
						var m map[string]any
						if json.Unmarshal([]byte(meta), &m) != nil || Num(m, "epoch") != p.Revision || Now()-Num(m, "lastSeen") > int64(30*time.Minute/time.Millisecond) {
							valid = false
						} else if Now()-Num(m, "lastSeen") > 60000 {
							m["lastSeen"] = Now()
							_, _ = a.Management.ExecContext(r.Context(), "UPDATE credentials SET key_ref=? WHERE credential_id=? AND key_ref=?", encode(m), cid, meta)
						}
					}
					if valid {
						r = r.WithContext(context.WithValue(r.Context(), authKey{}, &authInfo{p, token, cid, cookie}))
					}
				}
			}
		}
		if v, _ := r.Context().Value(authKey{}).(*authInfo); v != nil && v.Cookie && r.Method != "GET" && r.Method != "HEAD" && r.Method != "OPTIONS" {
			expected := a.csrf(v.Token)
			if !hmac.Equal([]byte(r.Header.Get("X-CSRF-Token")), []byte(expected)) || !a.sameOrigin(r) {
				Fail(w, r, 403, "CSRF_REJECTED", "invalid browser request origin or CSRF token")
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}
func (a *App) sameOrigin(r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return true
	}
	u, e := url.Parse(origin)
	if e != nil {
		return false
	}
	if a.Config.PublicURL != "" {
		p, e := url.Parse(a.Config.PublicURL)
		return e == nil && u.Scheme == p.Scheme && u.Host == p.Host
	}
	scheme := "https"
	if a.Config.AllowInsecureDev && r.TLS == nil {
		scheme = "http"
	}
	return u.Scheme == scheme && u.Host == r.Host
}
func (a *App) Auth(r *http.Request) *Principal {
	if v, _ := r.Context().Value(authKey{}).(*authInfo); v != nil {
		return v.Principal
	}
	return nil
}
func (a *App) Require(w http.ResponseWriter, r *http.Request, permission string) (*Principal, bool) {
	p := a.Auth(r)
	if p == nil {
		Fail(w, r, 401, "UNAUTHENTICATED", "valid user or application credentials required")
		return nil, false
	}
	if permission != "" && permission != "session:self" {
		allowed := false
		for _, b := range p.Bindings {
			for _, s := range rolePermissions[b.Role] {
				if s == "*" || s == permission {
					allowed = true
				}
			}
		}
		if !allowed {
			Fail(w, r, 403, "FORBIDDEN", "action not permitted")
			return nil, false
		}
	}
	return p, true
}
func (a *App) Can(p *Principal, permission, scopeID string) bool {
	return canQuery(context.Background(), a.Management, p, permission, scopeID)
}
func canQuery(ctx context.Context, q ctlQuery, p *Principal, permission, scopeID string) bool {
	if p == nil {
		return false
	}
	if scopeID == "" {
		scopeID = "scope_instance"
	}
	for _, b := range p.Bindings {
		permitted := false
		for _, s := range rolePermissions[b.Role] {
			if s == "*" || s == permission || permission == "" {
				permitted = true
			}
		}
		if !permitted {
			continue
		}
		var n int
		err := q.QueryRowContext(ctx, "WITH RECURSIVE ancestors(scope_id,parent_scope_id,status) AS (SELECT scope_id,parent_scope_id,status FROM resource_scopes WHERE scope_id=? UNION ALL SELECT s.scope_id,s.parent_scope_id,s.status FROM resource_scopes s JOIN ancestors a ON s.scope_id=a.parent_scope_id) SELECT count(*) FROM ancestors WHERE scope_id=? AND status='active' AND NOT EXISTS(SELECT 1 FROM ancestors WHERE status<>'active')", scopeID, b.ScopeID).Scan(&n)
		if err == nil && n > 0 {
			var active int
			if q.QueryRowContext(ctx, "SELECT count(*) FROM resource_scopes WHERE scope_id=? AND status='active'", scopeID).Scan(&active) == nil && active == 1 {
				return true
			}
		}
	}
	return false
}
func (a *App) CanGateway(p *Principal, permission, gatewayID string) bool {
	var scope string
	if a.Management.QueryRow("SELECT scope_id FROM gateways WHERE gateway_id=?", gatewayID).Scan(&scope) != nil {
		return false
	}
	return a.Can(p, permission, scope)
}
func (a *App) Gateway(r *http.Request) (string, bool) {
	hash := ""
	kind := "device_secret"
	if r.TLS != nil && len(r.TLS.VerifiedChains) > 0 && len(r.TLS.PeerCertificates) > 0 {
		h := sha256.Sum256(r.TLS.PeerCertificates[0].Raw)
		hash = hex.EncodeToString(h[:])
		kind = "certificate"
	} else if h := r.Header.Get("Authorization"); strings.HasPrefix(h, "Bearer ") {
		hash = TokenHash(strings.TrimPrefix(h, "Bearer "))
	}
	if hash == "" {
		return "", false
	}
	var id string
	e := a.Management.QueryRowContext(r.Context(), "SELECT c.gateway_id FROM credentials c JOIN gateways g ON g.gateway_id=c.gateway_id WHERE c.verifier=? AND c.kind=? AND c.status='active' AND c.not_before_ms<=? AND (c.expires_at_ms IS NULL OR c.expires_at_ms>?) AND g.lifecycle='active'", hash, kind, Now(), Now()).Scan(&id)
	return id, e == nil
}
func (a *App) RequireGateway(w http.ResponseWriter, r *http.Request, namespace, gateway string) bool {
	id, ok := a.Gateway(r)
	if !ok {
		Fail(w, r, 401, "GATEWAY_UNAUTHENTICATED", "valid gateway credentials required")
		return false
	}
	if id != gateway || namespace != a.Config.NamespaceID {
		Fail(w, r, 403, "GATEWAY_SCOPE_MISMATCH", "gateway identity does not match envelope")
		return false
	}
	return true
}

func (a *App) Bootstrap(ctx context.Context, name, password string) error {
	if len(name) < 1 || len(password) < 12 || len(password) > 256 {
		return errors.New("administrator name and 12-256 character password required")
	}
	hash, e := passwordHash(password)
	if e != nil {
		return e
	}
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	var n int
	if e = tx.QueryRow("SELECT count(*) FROM instance_settings").Scan(&n); e != nil {
		return e
	}
	if n > 0 {
		return Problem(409, "ALREADY_INITIALIZED", "instance already initialized")
	}
	now := Now()
	_, e = tx.Exec("INSERT INTO instance_settings VALUES(?,1,?,'B',?,'master.key',1,?,?)", a.Config.InstanceID, "BLE Gateway Manager", encode(map[string]any{"namespaceId": a.Config.NamespaceID}), now, now)
	if e != nil {
		return e
	}
	for _, v := range [][4]string{{"scope_instance", "instance", "", "Customer instance"}, {"scope_default_project", "project", "scope_instance", "Default project"}, {"scope_default_group", "group", "scope_default_project", "Default gateways"}} {
		var parent any = v[2]
		if v[2] == "" {
			parent = nil
		}
		if _, e = tx.Exec("INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,status,version,created_at_ms) VALUES(?,?,?,?,'active',1,?)", v[0], v[1], parent, v[3], now); e != nil {
			return e
		}
	}
	pid := ID("principal")
	_, e = tx.Exec("INSERT INTO principals VALUES(?,'human',?,'active',1,?,?)", pid, name, now, now)
	if e != nil {
		return e
	}
	_, e = tx.Exec("INSERT INTO role_bindings VALUES(?,?,'instance_admin','scope_instance',?)", ID("binding"), pid, now)
	if e != nil {
		return e
	}
	_, e = tx.Exec("INSERT INTO credentials(credential_id,label,principal_id,kind,verifier,status,not_before_ms,created_at_ms) VALUES(?,'Administrator password',?,'password',?,'active',?,?)", ID("credential"), pid, hash, now, now)
	if e != nil {
		return e
	}
	if e = a.Audit(ctx, tx, pid, "bootstrap", "instance", a.Config.InstanceID, map[string]any{"namespace": a.Config.NamespaceID}); e != nil {
		return e
	}
	return tx.Commit()
}
func (a *App) ProvisionGateway(ctx context.Context, gatewayID, secret string) error {
	if gatewayID == "" || len(secret) < 32 {
		return errors.New("gateway ID and a random token of at least 32 characters required")
	}
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return e
	}
	defer tx.Rollback()
	var boundElsewhere int
	if e = tx.QueryRowContext(ctx, "SELECT count(*) FROM credentials WHERE kind='device_secret' AND verifier=? AND gateway_id<>? AND status='active'", TokenHash(secret), gatewayID).Scan(&boundElsewhere); e != nil {
		return e
	}
	if boundElsewhere > 0 {
		return Problem(409, "DEVICE_CREDENTIAL_CONFLICT", "device credential is already bound to another gateway")
	}
	now := Now()
	_, e = tx.Exec("INSERT INTO gateways(gateway_id,serial_number,display_name,scope_id,model,hardware_version,firmware_version,lifecycle,capabilities_json,version,created_at_ms) VALUES(?,?,?,'scope_default_group','REFERENCE_GW','reference','reference-1','active',?,1,?) ON CONFLICT(gateway_id) DO NOTHING", gatewayID, gatewayID, "Reference gateway", `["CONFIGURE","REBOOT","DIAGNOSTICS"]`, now)
	if e != nil {
		return e
	}
	_, e = tx.Exec("UPDATE credentials SET status='revoked',revoked_at_ms=? WHERE gateway_id=? AND kind='device_secret'", now, gatewayID)
	if e != nil {
		return e
	}
	_, e = tx.Exec("INSERT INTO credentials(credential_id,label,gateway_id,kind,verifier,status,not_before_ms,created_at_ms) VALUES(?,'Local provisioning',?,'device_secret',?,'active',?,?)", ID("credential"), gatewayID, TokenHash(secret), now, now)
	if e != nil {
		return e
	}
	var actor string
	if e = tx.QueryRow("SELECT p.principal_id FROM principals p JOIN role_bindings b ON b.principal_id=p.principal_id WHERE p.status='active' AND b.role='instance_admin' LIMIT 1").Scan(&actor); e != nil {
		return e
	}
	_, e = tx.Exec("INSERT OR IGNORE INTO gateway_configs(config_id,gateway_id,revision,desired_json,content_hash,state,created_by,created_at_ms) VALUES(?,?,1,'{}',?,'desired',?,?)", ID("config"), gatewayID, Hash(map[string]any{}), actor, Now())
	if e != nil {
		return e
	}
	return tx.Commit()
}

func (a *App) RegisterCore(r chi.Router) {
	r.Post("/api/v1/auth/sessions", a.login)
	r.Delete("/api/v1/auth/sessions/current", a.logout)
	r.Get("/api/v1/auth/me", func(w http.ResponseWriter, r *http.Request) {
		if p, ok := a.Require(w, r, ""); ok {
			JSON(w, 200, p.View())
		}
	})
	r.Get("/api/v1/principals", a.listPrincipals)
	r.Post("/api/v1/principals", a.createPrincipal)
	r.Patch("/api/v1/principals/{principalId}", a.patchPrincipal)
	r.Get("/api/v1/principals/{principalId}/credentials", a.listCredentials)
	r.Post("/api/v1/principals/{principalId}/credentials", a.createCredential)
	r.Delete("/api/v1/principals/{principalId}/credentials/{credentialId}", a.revokeCredential)
	r.Get("/api/v1/resource-scopes", a.listScopes)
	r.Post("/api/v1/resource-scopes", a.createScope)
	r.Patch("/api/v1/resource-scopes/{scopeId}", a.patchScope)
	r.Get("/api/v1/audit-events", a.listAudit)
	r.Get("/api/v1/system/status", a.systemStatus)
}
func (a *App) login(w http.ResponseWriter, r *http.Request) {
	select {
	case a.loginSlots <- struct{}{}:
		defer func() { <-a.loginSlots }()
	default:
		Fail(w, r, 429, "LOGIN_BUSY", "login capacity reached; retry later")
		return
	}
	if !a.sameOrigin(r) {
		Fail(w, r, 403, "ORIGIN_REJECTED", "browser origin rejected")
		return
	}
	m, ok := a.Decode(w, r, "Login")
	if !ok {
		return
	}
	name := Str(m, "username")
	if name == "" {
		name = Str(m, "name")
	}
	a.loginMu.Lock()
	if time.Since(a.globalLogins.At) > time.Second {
		a.globalLogins = loginBucket{At: time.Now()}
	}
	a.globalLogins.Count++
	if a.globalLogins.Count > 40 {
		a.loginMu.Unlock()
		Fail(w, r, 429, "LOGIN_RATE_LIMIT", "login rate exceeded; retry later")
		return
	}
	b, tracked := a.loginBuckets[name]
	if !tracked && len(a.loginBuckets) >= 10000 {
		for k, v := range a.loginBuckets {
			if time.Since(v.At) > time.Minute {
				delete(a.loginBuckets, k)
			}
		}
		if len(a.loginBuckets) >= 10000 {
			a.loginMu.Unlock()
			Fail(w, r, 429, "LOGIN_BUSY", "login tracking capacity reached; retry later")
			return
		}
	}
	if time.Since(b.At) > time.Minute {
		b = loginBucket{At: time.Now()}
	}
	b.Count++
	a.loginBuckets[name] = b
	a.loginMu.Unlock()
	if b.Count > 10 {
		Fail(w, r, 429, "LOGIN_RATE_LIMIT", "too many login attempts")
		return
	}
	var pid, hash string
	e := a.Management.QueryRowContext(r.Context(), "SELECT p.principal_id,c.verifier FROM principals p JOIN credentials c ON c.principal_id=p.principal_id WHERE p.name=? AND p.kind='human' AND p.status='active' AND c.kind='password' AND c.status='active'", name).Scan(&pid, &hash)
	if e != nil || !passwordValid(Str(m, "password"), hash) {
		Fail(w, r, 401, "INVALID_CREDENTIALS", "invalid credentials")
		return
	}
	p, e := a.principal(r.Context(), pid)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	token := randomToken()
	expires := Now() + int64(8*time.Hour/time.Millisecond)
	_, e = a.Management.ExecContext(r.Context(), "INSERT INTO credentials(credential_id,label,principal_id,kind,verifier,key_ref,status,not_before_ms,expires_at_ms,created_at_ms) VALUES(?,'Browser session',?,'session',?,?,'active',?,?,?)", ID("session"), pid, TokenHash(token), encode(map[string]any{"epoch": p.Revision, "lastSeen": Now()}), Now(), expires, Now())
	if e != nil {
		RespondError(w, r, e)
		return
	}
	http.SetCookie(w, &http.Cookie{Name: "gw_session", Value: token, Path: "/", HttpOnly: true, Secure: !a.Config.AllowInsecureDev, SameSite: http.SameSiteStrictMode, Expires: time.UnixMilli(expires)})
	JSON(w, 200, map[string]any{"principal": p.View(), "expiresAt": Timestamp(expires), "csrfToken": a.csrf(token)})
}
func (a *App) logout(w http.ResponseWriter, r *http.Request) {
	v, _ := r.Context().Value(authKey{}).(*authInfo)
	if v == nil || !v.Cookie {
		Fail(w, r, 401, "SESSION_REQUIRED", "browser session required")
		return
	}
	_, e := a.Management.ExecContext(r.Context(), "UPDATE credentials SET status='revoked',revoked_at_ms=? WHERE credential_id=?", Now(), v.CredentialID)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	http.SetCookie(w, &http.Cookie{Name: "gw_session", Value: "", Path: "/", MaxAge: -1, HttpOnly: true, Secure: !a.Config.AllowInsecureDev, SameSite: http.SameSiteStrictMode})
	JSON(w, 204, nil)
}
func (a *App) requireAdmin(w http.ResponseWriter, r *http.Request) (*Principal, bool) {
	p, ok := a.Require(w, r, "identity:write")
	if !ok {
		return nil, false
	}
	if !a.Can(p, "identity:write", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "instance administrator required")
		return nil, false
	}
	return p, true
}
