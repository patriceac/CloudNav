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
	identity := "onedrive:" + f.driveID + ":" + root
	state := inventory.Load[*api.Item](f.opt.CloudNavCache, identity)
	if f.opt.CloudNavFull {
		state.Cursor = ""
	}
	for attempt := 0; attempt < 2; attempt++ {
		full := state.Cursor == ""
		mode := "changes"
		if full {
			mode = "full"
			state.Items = make(map[string]*api.Item)
		}
		progress := inventory.NewProgress(ctx, f.opt.CloudNavCache, mode)
		err = f.cloudNavDelta(ctx, &state, progress)
		if errors.Is(err, inventory.ErrRescan) && !full {
			state.Cursor = ""
			continue
		}
		if err != nil {
			return err
		}
		nodes := make(map[string]inventory.Node, len(state.Items))
		for id, item := range state.Items {
			if item == nil {
				return errors.New("incomplete OneDrive inventory")
			}
			if id == root {
				continue
			}
			parent := item.GetParentReference()
			if parent == nil {
				return errors.New("OneDrive inventory item has no parent")
			}
			nodes[id] = inventory.Node{Parent: parent.GetID(), Name: f.opt.Enc.ToStandardName(item.GetName()), Directory: item.GetFolder() != nil}
		}
		entries, err := inventory.Paths(root, nodes)
		if err != nil {
			return err
		}
		out := list.NewHelper(callback)
		// Shared targets have independent feeds. Refresh only those subtrees live.
		var shared func(string) error
		shared = func(remote string) error {
			started := time.Now()
			children, err := f.List(ctx, remote)
			if err != nil {
				return err
			}
			progress.Mode = "scan"
			progress.Page(len(children), started)
			for _, child := range children {
				if err := out.Add(child); err != nil {
					return err
				}
				if _, isDir := child.(fs.Directory); isDir {
					if err := shared(child.Remote()); err != nil {
						return err
					}
				}
			}
			return nil
		}
		for _, entry := range entries {
			if err := ctx.Err(); err != nil {
				return err
			}
			item := *state.Items[entry.ID]
			item.Name = nodes[entry.ID].Name
			parent := path.Dir(entry.Path)
			if parent == "." {
				parent = ""
			}
			value, err := f.itemToDirEntry(ctx, parent, &item)
			if err != nil {
				return err
			}
			if item.RemoteItem != nil && item.GetFolder() == nil {
				value, err = f.NewObject(ctx, entry.Path)
				if err != nil {
					return err
				}
			}
			if err = out.Add(value); err != nil {
				return err
			}
			if item.RemoteItem != nil && item.GetFolder() != nil {
				if err := shared(entry.Path); err != nil {
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

func (f *Fs) cloudNavDelta(ctx context.Context, state *inventory.Snapshot[*api.Item], progress *inventory.Progress) error {
	opts := f.buildDriveDeltaOpts("")
	allowed := opts.RootURL + "/" + f.driveID + "/"
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
			id := item.GetID()
			if id == "" {
				return errors.New("OneDrive change has no ID")
			}
			if !strings.Contains(id, "#") {
				id = f.driveID + "#" + id
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
