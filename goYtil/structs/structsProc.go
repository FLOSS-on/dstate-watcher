package structs

type Proces struct {
	TGID int32
	// PID        int32
	Name       string
	ComandName string
	Speed      int64
	Mark       uint32
	Limit      int64
}
