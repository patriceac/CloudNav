package drive

import (
	"context"
	"fmt"

	"github.com/rclone/rclone/fs"
	inventory "github.com/rclone/rclone/lib/cloudnavinventory"
)

func (f *Fs) cloudNavHistory(ctx context.Context, args []string) (any, error) {
	root, err := f.dirCache.FindDir(ctx, "", false)
	if err != nil {
		return nil, err
	}
	return inventory.ReadHistory(ctx, func(ctx context.Context, remote string) (fs.Object, string, error) {
		info, extension, exportName, exportMimeType, isDocument, err := f.getRemoteInfoWithExport(ctx, remote)
		if err != nil {
			return nil, "", err
		}
		object, err := f.newObjectWithExportInfo(ctx, remote, info, extension, exportName, exportMimeType, isDocument)
		if err == nil && object == nil {
			err = fs.ErrorObjectNotFound
		}
		version := ""
		if info.Id != "" && info.Version > 0 {
			version = fmt.Sprintf("%s:%d", info.Id, info.Version)
		}
		return object, version, err
	}, "drive:"+root, args)
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
