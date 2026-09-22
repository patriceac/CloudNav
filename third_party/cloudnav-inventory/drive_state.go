package drive

import (
	"context"
	"fmt"

	inventory "github.com/rclone/rclone/lib/cloudnavinventory"
)

func (f *Fs) cloudNavHistory(ctx context.Context, args []string) (any, error) {
	return inventory.ReadHistory(ctx, f.NewObject, args)
}

func (f *Fs) cloudNavIdentity(ctx context.Context) (any, error) {
	root, err := f.dirCache.FindDir(ctx, "", false)
	if err != nil {
		return nil, err
	}
	item, err := f.svc.Files.Get(root).Fields("id").Context(ctx).Do()
	if err != nil {
		return nil, err
	}
	if item.Id == "" {
		return nil, fmt.Errorf("Google Drive root identity is missing")
	}
	// A different selected view must never reuse a whole-drive deletion baseline.
	view := fmt.Sprintf(":%t:%t:%t:%t:%t", f.opt.SharedWithMe, f.opt.StarredOnly, f.opt.TrashedOnly,
		f.opt.SkipShortcuts, f.opt.SkipDanglingShortcuts)
	return map[string]string{"identity": "drive:" + item.Id + view, "rootId": item.Id}, nil
}
