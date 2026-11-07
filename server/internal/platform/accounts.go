package platform

import (
	"context"
	"database/sql"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

func bindings(m map[string]any) []Binding {
	out := []Binding{}
	for _, v := range List(m, "roleBindings") {
		b, _ := v.(map[string]any)
		out = append(out, Binding{Str(b, "role"), Str(b, "scopeId")})
	}
	return out
}
func validateBindings(ctx context.Context, tx *sql.Tx, bs []Binding) error {
	for _, b := range bs {
		var kind, state string
		if e := tx.QueryRowContext(ctx, "SELECT kind,status FROM resource_scopes WHERE scope_id=?", b.ScopeID).Scan(&kind, &state); e != nil || state != "active" {
			return Problem(422, "INVALID_ROLE_SCOPE", "role scope must exist and be active")
		}
		if b.Role == "ADMIN" && kind != "instance" {
			return Problem(422, "INVALID_ADMIN_SCOPE", "administrator requires instance scope")
		}
		if roleToDB[b.Role] == "" {
			return Problem(422, "INVALID_ROLE", "unknown role")
		}
	}
	return nil
}
func replaceBindings(ctx context.Context, tx *sql.Tx, id string, bs []Binding) error {
	if e := validateBindings(ctx, tx, bs); e != nil {
		return e
	}
	if _, e := tx.ExecContext(ctx, "DELETE FROM role_bindings WHERE principal_id=?", id); e != nil {
		return e
	}
	for _, b := range bs {
		if _, e := tx.ExecContext(ctx, "INSERT INTO role_bindings VALUES(?,?,?,?,?)", ID("binding"), id, roleToDB[b.Role], b.ScopeID, Now()); e != nil {
			return e
		}
	}
	return nil
}
func principalView(id, kind, name, state string, revision int64, bs []Binding) map[string]any {
	p := &Principal{ID: id, Type: kind, Name: name, State: state, Revision: revision, Bindings: bs, Scopes: []string{}}
	seen := map[string]bool{}
	for _, b := range bs {
		for _, s := range rolePermissions[b.Role] {
			if !seen[s] {
				p.Scopes = append(p.Scopes, s)
				seen[s] = true
			}
		}
	}
	return p.View()
}
func (a *App) listPrincipals(w http.ResponseWriter, r *http.Request) {
	if _, ok := a.requireAdmin(w, r); !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT principal_id FROM principals ORDER BY created_at_ms,principal_id LIMIT 10001")
	if e != nil {
		RespondError(w, r, e)
		return
	}
	ids := []string{}
	for rows.Next() {
		var id string
		if e = rows.Scan(&id); e != nil {
			break
		}
		ids = append(ids, id)
	}
	e = errors.Join(e, rows.Err(), rows.Close())
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for _, id := range ids {
		p, e := a.principal(r.Context(), id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, p.View())
	}
	a.Page(w, r, items)
}
func (a *App) createPrincipal(w http.ResponseWriter, r *http.Request) {
	p, ok := a.requireAdmin(w, r)
	if !ok {
		return
	}
	m, ok := a.Decode(w, r, "PrincipalCreate")
	if !ok {
		return
	}
	pass := ""
	var e error
	if Str(m, "type") == "USER" {
		pass, e = passwordHash(Str(m, "password"))
		if e != nil {
			RespondError(w, r, e)
			return
		}
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "principal.create", r.Header.Get("Idempotency-Key"), m, func(tx *sql.Tx) (int, string, any, error) {
		id := ID("principal")
		kind := map[string]string{"USER": "human", "APPLICATION": "application"}[Str(m, "type")]
		var n int
		if e := tx.QueryRow("SELECT count(*) FROM principals WHERE name=?", Str(m, "name")).Scan(&n); e != nil {
			return 0, "", nil, e
		}
		if n > 0 {
			return 0, "", nil, Problem(409, "NAME_EXISTS", "principal name already exists")
		}
		if _, e := tx.Exec("INSERT INTO principals VALUES(?,?,?,'active',1,?,?)", id, kind, Str(m, "name"), Now(), Now()); e != nil {
			return 0, "", nil, e
		}
		bs := bindings(m)
		if e := replaceBindings(r.Context(), tx, id, bs); e != nil {
			return 0, "", nil, e
		}
		if kind == "human" {
			if _, e := tx.Exec("INSERT INTO credentials(credential_id,label,principal_id,kind,verifier,status,not_before_ms,created_at_ms) VALUES(?,'User password',?,'password',?,'active',?,?)", ID("credential"), id, pass, Now(), Now()); e != nil {
				return 0, "", nil, e
			}
		}
		if e := a.Audit(r.Context(), tx, p.ID, "principal.create", "principal", id, map[string]any{"type": Str(m, "type"), "roleBindings": bs}); e != nil {
			return 0, "", nil, e
		}
		return 201, id, principalView(id, Str(m, "type"), Str(m, "name"), "ACTIVE", 1, bs), nil
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(1))
	JSON(w, status, v)
}
func (a *App) patchPrincipal(w http.ResponseWriter, r *http.Request) {
	actor, ok := a.requireAdmin(w, r)
	if !ok {
		return
	}
	m, ok := a.Decode(w, r, "PrincipalPatch")
	if !ok {
		return
	}
	id := chi.URLParam(r, "principalId")
	hash := ""
	var e error
	if password := Str(m, "password"); password != "" {
		hash, e = passwordHash(password)
		if e != nil {
			RespondError(w, r, e)
			return
		}
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	var kind, name, state string
	var revision int64
	e = tx.QueryRow("SELECT kind,name,status,auth_epoch FROM principals WHERE principal_id=?", id).Scan(&kind, &name, &state, &revision)
	if e != nil {
		Fail(w, r, 404, "NOT_FOUND", "principal not found")
		return
	}
	if e = Match(r, revision); e != nil {
		RespondError(w, r, e)
		return
	}
	if kind == "service" {
		Fail(w, r, 409, "MANAGED_PRINCIPAL", "installer manages service identities")
		return
	}
	if hash != "" && kind != "human" {
		Fail(w, r, 422, "PASSWORD_NOT_APPLICABLE", "application accounts use tokens")
		return
	}
	if v := Str(m, "state"); v != "" {
		state = strings.ToLower(v)
	}
	if _, present := m["roleBindings"]; present {
		if e = replaceBindings(r.Context(), tx, id, bindings(m)); e != nil {
			RespondError(w, r, e)
			return
		}
	}
	if _, e = tx.Exec("UPDATE principals SET status=?,auth_epoch=auth_epoch+1,updated_at_ms=? WHERE principal_id=?", state, Now(), id); e != nil {
		RespondError(w, r, e)
		return
	}
	var admins int
	e = tx.QueryRow("SELECT count(DISTINCT p.principal_id) FROM principals p JOIN role_bindings b ON b.principal_id=p.principal_id WHERE p.kind='human' AND p.status='active' AND b.role='instance_admin' AND b.scope_id='scope_instance'").Scan(&admins)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if admins == 0 {
		Fail(w, r, 409, "LAST_ADMIN", "at least one active human administrator is required")
		return
	}
	if hash != "" {
		if _, e = tx.Exec("UPDATE credentials SET status='revoked',revoked_at_ms=? WHERE principal_id=? AND kind='password' AND status='active'", Now(), id); e == nil {
			_, e = tx.Exec("INSERT INTO credentials(credential_id,label,principal_id,kind,verifier,status,not_before_ms,created_at_ms) VALUES(?,'User password',?,'password',?,'active',?,?)", ID("credential"), id, hash, Now(), Now())
		}
		if e != nil {
			RespondError(w, r, e)
			return
		}
	}
	changes := map[string]any{"state": strings.ToUpper(state), "revision": revision + 1, "passwordChanged": hash != ""}
	if e = a.Audit(r.Context(), tx, actor.ID, "principal.update", "principal", id, changes); e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	p, e := a.principal(r.Context(), id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", ETag(p.Revision))
	JSON(w, 200, p.View())
}
func (a *App) listCredentials(w http.ResponseWriter, r *http.Request) {
	if _, ok := a.requireAdmin(w, r); !ok {
		return
	}
	id := chi.URLParam(r, "principalId")
	rows, e := a.Management.QueryContext(r.Context(), "SELECT credential_id,label,status,expires_at_ms FROM credentials WHERE principal_id=? AND kind='api_key' ORDER BY created_at_ms,credential_id LIMIT 10001", id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var cid, label, state string
		var expires int64
		if e = rows.Scan(&cid, &label, &state, &expires); e != nil {
			RespondError(w, r, e)
			return
		}
		if state == "active" && expires <= Now() {
			state = "expired"
		}
		items = append(items, map[string]any{"credentialId": cid, "principalId": id, "label": label, "state": strings.ToUpper(state), "expiresAt": Timestamp(expires)})
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	a.Page(w, r, items)
}
func (a *App) createCredential(w http.ResponseWriter, r *http.Request) {
	actor, ok := a.requireAdmin(w, r)
	if !ok {
		return
	}
	m, ok := a.Decode(w, r, "CredentialCreate")
	if !ok {
		return
	}
	expires, e := time.Parse(time.RFC3339Nano, Str(m, "expiresAt"))
	if e != nil || !expires.After(time.Now()) || expires.After(time.Now().Add(366*24*time.Hour)) {
		Fail(w, r, 422, "INVALID_EXPIRY", "token expiry must be in the next 366 days")
		return
	}
	id := chi.URLParam(r, "principalId")
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	var kind, state string
	e = tx.QueryRow("SELECT kind,status FROM principals WHERE principal_id=?", id).Scan(&kind, &state)
	if e != nil {
		Fail(w, r, 404, "NOT_FOUND", "principal not found")
		return
	}
	if kind != "application" || state != "active" {
		Fail(w, r, 409, "CREDENTIAL_NOT_APPLICABLE", "active application account required")
		return
	}
	token := randomToken()
	cid := ID("credential")
	_, e = tx.Exec("INSERT INTO credentials(credential_id,label,principal_id,kind,verifier,status,not_before_ms,expires_at_ms,created_at_ms) VALUES(?,?,?,'api_key',?,'active',?,?,?)", cid, Str(m, "label"), id, TokenHash(token), Now(), expires.UnixMilli(), Now())
	if e == nil {
		e = a.Audit(r.Context(), tx, actor.ID, "credential.create", "principal", id, map[string]any{"credentialId": cid})
	}
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 201, map[string]any{"credentialId": cid, "token": token, "expiresAt": Timestamp(expires.UnixMilli())})
}
func (a *App) revokeCredential(w http.ResponseWriter, r *http.Request) {
	actor, ok := a.requireAdmin(w, r)
	if !ok {
		return
	}
	id := chi.URLParam(r, "principalId")
	cid := chi.URLParam(r, "credentialId")
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	result, e := tx.Exec("UPDATE credentials SET status='revoked',revoked_at_ms=? WHERE principal_id=? AND credential_id=? AND kind='api_key'", Now(), id, cid)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	n, _ := result.RowsAffected()
	if n == 0 {
		Fail(w, r, 404, "NOT_FOUND", "application credential not found")
		return
	}
	if e = a.Audit(r.Context(), tx, actor.ID, "credential.revoke", "principal", id, map[string]any{"credentialId": cid}); e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 204, nil)
}
