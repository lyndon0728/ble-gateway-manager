package platform

import (
	"os"
	"path/filepath"
)

const pipelineDiskReserve int64 = 128 << 20

type pipelineDiskStats struct{ Stored, Available int64 }

func (a *App) pipelineStorageStats() (pipelineDiskStats, error) {
	var s pipelineDiskStats
	for _, name := range []string{"pipeline.db", "pipeline.db-wal"} {
		st, err := os.Stat(filepath.Join(a.Config.DataDir, name))
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return s, err
		}
		s.Stored += st.Size()
	}
	free, err := pipelineDiskFree(a.Config.DataDir)
	if err != nil {
		return s, err
	}
	s.Available = free
	return s, nil
}

// The physical budget includes persistent receipt/job/index growth, not just
// payloads. Estimates leave a separate control-plane disk reserve untouched.
func pipelineStorageCheck(stats pipelineDiskStats, estimated, maxBytes int64) string {
	if estimated < 0 || stats.Available-estimated < pipelineDiskReserve {
		return "DISK_RESERVE"
	}
	if stats.Stored > maxBytes-estimated {
		return "QUEUE_FULL"
	}
	return ""
}
