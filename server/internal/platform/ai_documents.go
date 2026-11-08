package platform

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/xml"
	"errors"
	"fmt"
	"image"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"mime"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

const aiMaxDocumentBytes = 50 << 20
const aiMaxExtractedBytes = 2 << 20

func aiMaxStoredDocumentBytes() int64 {
	if limit, err := strconv.ParseInt(os.Getenv("GW_AI_DOCUMENT_MAX_BYTES"), 10, 64); err == nil && limit > 0 {
		return limit
	}
	return 256 << 20
}

func aiExecutable(envName, name string) string {
	if v := os.Getenv(envName); v != "" {
		if filepath.IsAbs(v) {
			if s, e := os.Stat(v); e == nil && !s.IsDir() {
				return v
			}
		}
		return ""
	}
	path, e := exec.LookPath(name)
	if e != nil {
		return ""
	}
	return path
}

type aiBoundedBuffer struct {
	bytes.Buffer
	limit int
}

func (b *aiBoundedBuffer) Write(p []byte) (int, error) {
	if b.Len()+len(p) > b.limit {
		return 0, errors.New("extracted output limit exceeded")
	}
	return b.Buffer.Write(p)
}
func aiReadBounded(r io.Reader, max int64) ([]byte, error) {
	b, e := io.ReadAll(io.LimitReader(r, max+1))
	if int64(len(b)) > max {
		return nil, errors.New("content exceeds size limit")
	}
	return b, e
}

