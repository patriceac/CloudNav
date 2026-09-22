package drive

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
	gdrive "google.golang.org/api/drive/v3"
	"google.golang.org/api/option"
)

func TestCloudNavGoogleHistoryVersions(t *testing.T) {
	ctx, _ := fs.AddConfig(context.Background())
	id, version, document, status := "history", "1", "first", 200
	metadata, downloads := 0, 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("alt") == "media" {
			downloads++
			_, _ = w.Write([]byte(document))
			return
		}
		metadata++
		w.Header().Set("Content-Type", "application/json")
		if status == 403 {
			w.WriteHeader(403)
			_, _ = w.Write([]byte(`{"error":{"code":403,"message":"denied"}}`))
			return
		}
		files := []any{}
		if status == 200 {
			if !strings.Contains(r.URL.Query().Get("fields"), "version") {
				t.Error("history lookup did not request version")
			}
			files = append(files, map[string]any{"id": id, "version": version, "name": "state.json", "mimeType": "application/json", "md5Checksum": "abc", "modifiedTime": "2026-09-23T00:00:00Z", "size": "5", "parents": []string{"root"}})
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"files": files})
	}))
	defer server.Close()
	svc, err := gdrive.NewService(ctx, option.WithEndpoint(server.URL+"/"), option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}
	f := &Fs{svc: svc, client: server.Client(), dirResourceKeys: new(sync.Map), opt: Options{SkipGdocs: true, V2DownloadMinSize: -1}, pacer: fs.NewPacer(ctx, pacer.NewDefault(pacer.MinSleep(time.Millisecond)))}
	f.dirCache = dircache.New("", "root", f)
	args := []string{"state.json", filepath.Join(t.TempDir(), "history.json")}
	read := func(wantDownloads int) {
		t.Helper()
		got, err := f.cloudNavHistory(ctx, args)
		if err != nil || got != document || downloads != wantDownloads {
			t.Fatal(got, err, downloads)
		}
	}
	read(1)
	read(1)
	if metadata != 3 {
		t.Fatal("warm history did not perform exactly one live check", metadata)
	}
	version, document = "2", "edited"
	read(2)
	id, document = "replacement", "replaced"
	read(3)
	status = 404
	if _, err := f.cloudNavHistory(ctx, args); !errors.Is(err, fs.ErrorObjectNotFound) {
		t.Fatal("deleted history served cache", err)
	}
	status = 403
	if _, err := f.cloudNavHistory(ctx, args); err == nil {
		t.Fatal("denied history served cache")
	}
	status, version = 200, "0"
	read(4)
	read(5)
}

func TestCloudNavHistoryIdentity(t *testing.T) {
	ctx := context.Background()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("fields") != "id" {
			t.Error("identity must request only the root ID")
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"id":"stable-root"}`))
	}))
	defer server.Close()
	svc, err := gdrive.NewService(ctx, option.WithEndpoint(server.URL+"/"), option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}
	for _, alias := range []string{"root", "stable-root"} {
		f := &Fs{svc: svc}
		f.dirCache = dircache.New("", alias, f)
		id, err := f.cloudNavIdentity(ctx)
		if err != nil || id.(map[string]string)["identity"] != "drive:stable-root:false:false:false:false:false" || id.(map[string]string)["rootId"] != "stable-root" {
			t.Fatal("nonportable root identity", id, err)
		}
		f.opt.StarredOnly = true
		selected, err := f.cloudNavIdentity(ctx)
		if err != nil || selected.(map[string]string)["identity"] == id.(map[string]string)["identity"] {
			t.Fatal("selected view reused whole-drive history")
		}
	}
}
