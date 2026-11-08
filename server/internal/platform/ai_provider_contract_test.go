package platform

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
)

const aiContractCredential = "contract-key-must-never-appear-in-response"

func aiContractProvider(t *testing.T, h *apiHarness, endpoint, kind, schemaMode, tokenParameter string, vision bool) (map[string]any, string) {
	t.Helper()
	provider := aiTestMutation(h, "POST", "/api/v1/ai/providers", map[string]any{
		"name": "Adapter contract", "baseUrl": endpoint, "kind": kind, "mode": "LOCAL",
		"allowInsecureLocal": true, "model": "contract-model", "enabled": true,
		"schemaMode": schemaMode, "tokenParameter": tokenParameter, "vision": vision,
		"maxOutputTokens": 512, "dailyRequestLimit": 20, "apiKey": aiContractCredential,
	}, 201, "")
	cfg, e := h.a.aiProvider(t.Context(), Str(provider, "providerId"))
	if e != nil {
		t.Fatal(e)
	}
	var actor string
	if e = h.a.Management.QueryRow("SELECT principal_id FROM principals WHERE name='admin'").Scan(&actor); e != nil {
		t.Fatal(e)
	}
	return cfg, actor
}

func aiContractCompletion(kind, content string) map[string]any {
	if kind == "OLLAMA" {
		return map[string]any{"done": true, "done_reason": "stop", "message": map[string]any{"role": "assistant", "content": content}, "prompt_eval_count": 23, "eval_count": 7}
	}
	return map[string]any{"choices": []any{map[string]any{"finish_reason": "stop", "message": map[string]any{"role": "assistant", "content": content}}}, "usage": map[string]any{"prompt_tokens": 23, "completion_tokens": 7}}
}

