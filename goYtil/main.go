package main

import (
	"fmt"
	proc "goYtil/funcs"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
)

func main() {
	app := tview.NewApplication()
	box := tview.NewBox().SetBorder(true).SetTitle("ksdfsf").SetBackgroundColor(tcell.ColorDefault)
	box.SetBorderColor(tcell.ColorGreen)

	text := tview.NewTextView()
	text.SetText("EShaperD")
	text.SetTextColor(tcell.ColorGreen)
	text.SetBackgroundColor(tcell.ColorDefault)

	table := tview.NewTable()
	table.SetFixed(1, 0)
	table.SetSelectable(true, false)
	table.Select(1, 0)

	table.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		row, _ := table.GetSelection()

		if event.Key() == tcell.KeyUp && row <= 1 {
			return nil
		}
		if event.Key() == tcell.KeyRune {
			switch event.Rune() {
			case 'q':
				app.Stop()
				return nil
			}
		}

		return event
	})
	table.SetCell(0, 0, tview.NewTableCell("TGID").SetTextColor(tcell.ColorGreen).
		SetAttributes(tcell.AttrBold).SetBackgroundColor(tcell.GetColor("#472043")))
	// table.SetCell(0, 1, tview.NewTableCell("PID").SetTextColor(tcell.ColorBlue).
	// 	SetAttributes(tcell.AttrBold).SetBackgroundColor(tcell.GetColor("#472043")))
	table.SetCell(0, 2, tview.NewTableCell("NAME").SetTextColor(tcell.ColorGreen).
		SetAttributes(tcell.AttrBold).SetBackgroundColor(tcell.GetColor("#472043")))
	// table.SetCell(0, 3, tview.NewTableCell("STATUS").SetTextColor(tcell.ColorBlue).
	// 	SetAttributes(tcell.AttrBold).SetBackgroundColor(tcell.GetColor("#472043")))
	table.SetCell(0, 4, tview.NewTableCell("SPEED").SetTextColor(tcell.ColorGreen).
		SetAttributes(tcell.AttrBold).SetBackgroundColor(tcell.GetColor("#472043")))
	table.SetCell(0, 5, tview.NewTableCell("LIMITATION").SetTextColor(tcell.ColorBlue).
		SetAttributes(tcell.AttrBold).SetBackgroundColor(tcell.GetColor("#472043")))
	table.SetBackgroundColor(tcell.GetColor("#472043"))
	table.SetCell(0, 6, tview.NewTableCell("COMMAND").SetTextColor(tcell.ColorGreen).
		SetAttributes(tcell.AttrBold).SetBackgroundColor(tcell.GetColor("#472043")))
	table.SetBackgroundColor(tcell.ColorDefault)
	table.SetSelectable(true, false)
	table.SetSeparator(tcell.RuneVLine)

	layout := tview.NewFlex().SetDirection(tview.FlexRow).AddItem(table, 0, 1, true)

	processes, _ := proc.ParseProc()
	row := 1

	for i, p := range processes {
		newGroup := i == 0 || p.TGID != processes[i-1].TGID

		if newGroup && i > 0 {
			for col := 0; col < 7; col++ {
				table.SetCell(row, 0,
					tview.NewTableCell("────").
						SetTextColor(tcell.ColorDarkGray))
			}
			row++
		}
		table.SetCell(row, 2, tview.NewTableCell(p.Name))

		if newGroup {
			table.SetCell(row, 0, tview.NewTableCell(fmt.Sprint(p.TGID)))
			table.SetCell(row, 6, tview.NewTableCell(p.ComandName))
		}

		row++
	}
	if err := app.SetRoot(layout, true).Run(); err != nil {
		panic(err)
	}
}
