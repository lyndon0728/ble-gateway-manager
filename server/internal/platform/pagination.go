package platform

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"
)

type pageSnapshot struct {
	Query, At string
	Keys      []string
	Expires   time.Time
}

func pageKey(v any) string {
	m, ok := v.(map[string]any)
	if !ok {
		return Hash(v)
	}
	ids := map[string]any{}
	for _, key := range []string{"principalId", "gatewayId", "scopeId", "credentialId", "taskId", "auditId", "bundleId", "targetId", "parserId", "eventId", "outputId", "deliveryId", "processingId", "operationId", "version", "namespaceId", "recordKey", "peerId", "ruleId", "connectionRef"} {
		if v, ok := m[key]; ok {
			ids[key] = v
		}
	}
	if len(ids) == 0 {
		return Hash(v)
	}
	return Hash(ids)
}
func (a *App) cursorSignature(raw string) string {
	mac := hmac.New(sha256.New, a.master)
	mac.Write([]byte("page:" + raw))
	return base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

// Membership is frozen for fifteen minutes; values and authorization are refreshed
// on each page. A restart invalidates cursors instead of silently moving offsets.
func (a *App) Page(w http.ResponseWriter, r *http.Request, items []any) {
	limit := 100
	if s := r.URL.Query().Get("limit"); s != "" {
		n, e := strconv.Atoi(s)
		if e != nil || n < 1 || n > 200 {
			Fail(w, r, 400, "INVALID_LIMIT", "limit must be 1..200")
			return
		}
		limit = n
	}
	if len(items) > 10000 {
		Fail(w, r, 422, "QUERY_TOO_BROAD", "narrow the query to at most 10000 records")
		return
	}
	q := r.URL.Query()
	q.Del("cursor")
	q.Del("limit")
	identity := ""
	if p := a.Auth(r); p != nil {
		identity = p.ID + fmt.Sprint(p.Revision) + encode(p.Bindings)
	}
	fingerprint := Hash(identity + r.URL.Path + "?" + q.Encode())
	offset := 0
	id := ""
	snapshot := pageSnapshot{Query: fingerprint, At: Timestamp(Now()), Expires: time.Now().Add(15 * time.Minute)}
	a.pageMu.Lock()
	defer a.pageMu.Unlock()
	for k, s := range a.pages {
		if time.Now().After(s.Expires) {
			delete(a.pages, k)
		}
	}
	if cursor := r.URL.Query().Get("cursor"); cursor != "" {
		parts := strings.Split(cursor, ".")
		if len(parts) != 3 || !hmac.Equal([]byte(parts[2]), []byte(a.cursorSignature(parts[0]+"."+parts[1]))) {
			Fail(w, r, 400, "INVALID_CURSOR", "invalid cursor signature")
			return
		}
		var e error
		offset, e = strconv.Atoi(parts[1])
		if e != nil || offset < 0 {
			Fail(w, r, 400, "INVALID_CURSOR", "invalid cursor position")
			return
		}
		id = parts[0]
		var ok bool
		snapshot, ok = a.pages[id]
		if !ok || snapshot.Query != fingerprint {
			Fail(w, r, 400, "CURSOR_EXPIRED", "cursor expired, authorization changed or query changed; start a new query")
			return
		}
		if offset > len(snapshot.Keys) {
			Fail(w, r, 400, "INVALID_CURSOR", "invalid cursor position")
			return
		}
	} else {
		for _, v := range items {
			snapshot.Keys = append(snapshot.Keys, pageKey(v))
		}
		id = ID("page")
		if len(a.pages) >= 32 {
			old := ""
			var at time.Time
			for k, v := range a.pages {
				if old == "" || v.Expires.Before(at) {
					old = k
					at = v.Expires
				}
			}
			delete(a.pages, old)
		}
		a.pages[id] = snapshot
	}
	current := make(map[string]any, len(items))
	for _, v := range items {
		current[pageKey(v)] = v
	}
	out := []any{}
	for offset < len(snapshot.Keys) && len(out) < limit {
		if v, ok := current[snapshot.Keys[offset]]; ok {
			out = append(out, v)
		}
		offset++
	}
	result := map[string]any{"items": out, "snapshotAt": snapshot.At}
	if offset < len(snapshot.Keys) {
		raw := id + "." + strconv.Itoa(offset)
		result["nextCursor"] = raw + "." + a.cursorSignature(raw)
	} else {
		delete(a.pages, id)
	}
	JSON(w, 200, result)
}
func (a *App) roleMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if a.Config.Role == "pipeline" && r.URL.Path != "/api/v1/gateway-ingress/batches" && !strings.HasPrefix(r.URL.Path, "/health/") {
			Fail(w, r, 404, "NOT_FOUND", "use the management listener")
			return
		}
		next.ServeHTTP(w, r)
	})
}
