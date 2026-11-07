//go:build !windows

package platform

import "golang.org/x/sys/unix"

func diskFree(path string) (uint64, error) {
	var s unix.Statfs_t
	e := unix.Statfs(path, &s)
	return uint64(s.Bavail) * uint64(s.Bsize), e
}
