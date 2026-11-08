package platform

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"image"
	"image/png"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/go-chi/chi/v5"
)

func aiTestMutation(h *apiHarness, method, path string, body any, status int, etag string) map[string]any {
	h.t.Helper()
	headers := map[string]string{"Idempotency-Key": ID("request")}
	if etag != "" {
		headers["If-Match"] = etag
	}
	w := h.do(method, path, body, headers)
	h.expect(w, status, "")
	return h.object(w)
}
func aiTestContent() map[string]any {
	c := aiEmptyContent()
	c["manufacturer"] = "Example"
	c["modelName"] = "Single packet sensor"
	c["parserSpec"] = map[string]any{"dslVersion": "1.0", "inputKind": "RAW", "length": 2, "fields": []any{map[string]any{"name": "temperature", "offset": 0, "type": "int16", "endian": "little", "scale": 0.01}}}
	c["sources"] = []any{map[string]any{"path": "parserSpec", "state": "USER", "note": "Reviewed with the vendor protocol and independent captured values."}}
	c["tests"] = []any{map[string]any{"name": "normal positive", "payloadHex": "2e09", "expected": []any{map[string]any{"temperature": 23.5}}, "source": "USER"}, map[string]any{"name": "truncated negative", "payloadHex": "2e", "expectError": true, "source": "USER"}}
	return c
}
func aiTestDraft(h *apiHarness, c map[string]any) (string, map[string]any) {
	h.t.Helper()
	job := aiTestMutation(h, "POST", "/api/v1/ai/import-jobs", map[string]any{"scopeId": "scope_default_group", "taskKind": "PARSER_PROFILE", "documentIds": []any{}, "useModel": false}, 202, "")
	id := Str(job, "draftId")
	d := aiTestMutation(h, "PATCH", "/api/v1/protocol-drafts/"+id, map[string]any{"content": c}, 200, ETag(1))
	return id, d
}
func aiTestPublish(h *apiHarness, c map[string]any) map[string]any {
	h.t.Helper()
	id, d := aiTestDraft(h, c)
	v := aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/validations", map[string]any{"draftRevision": d["revision"]}, 200, "")
	if !aiBool(v, "passed") {
		h.t.Fatal(v)
	}
	return aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/publications", map[string]any{"draftRevision": d["revision"], "validationId": v["validationId"], "version": "1.0.0"}, 201, "")
}
func aiTestUpload(h *apiHarness, name string, data []byte) map[string]any {
	h.t.Helper()
	w := aiTestUploadResponse(h, name, data)
	h.expect(w, 201, "")
	return h.object(w)
}
func aiTestUploadResponse(h *apiHarness, name string, data []byte) *httptest.ResponseRecorder {
	h.t.Helper()
	var buffer bytes.Buffer
	mw := multipart.NewWriter(&buffer)
	_ = mw.WriteField("scopeId", "scope_default_group")
	part, e := mw.CreateFormFile("file", name)
	if e != nil {
		h.t.Fatal(e)
	}
	_, _ = part.Write(data)
	_ = mw.Close()
	r := httptest.NewRequest("POST", "/api/v1/source-documents", &buffer)
	r.Header.Set("Content-Type", mw.FormDataContentType())
	r.Header.Set("Idempotency-Key", ID("upload"))
	r.AddCookie(h.cookie)
	r.Header.Set("X-CSRF-Token", h.csrf)
	w := httptest.NewRecorder()
	h.handler.ServeHTTP(w, r)
	return w
}
func aiTestProvider(h *apiHarness, url string, limit int) map[string]any {
	return aiTestMutation(h, "POST", "/api/v1/ai/providers", map[string]any{"name": "Local test model", "baseUrl": url, "kind": "OPENAI_COMPATIBLE", "mode": "LOCAL", "allowInsecureLocal": true, "model": "test-fixture", "enabled": true, "dailyRequestLimit": limit, "apiKey": "test-only-private-key"}, 201, "")
}
func TestAIManualDraftPublicationIsImmutableAndDoesNotExecute(t *testing.T) {
	h := newHarness(t)
	id, d := aiTestDraft(h, aiTestContent())
	validation := aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/validations", map[string]any{"draftRevision": d["revision"]}, 200, "")
	if !aiBool(validation, "passed") {
		t.Fatal(validation)
	}
	staleContent := aiTestContent()
	staleContent["modelName"] = "Revision changed"
	edited := aiTestMutation(h, "PATCH", "/api/v1/protocol-drafts/"+id, map[string]any{"content": staleContent}, 200, ETag(2))
	aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/publications", map[string]any{"draftRevision": 2, "validationId": validation["validationId"], "version": "1.0.0"}, 409, "")
	validation = aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/validations", map[string]any{"draftRevision": edited["revision"]}, 200, "")
	body := map[string]any{"draftRevision": edited["revision"], "validationId": validation["validationId"], "version": "1.0.0"}
	key := ID("publish")
	headers := map[string]string{"Idempotency-Key": key}
	w := h.do("POST", "/api/v1/protocol-drafts/"+id+"/publications", body, headers)
	h.expect(w, 201, "")
	model := h.object(w)
	replay := h.do("POST", "/api/v1/protocol-drafts/"+id+"/publications", body, headers)
	h.expect(replay, 201, "")
	if Str(h.object(replay), "modelId") != Str(model, "modelId") {
		t.Fatal("idempotency changed publication")
	}
	if Str(model, "verificationLevel") != "SAMPLE_VERIFIED" || len(Obj(model, "parserRef")) == 0 {
		t.Fatal(model)
	}
	aiTestMutation(h, "PATCH", "/api/v1/protocol-drafts/"+id, map[string]any{"content": staleContent}, 409, ETag(3))
	plan := aiTestMutation(h, "POST", "/api/v1/ai/apply-plans", map[string]any{"modelId": model["modelId"], "sourceContext": map[string]any{"pageId": "P11"}}, 201, "")
	confirmed := aiTestMutation(h, "POST", "/api/v1/ai/apply-plans/"+Str(plan, "planId")+"/executions", map[string]any{"confirmed": true}, 200, "")
	if Str(confirmed, "state") != "READY_TO_APPLY" {
		t.Fatal(confirmed)
	}
	var tasks int
	if e := h.a.Management.QueryRow("SELECT count(*) FROM command_tasks").Scan(&tasks); e != nil {
		t.Fatal(e)
	}
	if tasks != 0 {
		t.Fatal("preparing templates executed device tasks")
	}
}
func TestAIDocumentEvidenceAndCrossScopeProtection(t *testing.T) {
	h := newHarness(t)
	doc := aiTestUpload(h, "manual.md", []byte("Protocol v1\nTemperature is signed little endian, scale 0.01.\n"))
	w := h.do("GET", "/api/v1/source-documents/"+Str(doc, "documentId")+"/evidence", nil, nil)
	h.expect(w, 200, "")
	if len(List(h.object(w), "items")) != 1 {
		t.Fatal(w.Body.String())
	}
	_, e := h.a.Management.Exec("INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('ai_other','group','scope_default_project','AI other',?)", Now())
	if e != nil {
		t.Fatal(e)
	}
	actor := autoScopedPrincipal(t, h.a, "ai_dev_other", "developer", "ai_other")
	p, e := h.a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	r := httptest.NewRequest("GET", "/api/v1/source-documents/"+Str(doc, "documentId")+"/evidence", nil)
	r = r.WithContext(context.WithValue(r.Context(), authKey{}, &authInfo{Principal: p}))
	router := chi.NewRouter()
	h.a.RegisterAI(router)
	out := httptest.NewRecorder()
	router.ServeHTTP(out, r)
	if out.Code != 403 {
		t.Fatal("cross-project evidence leaked", out.Code, out.Body.String())
	}
	// Archive extraction preserves paragraphs without running document macros or links.
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	f, _ := zw.Create("word/document.xml")
	_, _ = f.Write([]byte("<document><p><t>Service 180f</t></p><p><t>Characteristic 2a19</t></p></document>"))
	_ = zw.Close()
	text, _, e := aiExtractDOCX(buf.Bytes())
	if e != nil || !strings.Contains(text, "Service 180f\nCharacteristic 2a19") {
		t.Fatal(text, e)
	}
}
func TestAIModelOutputCannotSelfCertifyHumanReview(t *testing.T) {
	h := newHarness(t)
	var calls atomic.Int32
	content := aiTestContent()
	content["issues"] = []any{map[string]any{"code": "CONFLICT", "message": "conflict requires confirmation", "resolved": true, "resolution": "model claims accepted"}}
	fake := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.URL.Path != "/chat/completions" {
			t.Errorf("path %s", r.URL.Path)
		}
		if r.Header.Get("Authorization") != "Bearer test-only-private-key" {
			t.Error("credential not provided to selected endpoint")
		}
		req := map[string]any{}
		_ = json.NewDecoder(r.Body).Decode(&req)
		if req["tools"] != nil {
			t.Error("model received tools")
		}
		if req["max_tokens"] == nil {
			t.Error("generic compatible provider did not use max_tokens")
		}
		JSON(w, 200, map[string]any{"choices": []any{map[string]any{"finish_reason": "stop", "message": map[string]any{"content": encode(content)}}}, "usage": map[string]any{"prompt_tokens": 100, "completion_tokens": 200}})
	}))
	defer fake.Close()
	provider := aiTestProvider(h, fake.URL, 10)
	if strings.Contains(encode(provider), "test-only-private-key") || provider["secretRef"] != nil {
		t.Fatal("credential leaked in provider")
	}
	doc := aiTestUpload(h, "manual.txt", []byte("Protocol fixture signed little endian temperature."))
	job := aiTestMutation(h, "POST", "/api/v1/ai/import-jobs", map[string]any{"scopeId": "scope_default_group", "taskKind": "PARSER_PROFILE", "documentIds": []any{doc["documentId"]}, "useModel": true, "providerId": provider["providerId"]}, 202, "")
	if e := h.a.ProcessAIOnce(t.Context()); e != nil {
		t.Fatal(e)
	}
	w := h.do("GET", "/api/v1/ai/import-jobs/"+Str(job, "jobId"), nil, nil)
	h.expect(w, 200, "")
	result := h.object(w)
	if Str(result, "state") != "NEEDS_REVIEW" {
		t.Fatal(result)
	}
	w = h.do("GET", "/api/v1/protocol-drafts/"+Str(result, "draftId"), nil, nil)
	h.expect(w, 200, "")
	draft := h.object(w)
	c := Obj(draft, "content")
	for _, test := range List(c, "tests") {
		if Str(test.(map[string]any), "source") != "MODEL" {
			t.Fatal("model self-certified sample", test)
		}
	}
	if aiBool(List(c, "issues")[0].(map[string]any), "resolved") {
		t.Fatal("model self-certified resolution")
	}
	v := aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+Str(result, "draftId")+"/validations", map[string]any{"draftRevision": draft["revision"]}, 200, "")
	if aiBool(v, "passed") {
		t.Fatal("unreviewed model output was publishable")
	}
	if e := h.a.ProcessAIOnce(t.Context()); e != nil {
		t.Fatal(e)
	}
	if calls.Load() != 1 {
		t.Fatal("completed generation repeated")
	}
}
func TestAIProviderBudgetChecksAndCancellation(t *testing.T) {
	h := newHarness(t)
	var calls atomic.Int32
	fake := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		JSON(w, 200, map[string]any{"choices": []any{map[string]any{"finish_reason": "stop", "message": map[string]any{"content": "{\"ok\":true}"}}}})
	}))
	defer fake.Close()
	provider := aiTestProvider(h, fake.URL, 1)
	path := "/api/v1/ai/providers/" + Str(provider, "providerId") + "/checks"
	key := ID("check")
	first := h.do("POST", path, map[string]any{}, map[string]string{"Idempotency-Key": key})
	h.expect(first, 200, "")
	if !aiBool(h.object(first), "passed") {
		t.Fatal(first.Body.String())
	}
	second := h.do("POST", path, map[string]any{}, map[string]string{"Idempotency-Key": key})
	h.expect(second, 200, "")
	if !aiBool(h.object(second), "passed") || calls.Load() != 1 {
		t.Fatal("check was not idempotent")
	}
	blocked := aiTestMutation(h, "POST", path, map[string]any{}, 200, "")
	if Str(blocked, "errorCode") != "AI_BUDGET_EXCEEDED" || calls.Load() != 1 {
		t.Fatal(blocked, calls.Load())
	}
	doc := aiTestUpload(h, "manual.txt", []byte("A protocol document"))
	job := aiTestMutation(h, "POST", "/api/v1/ai/import-jobs", map[string]any{"scopeId": "scope_default_group", "taskKind": "PARSER_PROFILE", "documentIds": []any{doc["documentId"]}, "useModel": true, "providerId": provider["providerId"]}, 202, "")
	aiTestMutation(h, "POST", "/api/v1/ai/import-jobs/"+Str(job, "jobId")+"/cancellation", map[string]any{}, 200, "")
	cfg, e := h.a.aiProvider(t.Context(), Str(provider, "providerId"))
	if e != nil {
		t.Fatal(e)
	}
	var actor string
	_ = h.a.Management.QueryRow("SELECT principal_id FROM principals WHERE name='admin'").Scan(&actor)
	if _, e = h.a.aiReserveUsage(t.Context(), cfg, actor, Str(job, "jobId")+":1"); e == nil {
		t.Fatal("canceled job reserved a dispatch")
	}
	if e = h.a.ProcessAIOnce(t.Context()); e != nil {
		t.Fatal(e)
	}
	if calls.Load() != 1 {
		t.Fatal("canceled job called model")
	}
}
func TestAIProviderAddressModesAndRedirectRejection(t *testing.T) {
	for _, tc := range []struct {
		ip, mode string
		allowed  bool
	}{{"127.0.0.1", "LOCAL", true}, {"10.0.0.7", "LOCAL", true}, {"8.8.8.8", "LOCAL", false}, {"127.0.0.1", "CLOUD", false}, {"8.8.8.8", "CLOUD", true}, {"169.254.169.254", "LOCAL", false}, {"100.100.100.200", "CLOUD", false}, {"::", "LOCAL", false}} {
		if got := aiCheckIP(net.ParseIP(tc.ip), tc.mode) == nil; got != tc.allowed {
			t.Errorf("%s %s got allowed=%v", tc.ip, tc.mode, got)
		}
	}
	h := newHarness(t)
	var targetCalls atomic.Int32
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { targetCalls.Add(1) }))
	defer target.Close()
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, target.URL, http.StatusTemporaryRedirect)
	}))
	defer redirect.Close()
	p := aiTestProvider(h, redirect.URL, 10)
	v := aiTestMutation(h, "POST", "/api/v1/ai/providers/"+Str(p, "providerId")+"/checks", map[string]any{}, 200, "")
	if aiBool(v, "passed") || targetCalls.Load() != 0 {
		t.Fatal("redirect followed", v)
	}
}
func TestAIPublishedTemplatesRedactPrivateEvidenceForDeviceRole(t *testing.T) {
	h := newHarness(t)
	content := aiTestContent()
	Obj(content, "parserSpec")["description"] = "private-original-excerpt-marker"
	field := List(Obj(content, "parserSpec"), "fields")[0].(map[string]any)
	field["description"] = "private-original-excerpt-marker"
	field["evidence"] = "private-original-excerpt-marker"
	model := aiTestPublish(h, content)
	actor := autoScopedPrincipal(t, h.a, "ai_device_reader", "device_admin", "scope_default_group")
	p, e := h.a.principal(t.Context(), actor)
	if e != nil {
		t.Fatal(e)
	}
	view := h.a.aiModelView(p, model)
	if len(List(Obj(view, "content"), "tests")) != 0 || len(List(Obj(view, "content"), "sources")) != 0 || !aiBool(view, "evidenceRestricted") {
		t.Fatal("private evidence was disclosed")
	}
	if len(Obj(Obj(view, "content"), "parserSpec")) == 0 {
		t.Fatal("usable template was removed")
	}
	if strings.Contains(encode(view), "private-original-excerpt-marker") {
		t.Fatal("parser annotations leaked original source material")
	}
	var scope string
	if e = h.a.Management.QueryRow("SELECT scope_id FROM protocol_parsers WHERE parser_id=?", Str(Obj(model, "parserRef"), "parserId")).Scan(&scope); e != nil {
		t.Fatal(e)
	}
	if scope != "scope_default_group" {
		t.Fatal("parser lost profile scope")
	}
}
func TestAIValidationRejectsFabricatedEvidenceAndMalformedNegativeSample(t *testing.T) {
	h := newHarness(t)
	c := aiTestContent()
	c["sources"] = []any{map[string]any{"path": "parserSpec", "state": "DOCUMENT", "evidenceId": "unrelated", "quote": "not in this document"}}
	List(c, "tests")[1].(map[string]any)["payloadHex"] = "not hex"
	id, d := aiTestDraft(h, c)
	v := aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/validations", map[string]any{"draftRevision": d["revision"]}, 200, "")
	if aiBool(v, "passed") {
		t.Fatal("fabricated sources or malformed negative tests accepted")
	}
	codes := encode(v["errors"])
	if !strings.Contains(codes, "EVIDENCE_UNVERIFIED") || !strings.Contains(codes, "NEGATIVE_SAMPLE_REQUIRED") {
		t.Fatal(v)
	}
}

