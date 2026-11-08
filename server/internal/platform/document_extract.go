package platform

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// The interpreter is selected by the server administrator, never by uploaded
// files or model output. Container deployments use native pdftotext instead.
func aiPythonPDFConfigured() bool {
	path := os.Getenv("GW_AI_PYTHON")
	if path == "" || !filepath.IsAbs(path) {
		return false
	}
	stat, err := os.Stat(path)
	return err == nil && stat.Mode().IsRegular()
}

type documentOutputBuffer struct {
	bytes.Buffer
	limit int
}

func (b *documentOutputBuffer) Write(p []byte) (int, error) {
	if len(p) > b.limit-b.Len() {
		return 0, errors.New("document extraction output limit")
	}
	return b.Buffer.Write(p)
}

func aiPythonPDFText(ctx context.Context, path string) (string, error) {
	if !aiPythonPDFConfigured() {
		return "", errors.New("PDF text extraction requires configured tools")
	}
	script, err := Assets.ReadFile("assets/ai_extract_pdf.py")
	if err != nil {
		return "", err
	}
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, os.Getenv("GW_AI_PYTHON"), "-I", "-c", string(script), path)
	out := &documentOutputBuffer{limit: 2 << 20}
	errOut := &documentOutputBuffer{limit: 1024}
	cmd.Stdout = out
	cmd.Stderr = errOut
	if err = cmd.Run(); err != nil {
		switch strings.TrimSpace(errOut.String()) {
		case "DOCUMENT_PAGE_LIMIT":
			return "", Problem(413, "DOCUMENT_PAGE_LIMIT", "PDF exceeds 200 pages")
		case "DOCUMENT_TEXT_LIMIT":
			return "", Problem(413, "DOCUMENT_TEXT_LIMIT", "PDF extracted text exceeds 2 MiB")
		case "DOCUMENT_ENCRYPTED":
			return "", Problem(422, "DOCUMENT_ENCRYPTED", "Upload an unlocked PDF")
		}
		return "", errors.New("PDF text extraction failed, timed out or exceeded limits")
	}
	return out.String(), nil
}