// These tests exercise the actual HTTP adapter and persistent usage accounting;
// no external model, credentials or network destination is required.
func TestAIProviderAdapterSuccessfulWireContracts(t *testing.T) {
	h := newHarness(t)
	schema := map[string]any{"type": "object", "properties": map[string]any{"ok": map[string]any{"type": "boolean"}}, "required": []string{"ok"}, "additionalProperties": false}
	const dataURL = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jDXsAAAAASUVORK5CYII="
	for _, tc := range []struct {
		name, kind, schemaMode, tokenParameter string
		vision                                 bool
	}{
		{"ollama_json_text", "OLLAMA", "JSON_OBJECT", "max_tokens", false},
		{"ollama_schema_vision", "OLLAMA", "JSON_SCHEMA", "max_tokens", true},
		{"compatible_schema_text", "OPENAI_COMPATIBLE", "JSON_SCHEMA", "max_tokens", false},
		{"compatible_schema_vision", "OPENAI_COMPATIBLE", "JSON_SCHEMA", "max_completion_tokens", true},
		{"compatible_json_text", "OPENAI_COMPATIBLE", "JSON_OBJECT", "max_tokens", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			captured := make(chan map[string]any, 1)
			fake := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				path := "/v1/chat/completions"
				if tc.kind == "OLLAMA" {
					path = "/api/chat"
				}
				if r.Method != "POST" || r.URL.Path != path || r.Header.Get("Content-Type") != "application/json" {
					t.Errorf("unexpected adapter request %s %s %s", r.Method, r.URL.Path, r.Header.Get("Content-Type"))
				}
				if r.Header.Get("Authorization") != "Bearer "+aiContractCredential {
					t.Error("credential did not reach selected model endpoint")
				}
				body := map[string]any{}
				if e := json.NewDecoder(r.Body).Decode(&body); e != nil {
					t.Errorf("request body is not JSON: %v", e)
				}
				captured <- body
				JSON(w, 200, aiContractCompletion(tc.kind, `{"ok":true}`))
			}))
			defer fake.Close()
			endpoint := fake.URL
			if tc.kind != "OLLAMA" {
				endpoint += "/v1/"
			}
			cfg, actor := aiContractProvider(t, h, endpoint, tc.kind, tc.schemaMode, tc.tokenParameter, tc.vision)
			var user any = "Document text"
			if tc.vision {
				user = map[string]any{"text": "Document text", "images": []string{dataURL}}
			}
			job := ID("contract")
			out, usage, e := h.a.aiCallModel(t.Context(), cfg, actor, job, "Return one JSON object.", user, schema)
			if e != nil || !aiBool(out, "ok") || Num(usage, "inputTokens") != 23 || Num(usage, "outputTokens") != 7 {
				t.Fatalf("successful completion failed: output=%v usage=%v error=%v", out, usage, e)
			}
			body := <-captured
			if Str(body, "model") != "contract-model" || body["stream"] != false || body["tools"] != nil || strings.Contains(encode(body), aiContractCredential) {
				t.Fatal("unexpected model invocation options", body)
			}
			messages := List(body, "messages")
			if len(messages) != 2 || Str(messages[0].(map[string]any), "role") != "system" || Str(messages[0].(map[string]any), "content") != "Return one JSON object." {
				t.Fatal("system instruction not preserved", messages)
			}
			message := messages[1].(map[string]any)
			if Str(message, "role") != "user" {
				t.Fatal("document not sent in user role", message)
			}
			if tc.kind == "OLLAMA" {
				if Num(Obj(body, "options"), "num_predict") != 512 || Num(Obj(body, "options"), "temperature") != 0 {
					t.Fatal("Ollama generation limit missing", body)
				}
				if tc.schemaMode == "JSON_OBJECT" && body["format"] != "json" || tc.schemaMode == "JSON_SCHEMA" && encode(body["format"]) != encode(schema) {
					t.Fatal("Ollama response format mismatch", body)
				}
				if Str(message, "content") != "Document text" {
					t.Fatal("Ollama user text changed", message)
				}
				if tc.vision && !reflect.DeepEqual(aiStringList(message["images"]), []string{strings.SplitN(dataURL, ",", 2)[1]}) {
					t.Fatal("Ollama images must be bare base64", message)
				}
			} else {
				if Num(body, tc.tokenParameter) != 512 || body["store"] != false {
					t.Fatal("compatible token cap or storage option missing", body)
				}
				otherToken := "max_completion_tokens"
				if tc.tokenParameter == otherToken {
					otherToken = "max_tokens"
				}
				if body[otherToken] != nil {
					t.Fatal("conflicting token parameter sent", body)
				}
				format := Obj(body, "response_format")
				if tc.schemaMode == "JSON_SCHEMA" {
					if Str(format, "type") != "json_schema" || encode(Obj(Obj(format, "json_schema"), "schema")) != encode(schema) {
						t.Fatal("compatible JSON schema contract changed", format)
					}
				} else if Str(format, "type") != "json_object" {
					t.Fatal("compatible JSON object contract changed", format)
				}
				if tc.vision {
					parts := List(message, "content")
					if len(parts) != 2 || Str(parts[0].(map[string]any), "type") != "text" || Str(parts[0].(map[string]any), "text") != "Document text" || Str(parts[1].(map[string]any), "type") != "image_url" || Str(Obj(parts[1].(map[string]any), "image_url"), "url") != dataURL {
						t.Fatal("compatible vision content changed", message)
					}
				} else if Str(message, "content") != "Document text" {
					t.Fatal("compatible user text changed", message)
				}
			}
			var state, recorded string
			if e = h.a.Management.QueryRow("SELECT state,usage_json FROM ai_usage WHERE provider_id=? AND job_id=?", Str(cfg, "providerId"), job).Scan(&state, &recorded); e != nil || state != "COMPLETED" || Num(aiMap(recorded), "outputTokens") != 7 {
				t.Fatal("successful usage not committed", state, recorded, e)
			}
		})
	}
}

