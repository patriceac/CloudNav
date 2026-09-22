package cloudnavinventory

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

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