func aiExtractDOCX(data []byte) (string, []string, error) {
	z, e := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if e != nil {
		return "", nil, e
	}
	var total uint64
	var text string
	warnings := []string{}
	hasImages := false
	for _, f := range z.File {
		total += f.UncompressedSize64
		if total > 100<<20 || len(z.File) > 2048 {
			return "", nil, errors.New("DOCX expanded size or entry limit exceeded")
		}
		if strings.HasPrefix(f.Name, "word/media/") && !hasImages {
			warnings = append(warnings, "DOCX_IMAGES_REQUIRE_REVIEW")
			hasImages = true
		}
		if f.Name != "word/document.xml" {
			continue
		}
		if f.UncompressedSize64 > aiMaxExtractedBytes*4 {
			return "", nil, errors.New("DOCX XML exceeds limit")
		}
		rd, e := f.Open()
		if e != nil {
			return "", nil, e
		}
		raw, e := aiReadBounded(rd, aiMaxExtractedBytes*4)
		rd.Close()
		if e != nil {
			return "", nil, e
		}
		d := xml.NewDecoder(bytes.NewReader(raw))
		var b strings.Builder
		inText := false
		for {
			tok, e := d.Token()
			if e == io.EOF {
				break
			}
			if e != nil {
				return "", nil, e
			}
			switch t := tok.(type) {
			case xml.StartElement:
				if t.Name.Local == "t" {
					inText = true
				}
				if t.Name.Local == "tab" {
					b.WriteString("\t")
				}
			case xml.EndElement:
				if t.Name.Local == "t" {
					inText = false
				}
				if t.Name.Local == "p" || t.Name.Local == "tr" {
					b.WriteString("\n")
				}
				if t.Name.Local == "tc" {
					b.WriteString("\t")
				}
			case xml.CharData:
				if inText {
					b.Write(t)
				}
			}
			if b.Len() > aiMaxExtractedBytes {
				return "", nil, errors.New("DOCX text exceeds limit")
			}
		}
		text = b.String()
	}
	if text == "" {
		return "", warnings, errors.New("DOCX has no readable document text")
	}
	if len(warnings) > 0 {
		warnings = []string{"DOCX_IMAGES_REQUIRE_REVIEW: embedded pictures are not in the extracted text; upload those pages as PDF/images or explicitly resolve their relevance."}
	}
	return text, warnings, nil
}
func aiExtractPDF(ctx context.Context, path string) (string, error) {
	executable := aiExecutable("GW_AI_PDFTOTEXT", "pdftotext")
	if executable == "" {
		if aiPythonPDFConfigured() {
			return aiPythonPDFText(ctx, path)
		}
		return "", errors.New("PDF text extractor is not installed")
	}
	cctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	cmd := exec.CommandContext(cctx, executable, "-enc", "UTF-8", "-layout", path, "-")
	out := &aiBoundedBuffer{limit: aiMaxExtractedBytes}
	cmd.Stdout = out
	cmd.Stderr = io.Discard
	if e := cmd.Run(); e != nil {
		return "", errors.New("PDF text extraction failed or timed out")
	}
	return out.String(), nil
}
func aiMakeEvidence(id, text string) []any {
	list := []any{}
	pages := strings.Split(text, "\f")
	for page, part := range pages {
		lines := strings.Split(part, "\n")
		start := 1
		var b strings.Builder
		flush := func(end int) {
			s := strings.TrimSpace(b.String())
			if s != "" {
				list = append(list, map[string]any{"evidenceId": fmt.Sprintf("%s_e%04d", id, len(list)+1), "page": page + 1, "location": fmt.Sprintf("page %d lines %d-%d", page+1, start, end), "text": s})
			}
			b.Reset()
			start = end + 1
		}
		for i, line := range lines {
			if b.Len()+len(line) > 6000 {
				flush(i)
			}
			if len(line) > 6000 {
				r := []rune(line)
				for len(r) > 1800 {
					b.WriteString(string(r[:1800]))
					flush(i + 1)
					r = r[1800:]
				}
				line = string(r)
			}
			b.WriteString(line)
			b.WriteByte('\n')
		}
		flush(len(lines))
	}
	return list
}
func (a *App) aiUploadDocument(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "document:write")
	if !ok {
		return
	}
	if !a.aiDocumentMu.TryLock() {
		Fail(w, r, 429, "DOCUMENT_UPLOAD_BUSY", "another document is being uploaded or extracted; retry after it finishes")
		return
	}
	defer a.aiDocumentMu.Unlock()
	r.Body = http.MaxBytesReader(w, r.Body, aiMaxDocumentBytes+1<<20)
	if e := r.ParseMultipartForm(4 << 20); e != nil {
		Fail(w, r, 413, "DOCUMENT_TOO_LARGE", "multipart document upload is limited to 50 MiB")
		return
	}
	if r.MultipartForm != nil {
		defer r.MultipartForm.RemoveAll()
	}
	scope := r.FormValue("scopeId")
	if e := a.aiAuthorized(r.Context(), p, "document:write", scope); e != nil {
		RespondError(w, r, e)
		return
	}
	if scope == "" {
		Fail(w, r, 422, "SCOPE_REQUIRED", "select a document resource scope")
		return
	}
	file, header, e := r.FormFile("file")
	if e != nil {
		Fail(w, r, 400, "DOCUMENT_REQUIRED", "file part is required")
		return
	}
	defer file.Close()
	raw, e := aiReadBounded(file, aiMaxDocumentBytes)
	if e != nil {
		Fail(w, r, 413, "DOCUMENT_TOO_LARGE", "document exceeds the size limit")
		return
	}
	if len(raw) == 0 {
		Fail(w, r, 422, "DOCUMENT_EMPTY", "empty document")
		return
	}
	ext := strings.ToLower(filepath.Ext(header.Filename))
	mimeType := ""
	state := "READY"
	text := ""
	warnings := []string{}
	switch ext {
	case ".txt", ".md":
		mimeType = "text/plain"
		if !utf8.Valid(raw) || bytes.Contains(raw, []byte{0}) {
			Fail(w, r, 422, "DOCUMENT_ENCODING", "UTF-8 text is required")
			return
		}
		if len(raw) > aiMaxExtractedBytes {
			Fail(w, r, 413, "EXTRACTION_LIMIT", "text content exceeds 2 MiB")
			return
		}
		text = string(raw)
	case ".docx":
		mimeType = "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
		text, warnings, e = aiExtractDOCX(raw)
		if e != nil {
			Fail(w, r, 422, "DOCUMENT_INVALID", e.Error())
			return
		}
	case ".pdf":
		mimeType = "application/pdf"
		if !bytes.HasPrefix(raw, []byte("%PDF-")) {
			Fail(w, r, 422, "DOCUMENT_INVALID", "invalid PDF signature")
			return
		}
	case ".png", ".jpg", ".jpeg":
		mimeType = http.DetectContentType(raw)
		cfg, format, err := image.DecodeConfig(bytes.NewReader(raw))
		if err != nil || (format != "png" && format != "jpeg") || cfg.Width < 1 || cfg.Height < 1 || int64(cfg.Width)*int64(cfg.Height) > 25000000 {
			Fail(w, r, 422, "DOCUMENT_INVALID", "PNG/JPEG up to 25 megapixels required")
			return
		}
		state = "NEEDS_VISION"
	default:
		Fail(w, r, 422, "DOCUMENT_UNSUPPORTED", "supported formats: PDF, DOCX, UTF-8 TXT/MD, PNG and JPEG")
		return
	}
	id := ID("doc")
	dir := filepath.Join(a.Config.DataDir, "ai-documents")
	if e = os.MkdirAll(dir, 0700); e != nil {
		RespondError(w, r, e)
		return
	}
	free, e := pipelineDiskFree(a.Config.DataDir)
	if e != nil {
		Fail(w, r, 503, "DOCUMENT_STORAGE_UNAVAILABLE", "document storage capacity could not be checked")
		return
	}
	if free < pipelineDiskReserve+2*int64(len(raw))+(8<<20) {
		Fail(w, r, 507, "DOCUMENT_DISK_RESERVE", "document upload would consume the data pipeline disk reserve")
		return
	}
	path := filepath.Join(dir, id)
	if e = writeExclusive(path, raw, 0600); e != nil {
		RespondError(w, r, e)
		return
	}
	committed := false
	defer func() {
		if !committed {
			_ = os.Remove(path)
		}
	}()
	if ext == ".pdf" {
		text, e = aiExtractPDF(r.Context(), path)
		var apiError *APIError
		if errors.As(e, &apiError) {
			RespondError(w, r, e)
			return
		}
		pages := len(strings.Split(text, "\f"))
		if strings.HasSuffix(text, "\f") {
			pages--
		}
		if pages > 200 {
			Fail(w, r, 413, "DOCUMENT_PAGE_LIMIT", "document exceeds 200 pages")
			return
		}
		if e != nil || strings.TrimSpace(text) == "" {
			state = "NEEDS_VISION"
			warnings = append(warnings, "PDF_TEXT_UNAVAILABLE: use a configured vision model with a PDF rasterizer, or upload UTF-8 text.")
			text = ""
		} else {
			for _, pageText := range strings.Split(strings.TrimSuffix(text, "\f"), "\f") {
				if strings.TrimSpace(pageText) == "" {
					state = "NEEDS_VISION"
					warnings = append(warnings, "PDF_PARTIAL_SCAN: one or more pages have no text; visual review is required.")
					break
				}
			}
		}
	}
	if state == "NEEDS_VISION" {
		warnings = append(warnings, "VISUAL_SOURCE_REVIEW_REQUIRED: visual extraction requires a vision-capable provider; manual notes remain available.")
	}
	evidence := aiMakeEvidence(id, text)
	if state == "NEEDS_VISION" {
		evidence = append(evidence, map[string]any{"evidenceId": id + "_image", "page": 1, "location": "visual document", "text": "", "visual": true})
	}
	hash := sha256.Sum256(raw)
	digest := hex.EncodeToString(hash[:])
	name := filepath.Base(strings.ReplaceAll(header.Filename, "\\", "/"))
	if len(name) > 255 {
		name = "document" + ext
	}
	body := map[string]any{"scopeId": scope, "sha256": digest, "name": name}
	status, result, e := a.DoIdempotent(r.Context(), p.ID, "ai.document.upload", r.Header.Get("Idempotency-Key"), body, func(tx *sql.Tx) (int, string, any, error) {
		var existingID string
		err := tx.QueryRowContext(r.Context(), "SELECT document_id FROM ai_documents WHERE scope_id=? AND sha256=? ORDER BY created_at_ms,document_id LIMIT 1", scope, digest).Scan(&existingID)
		if err == nil {
			if !canQuery(r.Context(), tx, p, "document:read", scope) {
				return 0, "", nil, Problem(403, "FORBIDDEN", "reusing an existing document requires source access")
			}
			existing, e := a.aiDocument(r.Context(), tx, existingID)
			if e != nil {
				return 0, "", nil, e
			}
			delete(existing, "ownerId")
			delete(existing, "evidence")
			return 201, existingID, existing, nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return 0, "", nil, err
		}
		var stored int64
		if e := tx.QueryRowContext(r.Context(), "SELECT coalesce(sum(size_bytes),0) FROM ai_documents").Scan(&stored); e != nil {
			return 0, "", nil, e
		}
		if stored > aiMaxStoredDocumentBytes()-int64(len(raw)) {
			return 0, "", nil, Problem(413, "DOCUMENT_STORAGE_LIMIT", "stored documents have reached the configured instance allowance")
		}
		if _, e := tx.ExecContext(r.Context(), "INSERT INTO ai_documents(document_id,scope_id,owner_id,name,mime_type,sha256,size_bytes,extraction_state,evidence_json,warnings_json,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?)", id, scope, p.ID, name, mimeType, digest, len(raw), state, encode(evidence), encode(warnings), Now()); e != nil {
			return 0, "", nil, e
		}
		if e := a.Audit(r.Context(), tx, p.ID, "ai.document.upload", "source_document", id, map[string]any{"sha256": digest, "sizeBytes": len(raw), "scopeId": scope}); e != nil {
			return 0, "", nil, e
		}
		return 201, id, map[string]any{"documentId": id, "scopeId": scope, "name": name, "mimeType": mimeType, "sizeBytes": len(raw), "sha256": digest, "extractionState": state, "warnings": warnings, "evidenceCount": len(evidence), "createdAt": Timestamp(Now())}, nil
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if m, ok := result.(map[string]any); ok && Str(m, "documentId") == id {
		committed = true
	}
	JSON(w, status, result)
}
func (a *App) aiDocument(ctx context.Context, q ctlQuery, id string) (map[string]any, error) {
	var scope, owner, name, mimeType, sha, state, evidence, warnings string
	var size, at int64
	e := q.QueryRowContext(ctx, "SELECT scope_id,owner_id,name,mime_type,sha256,size_bytes,extraction_state,evidence_json,warnings_json,created_at_ms FROM ai_documents WHERE document_id=?", id).Scan(&scope, &owner, &name, &mimeType, &sha, &size, &state, &evidence, &warnings, &at)
	if e != nil {
		return nil, aiNotFound(e, "DOCUMENT_NOT_FOUND")
	}
	return map[string]any{"documentId": id, "scopeId": scope, "ownerId": owner, "name": name, "mimeType": mimeType, "sha256": sha, "sizeBytes": size, "extractionState": state, "evidence": aiList(evidence), "evidenceCount": len(aiList(evidence)), "warnings": aiList(warnings), "createdAt": Timestamp(at)}, nil
}
func (a *App) aiDocumentEvidence(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "document:read")
	if !ok {
		return
	}
	d, e := a.aiDocument(r.Context(), a.Management, aiParam(r, "documentId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if e = a.aiAuthorized(r.Context(), p, "document:read", Str(d, "scopeId")); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"documentId": d["documentId"], "items": d["evidence"], "warnings": d["warnings"]})
}
func (a *App) aiDocumentContent(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "document:read")
	if !ok {
		return
	}
	d, e := a.aiDocument(r.Context(), a.Management, aiParam(r, "documentId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if e = a.aiAuthorized(r.Context(), p, "document:read", Str(d, "scopeId")); e != nil {
		RespondError(w, r, e)
		return
	}
	file, e := os.Open(filepath.Join(a.Config.DataDir, "ai-documents", Str(d, "documentId")))
	if e != nil {
		Fail(w, r, 404, "DOCUMENT_CONTENT_MISSING", "original document is unavailable")
		return
	}
	defer file.Close()
	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": Str(d, "name")}))
	w.Header().Set("Content-Type", Str(d, "mimeType"))
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "no-store")
	http.ServeContent(w, r, Str(d, "name"), time.Time{}, file)
}
