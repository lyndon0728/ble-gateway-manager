package platform

import (
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"net/http"
	"regexp"
	"strings"
	"unicode/utf8"
)

func aiValidateContentShape(c map[string]any) error {
	if c == nil || len(encode(c)) > 1<<20 {
		return Problem(422, "DRAFT_TOO_LARGE", "draft content must be an object below 1 MiB")
	}
	allowed := map[string]bool{"manufacturer": true, "modelName": true, "firmwareRange": true, "parserSpec": true, "connectionSpec": true, "mappingSpec": true, "tests": true, "sources": true, "issues": true}
	for k := range c {
		if !allowed[k] {
			return Problem(422, "DRAFT_FIELD_UNSUPPORTED", "unsupported draft section: "+k)
		}
	}
	for _, k := range []string{"manufacturer", "modelName", "firmwareRange"} {
		v, ok := c[k].(string)
		if !ok || len(v) > 256 {
			return Problem(422, "DRAFT_FIELD_INVALID", k+" must be a string up to 256 bytes")
		}
	}
	for _, k := range []string{"parserSpec", "connectionSpec", "mappingSpec"} {
		if c[k] != nil {
			if _, ok := c[k].(map[string]any); !ok {
				return Problem(422, "DRAFT_FIELD_INVALID", k+" must be object or null")
			}
		}
	}
	for k, max := range map[string]int{"tests": 64, "sources": 256, "issues": 128} {
		v, ok := c[k].([]any)
		if !ok || len(v) > max {
			return Problem(422, "DRAFT_FIELD_INVALID", k+" must be an array within its size limit")
		}
		for _, item := range v {
			if _, ok := item.(map[string]any); !ok {
				return Problem(422, "DRAFT_FIELD_INVALID", k+" entries must be objects")
			}
		}
	}
	return nil
}
func (a *App) aiDraft(ctx context.Context, q ctlQuery, id string) (map[string]any, error) {
	var job, scope, state, content, validation, published string
	var rev, created, updated int64
	e := q.QueryRowContext(ctx, "SELECT job_id,scope_id,revision,state,content_json,validation_json,published_model_id,created_at_ms,updated_at_ms FROM ai_drafts WHERE draft_id=?", id).Scan(&job, &scope, &rev, &state, &content, &validation, &published, &created, &updated)
	if e != nil {
		return nil, aiNotFound(e, "DRAFT_NOT_FOUND")
	}
	return map[string]any{"draftId": id, "jobId": job, "scopeId": scope, "revision": rev, "state": state, "content": aiMap(content), "validation": aiMap(validation), "publishedModelId": published, "createdAt": Timestamp(created), "updatedAt": Timestamp(updated)}, nil
}
func (a *App) aiGetDraft(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:write")
	if !ok {
		return
	}
	d, e := a.aiDraft(r.Context(), a.Management, aiParam(r, "draftId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !a.Can(p, "profile:write", Str(d, "scopeId")) || !a.Can(p, "document:read", Str(d, "scopeId")) {
		Fail(w, r, 403, "FORBIDDEN", "draft scope is not accessible")
		return
	}
	w.Header().Set("ETag", ETag(Num(d, "revision")))
	JSON(w, 200, d)
}
func (a *App) aiPatchDraft(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:write")
	if !ok {
		return
	}
	body, ok := aiDecode(w, r, "content")
	if !ok {
		return
	}
	content, ok := body["content"].(map[string]any)
	if !ok {
		Fail(w, r, 422, "DRAFT_FIELD_INVALID", "content must be an object")
		return
	}
	if e := aiValidateContentShape(content); e != nil {
		RespondError(w, r, e)
		return
	}
	id := aiParam(r, "draftId")
	a.aiMutation(w, r, p, "ai.draft.patch:"+id, map[string]any{"body": body, "etag": r.Header.Get("If-Match")}, func(tx *sql.Tx) (int, string, any, error) {
		d, e := a.aiDraft(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, e
		}
		if !canQuery(r.Context(), tx, p, "profile:write", Str(d, "scopeId")) || !canQuery(r.Context(), tx, p, "document:read", Str(d, "scopeId")) {
			return 0, "", nil, Problem(403, "FORBIDDEN", "draft scope is not accessible")
		}
		if Str(d, "state") == "PUBLISHED" {
			return 0, "", nil, Problem(409, "DRAFT_IMMUTABLE", "published drafts cannot be changed; create a new import")
		}
		if e = Match(r, Num(d, "revision")); e != nil {
			return 0, "", nil, e
		}
		_, e = tx.Exec("UPDATE ai_drafts SET revision=revision+1,state='DRAFT',content_json=?,validation_json='{}',updated_at_ms=? WHERE draft_id=?", encode(content), Now(), id)
		if e != nil {
			return 0, "", nil, e
		}
		d["content"] = content
		d["revision"] = Num(d, "revision") + 1
		d["state"] = "DRAFT"
		d["validation"] = map[string]any{}
		e = a.Audit(r.Context(), tx, p.ID, "ai.draft.edit", "protocol_draft", id, map[string]any{"revision": d["revision"], "contentDigest": Hash(content)})
		return 200, id, d, e
	})
}

func aiValidationError(code, message, path string) map[string]any {
	return map[string]any{"code": code, "message": message, "path": path}
}
func (a *App) aiEvaluateDraft(ctx context.Context, d map[string]any) (map[string]any, error) {
	c := Obj(d, "content")
	errs := []any{}
	results := []any{}
	add := func(code, message, path string) { errs = append(errs, aiValidationError(code, message, path)) }
	if e := aiValidateContentShape(c); e != nil {
		add("DRAFT_FIELD_INVALID", e.Error(), "content")
	}
	if strings.TrimSpace(Str(c, "modelName")) == "" {
		add("MODEL_NAME_REQUIRED", "Provide the device model or mapping template name.", "modelName")
	}
	if len(Obj(c, "parserSpec")) == 0 && len(Obj(c, "connectionSpec")) == 0 && len(Obj(c, "mappingSpec")) == 0 {
		add("EMPTY_PROFILE", "At least one supported parser, connection or mapping specification is required.", "content")
	}
	for i, v := range List(c, "issues") {
		m, _ := v.(map[string]any)
		if !aiBool(m, "resolved") {
			add("UNRESOLVED_ISSUE", Str(m, "message"), fmt.Sprintf("issues.%d", i))
		} else if len(strings.TrimSpace(Str(m, "resolution"))) < 3 {
			add("RESOLUTION_REQUIRED", "Resolved issues need a review note.", fmt.Sprintf("issues.%d", i))
		}
	}
	j, e := a.aiJob(ctx, a.Management, Str(d, "jobId"))
	if e != nil {
		return nil, e
	}
	evidence := map[string]map[string]any{}
	for _, id := range aiStringList(j["documentIds"]) {
		doc, e := a.aiDocument(ctx, a.Management, id)
		if e != nil {
			return nil, e
		}
		for _, v := range List(doc, "evidence") {
			m, _ := v.(map[string]any)
			evidence[Str(m, "evidenceId")] = m
		}
	}
	sourcePaths := []string{}
	for i, v := range List(c, "sources") {
		s, _ := v.(map[string]any)
		path := Str(s, "path")
		if path == "" {
			add("SOURCE_PATH_REQUIRED", "Source path is required.", fmt.Sprintf("sources.%d", i))
			continue
		}
		if Str(s, "state") == "USER" {
			if utf8.RuneCountInString(strings.TrimSpace(Str(s, "note"))) < 5 {
				add("USER_SOURCE_NOTE_REQUIRED", "Manual sources need at least five characters describing the basis.", path)
				continue
			}
		} else if Str(s, "state") == "DOCUMENT" {
			ev, found := evidence[Str(s, "evidenceId")]
			quote := strings.TrimSpace(Str(s, "quote"))
			if !found || quote == "" || !strings.Contains(Str(ev, "text"), quote) {
				add("EVIDENCE_UNVERIFIED", "Source quote must match the uploaded document text. Visual transcriptions require a USER review note.", path)
				continue
			}
		} else {
			add("SOURCE_STATE_INVALID", "Source state must be DOCUMENT or USER.", path)
			continue
		}
		sourcePaths = append(sourcePaths, path)
	}
	hasSource := func(path string) bool {
		for _, p := range sourcePaths {
			if path == p || strings.HasPrefix(path, p+".") {
				return true
			}
		}
		return false
	}
	parser := Obj(c, "parserSpec")
	if len(parser) > 0 {
		if e := ValidateProtocolSpec(parser); e != nil {
			add("PARSER_INVALID", e.Error(), "parserSpec")
		}
		for i := range List(parser, "fields") {
			path := fmt.Sprintf("parserSpec.fields.%d", i)
			if !hasSource(path) {
				add("EVIDENCE_REQUIRED", "Protocol fields require a document source or explicit manual basis.", path)
			}
		}
		// Header and layout assumptions also need review, not only decoded field names.
		if !hasSource("parserSpec") {
			add("LAYOUT_REVIEW_REQUIRED", "Add a parserSpec source/review for length, framing, byte order and checksum assumptions.", "parserSpec")
		}
	}
	connection := Obj(c, "connectionSpec")
	if len(connection) > 0 {
		if e := aiValidateConnection(connection); e != nil {
			add("CONNECTION_INVALID", e.Error(), "connectionSpec")
		}
		if !hasSource("connectionSpec") {
			add("EVIDENCE_REQUIRED", "Connection UUIDs, security and operation require a source/review.", "connectionSpec")
		}
	}
	mapping := Obj(c, "mappingSpec")
	if len(mapping) > 0 {
		if e := aiValidateMappingKeys(mapping); e != nil {
			add("MAPPING_INVALID", e.Error(), "mappingSpec")
		}
		if e := ValidateOutputMapping(map[string]any{"mappingVersion": "ai-draft", "mapping": mapping}); e != nil {
			add("MAPPING_INVALID", e.Error(), "mappingSpec")
		}
		if !hasSource("mappingSpec") {
			add("EVIDENCE_REQUIRED", "Mapping requires a document source or manual review.", "mappingSpec")
		}
	}
	validIndependent, negative := 0, 0
	for index, v := range List(c, "tests") {
		test, _ := v.(map[string]any)
		name := Str(test, "name")
		if name == "" {
			name = fmt.Sprintf("Sample %d", index+1)
		}
		outputs := []any{}
		testErr := ""
		var e error
		if len(parser) > 0 {
			var raw []byte
			if hexText := Str(test, "payloadHex"); hexText != "" {
				raw, e = hex.DecodeString(strings.Join(strings.Fields(hexText), ""))
			} else {
				raw, e = base64.StdEncoding.Strict().DecodeString(Str(test, "payloadBase64"))
			}
			if e == nil {
				var records []ParsedRecord
				records, e = ParseProtocolSpec(parser, raw)
				for _, record := range records {
					if e != nil {
						break
					}
					data := record.Data
					if len(mapping) > 0 {
						data, e = MapOutputData(data, map[string]any{"mappingVersion": "ai-draft", "mapping": mapping})
						if e != nil {
							outputs = []any{}
							break
						}
					}
					outputs = append(outputs, data)
				}
			}
		} else if len(mapping) > 0 {
			var out map[string]any
			out, e = MapOutputData(Obj(test, "input"), map[string]any{"mappingVersion": "ai-draft", "mapping": mapping})
			if e == nil {
				outputs = append(outputs, out)
			}
		} else {
			e = errors.New("only parser and mapping specifications support offline samples")
		}
		if e != nil {
			testErr = e.Error()
		}
		expectedError := aiBool(test, "expectError")
		passed := false
		if expectedError {
			// Malformed sample encodings do not count as a protocol rejection test.
			if len(parser) > 0 {
				raw, decodeErr := hex.DecodeString(strings.Join(strings.Fields(Str(test, "payloadHex")), ""))
				if Str(test, "payloadHex") == "" {
					raw, decodeErr = base64.StdEncoding.Strict().DecodeString(Str(test, "payloadBase64"))
				}
				_ = raw
				if decodeErr != nil {
					e = nil
					testErr = "invalid sample encoding is not a protocol negative test"
				}
			}
			passed = e != nil
			if passed {
				negative++
			}
		} else if e == nil {
			expected, exists := test["expected"]
			passed = exists && Hash(expected) == Hash(outputs)
			if !passed {
				testErr = "expected output does not match actual output"
			}
			if passed && (Str(test, "source") == "DOCUMENT" || Str(test, "source") == "USER") {
				validIndependent++
			}
		}
		if !passed {
			add("SAMPLE_FAILED", name+": "+testErr, fmt.Sprintf("tests.%d", index))
		}
		results = append(results, map[string]any{"name": name, "passed": passed, "outputs": outputs, "errors": testErr})
	}
	if len(parser) > 0 || len(mapping) > 0 {
		if validIndependent < 1 {
			add("INDEPENDENT_SAMPLE_REQUIRED", "At least one passing DOCUMENT or USER sample is required; model-generated examples alone are insufficient.", "tests")
		}
		if negative < 1 {
			add("NEGATIVE_SAMPLE_REQUIRED", "At least one correctly encoded malformed/invalid packet or missing-field mapping test must be rejected.", "tests")
		}
	}
	return map[string]any{"validationId": ID("validation"), "draftRevision": Num(d, "revision"), "contentDigest": Hash(c), "passed": len(errs) == 0, "errors": errs, "testResults": results, "verifiedAt": Timestamp(Now()), "verificationLevel": "DOCUMENT_REVIEWED", "independentSamples": validIndependent, "negativeSamples": negative}, nil
}
func aiValidateConnection(c map[string]any) error {
	allowed := map[string]bool{"serviceUuid": true, "characteristicUuid": true, "security": true, "operation": true, "mode": true, "valueBase64": true, "verification": true}
	for k := range c {
		if !allowed[k] {
			return fmt.Errorf("unsupported connection parameter %s", k)
		}
		if _, ok := c[k].(string); !ok {
			return fmt.Errorf("%s must be a string", k)
		}
	}
	for _, k := range []string{"serviceUuid", "characteristicUuid"} {
		if gw32CanonicalUUID(Str(c, k)) == "" {
			return fmt.Errorf("%s requires a valid UUID", k)
		}
	}
	if Str(c, "security") != "UNENCRYPTED" && Str(c, "security") != "BONDED_ENCRYPTED" {
		return errors.New("security must be UNENCRYPTED or BONDED_ENCRYPTED; pairing is not automatically supported")
	}
	if mode := Str(c, "mode"); mode != "" && mode != "NOTIFY" && mode != "INDICATE" {
		return errors.New("mode must be NOTIFY or INDICATE")
	}
	if v := Str(c, "verification"); v != "" && v != "NONE" && v != "READBACK" {
		return errors.New("verification must be NONE or READBACK")
	}
	if value := Str(c, "valueBase64"); value != "" {
		b, e := base64.StdEncoding.Strict().DecodeString(value)
		if e != nil || len(b) > 512 {
			return errors.New("valueBase64 must be a supported bounded encoded command")
		}
	}
	switch Str(c, "operation") {
	case "GATT_READ":
	case "GATT_WRITE":
		b, e := base64.StdEncoding.Strict().DecodeString(Str(c, "valueBase64"))
		if e != nil || len(b) == 0 || len(b) > 512 {
			return errors.New("write value must encode 1..512 bytes; actual MTU is checked at execution")
		}
		if Str(c, "verification") != "NONE" && Str(c, "verification") != "READBACK" {
			return errors.New("write verification must be NONE or READBACK")
		}
	case "GATT_SUBSCRIBE":
		if Str(c, "mode") != "NOTIFY" && Str(c, "mode") != "INDICATE" {
			return errors.New("subscription mode must be NOTIFY or INDICATE")
		}
	default:
		return errors.New("only read, WITH_RESPONSE write and subscription templates are supported")
	}
	return nil
}
func aiValidateMappingKeys(m map[string]any) error {
	for key := range m {
		if key != "fields" && key != "constants" && key != "wrapKey" {
			return fmt.Errorf("unsupported mapping parameter %s", key)
		}
	}
	for _, v := range List(m, "fields") {
		field, ok := v.(map[string]any)
		if !ok {
			return errors.New("mapping field must be object")
		}
		for key := range field {
			if key != "source" && key != "target" && key != "scale" && key != "offset" && key != "unit" {
				return fmt.Errorf("unsupported mapping field parameter %s", key)
			}
		}
	}
	return nil
}
func (a *App) aiValidateDraft(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:write")
	if !ok {
		return
	}
	body, ok := aiDecode(w, r, "draftRevision")
	if !ok {
		return
	}
	id := aiParam(r, "draftId")
	d, e := a.aiDraft(r.Context(), a.Management, id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if !a.Can(p, "profile:write", Str(d, "scopeId")) || !a.Can(p, "document:read", Str(d, "scopeId")) {
		Fail(w, r, 403, "FORBIDDEN", "draft scope is not accessible")
		return
	}
	if Num(body, "draftRevision") != Num(d, "revision") {
		Fail(w, r, 409, "VALIDATION_STALE", "draft revision changed")
		return
	}
	validation, e := a.aiEvaluateDraft(r.Context(), d)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	a.aiMutation(w, r, p, "ai.draft.validate:"+id, body, func(tx *sql.Tx) (int, string, any, error) {
		current, e := a.aiDraft(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, e
		}
		if Num(current, "revision") != Num(body, "draftRevision") || Str(current, "state") == "PUBLISHED" {
			return 0, "", nil, Problem(409, "VALIDATION_STALE", "draft changed or is already published")
		}
		state := "DRAFT"
		if aiBool(validation, "passed") {
			state = "VALIDATED"
		}
		if _, e = tx.Exec("UPDATE ai_drafts SET state=?,validation_json=?,updated_at_ms=? WHERE draft_id=?", state, encode(validation), Now(), id); e != nil {
			return 0, "", nil, e
		}
		e = a.Audit(r.Context(), tx, p.ID, "ai.draft.validate", "protocol_draft", id, map[string]any{"passed": validation["passed"], "draftRevision": body["draftRevision"]})
		return 200, id, validation, e
	})
}

var aiVersionPattern = regexp.MustCompile("^[0-9]+\\.[0-9]+\\.[0-9]+(?:-[A-Za-z0-9.-]+)?$")

func (a *App) aiPublishDraft(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:publish")
	if !ok {
		return
	}
	body, ok := aiDecode(w, r, "draftRevision", "validationId", "version")
	if !ok {
		return
	}
	id := aiParam(r, "draftId")
	version := Str(body, "version")
	if !aiVersionPattern.MatchString(version) || len(version) > 64 {
		Fail(w, r, 422, "VERSION_REQUIRED", "use a semantic version such as 1.0.0")
		return
	}
	a.aiMutation(w, r, p, "ai.draft.publish:"+id, body, func(tx *sql.Tx) (int, string, any, error) {
		d, e := a.aiDraft(r.Context(), tx, id)
		if e != nil {
			return 0, "", nil, e
		}
		if !canQuery(r.Context(), tx, p, "profile:publish", Str(d, "scopeId")) || !canQuery(r.Context(), tx, p, "document:read", Str(d, "scopeId")) {
			return 0, "", nil, Problem(403, "FORBIDDEN", "publication requires scope permission and source access")
		}
		if Str(d, "state") == "PUBLISHED" {
			return 0, "", nil, Problem(409, "DRAFT_IMMUTABLE", "draft was already published")
		}
		validation := Obj(d, "validation")
		content := Obj(d, "content")
		if Num(body, "draftRevision") != Num(d, "revision") || Str(body, "validationId") != Str(validation, "validationId") || Num(validation, "draftRevision") != Num(d, "revision") || !aiBool(validation, "passed") || Str(validation, "contentDigest") != Hash(content) {
			return 0, "", nil, Problem(409, "VALIDATION_STALE", "publish requires the passing validation of this exact draft revision")
		}
		j, e := a.aiJob(r.Context(), tx, Str(d, "jobId"))
		if e != nil {
			return 0, "", nil, e
		}
		if Str(j, "state") == "CANCELED" {
			return 0, "", nil, Problem(409, "IMPORT_CANCELED", "canceled import cannot be published")
		}
		modelID := ID("model")
		model := map[string]any{"modelId": modelID, "version": version, "scopeId": d["scopeId"], "manufacturer": content["manufacturer"], "modelName": content["modelName"], "firmwareRange": content["firmwareRange"], "content": content, "state": "PUBLISHED", "verificationLevel": "DOCUMENT_REVIEWED", "createdAt": Timestamp(Now()), "documentIds": j["documentIds"], "validationId": validation["validationId"]}
		if Num(validation, "independentSamples") > 0 {
			model["verificationLevel"] = "SAMPLE_VERIFIED"
		}
		if spec := Obj(content, "parserSpec"); len(spec) > 0 {
			parserID := "ai_" + strings.TrimPrefix(modelID, "model_")
			if e = RegisterAIParser(r.Context(), tx, parserID, version, spec, Str(d, "scopeId")); e != nil {
				return 0, "", nil, e
			}
			schema := Str(spec, "schemaVersion")
			if schema == "" {
				schema = "protocol-dsl-1"
			}
			model["parserRef"] = map[string]any{"parserId": parserID, "version": version, "schemaVersion": schema}
		}
		if _, e = tx.Exec("INSERT INTO ai_model_versions(model_id,scope_id,draft_id,version,model_json,created_at_ms) VALUES(?,?,?,?,?,?)", modelID, Str(d, "scopeId"), id, version, encode(model), Now()); e != nil {
			return 0, "", nil, e
		}
		if _, e = tx.Exec("UPDATE ai_drafts SET state='PUBLISHED',published_model_id=?,updated_at_ms=? WHERE draft_id=?", modelID, Now(), id); e != nil {
			return 0, "", nil, e
		}
		if _, e = tx.Exec("UPDATE ai_jobs SET state='READY',phase='PUBLISHED',updated_at_ms=? WHERE job_id=?", Now(), Str(d, "jobId")); e != nil {
			return 0, "", nil, e
		}
		e = a.Audit(r.Context(), tx, p.ID, "ai.profile.publish", "device_model", modelID, map[string]any{"draftId": id, "draftRevision": d["revision"], "version": version, "validationId": validation["validationId"], "contentDigest": Hash(content)})
		return 201, modelID, model, e
	})
}
