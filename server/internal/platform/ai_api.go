package platform

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"

	"github.com/go-chi/chi/v5"
)

func (a *App) RegisterAI(r chi.Router) {
	r.Get("/api/v1/ai/status", a.aiStatus)
	r.Get("/api/v1/ai/providers", a.aiListProviders)
	r.Post("/api/v1/ai/providers", a.aiCreateProvider)
	r.Patch("/api/v1/ai/providers/{providerId}", a.aiPatchProvider)
	r.Post("/api/v1/ai/providers/{providerId}/checks", a.aiCheckProvider)
	r.Post("/api/v1/source-documents", a.aiUploadDocument)
	r.Get("/api/v1/source-documents/{documentId}/evidence", a.aiDocumentEvidence)
	r.Get("/api/v1/source-documents/{documentId}/content", a.aiDocumentContent)
	r.Get("/api/v1/ai/import-jobs", a.aiListJobs)
	r.Post("/api/v1/ai/import-jobs", a.aiCreateJob)
	r.Get("/api/v1/ai/import-jobs/{jobId}", a.aiGetJob)
	r.Post("/api/v1/ai/import-jobs/{jobId}/cancellation", a.aiCancelJob)
	r.Post("/api/v1/ai/import-jobs/{jobId}/retries", a.aiRetryJob)
	r.Get("/api/v1/protocol-drafts/{draftId}", a.aiGetDraft)
	r.Patch("/api/v1/protocol-drafts/{draftId}", a.aiPatchDraft)
	r.Post("/api/v1/protocol-drafts/{draftId}/validations", a.aiValidateDraft)
	r.Post("/api/v1/protocol-drafts/{draftId}/publications", a.aiPublishDraft)
	r.Get("/api/v1/device-models", a.aiListModels)
	r.Get("/api/v1/device-models/{modelId}", a.aiGetModel)
	r.Post("/api/v1/ai/apply-plans", a.aiCreatePlan)
	r.Get("/api/v1/ai/apply-plans/{planId}", a.aiGetPlan)
	r.Post("/api/v1/ai/apply-plans/{planId}/executions", a.aiConfirmPlan)
}

