package drive

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
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
	gdrive "google.golang.org/api/drive/v3"
	"google.golang.org/api/option"
)

func TestCloudNavGoogleChanges(t *testing.T) {
	ctx, config := fs.AddConfig(context.Background())
	phase, fullRequests := 0, 0
	item := func(id, name, parent string, folder bool) map[string]any {
		mime := "text/plain"
		if folder {
			mime = driveFolderType
		}
		return map[string]any{"id": id, "name": name, "parents": []string{parent}, "mimeType": mime, "size": "1", "modifiedTime": "2026-09-22T00:00:00Z", "md5Checksum": "abc"}
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if phase == 7 && strings.HasSuffix(r.URL.Path, "changes") && r.URL.Query().Get("pageToken") != "before" {
			w.WriteHeader(400)
			_, _ = w.Write([]byte(`{"error":{"code":400,"message":"invalid page token"}}`))
			return
		}
		if phase >= 5 {
			alias := item("alias", "shared.txt", "root-id", false)
			alias["mimeType"] = shortcutMimeType
			alias["shortcutDetails"] = map[string]string{"targetId": "shared-file", "targetMimeType": "text/plain"}
			var response any
			switch {
			case strings.HasSuffix(r.URL.Path, "changes/startPageToken"):
				response = map[string]any{"startPageToken": "before"}
			case strings.HasSuffix(r.URL.Path, "files/shared-file"):
				value := item("shared-file", "target.txt", "elsewhere", false)
				value["size"] = fmt.Sprint(phase)
				response = value
			case strings.HasSuffix(r.URL.Path, "files"):
				fullRequests++
				response = map[string]any{"files": []any{alias}}
			default:
				response = map[string]any{"changes": []any{}, "newStartPageToken": "shared"}
			}
			_ = json.NewEncoder(w).Encode(response)
			return
		}
		if phase == 3 {
			w.WriteHeader(403)
			_, _ = w.Write([]byte(`{"error":{"code":403,"message":"denied"}}`))
			return
		}
		var response any
		switch {
		case strings.HasSuffix(r.URL.Path, "changes/startPageToken"):
			response = map[string]any{"startPageToken": "before"}
		case strings.HasSuffix(r.URL.Path, "files"):
			fullRequests++
			files := []any{item("folder", "before", "root-id", true), item("kept", "kept.txt", "folder", false), item("gone", "gone.txt", "root-id", false)}
			if phase == 2 || phase == 4 {
				files = []any{item("new", "new.txt", "root-id", false)}
			}
			response = map[string]any{"files": files}
		case r.URL.Query().Get("pageToken") == "two" && phase == 2:
			w.WriteHeader(410)
			_, _ = w.Write([]byte(`{"error":{"code":410,"message":"expired"}}`))
			return
		case r.URL.Query().Get("pageToken") == "before":
			response = map[string]any{"changes": []any{}, "newStartPageToken": "one"}
		default:
			response = map[string]any{"changes": []any{map[string]any{"fileId": "folder", "file": item("folder", "renamed", "root-id", true)}, map[string]any{"fileId": "gone", "removed": true}, map[string]any{"fileId": "new", "file": item("new", "new.txt", "root-id", false)}}, "newStartPageToken": "two"}
		}
		_ = json.NewEncoder(w).Encode(response)
	}))
	defer server.Close()
	svc, err := gdrive.NewService(ctx, option.WithEndpoint(server.URL+"/"), option.WithHTTPClient(server.Client()))
	if err != nil {
		t.Fatal(err)
	}
	f := &Fs{ci: config, rootFolderID: "root-id", svc: svc, opt: Options{CloudNavCache: filepath.Join(t.TempDir(), "inventory.json"), SkipGdocs: true}, pacer: fs.NewPacer(ctx, pacer.NewDefault(pacer.MinSleep(time.Millisecond)))}
	f.dirCache = dircache.New("", "root-id", f)
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
	read([]string{"shared.txt"})
	f.opt.CloudNavFull = false
	phase = 6
	if err := f.ListR(ctx, "", func(entries fs.DirEntries) error {
		if len(entries) != 1 || entries[0].Remote() != "shared.txt" || entries[0].Size() != 6 {
			t.Fatal("shared file metadata was cached")
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	if fullRequests != 4 {
		t.Fatal("shortcut caused another full root scan")
	}
	phase = 7
	read([]string{"shared.txt"})
	if fullRequests != 5 {
		t.Fatal("invalid cursor did not cause one full scan")
	}
}
