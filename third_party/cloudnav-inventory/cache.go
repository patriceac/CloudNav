// Package cloudnavinventory supports CloudNav's account inventory only.
package cloudnavinventory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

var ErrRescan = errors.New("inventory requires a full scan")
var ErrUnsupported = errors.New("inventory requires conventional listing")

type Snapshot[T any] struct {
	Identity string
	Cursor   string
	Items    map[string]T
	// Independent feeds (for example shared folders) commit with the root catalog.
	Scopes map[string]Snapshot[T] `json:",omitempty"`
}

// The checksum also rejects syntactically valid but incomplete cache files.
func Load[T any](name, identity string) Snapshot[T] {
	fresh := Snapshot[T]{Identity: identity, Items: make(map[string]T)}
	data, err := os.ReadFile(name)
	if err != nil {
		return fresh
	}
	var envelope struct {
		Version int
		Data    json.RawMessage
		SHA256  string
	}
	var state Snapshot[T]
	if json.Unmarshal(data, &envelope) != nil || envelope.Version != 1 ||
		fmt.Sprintf("%x", sha256.Sum256(envelope.Data)) != envelope.SHA256 ||
		json.Unmarshal(envelope.Data, &state) != nil || state.Identity != identity || state.Cursor == "" || state.Items == nil {
		return fresh
	}
	return state
}

func Save[T any](ctx context.Context, name string, state Snapshot[T]) error {
	data, err := json.Marshal(state)
	if err != nil {
		return err
	}
	envelope, err := json.Marshal(struct {
		Version int
		Data    json.RawMessage
		SHA256  string
	}{1, data, fmt.Sprintf("%x", sha256.Sum256(data))})
	if err != nil {
		return err
	}
	return atomicWrite(ctx, name, envelope)
}

func atomicWrite(ctx context.Context, name string, data []byte) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(name), ".inventory-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	_, err = f.Write(data)
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err == nil {
		err = closeErr
	}
	if err == nil {
		err = ctx.Err()
	}
	if err != nil {
		return err
	}
	return os.Rename(f.Name(), name)
}

type Node struct {
	Parent, Name string
	Directory    bool
}
type Entry struct{ ID, Path string }

// Resolve IDs after applying every page. Folder renames move descendants;
// missing/deleted parents make their descendants unreachable, not root files.
func Paths(root string, nodes map[string]Node) ([]Entry, error) {
	checked := map[string]bool{root: true}
	for id := range nodes {
		chain := make(map[string]bool)
		for current := id; !checked[current]; current = nodes[current].Parent {
			if _, exists := nodes[current]; !exists {
				break
			}
			if chain[current] {
				return nil, errors.New("cyclic inventory")
			}
			chain[current] = true
		}
		for current := range chain {
			checked[current] = true
		}
	}
	children := make(map[string][]string)
	for id, node := range nodes {
		if id != root {
			children[node.Parent] = append(children[node.Parent], id)
		}
	}
	for _, ids := range children {
		sort.Strings(ids)
	}
	queue := []Entry{{ID: root}}
	seen := map[string]bool{root: true}
	var result []Entry
	for i := 0; i < len(queue); i++ {
		for _, id := range children[queue[i].ID] {
			if seen[id] {
				return nil, errors.New("cyclic inventory")
			}
			seen[id] = true
			node := nodes[id]
			if node.Name == "" {
				return nil, errors.New("unnamed inventory item")
			}
			entry := Entry{id, path.Join(queue[i].Path, node.Name)}
			result = append(result, entry)
			if node.Directory {
				queue = append(queue, entry)
			}
		}
	}
	return result, nil
}

type Progress struct {
	Mode       string `json:"mode"`
	Pages      int    `json:"pages"`
	Items      int    `json:"items"`
	LastPageMS int64  `json:"lastPageMs"`
	name       string
	ctx        context.Context
	mu         sync.Mutex
}

func (p *Progress) Full() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.Mode = "full" // Keep full visible if any independent feed needs rebuilding.
}

func NewProgress(ctx context.Context, name, mode string) *Progress {
	p := &Progress{name: fmt.Sprintf("%s.progress-%d", name, os.Getppid()), Mode: mode, ctx: ctx, Pages: -1}
	p.Page(0, time.Now())
	return p
}

func (p *Progress) Page(items int, started time.Time) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.Pages++
	p.Items += items
	p.LastPageMS = time.Since(started).Milliseconds()
	data, _ := json.Marshal(p)
	_ = atomicWrite(p.ctx, p.name, data)
}
