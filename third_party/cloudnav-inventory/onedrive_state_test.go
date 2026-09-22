package onedrive

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/rclone/rclone/fs"
	"github.com/rclone/rclone/lib/dircache"
	"github.com/rclone/rclone/lib/pacer"
	"github.com/rclone/rclone/lib/rest"
)

func TestCloudNavOneDriveHistoryVersions(t *testing.T) {
	ctx, config := fs.AddConfig(context.Background())
	id, tag, document, status := "history", "tag1", "first", 200
	metadata, downloads := 0, 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/content") {
			downloads++
			_, _ = w.Write([]byte(document))
			return
		}
		metadata++
		w.Header().Set("Content-Type", "application/json")
		if status != 200 {
			w.WriteHeader(status)
			code := "itemNotFound"
			if status == 403 {
				code = "accessDenied"
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"error": map[string]string{"code": code}})
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"id": id, "name": "state.json", "size": len(document), "eTag": tag, "file": map[string]any{}, "parentReference": map[string]string{"driveId": "d", "id": "root"}})
	}))
	defer server.Close()
	f := &Fs{ci: config, driveID: "d", driveType: "personal", opt: Options{TenantURL: server.URL}, srv: rest.NewClient(server.Client()).SetErrorHandler(errorHandler), pacer: fs.NewPacer(ctx, pacer.NewDefault(pacer.MinSleep(time.Millisecond)))}
	f.dirCache = dircache.New("", "d#root", f)
	args := []string{"state.json", filepath.Join(t.TempDir(), "history.json")}
	read := func(wantDownloads int) {
		t.Helper()
		got, err := f.Command(ctx, "cloudnav-history", args, nil)
		if err != nil || got != document || downloads != wantDownloads {
			t.Fatal(got, err, downloads)
		}
	}
	read(1)
	read(1)
	if metadata != 3 {
		t.Fatal("warm history did not perform exactly one live check", metadata)
	}
	tag, document = "tag2", "edited"
	read(2)
	id, document = "replacement", "replaced"
	read(3)
	status = 404
	if _, err := f.Command(ctx, "cloudnav-history", args, nil); !errors.Is(err, fs.ErrorObjectNotFound) {
		t.Fatal("deleted history served cache", err)
	}
	status = 403
	if _, err := f.Command(ctx, "cloudnav-history", args, nil); err == nil || errors.Is(err, fs.ErrorObjectNotFound) {
		t.Fatal("denied history served cache")
	}
	status, tag = 200, ""
	read(4)
	read(5)
}

func TestCloudNavHistoryLock(t *testing.T) {
	ctx, config := fs.AddConfig(context.Background())
	var mu sync.Mutex
	locked := false
	lookups := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		switch r.Method {
		case "POST":
			var body map[string]any
			_ = json.NewDecoder(r.Body).Decode(&body)
			if body["@name.conflictBehavior"] != "fail" {
				t.Error("lock creation must fail on conflict")
			}
			if locked {
				w.WriteHeader(409)
				_, _ = w.Write([]byte(`{"error":{"code":"nameAlreadyExists"}}`))
				return
			}
			locked = true
		case "DELETE":
			locked = false
			w.WriteHeader(204)
			return
		case "GET":
			lookups++
			if !locked {
				w.WriteHeader(404)
				_, _ = w.Write([]byte(`{"error":{"code":"itemNotFound"}}`))
				return
			}
		}
		_, _ = w.Write([]byte(`{"id":"lock","name":"active","folder":{},"parentReference":{"driveId":"d","id":"parent"}}`))
	}))
	defer server.Close()
	newFs := func() *Fs {
		f := &Fs{ci: config, driveID: "d", driveType: "personal", opt: Options{TenantURL: server.URL}, srv: rest.NewClient(server.Client()), pacer: fs.NewPacer(ctx, pacer.NewDefault(pacer.MinSleep(time.Millisecond)))}
		f.dirCache = dircache.New("", "d#root", f)
		if err := f.dirCache.FindRoot(ctx, false); err != nil {
			t.Fatal(err)
		}
		f.dirCache.Put(cloudNavLockParent, "d#parent")
		return f
	}
	a, b := newFs(), newFs()
	owner, err := a.Command(ctx, "cloudnav-lock", nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	lock := owner.(map[string]string)
	if lock["id"] != "d#lock" || lock["parentId"] != "d#parent" {
		t.Fatal("lock did not retain its resolved IDs", owner)
	}
	if _, err = b.Command(ctx, "cloudnav-lock", nil, nil); err == nil {
		t.Fatal("second writer acquired lock")
	}
	if _, err = b.Command(ctx, "cloudnav-unlock", []string{"other-id"}, nil); err == nil || !locked {
		t.Fatal("wrong owner released lock")
	}
	// A new process has no directory cache. Supplying the saved parent must
	// need only the live ownership lookup, and must still reject a wrong ID.
	c := newFs()
	c.dirCache = dircache.New("", "d#root", c)
	if err := c.dirCache.FindRoot(ctx, false); err != nil {
		t.Fatal(err)
	}
	before := lookups
	if _, err = c.Command(ctx, "cloudnav-unlock", []string{"other-id", lock["parentId"]}, nil); err == nil || !locked {
		t.Fatal("cached parent bypassed ownership check")
	}
	if _, err = c.Command(ctx, "cloudnav-unlock", []string{lock["id"], lock["parentId"]}, nil); err != nil || locked {
		t.Fatal("owner could not release", err)
	}
	if lookups-before != 2 {
		t.Fatal("unlock repeated parent path lookups", lookups-before)
	}
	if _, err = b.Command(ctx, "cloudnav-lock", nil, nil); err != nil {
		t.Fatal("next writer could not acquire", err)
	}
}
