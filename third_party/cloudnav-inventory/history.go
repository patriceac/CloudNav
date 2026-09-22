package cloudnavinventory

import (
	"context"
	"fmt"
	"io"

	"github.com/rclone/rclone/fs"
)

// Open from the already resolved account root, without constructing another
// filesystem at the history file's path and resolving its parents again.
func ReadHistory(ctx context.Context, newObject func(context.Context, string) (fs.Object, error), args []string) (any, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("one history path required")
	}
	object, err := newObject(ctx, args[0])
	if err != nil {
		return nil, err
	}
	in, err := object.Open(ctx)
	if err != nil {
		return nil, err
	}
	defer in.Close()
	data, err := io.ReadAll(in)
	return string(data), err
}
