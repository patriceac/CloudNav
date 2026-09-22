package onedrive

import (
	"context"
	"fmt"

	"github.com/rclone/rclone/fs"
)

const cloudNavLockParent = ".CloudNav-history/.sync"

// The service creates this folder with conflictBehavior=fail. Unlike Mkdir,
// concurrent claimants cannot both succeed. Locks never expire under a writer.
func (f *Fs) Command(ctx context.Context, name string, args []string, _ map[string]string) (any, error) {
	switch name {
	case "cloudnav-identity":
		root, err := f.dirCache.FindDir(ctx, "", false)
		if err != nil {
			return nil, err
		}
		item, _, err := f.readMetaDataForPathRelativeToID(ctx, root, "")
		if err != nil {
			return nil, err
		}
		return "onedrive:" + f.driveID + ":" + item.GetID(), nil
	case "cloudnav-lock":
		parent, err := f.dirCache.FindDir(ctx, cloudNavLockParent, true)
		if err != nil {
			return nil, err
		}
		id, err := f.CreateDir(ctx, parent, "active")
		if err != nil {
			return nil, fmt.Errorf("CloudNav cloud lock is held or could not be acquired: %w", err)
		}
		return id, nil
	case "cloudnav-unlock":
		if len(args) != 1 || args[0] == "" {
			return nil, fmt.Errorf("exact lock ID required")
		}
		parent, err := f.dirCache.FindDir(ctx, cloudNavLockParent, false)
		if err == fs.ErrorDirNotFound {
			return nil, nil
		}
		if err != nil {
			return nil, err
		}
		id, found, err := f.FindLeaf(ctx, parent, "active")
		if err != nil || !found {
			return nil, err
		}
		if id != args[0] {
			return nil, fmt.Errorf("CloudNav lock belongs to another run")
		}
		return nil, f.deleteObject(ctx, id)
	default:
		return nil, fs.ErrorCommandNotFound
	}
}
