// Package runtimeguard prevents offline maintenance while cooperating server
// processes are running. Locks are released by the OS even after a crash.
package runtimeguard

import (
	"fmt"
	"github.com/gofrs/flock"
	"os"
	"path/filepath"
)

func Acquire(dir string, maintenance bool) (func(), error) {
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	f := flock.New(filepath.Join(dir, "runtime.lock"))
	var ok bool
	var err error
	if maintenance {
		ok, err = f.TryLock()
	} else {
		ok, err = f.TryRLock()
	}
	if err != nil || !ok {
		_ = f.Close()
		return nil, fmt.Errorf("data directory is in use; stop all server processes before maintenance (lock error: %v)", err)
	}
	return func() { _ = f.Unlock(); _ = f.Close() }, nil
}
