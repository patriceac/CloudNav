package cloudnavinventory

import (
	"context"
	"fmt"
	"io"

	"github.com/rclone/rclone/fs"
)

// Open from the already resolved account root, without constructing another
// filesystem at the history file's path and resolving its parents again.
func ReadHistory(ctx context.Context, lookup func(context.Context, string) (fs.Object, string, error), identity string, args []string) (any, error) {
	if len(args) < 1 || len(args) > 2 {
		return nil, fmt.Errorf("history path and optional cache path required")
	}
	object, version, err := lookup(ctx, args[0])
	if err != nil {
		return nil, err
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	identity += ":" + args[0]
	cache := ""
	if len(args) == 2 && version != "" {
		cache = args[1]
		state := Load[string](cache, identity)
		if document, ok := state.Items["document"]; ok && state.Cursor == version {
			return document, nil // Reuse only after a successful live path/version lookup.
		}
	}
	in, err := object.Open(ctx)
	if err != nil {
		return nil, err
	}
	data, err := io.ReadAll(in)
	closeErr := in.Close()
	if err != nil {
		return nil, err
	}
	if closeErr != nil {
		return nil, closeErr
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if cache != "" {
		// A write/replacement during download must not associate old bytes with
		// a new version, or authorize a plan using an inconsistent history.
		_, after, err := lookup(ctx, args[0])
		if err != nil {
			return nil, err
		}
		if after != version {
			return nil, fmt.Errorf("shared history changed while reading; analyze again")
		}
		if err := Save(ctx, cache, Snapshot[string]{Identity: identity, Cursor: version, Items: map[string]string{"document": string(data)}}); err != nil {
			if ctx.Err() != nil {
				return nil, ctx.Err()
			}
			fs.Debugf(nil, "History cache could not be saved: %v", err)
		}
	}
	return string(data), nil
}
