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
	f.features = &fs.Features{}
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
}

func TestCloudNavSharedDelta(t *testing.T) {
	ctx, config := fs.AddConfig(context.Background())
	phase, rootFull, sharedFull, sharedCalls := 0, 0, 0, 0
	item := func(drive, id, name, parent string, folder bool) map[string]any {
		// Real shared-folder feeds capitalize the drive ID differently from the shortcut.
		if drive == "s" {
			drive = "S"
		}
		v := map[string]any{"id": id, "name": name, "size": 1, "lastModifiedDateTime": "2026-09-22T00:00:00Z", "parentReference": map[string]string{"driveId": drive, "id": parent}}
		if folder {
			v["folder"] = map[string]any{}
		} else {
			v["file"] = map[string]any{}
		}
		return v
	}
	alias := func(id, name string) any {
		v := item("d", id, name, "root", true)
		v["remoteItem"] = map[string]any{"id": "target", "name": "owners-name", "folder": map[string]any{}, "parentReference": map[string]string{"driveId": "s", "id": "outside"}}
		return v
	}
	deleted := func(id string) any { return map[string]any{"id": id, "deleted": map[string]any{}} }
	var server *httptest.Server
	server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		base := server.URL + r.URL.Path
		values := []any{}
		full := r.URL.Query().Get("token") == ""
		switch r.URL.Path {
		case "/v2.0/drives/d/root/delta":
			if full {
				rootFull++
				values = []any{alias("a", "Shared"), alias("b", "Copy")}
			}
			if phase == 6 {
				values = []any{deleted("a")}
			}
			if phase == 7 {
				values = []any{deleted("b")}
			}
		case "/v2.0/drives/s/items/target/delta":
			sharedCalls++
			if phase == 4 || (phase == 5 && r.URL.Query().Get("page") == "2") {
				w.WriteHeader(403)
				_, _ = w.Write([]byte(`{"error":{"code":"accessDenied"}}`))
				return
			}
			if (phase == 3 || phase == 9) && !full {
				code := 410
				if phase == 9 {
					code = 400
				}
				w.WriteHeader(code)
				_, _ = w.Write([]byte(`{"error":{"code":"resyncRequired"}}`))
				return
			}
			if full {
				sharedFull++
				values = []any{item("s", "folder", "before", "target", true), item("s", "kept", "kept.txt", "folder", false), item("s", "gone", "gone.txt", "target", false)}
			}
			if phase == 1 || (full && phase > 1) {
				values = []any{item("s", "folder", "renamed", "target", true), item("s", "kept", "kept.txt", "target", false), deleted("gone"), item("s", "new", "new.txt", "folder", false)}
			}
			if phase == 5 {
				_ = json.NewEncoder(w).Encode(map[string]any{"value": []any{deleted("kept")}, "@odata.nextLink": base + "?token=shared&page=2"})
				return
			}
		default:
			t.Errorf("unexpected request (shared folders must use delta): %s", r.URL.Path)
			w.WriteHeader(400)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"value": values, "@odata.deltaLink": base + "?token=saved"})
	}))
	defer server.Close()
	f := &Fs{ci: config, driveID: "d", driveType: "personal", opt: Options{TenantURL: server.URL, ListChunk: 1000, CloudNavCache: filepath.Join(t.TempDir(), "inventory.json")}, srv: rest.NewClient(server.Client()), pacer: fs.NewPacer(ctx, pacer.NewDefault(pacer.MinSleep(time.Millisecond)))}
	f.dirCache = dircache.New("", "d#root", f)
	f.features = &fs.Features{}
	read := func(prefixes, children []string) {
		t.Helper()
		var got, want []string
		for _, prefix := range prefixes {
			want = append(want, prefix)
			for _, child := range children {
				want = append(want, prefix+"/"+child)
			}
		}
		if err := f.ListR(ctx, "", func(entries fs.DirEntries) error {
			for _, e := range entries {
				got = append(got, e.Remote())
			}
			return nil
		}); err != nil {
			t.Fatal(err)
		}
		sort.Strings(got)
		sort.Strings(want)
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("phase %d: got %v; want %v", phase, got, want)
		}
	}
	read([]string{"Shared", "Copy"}, []string{"before", "before/kept.txt", "gone.txt"})
	if rootFull != 1 || sharedFull != 1 || sharedCalls != 1 {
		t.Fatal("aliases must share a single target feed")
	}
	changed := []string{"renamed", "kept.txt", "renamed/new.txt"}
	for phase = 1; phase <= 3; phase++ {
		read([]string{"Shared", "Copy"}, changed)
	}
	if rootFull != 1 || sharedFull != 2 || sharedCalls != 5 {
		t.Fatal("warm checks or expired-token recovery rescanned unrelated folders")
	}
	before, _ := os.ReadFile(f.opt.CloudNavCache)
	for _, p := range []int{4, 5} {
		phase = p
		if f.ListR(ctx, "", func(fs.DirEntries) error { return nil }) == nil {
			t.Fatal("failed shared refresh accepted")
		}
		after, _ := os.ReadFile(f.opt.CloudNavCache)
		if string(before) != string(after) {
			t.Fatal("partial shared refresh advanced the catalog")
		}
	}
	phase = 6
	read([]string{"Copy"}, changed)
	calls := sharedCalls
	phase = 7
	read(nil, nil)
	if sharedCalls != calls {
		t.Fatal("removed shared folder was still queried")
	}
	data, _ := os.ReadFile(f.opt.CloudNavCache)
	if strings.Contains(string(data), `"Scopes"`) {
		t.Fatal("removed shared catalog retained")
	}
	phase = 8
	f.opt.CloudNavFull = true
	read([]string{"Shared", "Copy"}, changed)
	if rootFull != 2 || sharedFull != 3 {
		t.Fatal("independent verification reused a shared cursor")
	}
	phase = 9
	f.opt.CloudNavFull = false
	read([]string{"Shared", "Copy"}, changed)
	if rootFull != 2 || sharedFull != 4 {
		t.Fatal("invalid shared token did not reset only its own feed")
	}
}
