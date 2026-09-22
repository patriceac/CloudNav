package drive

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/rclone/rclone/lib/dircache"
	gdrive "google.golang.org/api/drive/v3"
	"google.golang.org/api/option"
)

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
