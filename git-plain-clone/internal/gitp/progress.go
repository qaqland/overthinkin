package gitp

import (
	"fmt"
	"io"
	"sync"
	"time"
)

type Progress interface {
	Stage(name string)
	Download(received int64, done bool)
	PackObjects(done, total int)
	ResolveDeltas(done, total int)
	Writing(done, total int)
	Finish(files int)
	Close()
}

type terminalProgress struct {
	mu         sync.Mutex
	w          io.Writer
	started    time.Time
	downloadAt time.Time
	lastPrint  time.Time
	lastLen    int
	closed     bool
}

func NewTerminalProgress(w io.Writer) Progress {
	return &terminalProgress{w: w, started: time.Now()}
}

func (p *terminalProgress) Stage(name string) {
	if name == "downloading pack" {
		p.mu.Lock()
		p.downloadAt = time.Now()
		p.mu.Unlock()
	}
	p.print(name, true)
}

func (p *terminalProgress) Download(received int64, done bool) {
	p.mu.Lock()
	start := p.downloadAt
	p.mu.Unlock()
	if start.IsZero() {
		start = time.Now()
	}
	elapsed := time.Since(start)
	msg := fmt.Sprintf("downloading pack: %s, %s/s", formatBytes(received), formatBytesPerSecond(received, elapsed))
	p.print(msg, done)
}

func (p *terminalProgress) PackObjects(done, total int) {
	p.print(fmt.Sprintf("parsing pack: %d/%d objects", done, total), done == total)
}

func (p *terminalProgress) ResolveDeltas(done, total int) {
	p.print(fmt.Sprintf("resolving deltas: %d/%d", done, total), done == total)
}

func (p *terminalProgress) Writing(done, total int) {
	p.print(fmt.Sprintf("writing files: %d/%d", done, total), done == total)
}

func (p *terminalProgress) Finish(files int) {
	p.print(fmt.Sprintf("done: %d files in %.1fs", files, time.Since(p.started).Seconds()), true)
	p.Close()
}

func (p *terminalProgress) Close() {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return
	}
	if p.lastLen > 0 {
		fmt.Fprint(p.w, "\n")
	}
	p.closed = true
}

func (p *terminalProgress) print(msg string, force bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return
	}
	now := time.Now()
	if !force && !p.lastPrint.IsZero() && now.Sub(p.lastPrint) < 100*time.Millisecond {
		return
	}
	padding := ""
	if p.lastLen > len(msg) {
		padding = fmt.Sprintf("%*s", p.lastLen-len(msg), "")
	}
	fmt.Fprintf(p.w, "\r%s%s", msg, padding)
	p.lastPrint = now
	p.lastLen = len(msg)
}

func formatBytes(n int64) string {
	units := []string{"B", "KiB", "MiB", "GiB"}
	v := float64(n)
	unit := units[0]
	for i := 0; i < len(units)-1 && v >= 1024; i++ {
		v /= 1024
		unit = units[i+1]
	}
	if unit == "B" {
		return fmt.Sprintf("%d %s", n, unit)
	}
	return fmt.Sprintf("%.1f %s", v, unit)
}

func formatBytesPerSecond(n int64, elapsed time.Duration) string {
	if elapsed <= 0 {
		return formatBytes(0)
	}
	perSecond := int64(float64(n) / elapsed.Seconds())
	return formatBytes(perSecond)
}
