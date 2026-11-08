package platform

import (
	"bytes"
	"context"
	"crypto/tls"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

var aiProviderFields = []string{"name", "kind", "mode", "baseUrl", "model", "enabled", "allowExternal", "allowInsecureLocal", "vision", "schemaMode", "timeoutSeconds", "maxOutputTokens", "dailyRequestLimit", "tokenParameter", "apiKey"}

func aiProviderDefaults() map[string]any {
	return map[string]any{"name": "", "kind": "OPENAI_COMPATIBLE", "mode": "LOCAL", "baseUrl": "", "model": "", "enabled": false, "allowExternal": false, "allowInsecureLocal": false, "vision": false, "schemaMode": "JSON_OBJECT", "timeoutSeconds": 120, "maxOutputTokens": 4096, "dailyRequestLimit": 50, "tokenParameter": "max_tokens"}
}
func aiPublicProvider(m map[string]any) map[string]any {
	out := map[string]any{}
	for k, v := range m {
		if k != "secretRef" && k != "apiKey" {
			out[k] = v
		}
	}
	out["hasCredential"] = Str(m, "secretRef") != ""
	return out
}
func (a *App) aiProvider(ctx context.Context, id string) (map[string]any, error) {
	var cfg, check string
	var rev, created, updated int64
	e := a.Management.QueryRowContext(ctx, "SELECT config_json,revision,last_check_json,created_at_ms,updated_at_ms FROM ai_providers WHERE provider_id=?", id).Scan(&cfg, &rev, &check, &created, &updated)
	if e != nil {
		return nil, aiNotFound(e, "PROVIDER_NOT_FOUND")
	}
	m := aiMap(cfg)
	m["providerId"] = id
	m["revision"] = rev
	m["lastCheck"] = aiMap(check)
	m["createdAt"] = Timestamp(created)
	m["updatedAt"] = Timestamp(updated)
	return m, nil
}
func (a *App) aiStatus(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT provider_id,config_json FROM ai_providers ORDER BY created_at_ms")
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	enabled := false
	for rows.Next() {
		var id, cfg string
		if e = rows.Scan(&id, &cfg); e != nil {
			break
		}
		m := aiMap(cfg)
		v := map[string]any{"providerId": id, "name": m["name"], "kind": m["kind"], "mode": m["mode"], "model": m["model"], "enabled": m["enabled"], "vision": m["vision"], "schemaMode": m["schemaMode"]}
		items = append(items, v)
		enabled = enabled || aiBool(m, "enabled")
	}
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	var used int
	_ = a.Management.QueryRowContext(r.Context(), "SELECT count(*) FROM ai_usage WHERE actor_id=? AND created_at_ms>=?", p.ID, aiDayStart()).Scan(&used)
	JSON(w, 200, map[string]any{"configured": len(items) > 0, "enabled": enabled, "providers": items, "todayRequests": used, "limits": map[string]any{"maxFiles": 5, "maxBytes": 50 << 20, "maxPages": 200, "maxStoredDocumentBytes": aiMaxStoredDocumentBytes()}, "extraction": map[string]any{"pdfTextAvailable": aiExecutable("GW_AI_PDFTOTEXT", "pdftotext") != "" || aiPythonPDFConfigured(), "pdfRasterAvailable": aiExecutable("GW_AI_PDFTOPPM", "pdftoppm") != ""}})
}
func (a *App) aiListProviders(w http.ResponseWriter, r *http.Request) {
	if _, ok := a.aiAdmin(w, r); !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT provider_id FROM ai_providers ORDER BY created_at_ms")
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
	rows.Close()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	items := []any{}
	for _, id := range ids {
		p, e := a.aiProvider(r.Context(), id)
		if e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, aiPublicProvider(p))
	}
	a.Page(w, r, items)
}
func validateAIProvider(m map[string]any) error {
	for _, k := range []string{"name", "model"} {
		if s := Str(m, k); s == "" || len(s) > 128 {
			return Problem(422, "INVALID_PROVIDER", k+" must be 1..128 characters")
		}
	}
	if Str(m, "kind") != "OPENAI_COMPATIBLE" && Str(m, "kind") != "OLLAMA" {
		return Problem(422, "INVALID_PROVIDER", "unsupported provider kind")
	}
	if Str(m, "mode") != "LOCAL" && Str(m, "mode") != "CLOUD" {
		return Problem(422, "INVALID_PROVIDER", "mode must be LOCAL or CLOUD")
	}
	if Str(m, "schemaMode") != "JSON_OBJECT" && Str(m, "schemaMode") != "JSON_SCHEMA" {
		return Problem(422, "INVALID_PROVIDER", "schemaMode must be JSON_OBJECT or JSON_SCHEMA")
	}
	if Str(m, "tokenParameter") != "max_tokens" && Str(m, "tokenParameter") != "max_completion_tokens" {
		return Problem(422, "INVALID_PROVIDER", "tokenParameter must be max_tokens or max_completion_tokens")
	}
	for _, k := range []string{"enabled", "allowExternal", "allowInsecureLocal", "vision"} {
		if _, ok := m[k].(bool); !ok {
			return Problem(422, "INVALID_PROVIDER", k+" must be boolean")
		}
	}
	for k, b := range map[string][2]int64{"timeoutSeconds": {5, 180}, "maxOutputTokens": {256, 16384}, "dailyRequestLimit": {1, 10000}} {
		n := Num(m, k)
		f, valid := mappingNumber(m[k])
		if !valid || math.Trunc(f) != f || n < b[0] || n > b[1] {
			return Problem(422, "INVALID_PROVIDER", k+" is outside the supported integer range")
		}
	}
	u, e := url.Parse(Str(m, "baseUrl"))
	if e != nil || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || len(u.String()) > 2048 {
		return Problem(422, "INVALID_PROVIDER", "baseUrl requires an absolute HTTP(S) URL without credentials, query or fragment")
	}
	if u.Scheme != "https" && !(u.Scheme == "http" && Str(m, "mode") == "LOCAL" && aiBool(m, "allowInsecureLocal")) {
		return Problem(422, "INVALID_PROVIDER", "HTTPS required; local HTTP needs explicit allowInsecureLocal")
	}
	if aiBool(m, "enabled") && Str(m, "mode") == "CLOUD" && !aiBool(m, "allowExternal") {
		return Problem(422, "AI_EXTERNAL_DISABLED", "external document sending must be explicitly enabled")
	}
	if ip := net.ParseIP(u.Hostname()); ip != nil {
		if e := aiCheckIP(ip, Str(m, "mode")); e != nil {
			return e
		}
	}
	return nil
}
func (a *App) aiCreateProvider(w http.ResponseWriter, r *http.Request) {
	p, ok := a.aiAdmin(w, r)
	if !ok {
		return
	}
	m, ok := aiDecode(w, r, aiProviderFields...)
	if !ok {
		return
	}
	cfg := aiProviderDefaults()
	for k, v := range m {
		if k != "apiKey" {
			cfg[k] = v
		}
	}
	if e := validateAIProvider(cfg); e != nil {
		RespondError(w, r, e)
		return
	}
	a.aiMutation(w, r, p, "ai.provider.create", m, func(tx *sql.Tx) (int, string, any, error) {
		if key := Str(m, "apiKey"); key != "" {
			if len(key) > 8192 {
				return 0, "", nil, Problem(422, "INVALID_PROVIDER", "credential too long")
			}
			ref, e := a.SecretPut(key)
			if e != nil {
				return 0, "", nil, e
			}
			cfg["secretRef"] = ref
		}
		id := ID("aip")
		now := Now()
		_, e := tx.ExecContext(r.Context(), "INSERT INTO ai_providers(provider_id,revision,config_json,created_at_ms,updated_at_ms) VALUES(?,1,?,?,?)", id, encode(cfg), now, now)
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "ai.provider.create", "ai_provider", id, map[string]any{"mode": cfg["mode"], "enabled": cfg["enabled"]})
		}
		out := aiPublicProvider(cfg)
		out["providerId"] = id
		out["revision"] = int64(1)
		return 201, id, out, e
	})
}
func (a *App) aiPatchProvider(w http.ResponseWriter, r *http.Request) {
	p, ok := a.aiAdmin(w, r)
	if !ok {
		return
	}
	body, ok := aiDecode(w, r, aiProviderFields...)
	if !ok {
		return
	}
	id := r.PathValue("providerId")
	if id == "" {
		id = aiParam(r, "providerId")
	}
	a.aiMutation(w, r, p, "ai.provider.patch:"+id, map[string]any{"body": body, "etag": r.Header.Get("If-Match")}, func(tx *sql.Tx) (int, string, any, error) {
		var raw string
		var rev int64
		if e := tx.QueryRow("SELECT config_json,revision FROM ai_providers WHERE provider_id=?", id).Scan(&raw, &rev); e != nil {
			return 0, "", nil, aiNotFound(e, "PROVIDER_NOT_FOUND")
		}
		if e := Match(r, rev); e != nil {
			return 0, "", nil, e
		}
		cfg := aiMap(raw)
		for k, v := range body {
			if k != "apiKey" {
				cfg[k] = v
			}
		}
		if e := validateAIProvider(cfg); e != nil {
			return 0, "", nil, e
		}
		if key := Str(body, "apiKey"); key != "" {
			if len(key) > 8192 {
				return 0, "", nil, Problem(422, "INVALID_PROVIDER", "credential too long")
			}
			ref, e := a.SecretPut(key)
			if e != nil {
				return 0, "", nil, e
			}
			cfg["secretRef"] = ref
		}
		_, e := tx.Exec("UPDATE ai_providers SET revision=revision+1,config_json=?,last_check_json='{}',updated_at_ms=? WHERE provider_id=?", encode(cfg), Now(), id)
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "ai.provider.update", "ai_provider", id, map[string]any{"revision": rev + 1, "mode": cfg["mode"], "enabled": cfg["enabled"]})
		}
		out := aiPublicProvider(cfg)
		out["providerId"] = id
		out["revision"] = rev + 1
		return 200, id, out, e
	})
}
func aiDayStart() int64 {
	n := time.Now().UTC()
	return time.Date(n.Year(), n.Month(), n.Day(), 0, 0, 0, 0, time.UTC).UnixMilli()
}
func (a *App) aiReserveUsage(ctx context.Context, cfg map[string]any, actor, job string) (string, error) {
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		return "", e
	}
	defer tx.Rollback()
	if strings.HasPrefix(job, "aijob_") {
		id := strings.SplitN(job, ":", 2)[0]
		var state, scope, owner string
		var revision int64
		if e = tx.QueryRowContext(ctx, "SELECT state,scope_id,owner_id,provider_revision FROM ai_jobs WHERE job_id=?", id).Scan(&state, &scope, &owner, &revision); e != nil {
			return "", e
		}
		if state != "GENERATING" || owner != actor {
			return "", Problem(409, "IMPORT_CANCELED", "job is no longer eligible for model dispatch")
		}
		p, e := a.principalWithQuery(ctx, tx, actor)
		if e != nil || !canQuery(ctx, tx, p, "ai:use", scope) || !canQuery(ctx, tx, p, "document:read", scope) || !canQuery(ctx, tx, p, "profile:write", scope) {
			return "", Problem(403, "FORBIDDEN", "document or model usage permission changed")
		}
		var current int64
		if e = tx.QueryRowContext(ctx, "SELECT revision FROM ai_providers WHERE provider_id=?", Str(cfg, "providerId")).Scan(&current); e != nil {
			return "", e
		}
		if current != revision || current != Num(cfg, "revision") {
			return "", Problem(409, "AI_PROVIDER_CHANGED", "provider policy changed before dispatch")
		}
		if _, e = tx.ExecContext(ctx, "UPDATE ai_jobs SET phase='MODEL_DISPATCH',updated_at_ms=? WHERE job_id=?", Now(), id); e != nil {
			return "", e
		}
	}
	var prior int
	if e = tx.QueryRowContext(ctx, "SELECT count(*) FROM ai_usage WHERE provider_id=? AND job_id=?", Str(cfg, "providerId"), job).Scan(&prior); e != nil {
		return "", e
	}
	if prior > 0 {
		return "", Problem(409, "AI_REQUEST_ALREADY_DISPATCHED", "this request was already dispatched; query its status instead of sending it again")
	}
	var count int
	if e = tx.QueryRowContext(ctx, "SELECT count(*) FROM ai_usage WHERE provider_id=? AND created_at_ms>=?", Str(cfg, "providerId"), aiDayStart()).Scan(&count); e != nil {
		return "", e
	}
	if int64(count) >= Num(cfg, "dailyRequestLimit") {
		return "", Problem(429, "AI_BUDGET_EXCEEDED", "daily request allowance exhausted")
	}
	id := ID("aiuse")
	_, e = tx.ExecContext(ctx, "INSERT INTO ai_usage(usage_id,provider_id,job_id,actor_id,state,created_at_ms) VALUES(?,?,?,?,'RESERVED',?)", id, Str(cfg, "providerId"), job, actor, Now())
	if e != nil {
		return "", e
	}
	return id, tx.Commit()
}
func aiCheckIP(ip net.IP, mode string) error {
	if ip.IsUnspecified() || ip.IsMulticast() || ip.IsLinkLocalUnicast() || ip.IsLinkLocalMulticast() {
		return Problem(422, "AI_ENDPOINT_BLOCKED", "metadata, link-local, unspecified and multicast endpoints are prohibited")
	}
	for _, cidr := range []string{"0.0.0.0/8", "100.64.0.0/10", "192.0.0.0/24", "192.0.2.0/24", "198.18.0.0/15", "198.51.100.0/24", "203.0.113.0/24", "240.0.0.0/4", "2001:db8::/32"} {
		_, network, _ := net.ParseCIDR(cidr)
		if network.Contains(ip) {
			return Problem(422, "AI_ENDPOINT_BLOCKED", "reserved and metadata destination ranges are prohibited")
		}
	}
	if mode == "CLOUD" && (ip.IsPrivate() || ip.IsLoopback()) {
		return Problem(422, "AI_ENDPOINT_BLOCKED", "cloud endpoint must resolve to public addresses")
	}
	if mode == "LOCAL" && !(ip.IsPrivate() || ip.IsLoopback()) {
		return Problem(422, "AI_ENDPOINT_BLOCKED", "local endpoint must resolve only to private or loopback addresses")
	}
	return nil
}
func (a *App) aiHTTPClient(cfg map[string]any) *http.Client {
	transport := &http.Transport{TLSClientConfig: &tls.Config{MinVersion: tls.VersionTLS12}, Proxy: nil, ResponseHeaderTimeout: time.Duration(Num(cfg, "timeoutSeconds")) * time.Second}
	transport.DialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
		host, port, e := net.SplitHostPort(address)
		if e != nil {
			return nil, e
		}
		ips, e := net.DefaultResolver.LookupIPAddr(ctx, host)
		if e != nil || len(ips) == 0 {
			return nil, errors.New("model endpoint DNS lookup failed")
		}
		for _, ip := range ips {
			if e := aiCheckIP(ip.IP, Str(cfg, "mode")); e != nil {
				return nil, e
			}
		}
		// Do not allow the model endpoint to target this application's own API.
		_, listenPort, _ := net.SplitHostPort(a.Config.ListenAddr)
		if listenPort != "" && port == listenPort {
			for _, ip := range ips {
				if ip.IP.IsLoopback() {
					return nil, errors.New("model endpoint cannot be the management listener")
				}
			}
		}
		return (&net.Dialer{Timeout: 10 * time.Second}).DialContext(ctx, network, net.JoinHostPort(ips[0].IP.String(), port))
	}
	return &http.Client{Transport: transport, Timeout: time.Duration(Num(cfg, "timeoutSeconds")) * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
}
func (a *App) aiCallModel(ctx context.Context, cfg map[string]any, actor, job, system string, user any, schema map[string]any) (map[string]any, map[string]any, error) {
	if !aiBool(cfg, "enabled") {
		return nil, nil, Problem(409, "AI_DISABLED", "selected model service is disabled")
	}
	if Str(cfg, "mode") == "CLOUD" && !aiBool(cfg, "allowExternal") {
		return nil, nil, Problem(403, "AI_EXTERNAL_DISABLED", "external document sending is disabled")
	}
	if e := validateAIProvider(cfg); e != nil {
		return nil, nil, e
	}
	usageID, e := a.aiReserveUsage(ctx, cfg, actor, job)
	if e != nil {
		return nil, nil, e
	}
	success := false
	usage := map[string]any{}
	defer func() {
		state := "FAILED_OR_UNCERTAIN"
		if success {
			state = "COMPLETED"
		}
		_, _ = a.Management.ExecContext(context.Background(), "UPDATE ai_usage SET state=?,usage_json=? WHERE usage_id=?", state, encode(usage), usageID)
	}()
	body := map[string]any{"model": cfg["model"], "stream": false}
	endpoint := strings.TrimRight(Str(cfg, "baseUrl"), "/")
	userMessage := map[string]any{"role": "user", "content": user}
	if multimodal, ok := user.(map[string]any); ok {
		if Str(cfg, "kind") == "OLLAMA" {
			encoded := []string{}
			for _, image := range aiStringList(multimodal["images"]) {
				parts := strings.SplitN(image, ",", 2)
				if len(parts) != 2 {
					return nil, usage, Problem(422, "VISION_FORMAT_UNSUPPORTED", "invalid image input")
				}
				encoded = append(encoded, parts[1])
			}
			userMessage["content"] = Str(multimodal, "text")
			userMessage["images"] = encoded
		} else {
			parts := []any{map[string]any{"type": "text", "text": Str(multimodal, "text")}}
			for _, image := range aiStringList(multimodal["images"]) {
				parts = append(parts, map[string]any{"type": "image_url", "image_url": map[string]any{"url": image}})
			}
			userMessage["content"] = parts
		}
	}
	if Str(cfg, "kind") == "OLLAMA" {
		endpoint += "/api/chat"
		body["messages"] = []any{map[string]any{"role": "system", "content": system}, userMessage}
		body["format"] = "json"
		if Str(cfg, "schemaMode") == "JSON_SCHEMA" {
			body["format"] = schema
		}
		body["options"] = map[string]any{"num_predict": Num(cfg, "maxOutputTokens"), "temperature": 0}
	} else {
		endpoint += "/chat/completions"
		body["messages"] = []any{map[string]any{"role": "system", "content": system}, userMessage}
		body[Str(cfg, "tokenParameter")] = Num(cfg, "maxOutputTokens")
		body["store"] = false
		body["response_format"] = map[string]any{"type": "json_object"}
		if Str(cfg, "schemaMode") == "JSON_SCHEMA" {
			body["response_format"] = map[string]any{"type": "json_schema", "json_schema": map[string]any{"name": "device_protocol_draft", "strict": false, "schema": schema}}
		}
	}
	req, e := http.NewRequestWithContext(ctx, "POST", endpoint, bytes.NewBufferString(encode(body)))
	if e != nil {
		return nil, usage, e
	}
	req.Header.Set("Content-Type", "application/json")
	if ref := Str(cfg, "secretRef"); ref != "" {
		secret, e := a.SecretGet(ref)
		if e != nil {
			return nil, usage, Problem(503, "AI_CREDENTIAL_UNAVAILABLE", "model credential is unavailable")
		}
		req.Header.Set("Authorization", "Bearer "+secret)
	}
	client := a.aiHTTPClient(cfg)
	defer client.CloseIdleConnections()
	resp, e := client.Do(req)
	if e != nil {
		return nil, usage, Problem(503, "MODEL_UNAVAILABLE", "model connection failed or timed out; submitted request may have incurred usage")
	}
	defer resp.Body.Close()
	raw, e := io.ReadAll(io.LimitReader(resp.Body, 2<<20+1))
	if e != nil || len(raw) > 2<<20 {
		return nil, usage, Problem(502, "MODEL_OUTPUT_INVALID", "model response exceeded supported size")
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		code := "MODEL_UNAVAILABLE"
		status := 502
		if resp.StatusCode == 429 {
			code = "MODEL_RATE_LIMITED"
			status = 429
		}
		return nil, usage, Problem(status, code, "model returned HTTP "+strconv.Itoa(resp.StatusCode)+"; provider response content was withheld")
	}
	result := map[string]any{}
	if e = json.Unmarshal(raw, &result); e != nil {
		return nil, usage, Problem(502, "MODEL_OUTPUT_INVALID", "model endpoint returned invalid JSON")
	}
	content := ""
	if Str(cfg, "kind") == "OLLAMA" {
		if !aiBool(result, "done") || Str(result, "done_reason") == "length" {
			return nil, usage, Problem(502, "MODEL_OUTPUT_INCOMPLETE", "model generation did not complete")
		}
		content = Str(Obj(result, "message"), "content")
		usage = map[string]any{"inputTokens": Num(result, "prompt_eval_count"), "outputTokens": Num(result, "eval_count")}
	} else {
		choices := List(result, "choices")
		if len(choices) != 1 {
			return nil, usage, Problem(502, "MODEL_OUTPUT_INVALID", "expected one model completion")
		}
		choice, _ := choices[0].(map[string]any)
		msg := Obj(choice, "message")
		if Str(msg, "refusal") != "" {
			return nil, usage, Problem(422, "MODEL_REFUSED", "model declined this document request")
		}
		if Str(choice, "finish_reason") != "stop" {
			return nil, usage, Problem(502, "MODEL_OUTPUT_INCOMPLETE", "model output was truncated or interrupted")
		}
		content = Str(msg, "content")
		u := Obj(result, "usage")
		usage = map[string]any{"inputTokens": Num(u, "prompt_tokens"), "outputTokens": Num(u, "completion_tokens")}
	}
	parsed := map[string]any{}
	decoder := json.NewDecoder(strings.NewReader(content))
	if e = decoder.Decode(&parsed); e != nil || parsed == nil {
		return nil, usage, Problem(502, "MODEL_OUTPUT_INVALID", "model content must be a JSON object")
	}
	if decoder.Decode(new(any)) != io.EOF {
		return nil, usage, Problem(502, "MODEL_OUTPUT_INVALID", "trailing model output is not accepted")
	}
	success = true
	return parsed, usage, nil
}
func (a *App) aiCheckProvider(w http.ResponseWriter, r *http.Request) {
	p, ok := a.aiAdmin(w, r)
	if !ok {
		return
	}
	if _, ok = aiDecode(w, r); !ok {
		return
	}
	cfg, e := a.aiProvider(r.Context(), aiParam(r, "providerId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	// Checks are explicit potentially billable operations. Claim an idempotent record
	// before dispatch; a repeated check key never dispatches a second model request.
	key := r.Header.Get("Idempotency-Key")
	if key == "" {
		Fail(w, r, 400, "IDEMPOTENCY_KEY_REQUIRED", "Idempotency-Key required")
		return
	}
	usageJob := "check:" + Str(cfg, "providerId") + ":" + p.ID + ":" + Hash(key)
	var previousState, previousResult string
	e = a.Management.QueryRowContext(r.Context(), "SELECT state,result_json FROM ai_usage WHERE provider_id=? AND job_id=?", Str(cfg, "providerId"), usageJob).Scan(&previousState, &previousResult)
	if e == nil {
		cached := aiMap(previousResult)
		if len(cached) > 0 {
			JSON(w, 200, cached)
		} else {
			JSON(w, 200, map[string]any{"passed": false, "state": previousState, "message": "This check is in flight or its result is unknown. The same key will not send another request.", "checkedAt": Timestamp(Now())})
		}
		return
	}
	if !errors.Is(e, sql.ErrNoRows) {
		RespondError(w, r, e)
		return
	}
	if !aiBool(cfg, "enabled") {
		cfg["enabled"] = true
	}
	cfg["timeoutSeconds"] = int64(15)
	out, usage, e := a.aiCallModel(r.Context(), cfg, p.ID, usageJob, "Return JSON only: {\"ok\":true}. Do not use tools.", "Connection check without customer data.", map[string]any{"type": "object", "properties": map[string]any{"ok": map[string]any{"type": "boolean"}}, "required": []string{"ok"}, "additionalProperties": false})
	if usage == nil {
		usage = map[string]any{}
	}
	result := map[string]any{"passed": e == nil && aiBool(out, "ok"), "checkedAt": Timestamp(Now()), "usage": usage, "message": "Model endpoint responded with validated JSON."}
	if e != nil {
		var api *APIError
		if errors.As(e, &api) {
			result["message"] = api.Message
			result["errorCode"] = api.Code
		} else {
			result["message"] = "Model check failed."
		}
	}
	_, _ = a.Management.ExecContext(r.Context(), "UPDATE ai_usage SET result_json=? WHERE provider_id=? AND job_id=?", encode(result), Str(cfg, "providerId"), usageJob)
	_, _ = a.Management.ExecContext(r.Context(), "UPDATE ai_providers SET last_check_json=? WHERE provider_id=? AND revision=?", encode(result), Str(cfg, "providerId"), Num(cfg, "revision"))
	_ = a.Audit(r.Context(), nil, p.ID, "ai.provider.check", "ai_provider", Str(cfg, "providerId"), map[string]any{"passed": result["passed"]})
	JSON(w, 200, result)
}
