package main

import (
	"bytes"
	"io"
	"os"
	"strconv"
	"time"

	"golang.org/x/sys/unix"
)

type SleepTask struct {
	PID      string
	Comm     string
	StartNS  uint64
	Duration time.Duration
}

var ProcPatch string = "/proc/my_dir/dstat_info"
var tasks []SleepTask

func main() {
	go func() {
		buf := make([]byte, 32*1024)
		ticker := time.NewTicker(300 * time.Millisecond)
		defer ticker.Stop()
		for {
			tmpf, err := os.Open(ProcPatch)
			if err != nil {
				println(err.Error())
				return
			}
			MainfileByets, err := tmpf.Read(buf)
			if err != nil && err != io.EOF {
				println(err.Error())
				return
			}
			tmpf.Close()
			data := buf[:MainfileByets]
			lines := bytes.Split(data, []byte("\n"))
			var ts unix.Timespec
			unix.ClockGettime(unix.CLOCK_MONOTONIC, &ts)
			nowNs := uint64(ts.Sec)*1_000_000_000 + uint64(ts.Nsec)
			for _, line := range lines {
				if len(line) == 0 {
					continue
				}
				parts := bytes.Split(line, []byte("|"))
				if len(parts) != 3 {
					continue
				}

				pidT := string(bytes.TrimPrefix(parts[0], []byte("PID:")))
				commStr := string(bytes.TrimSpace(bytes.TrimPrefix(parts[1], []byte("Comm:"))))
				startStr := string(bytes.TrimPrefix(parts[2], []byte("Start:")))
				startNs, err := strconv.ParseUint(startStr, 10, 64)
				if err != nil {
					continue

				}

				duration := time.Duration(nowNs - startNs)
				tasks = append(tasks, SleepTask{
					PID:      pidT,
					Comm:     commStr,
					StartNS:  startNs,
					Duration: duration,
				})
			}
			<-ticker.C
		}

	}()

}