func TestAICombinedParserMappingValidatesEntireChain(t *testing.T) {
	for _, tc := range []struct {
		name, source string
		expected     []any
		wantPassed   bool
	}{
		{"mapped output publishes", "temperature", []any{map[string]any{"reading": 47.0}}, true},
		{"intermediate parser output cannot publish", "temperature", []any{map[string]any{"temperature": 23.5}}, false},
		{"missing mapping source cannot publish", "missing", []any{map[string]any{"temperature": 23.5}}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			h := newHarness(t)
			c := aiTestContent()
			c["mappingSpec"] = map[string]any{"fields": []any{map[string]any{"source": tc.source, "target": "reading", "scale": 2}}}
			c["sources"] = append(List(c, "sources"), map[string]any{"path": "mappingSpec", "state": "USER", "note": "Reviewed customer mapping schema and independent end-to-end example."})
			List(c, "tests")[0].(map[string]any)["expected"] = tc.expected
			if tc.source == "missing" {
				// This valid packet parses successfully, but the mapping must reject it.
				List(c, "tests")[1].(map[string]any)["payloadHex"] = "2e09"
			}
			id, d := aiTestDraft(h, c)
			v := aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/validations", map[string]any{"draftRevision": d["revision"]}, 200, "")
			if aiBool(v, "passed") != tc.wantPassed || Num(v, "negativeSamples") != 1 {
				t.Fatal(v)
			}
			status := 409
			if tc.wantPassed {
				status = 201
			} else if Num(v, "independentSamples") != 0 || !strings.Contains(encode(v["errors"]), "SAMPLE_FAILED") {
				t.Fatal("invalid mapping acquired verified sample", v)
			}
			aiTestMutation(h, "POST", "/api/v1/protocol-drafts/"+id+"/publications", map[string]any{"draftRevision": d["revision"], "validationId": v["validationId"], "version": "1.0.0"}, status, "")
		})
	}
}

