package proc

import (
	"goYtil/structs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

func ParseProc() ([]structs.Proces, error) {
	var processList []structs.Proces

	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil, err
	}

	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}

		tgidValue, err := strconv.Atoi(entry.Name())
		if err != nil {
			continue
		}

		procPath := filepath.Join("/proc", entry.Name())

		commS, err := os.ReadFile(filepath.Join(procPath, "comm"))
		if err != nil {
			continue
		}

		nameTGID := strings.TrimSpace(string(commS))

		cmdS, _ := os.ReadFile(filepath.Join(procPath, "cmdline"))

		commandName := strings.TrimSpace(
			strings.ReplaceAll(string(cmdS), "\x00", " "),
		)

		if commandName == "" {
			commandName = "[" + nameTGID + "]"
		}

		processList = append(processList, structs.Proces{
			TGID:       int32(tgidValue),
			Name:       nameTGID,
			ComandName: commandName,
			Speed:      0,
			Limit:      0,
		})
	}

	sort.Slice(processList, func(i, j int) bool {
		return processList[i].TGID < processList[j].TGID
	})

	return processList, nil
}