func TestAIProviderAdapterRejectsIncompleteAndUnsafeResponses(t *testing.T) {
	h := newHarness(t)
	for _, tc := range []struct {
		name, kind, code string
		status           int
		mutate           func(map[string]any)
		content, suffix  string
	}{
		{"compatible_length", "OPENAI_COMPATIBLE", "MODEL_OUTPUT_INCOMPLETE", 200, func(m map[string]any) { List(m, "choices")[0].(map[string]any)["finish_reason"] = "length" }, `{"ok":true}`, ""},
		{"compatible_refusal", "OPENAI_COMPATIBLE", "MODEL_REFUSED", 200, func(m map[string]any) {
			Obj(List(m, "choices")[0].(map[string]any), "message")["refusal"] = aiContractCredential
		}, `{"ok":true}`, ""},
		{"compatible_trailing_content", "OPENAI_COMPATIBLE", "MODEL_OUTPUT_INVALID", 200, nil, `{"ok":true} {"extra":true}`, ""},
		{"compatible_trailing_envelope", "OPENAI_COMPATIBLE", "MODEL_OUTPUT_INVALID", 200, nil, `{"ok":true}`, ` {"extra":true}`},
		{"ollama_length", "OLLAMA", "MODEL_OUTPUT_INCOMPLETE", 200, func(m map[string]any) { m["done_reason"] = "length" }, `{"ok":true}`, ""},
		{"ollama_not_done", "OLLAMA", "MODEL_OUTPUT_INCOMPLETE", 200, func(m map[string]any) { m["done"] = false }, `{"ok":true}`, ""},
		{"ollama_trailing_content", "OLLAMA", "MODEL_OUTPUT_INVALID", 200, nil, `{"ok":true} {"extra":true}`, ""},
		{"compatible_http_500", "OPENAI_COMPATIBLE", "MODEL_UNAVAILABLE", 500, nil, aiContractCredential, ""},
		{"ollama_http_429", "OLLAMA", "MODEL_RATE_LIMITED", 429, nil, aiContractCredential, ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			fake := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.Header.Get("Authorization") != "Bearer "+aiContractCredential {
					t.Error("test credential did not reach endpoint")
				}
				response := aiContractCompletion(tc.kind, tc.content)
				if tc.mutate != nil {
					tc.mutate(response)
				}
				w.Header().Set("Content-Type", "application/json")
				w.WriteHeader(tc.status)
				_, _ = w.Write([]byte(encode(response) + tc.suffix))
			}))
			defer fake.Close()
			cfg, actor := aiContractProvider(t, h, fake.URL, tc.kind, "JSON_OBJECT", "max_tokens", false)
			job := ID("contract")
			out, usage, e := h.a.aiCallModel(t.Context(), cfg, actor, job, "Return JSON.", "Document text", map[string]any{"type": "object"})
			var problem *APIError
			if !errors.As(e, &problem) || problem.Code != tc.code || out != nil {
				t.Fatalf("unexpected rejection: output=%v error=%v expected=%s", out, e, tc.code)
			}
			if strings.Contains(e.Error(), aiContractCredential) || strings.Contains(encode(usage), aiContractCredential) {
				t.Fatal("provider response disclosed credential")
			}
			var state, recorded string
			if e = h.a.Management.QueryRow("SELECT state,usage_json FROM ai_usage WHERE provider_id=? AND job_id=?", Str(cfg, "providerId"), job).Scan(&state, &recorded); e != nil || state != "FAILED_OR_UNCERTAIN" || strings.Contains(recorded, aiContractCredential) {
				t.Fatal("invalid completion was recorded as successful or leaked response", state, e)
			}
			// Also prove the admin-facing check wrapper and its idempotent result
			// do not return provider diagnostic bodies or refusal text.
			check := aiTestMutation(h, "POST", "/api/v1/ai/providers/"+Str(cfg, "providerId")+"/checks", map[string]any{}, 200, "")
			if aiBool(check, "passed") || Str(check, "errorCode") != tc.code || strings.Contains(encode(check), aiContractCredential) {
				t.Fatal("public check response bypassed safe rejection", check)
			}
		})
	}
}