func TestAIMultipleDocumentsShareVisionByteBudget(t *testing.T) {
	h := newHarness(t)
	var calls atomic.Int32
	model := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer model.Close()
	provider := aiTestProvider(h, model.URL, 10)
	provider = aiTestMutation(h, "PATCH", "/api/v1/ai/providers/"+Str(provider, "providerId"), map[string]any{"vision": true}, 200, ETag(1))
	// Four individually valid PNG files under 4 MiB exceed the shared 12 MiB
	// allowance. An ancillary PNG text chunk makes the boundary deterministic.
	var small bytes.Buffer
	if e := png.Encode(&small, image.NewRGBA(image.Rect(0, 0, 1, 1))); e != nil {
		t.Fatal(e)
	}
	payload := append([]byte("Comment\x00"), bytes.Repeat([]byte("x"), 3<<20)...)
	chunk := make([]byte, 12+len(payload))
	binary.BigEndian.PutUint32(chunk[:4], uint32(len(payload)))
	copy(chunk[4:8], "tEXt")
	copy(chunk[8:], payload)
	binary.BigEndian.PutUint32(chunk[len(chunk)-4:], crc32.ChecksumIEEE(chunk[4:len(chunk)-4]))
	data := append([]byte{}, small.Bytes()[:small.Len()-12]...)
	data = append(data, chunk...)
	data = append(data, small.Bytes()[small.Len()-12:]...)
	documentIDs := []any{}
	for i := 0; i < 4; i++ {
		// Give each valid PNG a distinct digest while preserving its size.
		data[len(data)-17] = byte('a' + i)
		binary.BigEndian.PutUint32(data[len(data)-16:len(data)-12], crc32.ChecksumIEEE(data[small.Len()-12+4:len(data)-16]))
		d := aiTestUpload(h, "page.png", data)
		documentIDs = append(documentIDs, d["documentId"])
	}
	j := aiTestMutation(h, "POST", "/api/v1/ai/import-jobs", map[string]any{"scopeId": "scope_default_group", "taskKind": "PARSER_PROFILE", "documentIds": documentIDs, "useModel": true, "providerId": provider["providerId"]}, 202, "")
	if e := h.a.ProcessAIOnce(context.Background()); e != nil {
		t.Fatal(e)
	}
	w := h.do("GET", "/api/v1/ai/import-jobs/"+Str(j, "jobId"), nil, nil)
	h.expect(w, 200, "")
	job := h.object(w)
	if Str(job, "state") != "FAILED" || Str(job, "errorCode") != "VISION_IMAGE_LIMIT" || calls.Load() != 0 {
		t.Fatal("over-budget images reached model", job, calls.Load())
	}
	var usage int
	if e := h.a.Management.QueryRow("SELECT count(*) FROM ai_usage").Scan(&usage); e != nil || usage != 0 {
		t.Fatal("over-budget request reserved model usage", usage, e)
	}
}