func aiDecode(w http.ResponseWriter, r *http.Request, allowed ...string) (map[string]any, bool) {
	r.Body = http.MaxBytesReader(w, r.Body, 2<<20)
	d := json.NewDecoder(r.Body)
	var m map[string]any
	if e := d.Decode(&m); e != nil || m == nil {
		Fail(w, r, 400, "INVALID_JSON", "a JSON object is required")
		return nil, false
	}
	if e := d.Decode(new(any)); e != io.EOF {
		Fail(w, r, 400, "INVALID_JSON", "only one JSON object is allowed")
		return nil, false
	}
	for k := range m {
		found := false
		for _, key := range allowed {
			if k == key {
				found = true
				break
			}
		}
		if !found {
			Fail(w, r, 422, "UNKNOWN_FIELD", "unsupported field: "+k)
			return nil, false
		}
		validType := true
		switch k {
		case "scopeId", "taskKind", "providerId", "name", "kind", "mode", "baseUrl", "model", "schemaMode", "tokenParameter", "apiKey", "validationId", "version", "modelId":
			_, validType = m[k].(string)
		case "useModel", "enabled", "allowExternal", "allowInsecureLocal", "vision", "confirmed":
			_, validType = m[k].(bool)
		case "sourceContext", "content":
			_, validType = m[k].(map[string]any)
		case "documentIds":
			_, validType = m[k].([]any)
		case "draftRevision", "timeoutSeconds", "maxOutputTokens", "dailyRequestLimit":
			f, ok := m[k].(float64)
			validType = ok && f == float64(int64(f)) && f >= 0
		}
		if !validType {
			Fail(w, r, 422, "INVALID_FIELD_TYPE", "invalid type for "+k)
			return nil, false
		}
	}
	return m, true
}
func aiBool(m map[string]any, k string) bool   { b, _ := m[k].(bool); return b }
func aiParam(r *http.Request, k string) string { return chi.URLParam(r, k) }
func aiStringList(v any) []string {
	out := []string{}
	for _, x := range anySlice(v) {
		if s, ok := x.(string); ok {
			out = append(out, s)
		}
	}
	return out
}
func anySlice(v any) []any {
	if x, ok := v.([]any); ok {
		return x
	}
	if x, ok := v.([]string); ok {
		a := []any{}
		for _, s := range x {
			a = append(a, s)
		}
		return a
	}
	return []any{}
}
func aiMap(raw string) map[string]any {
	m := map[string]any{}
	_ = json.Unmarshal([]byte(raw), &m)
	return m
}
func aiList(raw string) []any { v := []any{}; _ = json.Unmarshal([]byte(raw), &v); return v }
func aiNotFound(e error, code string) error {
	if errors.Is(e, sql.ErrNoRows) {
		return Problem(404, code, "resource not found in the authorized scope")
	}
	return e
}
func (a *App) aiAdmin(w http.ResponseWriter, r *http.Request) (*Principal, bool) {
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
func (a *App) aiMutation(w http.ResponseWriter, r *http.Request, p *Principal, op string, body any, fn func(*sql.Tx) (int, string, any, error)) {
	if e := a.aiMutationAccess(r.Context(), p.ID, op, body); e != nil {
		RespondError(w, r, e)
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, op, r.Header.Get("Idempotency-Key"), body, fn)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	// Cached successful mutations still require current access to their resource.
	// Reload after completion as well, so revocation during work cannot disclose
	// a previously authorized document-bearing response.
	if e = a.aiMutationAccess(r.Context(), p.ID, op, body); e != nil {
		RespondError(w, r, e)
		return
	}
	if m, ok := v.(map[string]any); ok {
		if rev := Num(m, "revision"); rev > 0 {
			w.Header().Set("ETag", ETag(rev))
		}
	}
	JSON(w, status, v)
}

func (a *App) aiMutationAccess(ctx context.Context, actor, op string, body any) error {
	action, id, _ := strings.Cut(op, ":")
	if !strings.HasPrefix(action, "ai.draft.") && !strings.HasPrefix(action, "ai.job.") && !strings.HasPrefix(action, "ai.plan.") {
		return nil
	}
	p, e := a.principal(ctx, actor)
	if e != nil || p == nil || p.State != "ACTIVE" {
		return Problem(403, "FORBIDDEN", "current active principal access is required")
	}
	m, _ := body.(map[string]any)
	var scope string
	permissions := []string{"profile:write", "document:read"}
	switch {
	case strings.HasPrefix(action, "ai.draft."):
		draft, err := a.aiDraft(ctx, a.Management, id)
		if err != nil {
			return err
		}
		scope = Str(draft, "scopeId")
		if action == "ai.draft.publish" {
			permissions = []string{"profile:publish", "document:read"}
		}
	case action == "ai.job.create":
		scope = Str(m, "scopeId")
		if aiBool(m, "useModel") {
			permissions = append(permissions, "ai:use")
		}
	case strings.HasPrefix(action, "ai.job."):
		job, err := a.aiJob(ctx, a.Management, id)
		if err != nil {
			return err
		}
		scope = Str(job, "scopeId")
		if action == "ai.job.retry" {
			permissions = append(permissions, "ai:use")
		}
	case action == "ai.plan.create":
		model, err := a.aiModel(ctx, a.Management, Str(m, "modelId"))
		if err != nil {
			return err
		}
		scope = Str(model, "scopeId")
		permissions = []string{"profile:read"}
	case action == "ai.plan.confirm":
		plan, owner, err := a.aiPlan(ctx, a.Management, id)
		if err != nil {
			return err
		}
		if owner != actor {
			return Problem(403, "FORBIDDEN", "apply plan belongs to another principal")
		}
		scope = Str(plan, "scopeId")
		permissions = []string{"profile:read"}
	default:
		return Problem(403, "FORBIDDEN", "unsupported scoped mutation")
	}
	for _, permission := range permissions {
		if !canQuery(ctx, a.Management, p, permission, scope) {
			return Problem(403, "FORBIDDEN", "current permission for this resource scope is required")
		}
	}
	return nil
}
func (a *App) aiAuthorized(ctx context.Context, p *Principal, permission, scope string) error {
	if !canQuery(ctx, a.Management, p, permission, scope) {
		return Problem(403, "FORBIDDEN", "permission or active resource scope is required")
	}
	return nil
}
func aiTrim(s string, max int) string {
	s = strings.TrimSpace(s)
	if len(s) > max {
		return s[:max]
	}
	return s
}
