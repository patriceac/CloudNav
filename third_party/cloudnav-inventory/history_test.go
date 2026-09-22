package cloudnavinventory

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/rclone/rclone/fs"
	"github.com/rclone/rclone/fstest/mockobject"
)

func TestCloudNavHistoryRead(t *testing.T) {
	ctx := context.Background()
	path, document := ".CloudNav-history/.sync/pair/state.json", "{\"name\":\"é\"}\n"
	lookup := func(_ context.Context, remote string) (fs.Object, string, error) {
		if remote != path {
			t.Fatal("history path changed", remote)
		}
		return mockobject.New(remote).WithContent([]byte(document), mockobject.SeekModeNone), "", nil
	}
	got, err := ReadHistory(ctx, lookup, "account", []string{path})
	if err != nil || got != document {
		t.Fatal("history bytes changed", got, err)
	}
	for _, failure := range []error{fs.ErrorObjectNotFound, context.Canceled, errors.New("permission denied")} {
		_, err = ReadHistory(ctx, func(context.Context, string) (fs.Object, string, error) { return nil, "", failure }, "account", []string{path})
		if !errors.Is(err, failure) {
			t.Fatal("history failure was hidden", failure, err)
		}
	}
}

type historyObject struct {
	fs.Object
	open func() (io.ReadCloser, error)
}

func (o historyObject) Open(context.Context, ...fs.OpenOption) (io.ReadCloser, error) {
	return o.open()
}

type historyReader struct {
	io.Reader
	closeError error
}

func (r historyReader) Close() error { return r.closeError }

func TestCloudNavHistoryCache(t *testing.T) {
	ctx := context.Background()
	cache := filepath.Join(t.TempDir(), "history.json")
	args := []string{"state.json", cache}
	version, document, account := "id:1", "original", "account"
	opens, lookups := 0, 0
	var failure, openFailure, closeFailure error
	changeDuringRead := false
	lookup := func(context.Context, string) (fs.Object, string, error) {
		lookups++
		return historyObject{open: func() (io.ReadCloser, error) {
			opens++
			if changeDuringRead {
				version = "id:changed-during-read"
			}
			return historyReader{strings.NewReader(document), closeFailure}, openFailure
		}}, version, failure
	}
	read := func(wantOpens, wantLookups int) {
		t.Helper()
		got, err := ReadHistory(ctx, lookup, account, args)
		if err != nil || got != document || opens != wantOpens || lookups != wantLookups {
			t.Fatalf("read=%v error=%v opens=%d lookups=%d", got, err, opens, lookups)
		}
	}
	read(1, 2)
	read(1, 3) // Warm reads still contact the provider, but never open content.
	version, document = "id:2", "updated"
	read(2, 5)
	version, document = "replacement:2", "replacement"
	read(3, 7) // A replacement with the same version number is a different file.
	account = "another-account"
	read(4, 9)
	args[0] = "another-path.json"
	read(5, 11)
	if err := os.WriteFile(cache, []byte(`{"Version":1,"Data":{},"SHA256":"corrupt"}`), 0600); err != nil {
		t.Fatal(err)
	}
	read(6, 13)
	before, _ := os.ReadFile(cache)
	for _, failure = range []error{fs.ErrorObjectNotFound, context.Canceled, errors.New("permission denied"), errors.New("network failed")} {
		got, err := ReadHistory(ctx, lookup, account, args)
		if got != nil || !errors.Is(err, failure) {
			t.Fatal("served cache despite live failure", got, err)
		}
		after, _ := os.ReadFile(cache)
		if string(before) != string(after) {
			t.Fatal("failure changed cache")
		}
	}
	failure = nil
	version = "id:3"
	for _, scenario := range []string{"open", "close", "changed"} {
		openFailure, closeFailure, changeDuringRead = nil, nil, false
		switch scenario {
		case "open":
			openFailure = errors.New("download failed")
		case "close":
			closeFailure = errors.New("truncated download")
		case "changed":
			changeDuringRead = true
		}
		if got, err := ReadHistory(ctx, lookup, account, args); got != nil || err == nil {
			t.Fatal("failed read accepted", scenario)
		}
		after, _ := os.ReadFile(cache)
		if string(before) != string(after) {
			t.Fatal("failed read committed cache", scenario)
		}
	}
	openFailure, closeFailure, changeDuringRead = nil, nil, false
	version, document = "", "no-version-must-download"
	prior := opens
	for i := 0; i < 2; i++ {
		got, err := ReadHistory(ctx, lookup, account, args)
		if err != nil || got != document {
			t.Fatal(got, err)
		}
	}
	if opens != prior+2 {
		t.Fatal("missing version reused cache")
	}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if _, err := ReadHistory(cancelled, lookup, account, args); !errors.Is(err, context.Canceled) {
		t.Fatal("cancel ignored", err)
	}
}