func TestAIDocumentStorageLimitAndConcurrentUploadLeaveNoResidue(t *testing.T) {
	t.Setenv("GW_AI_DOCUMENT_MAX_BYTES", "16")
	h := newHarness(t)
	aiTestUpload(h, "first.txt", []byte("12345678"))
	w := aiTestUploadResponse(h, "second.txt", []byte("123456789"))
	h.expect(w, 413, "")
	if !strings.Contains(w.Body.String(), "DOCUMENT_STORAGE_LIMIT") {
		t.Fatal(w.Body.String())
	}
	h.a.aiDocumentMu.Lock()
	w = aiTestUploadResponse(h, "third.txt", []byte("x"))
	h.a.aiDocumentMu.Unlock()
	h.expect(w, 429, "")
	if !strings.Contains(w.Body.String(), "DOCUMENT_UPLOAD_BUSY") {
		t.Fatal(w.Body.String())
	}
	files, e := os.ReadDir(filepath.Join(h.a.Config.DataDir, "ai-documents"))
	if e != nil || len(files) != 1 {
		t.Fatal("rejected upload left an original document behind", files, e)
	}
	var count, stored int
	if e = h.a.Management.QueryRow("SELECT count(*),sum(size_bytes) FROM ai_documents").Scan(&count, &stored); e != nil || count != 1 || stored != 8 {
		t.Fatal("rejected upload changed document storage", count, stored, e)
	}
}

