package platform

import (
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"strings"
)

// Cursor membership is bounded by the first page's rowid high-water mark.
// Resource state and authorization are read live on every page.
type pipeCursor struct {
	After string `json:"after"`
	Upper int64  `json:"upper"`
	At    int64  `json:"at"`
	Query string `json:"query"`
	Actor string `json:"actor"`
	Kind  string `json:"kind"`
}

func pipeQueryHash(r *http.Request) string {
	q := r.URL.Query()
	q.Del("cursor")
	q.Del("limit")
	return pipeHash([]string{r.URL.Path, q.Encode()})
}
func (a *App) readPipeCursor(w http.ResponseWriter, r *http.Request, p *Principal, kind, table string, db *sql.DB) (pipeCursor, bool) {
	c := pipeCursor{At: pnow(), Query: pipeQueryHash(r), Actor: p.ID, Kind: kind}
	raw := r.URL.Query().Get("cursor")
	if raw == "" {
		if err := db.QueryRowContext(r.Context(), "SELECT COALESCE(MAX(rowid),0) FROM "+table).Scan(&c.Upper); err != nil {
			pfail(w, r, 503, "STORE_UNAVAILABLE", "cannot establish query snapshot")
			return c, false
		}
		return c, true
	}
	fail := func() (pipeCursor, bool) {
		pfail(w, r, 400, "INVALID_CURSOR", "cursor is invalid, expired or belongs to a different query")
		return c, false
	}
	parts := strings.Split(raw, ".")
	if len(parts) != 2 || len(raw) > 4096 {
		return fail()
	}
	data, e := base64.RawURLEncoding.DecodeString(parts[0])
	if e != nil {
		return fail()
	}
	mac, e := base64.RawURLEncoding.DecodeString(parts[1])
	if e != nil {
		return fail()
	}
	h := hmac.New(sha256.New, a.master)
	h.Write(data)
	if !hmac.Equal(mac, h.Sum(nil)) {
		return fail()
	}
	var parsed pipeCursor
	if json.Unmarshal(data, &parsed) != nil || parsed.Actor != p.ID || parsed.Kind != kind || parsed.Query != c.Query || parsed.At > pnow() || parsed.At < pnow()-86400000 || parsed.Upper < 0 {
		return fail()
	}
	return parsed, true
}
func (a *App) cursorPage(items any, last string, c pipeCursor) pipeMap {
	page := pipeMap{"items": items, "snapshotAt": pts(c.At)}
	if last != "" {
		c.After = last
		raw, _ := json.Marshal(c)
		h := hmac.New(sha256.New, a.master)
		h.Write(raw)
		page["nextCursor"] = base64.RawURLEncoding.EncodeToString(raw) + "." + base64.RawURLEncoding.EncodeToString(h.Sum(nil))
	}
	return page
}
