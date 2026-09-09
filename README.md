<p align="center">
  <img src="assets/CloudNav-icon.png" width="132" alt="CloudNav icon">
</p>

# CloudNav 1.4.1

CloudNav is a native, portable Windows 11 utility that brings together two settings that are usually scattered across the system: cloud entries in the File Explorer navigation pane and the locations of Windows personal folders.

**[Download CloudNav for Windows](https://github.com/patriceac/CloudNav/releases/latest/download/CloudNav.exe)**

One standalone `.exe`, with no installer or additional application runtime.

The shared cloud analysis and synchronization modes described below are available in source builds. They are not included in the v1.2.2 release linked above.

## Features

- show or hide the **My Drive** entry in the navigation pane;
- show or hide the detected **OneDrive** account entry;
- hide the **Google Drive** virtual drive letter without blocking access to its files;
- redirect Desktop, Documents, Pictures, Downloads, Music, and Videos to their local location, OneDrive, Google Drive, or a custom folder;
- identify providers consistently across Explorer visibility, personal folders, and migration;
- disable OneDrive automatic startup or launch its uninstaller when no managed personal folder still depends on OneDrive;
- analyze OneDrive and Google Drive together, then choose either one-way copy direction or a manual bidirectional sync without repeating the comparison;
- preview per-file actions, preserve both conflicting versions, archive overwritten/deleted files, and recover interrupted transfers through a reviewed merge;
- inspect complete current/proposed paths and collateral folder changes before an action-specific final confirmation;
- apply Explorer visibility independently, with the Apply button enabled only for pending changes;
- remember the window position and bring it back onto a visible display if the monitor layout changes.

CloudNav detects the paths and account labels available on the current PC. No user name, account name, or user-specific path is hard-coded in the application.

## Safety

The checkboxes in the main window only control visibility in the File Explorer navigation pane. Hiding the Google Drive letter uses the Windows `NoDrives` policy: the icon disappears, but the drive and its files remain accessible.

Personal-folder changes are shown in a separate review before anything is applied. Current and proposed paths can be selected and copied in full. CloudNav validates destinations, blocks nested paths, and attempts to restore previous locations if a redirect-only operation fails. Copy/move failures report any steps that already completed.

OneDrive client actions stay disabled while Desktop, Documents, Pictures, Downloads, Music, or Videos points anywhere inside the detected OneDrive root. CloudNav names the folders that must be moved and checks their locations again immediately before disabling automatic startup or launching the uninstaller. This check covers those six managed personal folders only, not every other folder that OneDrive may synchronize. Before uninstalling, CloudNav asks the user to verify that OneDrive is up to date and warns that online-only files will remain accessible through OneDrive.com.

**Analyser** reads an inventory from each account and compares their union. The same comparison supports **OneDrive → Google Drive**, **Google Drive → OneDrive**, and **OneDrive ↔ Google Drive**. Changing the direction only rebuilds the action plan in memory. The report distinguishes OneDrive-only, Google-Drive-only, different, identical, and blocked files, with a proposed action for each path.

The selected direction is saved immediately for the current Windows user and restored when the assistant or application is reopened. A missing or invalid preference defaults to OneDrive → Google Drive. Restoring a direction does not start a transfer or bypass analysis and review.

One-way modes copy the selected source's new and different files with `rclone copy`, preserving destination-only files. Bidirectional mode compares both inventories against the last successful two-way baseline. Without a baseline it merges files without propagating deletions. If both versions changed, both accounts retain the OneDrive version under the original name and the Google version under a unique `.conflit-Google-…` sibling name. When an edit conflicts with a deletion, the edited version is restored to the other account.

Every real transfer has a final review. CloudNav rereads both accounts before applying the plan and refuses to start if the inventories, account identity, or baseline changed since analysis. This is not an atomic cloud snapshot: files should remain idle during transfer. Copies use literal file lists and direct destination lookups. Overwritten files and propagated deletions are retained under `.CloudNav-history/<run>/` on the affected account, outside the compared set. These archives consume cloud storage and are not automatically purged; restore files from them using the provider's ordinary file tools.

Bidirectional completion requires another comparison and saves the new baseline only when both inventories match under the comparison rules. A pending journal is written before mutations. Failed/interrupted transfers and one-way changes leave the next two-way run in an explicitly reviewed, non-deleting recovery merge. Missing, corrupt, or account-mismatched history never authorizes deletions. Reanalyze after any real transfer attempt. Only one CloudNav cloud operation can run at a time in the Windows session; external synchronization must be paused separately. Scheduling and automatic background execution are not included.

Comparison prefers a shared checksum when available; otherwise it uses size and UTC modification time at second precision. OneDrive and Google Drive do not always expose a common hash. Equality is therefore not a promise of a downloaded byte-for-byte comparison. Duplicate names, case/Unicode collisions, file/directory collisions, unsupported paths, and unreadable inventories block execution.

CloudNav embeds the pinned rclone 1.75.0 engine, so the distributed application remains one portable executable. The engine is extracted into the current user's local application-data directory and byte-verified before use. OAuth credentials and migration state remain per-user in `%LOCALAPPDATA%\CloudNav`; no credentials are embedded. rclone is redistributed under the MIT License; see [`third_party/rclone-LICENSE.txt`](third_party/rclone-LICENSE.txt).

When Desktop, Documents, or Pictures moves from OneDrive to Google Drive, CloudNav can disable OneDrive Known Folder Backup before a redirect-only operation. The preview and final review explicitly list other protected folders that will return to their local locations. No OneDrive file is deleted. Copy/move plans requiring this backup release are blocked: use the migration assistant to copy the original cloud data and check the result yourself before redirecting, so releasing backup cannot change the copy source to an empty local folder.

If a scheduled task named `OneDrive-GDrive-Bidirectional-Sync` is present, CloudNav reports **external synchronization detected**. Completing a transfer makes the ordinary personal-folder manager available without automatically recommending redirect-only.

Analysis shows files received from each provider with an animated activity bar. Transfers show the current operation's percentage, speed, and ETA when available; these are not an overall multi-operation ETA. The pre-transfer comparison remains available for consultation. Progress statistics are emitted once per second during active transfer operations.

Inventories use rclone's `lsjson --recursive --hash`, with `--fast-list` and `--onedrive-delta` to reduce directory-listing requests. Directory entries are checked for ambiguity; transfer plans contain files only. Transfers use `--files-from-raw` and `--no-traverse` to restrict work to the plan. Empty directories, OneDrive Personal Vault, native Google documents, and `.CloudNav-history` are outside the compared file set and are not counted. Native Google document export is not supported by this sync flow.

**Voir les détails…** opens the comparison table with category filters, proposed actions, and selectable full paths. Interrupted results are marked partial, and a new analysis or account connection clears the previous report. Two-way baseline metadata is stored per user beside the rclone configuration; it contains file paths, sizes, times, available hashes, and a digest binding it to the configured account pair. OAuth tokens are not copied into this baseline.

## Usage

1. Download `CloudNav.exe` from the latest GitHub Release.
2. Make sure Google Drive for desktop and/or OneDrive is installed and running, depending on the features you need.
3. Run the executable directly; no installation is required.
4. Use **Appliquer la visibilité** for Explorer entries, **Dossiers personnels…** for folder locations, or **Comparer les comptes…** in **Synchronisation cloud** for cloud analysis and transfer.
5. In the folder manager, select **Vérifier…**, review all affected folders, then choose the explicit copy, move, or redirect action.

File Explorer may restart once to reload its configuration. Any open folder windows will close when that happens.

## Requirements

- Windows 11 x64;
- Google Drive for desktop and/or OneDrive, depending on the selected features;
- no additional library or application runtime.

The CloudNav user interface is currently available in French.

Official OneDrive and Google Drive artwork is used only to identify the corresponding storage provider. Source and trademark details are documented in [`assets/BRAND_ASSETS.md`](assets/BRAND_ASSETS.md).

## Build from source

Visual Studio Build Tools 2022 with the x64 C++ toolchain is required. From PowerShell:

```powershell
.\build.ps1 -Configuration Release
```

The build downloads the official pinned rclone archive, verifies its SHA-256 checksum, and embeds the engine as a resource. The Release build uses the static C++ runtime (`/MT`) and produces:

- `build\Release\CloudNav.exe`;
- `build\Release\CloudNavTests.exe`;
- `build\Release\CloudNavWindowPositionTests.exe`;
- `build\Release\CloudNavUiTests.exe`.

The build script runs the logic tests automatically. UI and integration scenarios for the isolated Windows test harness are available in `tests/`.

With the Codex Hyper-V SYSTEM broker installed, run the repeatable offline release checks:

```powershell
.\tests\Invoke-ReleaseChecks.ps1
# Reuse an existing release build and resume matching passed checks:
.\tests\Invoke-ReleaseChecks.ps1 -SkipBuild
# Run only the relevant scenarios:
.\tests\Invoke-ReleaseChecks.ps1 -SkipBuild -Scenario Ui,MigrationResume
# Check analysis progress, cancellation/resume, and the embedded engine:
.\tests\Invoke-ReleaseChecks.ps1 -SkipBuild -Scenario Migration,MigrationResume,MigrationEngine
# Check the KPI summary, category filters, and partial/stale report handling:
.\tests\Invoke-ReleaseChecks.ps1 -SkipBuild -Scenario MigrationReport,MigrationEngine
```

The runner keys checkpoints to the exact executable, test driver, action file, and runner hashes. It checks declared assertions, screenshot presence, process cleanup, VM shutdown, and disposable payload deletion before saving a pass. Screenshots still require visual review. The engine test exercises the embedded rclone against synthetic local directories with no cloud credentials: read-only analysis, both copy directions, first merge, conflict preservation, archived deletion, stale-plan rejection, and interrupted recovery. UI tests check switching modes without discarding the comparison. Offline fixtures and demo operations do not qualify real OAuth, provider-specific cloud behavior, cloud performance, or actual OneDrive uninstallation.

The JSON parser is vendored from nlohmann/json v3.12.0 under the MIT License; see [`third_party/nlohmann/LICENSE.MIT`](third_party/nlohmann/LICENSE.MIT).
