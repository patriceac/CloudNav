package cloudnavinventory

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"sync"
	"testing"
	"time"
)

func TestCloudNavConcurrentProgress(t *testing.T) {
	p := NewProgress(context.Background(), filepath.Join(t.TempDir(), "inventory"), "changes")
	var workers sync.WaitGroup
	for i := 0; i < 3; i++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			p.Full()
			for n := 0; n < 10; n++ {
				p.Page(2, time.Now())
			}
		}()
	}
	done := make(chan struct{})
	go func() { workers.Wait(); close(done) }()
	last := 0
	for {
		data, err := os.ReadFile(p.name)
		var status struct {
			Mode         string
			Pages, Items int
		}
		// Windows can briefly deny a read during replacement. The application
		// retains its previous display then; any readable document must be whole.
		if err == nil && (json.Unmarshal(data, &status) != nil || status.Pages < last || status.Items != 2*status.Pages) {
			t.Fatal("torn/nonmonotonic progress", string(data))
		}
		if err == nil {
			last = status.Pages
		}
		select {
		case <-done:
			data, _ = os.ReadFile(p.name)
			if json.Unmarshal(data, &status) != nil || status.Pages < last || status.Items != 2*status.Pages ||
				p.Mode != "full" || p.Pages != 30 || p.Items != 60 {
				t.Fatal("lost progress updates", string(data))
			}
			return
		default:
			time.Sleep(time.Millisecond)
		}
	}
}

func TestCloudNavAtomicCacheAndIdentity(t *testing.T) {
	ctx := context.Background()
	name := filepath.Join(t.TempDir(), "inventory.json")
	state := Snapshot[int]{Identity: "account/root", Cursor: "cursor-1", Items: map[string]int{"kept": 1}}
	if err := Save(ctx, name, state); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(name)
	if !reflect.DeepEqual(Load[int](name, state.Identity), state) {
		t.Fatal("cache changed")
	}
	if Load[int](name, "another-account/root").Cursor != "" {
		t.Fatal("wrong account reused")
	}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	state.Cursor = "cursor-2"
	if Save(cancelled, name, state) == nil {
		t.Fatal("cancelled snapshot promoted")
	}
	after, _ := os.ReadFile(name)
	if string(before) != string(after) {
		t.Fatal("last good cache overwritten")
	}
	for _, bad := range []string{`{"Version":1}`, string(before[:len(before)/2]), string(append(append([]byte{}, before[:20]...), before[21:]...))} {
		if err := os.WriteFile(name, []byte(bad), 0600); err != nil {
			t.Fatal(err)
		}
		if Load[int](name, "account/root").Cursor != "" {
			t.Fatal("corrupt cache reused")
		}
	}
}

func TestCloudNavFolderChanges(t *testing.T) {
	nodes := map[string]Node{"folder": {Parent: "root", Name: "before", Directory: true}, "file": {Parent: "folder", Name: "kept.txt"}, "outside": {Parent: "missing", Name: "hidden"}}
	before, err := Paths("root", nodes)
	if err != nil || len(before) != 2 || before[1].Path != "before/kept.txt" {
		t.Fatalf("initial: %v %v", before, err)
	}
	nodes["folder"] = Node{Parent: "root", Name: "renamed", Directory: true}
	after, err := Paths("root", nodes)
	if err != nil || after[1].Path != "renamed/kept.txt" {
		t.Fatalf("rename: %v %v", after, err)
	}
	delete(nodes, "folder")
	if remaining, err := Paths("root", nodes); err != nil || len(remaining) != 0 {
		t.Fatal("deleted folder's descendants remain visible")
	}
	nodes["a"] = Node{Parent: "b", Name: "a", Directory: true}
	nodes["b"] = Node{Parent: "a", Name: "b", Directory: true}
	if _, err := Paths("root", nodes); err == nil {
		t.Fatal("cyclic provider data accepted")
	}
}
