package drive

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/rclone/rclone/fs"
	"github.com/rclone/rclone/fs/list"
	inventory "github.com/rclone/rclone/lib/cloudnavinventory"
	drive "google.golang.org/api/drive/v3"
	"google.golang.org/api/googleapi"
)

func (f *Fs) cloudNavList(ctx context.Context, dir string, callback fs.ListRCallback) error {
	// These views can span independent change feeds or apply special membership rules.
	if f.isTeamDrive || f.opt.SharedWithMe || f.opt.StarredOnly || f.opt.TrashedOnly || f.rootFolderID == "appDataFolder" {
		return inventory.ErrUnsupported
	}
	root, err := f.dirCache.FindDir(ctx, dir, false)
	if err != nil {
		return err
	}
	if root == "root" {
		var item *drive.File
		err = f.pacer.Call(func() (bool, error) {
			item, err = f.svc.Files.Get("root").Fields("id").Context(ctx).Do()
			return f.shouldRetry(ctx, err)
		})
		if err != nil {
			return err
		}
		root = item.Id
		f.dirCache.Put("", root)
	}
	state := inventory.Load[*drive.File](f.opt.CloudNavCache, "drive:"+root)
	if f.opt.CloudNavFull {
		state.Cursor = ""
	}
	for attempt := 0; attempt < 2; attempt++ {
		full := state.Cursor == ""
		mode := "changes"
		if full {
			mode = "full"
			state.Items = make(map[string]*drive.File)
		}
		progress := inventory.NewProgress(ctx, f.opt.CloudNavCache, mode)
		if full {
			// Take the token BEFORE enumerating, then apply changes made during the scan.
			state.Cursor, err = f.changeNotifyStartPageToken(ctx)
			if err == nil {
				err = f.cloudNavFull(ctx, &state, progress)
			}
		}
		if err == nil {
			err = f.cloudNavChanges(ctx, &state, progress)
		}
		if errors.Is(err, inventory.ErrRescan) && !full {
			state.Cursor = ""
			err = nil
			continue
		}
		if err != nil {
			return err
		}
		nodes := make(map[string]inventory.Node, len(state.Items))
		for id, item := range state.Items {
			if item == nil {
				return errors.New("incomplete Google inventory")
			}
			if len(item.Parents) > 1 {
				return inventory.ErrUnsupported
			}
			if len(item.Parents) == 1 {
				nodes[id] = inventory.Node{Parent: item.Parents[0], Name: f.opt.Enc.ToStandardName(item.Name), Directory: item.MimeType == driveFolderType}
			}
		}
		entries, err := inventory.Paths(root, nodes)
		if err != nil {
			return err
		}
		out := list.NewHelper(callback)
		for _, entry := range entries {
			if err := ctx.Err(); err != nil {
				return err
			}
			item := *state.Items[entry.ID]
			item.Name = nodes[entry.ID].Name
			info := &item
			shortcut := isShortcut(info)
			if shortcut {
				if f.opt.SkipShortcuts {
					continue
				}
				info, err = f.resolveShortcut(ctx, info)
				if err != nil {
					return err
				}
				if f.opt.SkipDanglingShortcuts && info.MimeType == shortcutMimeTypeDangling {
					continue
				}
			}
			value, err := f.itemToDirEntry(ctx, entry.Path, info)
			if err != nil {
				return err
			}
			if err = out.Add(value); err != nil {
				return err
			}
			if shortcut && value != nil && info.MimeType == driveFolderType {
				if err := f.cloudNavConventional(ctx, entry.Path, func(children fs.DirEntries) error {
					for _, child := range children {
						if err := out.Add(child); err != nil {
							return err
						}
					}
					return nil
				}); err != nil {
					return err
				}
			}
		}
		if err := out.Flush(); err != nil {
			return err
		}
		return inventory.Save(ctx, f.opt.CloudNavCache, state)
	}
	return inventory.ErrRescan
}

func (f *Fs) cloudNavConventional(ctx context.Context, dir string, callback fs.ListRCallback) error {
	p := inventory.NewProgress(ctx, f.opt.CloudNavCache, "scan")
	return f.listRFull(ctx, dir, func(entries fs.DirEntries) error { p.Page(len(entries), time.Now()); return callback(entries) })
}

func (f *Fs) cloudNavFull(ctx context.Context, state *inventory.Snapshot[*drive.File], progress *inventory.Progress) error {
	request := f.svc.Files.List().Q("trashed=false").Spaces("drive").Corpora("user").PageSize(1000).
		Fields(googleapi.Field(fmt.Sprintf("nextPageToken,incompleteSearch,files(%s,trashed)", f.getFileFields(ctx))))
	for {
		started := time.Now()
		var result *drive.FileList
		err := f.pacer.Call(func() (bool, error) {
			var err error
			result, err = request.Context(ctx).Do()
			return f.shouldRetry(ctx, err)
		})
		if err != nil {
			return err
		}
		if result.IncompleteSearch {
			return errors.New("Google inventory search is incomplete")
		}
		progress.Page(len(result.Files), started)
		for _, item := range result.Files {
			if item == nil || item.Id == "" {
				return errors.New("Google inventory item has no ID")
			}
			state.Items[item.Id] = item
		}
		if result.NextPageToken == "" {
			return nil
		}
		request.PageToken(result.NextPageToken)
	}
}

func (f *Fs) cloudNavChanges(ctx context.Context, state *inventory.Snapshot[*drive.File], progress *inventory.Progress) error {
	for token := state.Cursor; ; {
		started := time.Now()
		request := f.svc.Changes.List(token).PageSize(1000).Spaces("drive").IncludeRemoved(true).
			Fields(googleapi.Field(fmt.Sprintf("nextPageToken,newStartPageToken,changes(fileId,removed,file(%s,trashed))", f.getFileFields(ctx))))
		var result *drive.ChangeList
		err := f.pacer.Call(func() (bool, error) {
			var err error
			result, err = request.Context(ctx).Do()
			return f.shouldRetry(ctx, err)
		})
		if err != nil {
			var apiError *googleapi.Error
			if errors.As(err, &apiError) && (apiError.Code == 400 || apiError.Code == 410 || apiError.Code == 404) {
				return inventory.ErrRescan
			}
			return err
		}
		progress.Page(len(result.Changes), started)
		for _, change := range result.Changes {
			if change.FileId == "" {
				return errors.New("Google change has no ID")
			}
			if change.Removed || (change.File != nil && change.File.Trashed) {
				delete(state.Items, change.FileId)
			} else {
				if change.File == nil || change.File.Id != change.FileId {
					return errors.New("Google change has no file metadata")
				}
				// A newly accessible folder need not enumerate all its descendants
				// in the change feed. Rebuild membership before trusting that subtree.
				if _, known := state.Items[change.FileId]; !known && change.File.MimeType == driveFolderType {
					return inventory.ErrRescan
				}
				state.Items[change.FileId] = change.File
			}
		}
		if result.NextPageToken == "" {
			if result.NewStartPageToken == "" {
				return errors.New("Google listing has no change token")
			}
			state.Cursor = result.NewStartPageToken
			return nil
		}
		token = result.NextPageToken
	}
}
