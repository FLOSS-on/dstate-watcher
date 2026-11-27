package main

import (
	"bytes"
	"io"
	"os"
	"strconv"
	"time"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
	"golang.org/x/sys/unix"
)

type SleepTask struct {
	PID      string
	Comm     string
	StartNS  uint64
	Duration time.Duration
}

var ProcPath string = "/proc/my_dir/dstat_info"

func main() {
	app := tview.NewApplication()
	app.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		if event.Rune() == 'q' || event.Rune() == 'Q' || event.Rune() == 'й' || event.Rune() == 'Й' {
			app.Stop()
			return nil
		}
		return event
	})

	table := tview.NewTable().SetBorders(true)
	table.SetBorder(true).SetTitle(" D-State Watcher ")

	drawHeader := func() {
		table.SetCell(0, 0, tview.NewTableCell("PID").SetTextColor(tcell.ColorYellow).SetExpansion(1))
		table.SetCell(0, 1, tview.NewTableCell("Name").SetTextColor(tcell.ColorYellow).SetExpansion(2))
		table.SetCell(0, 2, tview.NewTableCell("Время").SetTextColor(tcell.ColorYellow).SetExpansion(1))
	}

	drawHeader()

	go func() {
		buf := make([]byte, 32*1024)
		ticker := time.NewTicker(300 * time.Millisecond)
		defer ticker.Stop()

		for {
			tmpf, err := os.Open(ProcPath)
			if err != nil {
				app.Stop()
				println(err.Error())
				return
			}

			n, err := tmpf.Read(buf)
			tmpf.Close()
			if err != nil && err != io.EOF {
				app.Stop()
				println(err.Error())
				return
			}

			var ts unix.Timespec
			unix.ClockGettime(unix.CLOCK_MONOTONIC, &ts)
			nowNs := uint64(ts.Sec)*1_000_000_000 + uint64(ts.Nsec)

			var currentTasks []SleepTask
			lines := bytes.Split(buf[:n], []byte("\n"))

			for _, line := range lines {
				if len(line) == 0 {
					continue
				}
				parts := bytes.Split(line, []byte("|"))
				if len(parts) != 3 {
					continue
				}

				pidT := string(bytes.TrimSpace(bytes.TrimPrefix(parts[0], []byte("PID:"))))
				commStr := string(bytes.TrimSpace(bytes.TrimPrefix(parts[1], []byte("Comm:"))))
				startStr := string(bytes.TrimSpace(bytes.TrimPrefix(parts[2], []byte("Start:"))))

				startNs, err := strconv.ParseUint(startStr, 10, 64)
				if err != nil {
					continue
				}

				currentTasks = append(currentTasks, SleepTask{
					PID:      pidT,
					Comm:     commStr,
					StartNS:  startNs,
					Duration: time.Duration(nowNs - startNs),
				})
			}

			app.QueueUpdateDraw(func() {
				table.Clear()
				drawHeader()

				for i, tmp := range currentTasks {
					row := i + 1
					table.SetCell(row, 0, tview.NewTableCell(tmp.PID).SetExpansion(1))
					table.SetCell(row, 1, tview.NewTableCell(tmp.Comm).SetExpansion(2))
					table.SetCell(row, 2, tview.NewTableCell(tmp.Duration.Round(time.Millisecond).String()).SetExpansion(1))
				}

			})

			<-ticker.C
		}
	}()

	if err := app.SetRoot(table, true).Run(); err != nil {
		panic(err)
	}
}