func TestAIEntityPaginationDoesNotCollapseSameScopeVersion(t *testing.T) {
	for _, key := range []string{"modelId", "documentId", "providerId", "bindingId", "planId", "draftId", "validationId", "snapshotId"} {
		one := map[string]any{"scopeId": "same", "version": "1.0.0", key: "first"}
		two := map[string]any{"scopeId": "same", "version": "1.0.0", key: "second"}
		if pageKey(one) == pageKey(two) {
			t.Fatal("entity pagination identity collision", key)
		}
	}
	h := newHarness(t)
	first := aiTestPublish(h, aiTestContent())
	content := aiTestContent()
	content["modelName"] = "Another model, same scope and version"
	second := aiTestPublish(h, content)
	w := h.do("GET", "/api/v1/device-models?limit=1", nil, nil)
	h.expect(w, 200, "")
	page := h.object(w)
	seen := map[string]bool{}
	for _, row := range List(page, "items") {
		seen[Str(row.(map[string]any), "modelId")] = true
	}
	if Str(page, "nextCursor") == "" {
		t.Fatal("second template disappeared", page)
	}
	w = h.do("GET", "/api/v1/device-models?limit=1&cursor="+Str(page, "nextCursor"), nil, nil)
	h.expect(w, 200, "")
	page = h.object(w)
	for _, row := range List(page, "items") {
		seen[Str(row.(map[string]any), "modelId")] = true
	}
	if len(seen) != 2 || !seen[Str(first, "modelId")] || !seen[Str(second, "modelId")] || Str(page, "nextCursor") != "" {
		t.Fatal("template identity duplicated across pages", seen, page)
	}
}

