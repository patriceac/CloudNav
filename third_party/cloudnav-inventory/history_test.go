package cloudnavinventory

import (
	"context"
	"errors"
	"testing"

	"github.com/rclone/rclone/fs"
	"github.com/rclone/rclone/fstest/mockobject"
)

func TestCloudNavHistoryRead(t *testing.T) {
	ctx := context.Background()
	path, document := ".CloudNav-history/.sync/pair/state.json", "{\"name\":\"é\"}\n"
	lookup := func(_ context.Context, remote string) (fs.Object, error) {
		if remote != path {
			t.Fatal("history path changed", remote)
		}
		return mockobject.New(remote).WithContent([]byte(document), mockobject.SeekModeNone), nil
	}
	got, err := ReadHistory(ctx, lookup, []string{path})
	if err != nil || got != document {
		t.Fatal("history bytes changed", got, err)
	}
	for _, failure := range []error{fs.ErrorObjectNotFound, context.Canceled, errors.New("permission denied")} {
		_, err = ReadHistory(ctx, func(context.Context, string) (fs.Object, error) { return nil, failure }, []string{path})
		if !errors.Is(err, failure) {
			t.Fatal("history failure was hidden", failure, err)
		}
	}
}
