//go:build windows

package platform

import "golang.org/x/sys/windows"

func pipelineDiskFree(path string) (int64, error) {
	p, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return 0, err
	}
	var available, total, free uint64
	if err = windows.GetDiskFreeSpaceEx(p, &available, &total, &free); err != nil {
		return 0, err
	}
	if available > uint64(1<<63-1) {
		return 1<<63 - 1, nil
	}
	return int64(available), nil
}
