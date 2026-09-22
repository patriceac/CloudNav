package onedrive

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/rclone/rclone/fs"
	"github.com/rclone/rclone/lib/dircache"
	"github.com/rclone/rclone/lib/pacer"
	"github.com/rclone/rclone/lib/rest"
)

func TestCloudNavOneDriveChanges(t *testing.T) {
	ctx, config := fs.AddConfig(context.Background())
	phase, fullRequests := 0, 0
	item := func(id, name, parent string, folder bool) map[string]any {
		v := map[string]any{"id": id, "name": name, "size": 1, "lastModifiedDateTime": "2026-09-22T00:00:00Z", "parentReference": map[string]string{"driveId": "d", "id": parent}}
		if folder {
			v["folder"] = map[string]any{}
		} else {
			v["file"] = map[string]any{"hashes": map[string]string{"sha1Hash": "abc"}}
		}
		return v
	}
	var server *httptest.Server
	server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		base := server.URL + "/v2.0/drives/d/root/delta"
		w.Header().Set("Content-Type", "application/json")
		if phase == 7 && r.URL.Query().Get("token") != "" {
			w.WriteHeader(400)
			_, _ = w.Write([]byte(`{"error":{"code":"invalidRequest"}}`))
			return
		}
		if phase >= 5 {
			if strings.Contains(r.URL.Path, "/children") {
				name := "shared.txt"
				if phase >= 6 {
					name = "shared-changed.txt"
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"value": []any{item("shared-file", name, "shared-folder", false)}})
			} else {
				if r.URL.Query().Get("token") == "" {
					fullRequests++
				}
				shared := item("alias", "shared", "root", true)
				shared["remoteItem"] = map[string]any{"id": "shared-folder", "name": "shared", "folder": map[string]any{}, "parentReference": map[string]string{"driveId": "d", "id": "remote-root"}}
				_ = json.NewEncoder(w).Encode(map[string]any{"value": []any{shared}, "@odata.deltaLink": base + "?token=shared"})
			}
			return
		}
		if phase == 3 {
			w.WriteHeader(403)
			_, _ = w.Write([]byte(`{"error":{"code":"accessDenied"}}`))
			return
		}
		if r.URL.Query().Get("token") == "two" && phase == 2 {
			w.WriteHeader(410)
			_, _ = w.Write([]byte(`{"error":{"code":"resyncRequired"}}`))
			return
		}
		var response map[string]any
		switch {
		case r.URL.Query().Get("token") != "":
			response = map[string]any{"value": []any{item("folder", "renamed", "root", true), map[string]any{"id": "gone", "deleted": map[string]any{}}, item("new", "new.txt", "root", false)}, "@odata.deltaLink": base + "?token=two"}
		case r.URL.Query().Get("page") == "2":
			response = map[string]any{"value": []any{}, "@odata.nextLink": base + "?page=3"}
		case r.URL.Query().Get("page") == "3":
			response = map[string]any{"value": []any{item("gone", "gone.txt", "root", false)}, "@odata.deltaLink": base + "?token=one"}
		default:
			fullRequests++
			if phase == 2 || phase == 4 {
				response = map[string]any{"value": []any{item("new", "new.txt", "root", false)}, "@odata.deltaLink": base + "?token=three"}
			} else {
				response = map[string]any{"value": []any{item("kept", "kept.txt", "folder", false), item("folder", "before", "root", true)}, "@odata.nextLink": base + "?page=2"}
			}
		}
		_ = json.NewEncoder(w).Encode(response)
	}))
	defer server.Close()
	f := &Fs{ci: config, driveID: "d", driveType: "personal", opt: Options{TenantURL: server.URL, ListChunk: 1000, CloudNavCache: filepath.Join(t.TempDir(), "inventory.json")}, srv: rest.NewClient(server.Client()), pacer: fs.NewPacer(ctx, pacer.NewDefault(pacer.MinSleep(time.Millisecond)))}
	f.dirCache = dircache.New("", "d#root", f)
	read := func(want []string) {
		t.Helper()
		var paths []string
		if err := f.ListR(ctx, "", func(entries fs.DirEntries) error {
			for _, e := range entries {
				paths = append(paths, e.Remote())
			}
			return nil
		}); err != nil {
			t.Fatal(err)
		}
		sort.Strings(paths)
		sort.Strings(want)
		if !reflect.DeepEqual(paths, want) {
			t.Fatalf("got %v; want %v", paths, want)
		}
	}
	read([]string{"before", "before/kept.txt", "gone.txt"})
	phase = 1
	before, _ := os.ReadFile(f.opt.CloudNavCache)
	if f.ListR(ctx, "", func(fs.DirEntries) error { return errors.New("cancel output") }) == nil {
		t.Fatal("failed output accepted")
	}
	after, _ := os.ReadFile(f.opt.CloudNavCache)
	if string(before) != string(after) {
		t.Fatal("failed output changed cache")
	}
	read([]string{"renamed", "renamed/kept.txt", "new.txt"})
	if fullRequests != 1 {
		t.Fatal("incremental scan repeated full inventory")
	}
	phase = 2
	read([]string{"new.txt"})
	if fullRequests != 2 {
		t.Fatal("expired token did not cause one full scan")
	}
	before, _ = os.ReadFile(f.opt.CloudNavCache)
	phase = 3
	if f.ListR(ctx, "", func(fs.DirEntries) error { return nil }) == nil {
		t.Fatal("permission error served cached data")
	}
	after, _ = os.ReadFile(f.opt.CloudNavCache)
	if string(before) != string(after) {
		t.Fatal("failed refresh changed cache")
	}
	phase = 4
	f.opt.CloudNavFull = true
	read([]string{"new.txt"})
	if fullRequests != 3 {
		t.Fatal("verification reused delta")
	}
	phase = 5
	read([]string{"shared", "shared/shared.txt"})
	f.opt.CloudNavFull = false
	phase = 6
	read([]string{"shared", "shared/shared-changed.txt"})
	if fullRequests != 4 {
		t.Fatal("shared target caused another full root scan")
	}
	phase = 7
	read([]string{"shared", "shared/shared-changed.txt"})
	if fullRequests != 5 {
		t.Fatal("invalid cursor did not cause one full scan")
	}
}
