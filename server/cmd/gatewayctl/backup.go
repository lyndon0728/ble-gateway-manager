package main

import (
	"archive/tar"
	"compress/gzip"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/runtimeguard"
	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/simkit"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

const backupMagic = "BGBACKUP1\n"

func backupKey(path string, create bool) ([]byte, error) {
	if create {
		if _, e := os.Stat(path); os.IsNotExist(e) {
			b := make([]byte, 32)
			if _, e = rand.Read(b); e != nil {
				return nil, e
			}
			if e = writeNew(path, []byte(base64.StdEncoding.EncodeToString(b)+"\n")); e != nil {
				return nil, e
			}
		}
	}
	s, e := simkit.Secret(path)
	if e != nil {
		return nil, e
	}
	b, e := base64.StdEncoding.DecodeString(s)
	if e != nil || len(b) != 32 {
		return nil, errors.New("backup key must be 32 bytes in standard base64")
	}
	return b, nil
}
func backup(dir, out, keyPath string) error {
	absolute, e := filepath.Abs(dir)
	if e != nil {
		return e
	}
	outAbs, e := filepath.Abs(out)
	if e != nil {
		return e
	}
	keyAbs, e := filepath.Abs(keyPath)
	if e != nil {
		return e
	}
	if inside(outAbs, absolute) || inside(keyAbs, absolute) {
		return errors.New("backup archive and encryption key must be outside the data directory")
	}
	release, e := runtimeguard.Acquire(dir, true)
	if e != nil {
		return e
	}
	defer release()
	if _, e = os.Stat(filepath.Join(dir, "master.key")); e != nil {
		return errors.New("no initialized data directory")
	}
	key, e := backupKey(keyPath, true)
	if e != nil {
		return e
	}
	if e = os.MkdirAll(filepath.Dir(out), 0700); e != nil {
		return e
	}
	f, e := os.OpenFile(out, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if e != nil {
		return e
	}
	success := false
	defer func() {
		f.Close()
		if !success {
			_ = os.Remove(out)
		}
	}()
	enc, e := newEncryptWriter(f, key)
	if e != nil {
		return e
	}
	gz := gzip.NewWriter(enc)
	tw := tar.NewWriter(gz)
	e = filepath.WalkDir(dir, func(path string, d fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == dir {
			return nil
		}
		rel, e := filepath.Rel(dir, path)
		if e != nil {
			return e
		}
		if rel == "runtime.lock" {
			return nil
		}
		info, e := d.Info()
		if e != nil {
			return e
		}
		if !info.Mode().IsRegular() && !info.IsDir() {
			return fmt.Errorf("refusing non-regular backup entry: %s", rel)
		}
		h, e := tar.FileInfoHeader(info, "")
		if e != nil {
			return e
		}
		h.Name = filepath.ToSlash(rel)
		h.Mode = 0600
		if info.IsDir() {
			h.Mode = 0700
		}
		if e = tw.WriteHeader(h); e != nil {
			return e
		}
		if info.IsDir() {
			return nil
		}
		in, e := os.Open(path)
		if e != nil {
			return e
		}
		_, e = io.Copy(tw, in)
		return errors.Join(e, in.Close())
	})
	if e != nil {
		return e
	}
	if e = tw.Close(); e != nil {
		return e
	}
	if e = gz.Close(); e != nil {
		return e
	}
	if e = enc.Close(); e != nil {
		return e
	}
	if e = f.Sync(); e != nil {
		return e
	}
	if e = f.Close(); e != nil {
		return e
	}
	success = true
	fmt.Println("Encrypted offline backup completed. Store the separate key securely.")
	return nil
}

type encryptWriter struct {
	w      io.Writer
	a      cipher.AEAD
	prefix [8]byte
	seq    uint64
	buf    []byte
}

func newEncryptWriter(w io.Writer, key []byte) (*encryptWriter, error) {
	block, e := aes.NewCipher(key)
	if e != nil {
		return nil, e
	}
	a, e := cipher.NewGCM(block)
	if e != nil {
		return nil, e
	}
	v := &encryptWriter{w: w, a: a}
	if _, e = rand.Read(v.prefix[:]); e != nil {
		return nil, e
	}
	if _, e = io.WriteString(w, backupMagic); e != nil {
		return nil, e
	}
	_, e = w.Write(v.prefix[:])
	return v, e
}
func (v *encryptWriter) frame(b []byte) error {
	nonce := make([]byte, 12)
	copy(nonce, v.prefix[:])
	if v.seq > 0xffffffff {
		return errors.New("backup frame count exceeds format limit")
	}
	binary.BigEndian.PutUint32(nonce[8:], uint32(v.seq))
	sealed := v.a.Seal(nil, nonce, b, []byte(backupMagic))
	var size [4]byte
	binary.BigEndian.PutUint32(size[:], uint32(len(sealed)))
	if _, e := v.w.Write(size[:]); e != nil {
		return e
	}
	if _, e := v.w.Write(sealed); e != nil {
		return e
	}
	v.seq++
	return nil
}
func (v *encryptWriter) Write(b []byte) (int, error) {
	n := len(b)
	for len(b) > 0 {
		take := 65536 - len(v.buf)
		if take > len(b) {
			take = len(b)
		}
		v.buf = append(v.buf, b[:take]...)
		b = b[take:]
		if len(v.buf) == 65536 {
			if e := v.frame(v.buf); e != nil {
				return 0, e
			}
			v.buf = v.buf[:0]
		}
	}
	return n, nil
}
func (v *encryptWriter) Close() error {
	if len(v.buf) > 0 {
		if e := v.frame(v.buf); e != nil {
			return e
		}
	}
	return v.frame(nil)
}
func decrypt(r io.Reader, w io.Writer, key []byte) error {
	header := make([]byte, len(backupMagic)+8)
	if _, e := io.ReadFull(r, header); e != nil {
		return e
	}
	if string(header[:len(backupMagic)]) != backupMagic {
		return errors.New("unsupported backup format")
	}
	block, e := aes.NewCipher(key)
	if e != nil {
		return e
	}
	a, e := cipher.NewGCM(block)
	if e != nil {
		return e
	}
	for seq := uint64(0); ; seq++ {
		var size [4]byte
		if _, e = io.ReadFull(r, size[:]); e != nil {
			return errors.New("backup truncated before authenticated end marker")
		}
		n := binary.BigEndian.Uint32(size[:])
		if n < uint32(a.Overhead()) || n > 65536+uint32(a.Overhead()) {
			return errors.New("invalid backup frame size")
		}
		sealed := make([]byte, n)
		if _, e = io.ReadFull(r, sealed); e != nil {
			return e
		}
		nonce := make([]byte, 12)
		copy(nonce, header[len(backupMagic):])
		if seq > 0xffffffff {
			return errors.New("backup frame count exceeds format limit")
		}
		binary.BigEndian.PutUint32(nonce[8:], uint32(seq))
		plain, e := a.Open(nil, nonce, sealed, []byte(backupMagic))
		if e != nil {
			return errors.New("backup authentication failed (wrong key or damaged archive)")
		}
		if len(plain) == 0 {
			var extra [1]byte
			n, e := r.Read(extra[:])
			if n != 0 || e != io.EOF {
				return errors.New("unexpected data after backup end")
			}
			return nil
		}
		if _, e = w.Write(plain); e != nil {
			return e
		}
	}
}
func restore(dir, archive, keyPath string) error {
	release, e := runtimeguard.Acquire(dir, true)
	if e != nil {
		return e
	}
	defer release()
	entries, e := os.ReadDir(dir)
	if e != nil {
		return e
	}
	for _, entry := range entries {
		if entry.Name() != "runtime.lock" {
			return errors.New("restore requires an empty data directory; preserve the old directory first")
		}
	}
	key, e := backupKey(keyPath, false)
	if e != nil {
		return e
	}
	file, e := os.Open(archive)
	if e != nil {
		return e
	}
	defer file.Close()
	stage, e := os.MkdirTemp(filepath.Dir(dir), ".gateway-restore-")
	if e != nil {
		return e
	}
	defer os.RemoveAll(stage)
	pr, pw := io.Pipe()
	decrypted := make(chan error, 1)
	go func() { err := decrypt(file, pw, key); _ = pw.CloseWithError(err); decrypted <- err }()
	defer pr.Close()
	gz, e := gzip.NewReader(pr)
	if e != nil {
		return e
	}
	defer gz.Close()
	tr := tar.NewReader(gz)
	seen := map[string]bool{}
	var total int64
	for {
		h, e := tr.Next()
		if e == io.EOF {
			break
		}
		if e != nil {
			return e
		}
		clean := filepath.Clean(filepath.FromSlash(h.Name))
		if clean == "." || clean == "runtime.lock" || filepath.IsAbs(clean) || strings.Contains(h.Name, "\\") || strings.Contains(clean, ":") || !inside(filepath.Join(stage, clean), stage) || seen[clean] {
			return errors.New("unsafe or duplicate archive path")
		}
		seen[clean] = true
		dest := filepath.Join(stage, clean)
		switch h.Typeflag {
		case tar.TypeDir:
			if e = os.MkdirAll(dest, 0700); e != nil {
				return e
			}
		case tar.TypeReg:
			if h.Size < 0 || h.Size > 1<<40 || total+h.Size > 1<<40 {
				return errors.New("backup exceeds 1 TiB restore safety limit")
			}
			total += h.Size
			if e = os.MkdirAll(filepath.Dir(dest), 0700); e != nil {
				return e
			}
			f, e := os.OpenFile(dest, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
			if e != nil {
				return e
			}
			_, e = io.CopyN(f, tr, h.Size)
			if e == nil {
				e = f.Sync()
			}
			e = errors.Join(e, f.Close())
			if e != nil {
				return e
			}
		default:
			return errors.New("archive links and special files are forbidden")
		}
	}
	// Consume gzip trailer and the authenticated encryption end marker before
	// exposing any restored file in the requested data directory.
	if _, e = io.Copy(io.Discard, gz); e != nil {
		return e
	}
	if _, e = io.Copy(io.Discard, pr); e != nil {
		return e
	}
	if e = <-decrypted; e != nil {
		return e
	}
	for _, required := range []string{"master.key", "management.db", "pipeline.db"} {
		if !seen[required] {
			return fmt.Errorf("backup lacks %s", required)
		}
	}
	entries, e = os.ReadDir(stage)
	if e != nil {
		return e
	}
	// Persist the guard before installing any database file, so interruption
	// between renames cannot expose an unguarded restored processing revision.
	if e = writeNew(filepath.Join(dir, "restore-reconciliation.required"), []byte("Merge maximum processingRevision from every customer receiver before reprocessing.\n")); e != nil {
		return e
	}
	for _, entry := range entries {
		if entry.Name() == "restore-reconciliation.required" {
			continue
		}
		if e = os.Rename(filepath.Join(stage, entry.Name()), filepath.Join(dir, entry.Name())); e != nil {
			return e
		}
	}
	fmt.Println("Restored authenticated offline backup to the empty data directory.")
	return nil
}
