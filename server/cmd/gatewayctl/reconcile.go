package main

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/runtimeguard"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
)

type revisionRow struct {
	GatewayID          string `json:"gatewayId"`
	EventID            string `json:"eventId"`
	ProcessingRevision int64  `json:"processingRevision"`
}
type revisionDocument struct {
	NamespaceID string        `json:"namespaceId"`
	Complete    bool          `json:"complete"`
	Revisions   []revisionRow `json:"revisions"`
}

func reconcileRevisions(dir, namespace, file string) error {
	release, e := runtimeguard.Acquire(dir, true)
	if e != nil {
		return e
	}
	defer release()
	abs, e := filepath.Abs(filepath.Join(dir, "management.db"))
	if e != nil {
		return e
	}
	uriPath := filepath.ToSlash(abs)
	if filepath.VolumeName(abs) != "" {
		uriPath = "/" + uriPath
	}
	u := url.URL{Scheme: "file", Path: uriPath}
	q := u.Query()
	q.Set("mode", "ro")
	u.RawQuery = q.Encode()
	db, e := sql.Open("sqlite", u.String())
	if e != nil {
		return e
	}
	defer db.Close()
	var storedNamespace string
	if e = db.QueryRow("SELECT json_extract(settings_json,'$.namespaceId') FROM instance_settings WHERE singleton=1").Scan(&storedNamespace); e != nil {
		return e
	}
	if namespace != storedNamespace {
		return errors.New("--namespace does not match restored installation")
	}
	f, e := os.Open(file)
	if e != nil {
		return e
	}
	defer f.Close()
	var document revisionDocument
	dec := json.NewDecoder(io.LimitReader(f, 64<<20))
	dec.DisallowUnknownFields()
	if e = dec.Decode(&document); e != nil {
		return e
	}
	if e = dec.Decode(new(any)); e != io.EOF {
		return errors.New("expected a single JSON document")
	}
	if !document.Complete || document.NamespaceID != namespace {
		return errors.New("complete:true and matching --namespace are required")
	}
	if document.Revisions == nil {
		return errors.New("revisions array is required")
	}
	valid := regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$`)
	seen := map[string]bool{}
	for _, r := range document.Revisions {
		if !valid.MatchString(r.GatewayID) || !valid.MatchString(r.EventID) || r.ProcessingRevision < 1 {
			return errors.New("invalid revision identity or nonpositive revision")
		}
		key := r.GatewayID + "\x00" + r.EventID
		if seen[key] {
			return errors.New("duplicate source identity; merge maxima before import")
		}
		seen[key] = true
	}
	marker := filepath.Join(dir, "restore-reconciliation.required")
	if _, e = os.Stat(marker); e != nil {
		return errors.New("no pending restore reconciliation marker")
	}
	final := filepath.Join(dir, "revision-highwater.json")
	if previousRaw, err := os.ReadFile(final); err == nil {
		var previous revisionDocument
		if err = json.Unmarshal(previousRaw, &previous); err != nil {
			return err
		}
		if previous.NamespaceID != namespace {
			return errors.New("previous high-water namespace mismatch")
		}
		positions := map[string]int{}
		for i, row := range document.Revisions {
			positions[row.GatewayID+"\x00"+row.EventID] = i
		}
		for _, row := range previous.Revisions {
			if !valid.MatchString(row.GatewayID) || !valid.MatchString(row.EventID) || row.ProcessingRevision < 1 {
				return errors.New("invalid previous high-water record")
			}
			key := row.GatewayID + "\x00" + row.EventID
			if i, exists := positions[key]; exists {
				if row.ProcessingRevision > document.Revisions[i].ProcessingRevision {
					document.Revisions[i].ProcessingRevision = row.ProcessingRevision
				}
			} else {
				positions[key] = len(document.Revisions)
				document.Revisions = append(document.Revisions, row)
			}
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	raw, e := json.MarshalIndent(document, "", "  ")
	if e != nil {
		return e
	}
	temp := filepath.Join(dir, "revision-highwater.json.new")
	if e = writeNew(temp, append(raw, '\n')); e != nil {
		return e
	}
	defer os.Remove(temp)
	if _, e = os.Stat(final); e == nil {
		backup := filepath.Join(dir, "revision-highwater.previous.json")
		if _, e = os.Stat(backup); e == nil {
			return errors.New("archive existing revision-highwater.previous.json before importing again")
		}
		if e = os.Rename(final, backup); e != nil {
			return e
		}
	}
	if e = os.Rename(temp, final); e != nil {
		return e
	}
	if e = os.Remove(marker); e != nil {
		return e
	}
	fmt.Println("Imported complete customer revision high-water marks; reprocessing is unlocked.")
	return nil
}
