package platform

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestAIAPIResponseContracts(t *testing.T) {
	h := newHarness(t)
	check := func(schema string, v map[string]any) {
		t.Helper()
		if e := h.a.Validate(schema, v); e != nil {
			t.Fatalf("%s response contract failed: %v; %s", schema, e, encode(v))
		}
	}
	request := func(method, path, schema string, body any, status int, etag string) map[string]any {
		t.Helper()
		var v map[string]any
		if method == "GET" {
			w := h.do(method, path, nil, nil)
			h.expect(w, status, "")
			v = h.object(w)
		} else {
			v = aiTestMutation(h, method, path, body, status, etag)
		}
		check(schema, v)
		return v
	}
	request("GET", "/api/v1/ai/status", "AIStatus", nil, 200, "")
	provider := request("POST", "/api/v1/ai/providers", "AIProvider", map[string]any{"name": "Unconfigured local", "kind": "OLLAMA", "mode": "LOCAL", "baseUrl": "http://127.0.0.1:11434", "model": "fixture", "allowInsecureLocal": true}, 201, "")
	request("PATCH", "/api/v1/ai/providers/"+Str(provider, "providerId"), "AIProvider", map[string]any{"name": "Updated local"}, 200, ETag(1))
	request("GET", "/api/v1/ai/providers", "AIProviderPage", nil, 200, "")
	doc := aiTestUpload(h, "reference.txt", []byte("Little endian signed temperature, scale 0.01."))
	check("SourceDocument", doc)
	request("GET", "/api/v1/source-documents/"+Str(doc, "documentId")+"/evidence", "DocumentEvidencePage", nil, 200, "")
	job := request("POST", "/api/v1/ai/import-jobs", "AIImportJob", map[string]any{"scopeId": "scope_default_group", "taskKind": "PARSER_PROFILE", "documentIds": []any{doc["documentId"]}, "useModel": false}, 202, "")
	request("GET", "/api/v1/ai/import-jobs", "AIImportJobPage", nil, 200, "")
	request("GET", "/api/v1/ai/import-jobs/"+Str(job, "jobId"), "AIImportJob", nil, 200, "")
	path := "/api/v1/protocol-drafts/" + Str(job, "draftId")
	request("GET", path, "ProtocolDraft", nil, 200, "")
	draft := request("PATCH", path, "ProtocolDraft", map[string]any{"content": aiTestContent()}, 200, ETag(1))
	validation := request("POST", path+"/validations", "ProtocolValidation", map[string]any{"draftRevision": draft["revision"]}, 200, "")
	model := request("POST", path+"/publications", "DeviceModel", map[string]any{"draftRevision": draft["revision"], "validationId": validation["validationId"], "version": "1.0.0"}, 201, "")
	request("GET", "/api/v1/device-models", "DeviceModelPage", nil, 200, "")
	request("GET", "/api/v1/device-models/"+Str(model, "modelId"), "DeviceModel", nil, 200, "")
	plan := request("POST", "/api/v1/ai/apply-plans", "AIApplyPlan", map[string]any{"modelId": model["modelId"], "sourceContext": map[string]any{"pageId": "P24"}}, 201, "")
	request("GET", "/api/v1/ai/apply-plans/"+Str(plan, "planId"), "AIApplyPlan", nil, 200, "")
	request("POST", "/api/v1/ai/apply-plans/"+Str(plan, "planId")+"/executions", "AIApplyPlan", map[string]any{"confirmed": true}, 200, "")
	fake := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		JSON(w, 200, map[string]any{"choices": []any{map[string]any{"finish_reason": "stop", "message": map[string]any{"content": "{\"ok\":true}"}}}})
	}))
	defer fake.Close()
	p := aiTestProvider(h, fake.URL, 1)
	request("POST", "/api/v1/ai/providers/"+Str(p, "providerId")+"/checks", "AIProviderCheck", map[string]any{}, 200, "")
	request("POST", "/api/v1/ai/providers/"+Str(p, "providerId")+"/checks", "AIProviderCheck", map[string]any{}, 200, "")
}

func TestAIPythonPDFFallbackBoundaries(t *testing.T) {
	interpreter := os.Getenv("GW_AI_TEST_PYTHON")
	if interpreter == "" {
		t.Skip("set GW_AI_TEST_PYTHON to run the isolated local pypdf worker integration")
	}
	t.Setenv("GW_AI_PYTHON", interpreter)
	t.Setenv("GW_AI_PDFTOTEXT", filepath.Join(t.TempDir(), "not-installed"))
	dir := t.TempDir()
	script := "from pypdf import PdfWriter\nfrom pypdf.generic import DictionaryObject,NameObject,DecodedStreamObject\nfrom pathlib import Path\nimport sys\np=Path(sys.argv[1])\nw=PdfWriter();page=w.add_blank_page(width=612,height=792)\nfont=DictionaryObject({NameObject('/Type'):NameObject('/Font'),NameObject('/Subtype'):NameObject('/Type1'),NameObject('/BaseFont'):NameObject('/Helvetica')})\npage[NameObject('/Resources')]=DictionaryObject({NameObject('/Font'):DictionaryObject({NameObject('/F1'):w._add_object(font)})})\ns=DecodedStreamObject();s.set_data(b'BT /F1 12 Tf 72 720 Td (BLE temperature protocol 1.0) Tj ET');page[NameObject('/Contents')]=w._add_object(s)\nw.write(p/'text.pdf')\nx=PdfWriter()\nfor _ in range(201):x.add_blank_page(width=612,height=792)\nx.write(p/'too-many.pdf')\ny=PdfWriter();y.add_blank_page(width=612,height=792);y.encrypt('fixture-password');y.write(p/'encrypted.pdf')\nz=PdfWriter()\nfor _ in range(200):z.add_blank_page(width=612,height=792)\nz.write(p/'limit.pdf')\n"
	cmd := exec.Command(interpreter, "-I", "-c", script, dir)
	if out, e := cmd.CombinedOutput(); e != nil {
		t.Fatalf("fixture creation: %v %s", e, out)
	}
	text, e := aiExtractPDF(context.Background(), filepath.Join(dir, "text.pdf"))
	if e != nil || !strings.Contains(text, "BLE temperature protocol 1.0") || !strings.HasSuffix(text, "\f") {
		t.Fatal("text extraction", text, e)
	}
	text, e = aiExtractPDF(context.Background(), filepath.Join(dir, "limit.pdf"))
	if e != nil || strings.Count(text, "\f") != 200 {
		t.Fatal("200-page boundary", len(text), e)
	}
	for _, tc := range []struct{ file, code string }{{"too-many.pdf", "DOCUMENT_PAGE_LIMIT"}, {"encrypted.pdf", "DOCUMENT_ENCRYPTED"}} {
		_, e = aiExtractPDF(context.Background(), filepath.Join(dir, tc.file))
		var api *APIError
		if !errors.As(e, &api) || api.Code != tc.code {
			t.Fatalf("%s: expected %s, got %v", tc.file, tc.code, e)
		}
	}
}