func TestAIDOCXMediaWarningsAreDeduplicatedAcrossDocuments(t *testing.T) {
	var source bytes.Buffer
	zw := zip.NewWriter(&source)
	f, _ := zw.Create("word/document.xml")
	_, _ = f.Write([]byte("<document><p><t>Protocol with images.</t></p></document>"))
	for i := 0; i < 200; i++ {
		f, _ = zw.Create(fmt.Sprintf("word/media/image%d.png", i))
		_, _ = f.Write([]byte("embedded image placeholder"))
	}
	_ = zw.SetComment("A")
	if e := zw.Close(); e != nil {
		t.Fatal(e)
	}
	_, warnings, e := aiExtractDOCX(source.Bytes())
	if e != nil || len(warnings) != 1 || !strings.HasPrefix(warnings[0], "DOCX_IMAGES_REQUIRE_REVIEW") {
		t.Fatal("media warning multiplied by embedded file count", warnings, e)
	}
	h := newHarness(t)
	model := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		JSON(w, 200, map[string]any{"choices": []any{map[string]any{"finish_reason": "stop", "message": map[string]any{"content": encode(aiEmptyContent())}}}})
	}))
	defer model.Close()
	provider := aiTestProvider(h, model.URL, 10)
	documents := []any{}
	for i := 0; i < 2; i++ {
		data := append([]byte{}, source.Bytes()...)
		data[len(data)-1] = byte('A' + i) // Distinct valid ZIP comments.
		d := aiTestUpload(h, fmt.Sprintf("manual%d.docx", i), data)
		documents = append(documents, d["documentId"])
	}
	j := aiTestMutation(h, "POST", "/api/v1/ai/import-jobs", map[string]any{"scopeId": "scope_default_group", "taskKind": "PARSER_PROFILE", "documentIds": documents, "useModel": true, "providerId": provider["providerId"]}, 202, "")
	if e = h.a.ProcessAIOnce(context.Background()); e != nil {
		t.Fatal(e)
	}
	job, e := h.a.aiJob(context.Background(), h.a.Management, Str(j, "jobId"))
	if e != nil || Str(job, "state") != "NEEDS_REVIEW" {
		t.Fatal(job, e)
	}
	draft, e := h.a.aiDraft(context.Background(), h.a.Management, Str(job, "draftId"))
	if e != nil || len(List(Obj(draft, "content"), "issues")) != 1 {
		t.Fatal("same warning duplicated across documents", draft, e)
	}
}

