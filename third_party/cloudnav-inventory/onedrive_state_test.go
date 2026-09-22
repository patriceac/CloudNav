package onedrive

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"

	"github.com/rclone/rclone/fs"
	"github.com/rclone/rclone/lib/dircache"
	"github.com/rclone/rclone/lib/pacer"
	"github.com/rclone/rclone/lib/rest"
)

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
