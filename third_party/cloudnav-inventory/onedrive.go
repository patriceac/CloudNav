package onedrive

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"path"
	"strings"
	"time"

	"github.com/rclone/rclone/backend/onedrive/api"
	"github.com/rclone/rclone/fs"
	"github.com/rclone/rclone/fs/list"
	inventory "github.com/rclone/rclone/lib/cloudnavinventory"
)

func (f *Fs) cloudNavList(ctx context.Context, dir string, callback fs.ListRCallback) error {
	root, err := f.dirCache.FindDir(ctx, dir, false)
	if err != nil {
		return err
	}
	// v2 keys shortcuts by their local ID, independently of the shared target.
	identity := "onedrive-v2:" + f.driveID + ":" + root
	state := inventory.Load[*api.Item](f.opt.CloudNavCache, identity)
	progress := inventory.NewProgress(ctx, f.opt.CloudNavCache, "changes")
	if err := f.cloudNavRefresh(ctx, root, true, &state, progress); err != nil {
		return err
	}
	cached := state.Scopes
	state.Scopes = make(map[string]inventory.Snapshot[*api.Item])
	out := list.NewHelper(callback)
	active := make(map[string]bool)
	var emit func(string, string, inventory.Snapshot[*api.Item]) error
	emit = func(root, prefix string, scope inventory.Snapshot[*api.Item]) error {
		if active[root] {
			return errors.New("cyclic OneDrive shared folders")
		}
		active[root] = true
		defer delete(active, root)
		_, drive, _ := f.parseNormalizedID(root)
		nodes := make(map[string]inventory.Node, len(scope.Items))
		for id, item := range scope.Items {
			if item == nil {
				return errors.New("incomplete OneDrive inventory")
			}
			if id == root {
				continue
			}
			parent := item.ParentReference
			if parent == nil {
				return errors.New("OneDrive inventory item has no parent")
			}
			// Graph can change a shared drive ID's casing between its shortcut and
			// its delta response. Use the feed's drive ID for both keys and parents.
			parentID, _, _ := f.parseNormalizedID(parent.GetID())
			nodes[id] = inventory.Node{Parent: drive + "#" + parentID, Name: f.opt.Enc.ToStandardName(item.Name), Directory: item.GetFolder() != nil}
		}
		entries, err := inventory.Paths(root, nodes)
		if err != nil {
			return err
		}
		for _, entry := range entries {
			if err := ctx.Err(); err != nil {
				return err
			}
			item := *scope.Items[entry.ID]
			item.Name = nodes[entry.ID].Name
			if item.RemoteItem != nil {
				if item.RemoteItem.ID == "" || item.RemoteItem.ParentReference == nil || item.RemoteItem.ParentReference.DriveID == "" {
					return errors.New("OneDrive shared item has no target identity")
				}
				remote := *item.RemoteItem
				remote.Name = item.Name // Preserve this shortcut's local name.
				item.RemoteItem = &remote
			}
			remotePath := path.Join(prefix, entry.Path)
			parent := path.Dir(remotePath)
			if parent == "." {
				parent = ""
			}
			value, err := f.itemToDirEntry(ctx, parent, &item)
			if err != nil {
				return err
			}
			if item.RemoteItem != nil && item.GetFolder() == nil {
				value, err = f.NewObject(ctx, remotePath)
				if err != nil {
					return err
				}
			}
			if err = out.Add(value); err != nil {
				return err
			}
			if item.RemoteItem != nil && item.GetFolder() != nil {
				target := item.GetID()
				shared, refreshed := state.Scopes[target]
				if !refreshed {
					shared = cached[target]
					if shared.Identity != target || shared.Items == nil {
						shared = inventory.Snapshot[*api.Item]{Identity: target}
					}
					if err := f.cloudNavRefresh(ctx, target, false, &shared, progress); err != nil {
						return fmt.Errorf("refresh OneDrive shared folder: %w", err)
					}
					state.Scopes[target] = shared
				}
				if err := emit(target, remotePath, shared); err != nil {
					return err
				}
			}
		}
		return nil
	}
	if err := emit(root, "", state); err != nil {
		return err
	}
	if err := out.Flush(); err != nil {
		return err
	}
	return inventory.Save(ctx, f.opt.CloudNavCache, state)
}

func (f *Fs) cloudNavRefresh(ctx context.Context, root string, main bool, state *inventory.Snapshot[*api.Item], progress *inventory.Progress) error {
	if f.opt.CloudNavFull {
		state.Cursor = ""
	}
	for attempt := 0; attempt < 2; attempt++ {
		full := state.Cursor == ""
		progress.Mode = "changes"
		if full {
			progress.Mode = "full"
			state.Items = make(map[string]*api.Item)
		}
		err := f.cloudNavDelta(ctx, root, main, state, progress)
		if errors.Is(err, inventory.ErrRescan) && !full {
			state.Cursor = ""
			continue
		}
		return err
	}
	return inventory.ErrRescan
}

func (f *Fs) cloudNavConventional(ctx context.Context, dir string, callback fs.ListRCallback) error {
	p := inventory.NewProgress(ctx, f.opt.CloudNavCache, "scan")
	return f.listRFull(ctx, dir, func(entries fs.DirEntries) error { p.Page(len(entries), time.Now()); return callback(entries) })
}

func (f *Fs) cloudNavDelta(ctx context.Context, root string, main bool, state *inventory.Snapshot[*api.Item], progress *inventory.Progress) error {
	opts := f.newOptsCall(root, "GET", "/delta")
	if main {
		opts = f.buildDriveDeltaOpts("")
	}
	_, drive, _ := f.parseNormalizedID(root)
	allowed := opts.RootURL + "/" + drive + "/"
	opts.Parameters = map[string][]string{"$top": {fmt.Sprint(f.opt.ListChunk)}}
	if state.Cursor != "" {
		if !strings.HasPrefix(state.Cursor, allowed) {
			return inventory.ErrRescan
		}
		opts.RootURL, opts.Path, opts.Parameters = state.Cursor, "", nil
	}
	for {
		started := time.Now()
		var result api.DeltaResponse
		var response *http.Response
		err := f.pacer.Call(func() (bool, error) {
			var err error
			response, err = f.srv.CallJSON(ctx, &opts, nil, &result)
			return shouldRetry(ctx, response, err)
		})
		if err != nil {
			if response != nil && (response.StatusCode == http.StatusGone || response.StatusCode == http.StatusBadRequest) {
				return inventory.ErrRescan
			}
			return err
		}
		progress.Page(len(result.Value), started)
		for i := range result.Value {
			item := &result.Value[i]
			id := item.ID // The feed's local ID also identifies shortcut deletions.
			if id == "" {
				return errors.New("OneDrive change has no ID")
			}
			if !strings.Contains(id, "#") {
				id = drive + "#" + id
			}
			if item.Deleted != nil {
				delete(state.Items, id)
			} else {
				state.Items[id] = item
			}
		}
		if result.NextLink == "" {
			if !strings.HasPrefix(result.DeltaLink, allowed) {
				return errors.New("OneDrive listing has no valid change token")
			}
			state.Cursor = result.DeltaLink
			return nil
		}
		if !strings.HasPrefix(result.NextLink, allowed) {
			return errors.New("OneDrive listing has an invalid next page")
		}
		opts.RootURL, opts.Path, opts.Parameters = result.NextLink, "", nil
	}
}