func TestAIDraftReplayRechecksRevokedScopeBeforeReturningCachedContent(t *testing.T) {
	h := newHarness(t)
	id, _ := aiTestDraft(h, aiTestContent())
	actor := autoScopedPrincipal(t, h.a, "replay_developer", "developer", "scope_default_group")
	content := aiTestContent()
	content["manufacturer"] = "private-document-marker"
	request := func() *httptest.ResponseRecorder {
		p, e := h.a.principal(t.Context(), actor)
		if e != nil {
			t.Fatal(e)
		}
		r := httptest.NewRequest("PATCH", "/api/v1/protocol-drafts/"+id, strings.NewReader(encode(map[string]any{"content": content})))
		r.Header.Set("Content-Type", "application/json")
		r.Header.Set("Idempotency-Key", "scoped-draft-edit")
		r.Header.Set("If-Match", ETag(2))
		r = r.WithContext(context.WithValue(r.Context(), authKey{}, &authInfo{Principal: p}))
		router := chi.NewRouter()
		h.a.RegisterAI(router)
		w := httptest.NewRecorder()
		router.ServeHTTP(w, r)
		return w
	}
	w := request()
	h.expect(w, 200, "")
	if !strings.Contains(w.Body.String(), "private-document-marker") {
		t.Fatal("fixture did not store document-bearing response")
	}
	w = request()
	h.expect(w, 200, "")
	if _, e := h.a.Management.Exec("INSERT INTO resource_scopes(scope_id,kind,parent_scope_id,name,created_at_ms) VALUES('replay_other','group','scope_default_project','Other group',?)", Now()); e != nil {
		t.Fatal(e)
	}
	if _, e := h.a.Management.Exec("UPDATE role_bindings SET scope_id='replay_other' WHERE principal_id=?", actor); e != nil {
		t.Fatal(e)
	}
	w = request()
	h.expect(w, 403, "")
	if strings.Contains(w.Body.String(), "private-document-marker") {
		t.Fatal("cached draft leaked after scope access was revoked", w.Body.String())
	}
}

func TestAIDocumentHashReusePrecedesStorageQuota(t *testing.T) {
	t.Setenv("GW_AI_DOCUMENT_MAX_BYTES", "8")
	h := newHarness(t)
	first := aiTestUpload(h, "first.txt", []byte("12345678"))
	second := aiTestUpload(h, "renamed.txt", []byte("12345678"))
	if Str(first, "documentId") != Str(second, "documentId") {
		t.Fatal("same scoped document hash created another document", first, second)
	}
	if _, found := second["evidence"]; found {
		t.Fatal("upload reuse returned raw evidence")
	}
	files, e := os.ReadDir(filepath.Join(h.a.Config.DataDir, "ai-documents"))
	if e != nil || len(files) != 1 {
		t.Fatal("deduplicated upload left a duplicate original", files, e)
	}
	var count, stored int
	if e = h.a.Management.QueryRow("SELECT count(*),sum(size_bytes) FROM ai_documents").Scan(&count, &stored); e != nil || count != 1 || stored != 8 {
		t.Fatal("deduplicated upload consumed quota", count, stored, e)
	}
}
