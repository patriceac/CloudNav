<p align="center">
  <img src="assets/CloudNav-icon.png" width="132" alt="CloudNav icon">
</p>

# CloudNav 1.4.1

CloudNav is a native, portable Windows 11 utility that brings together two settings that are usually scattered across the system: cloud entries in the File Explorer navigation pane and the locations of Windows personal folders.

**[Download CloudNav for Windows](https://github.com/patriceac/CloudNav/releases/latest/download/CloudNav.exe)**

One standalone `.exe`, with no installer or additional application runtime.

## Features

- show or hide the **My Drive** entry in the navigation pane;
- show or hide the detected **OneDrive** account entry;
- hide the **Google Drive** virtual drive letter without blocking access to its files;
- redirect Desktop, Documents, Pictures, Downloads, Music, and Videos to their local location, OneDrive, Google Drive, or a custom folder;
- identify providers consistently across Explorer visibility, personal folders, and migration;
- disable OneDrive automatic startup or launch its uninstaller when no managed personal folder still depends on OneDrive;
- migrate OneDrive to Google Drive with a one-way copy, live percentage/speed/ETA, an analysis-based file list, safe cancellation and resume;
- inspect complete current/proposed paths and collateral folder changes before an action-specific final confirmation;
- apply Explorer visibility independently, with the Apply button enabled only for pending changes;
- remember the window position and bring it back onto a visible display if the monitor layout changes.

CloudNav detects the paths and account labels available on the current PC. No user name, account name, or user-specific path is hard-coded in the application.

## Safety

The checkboxes in the main window only control visibility in the File Explorer navigation pane. Hiding the Google Drive letter uses the Windows `NoDrives` policy: the icon disappears, but the drive and its files remain accessible.

Personal-folder changes are shown in a separate review before anything is applied. Current and proposed paths can be selected and copied in full. CloudNav validates destinations, blocks nested paths, and attempts to restore previous locations if a redirect-only operation fails. Copy/move failures report any steps that already completed.

OneDrive client actions stay disabled while Desktop, Documents, Pictures, Downloads, Music, or Videos points anywhere inside the detected OneDrive root. CloudNav names the folders that must be moved and checks their locations again immediately before disabling automatic startup or launching the uninstaller. This check covers those six managed personal folders only, not every other folder that OneDrive may synchronize. Before uninstalling, CloudNav asks the user to verify that OneDrive is up to date and warns that online-only files will remain accessible through OneDrive.com.

The migration assistant uses `rclone copy`: it never deletes destination files. After analysis, **Copier** copies only the new and changed files in that analysis, using a literal file list and direct destination lookups. It does not rescan either entire account or run a final verification. Files added outside that list afterward are left for a future analysis. Selected files are read in their current state; this is not a frozen snapshot. Cancellation and retry reuse the same list within the open assistant and skip files already matching. After reopening the assistant, analyze again to create a fresh list. Users can run **Analyser** whenever they want another comparison. Completion reports the copy outcome, not verified account equality.

CloudNav embeds the pinned rclone 1.75.0 engine, so the distributed application remains one portable executable. The engine is extracted into the current user's local application-data directory and byte-verified before use. OAuth credentials and migration state remain per-user in `%LOCALAPPDATA%\CloudNav`; no credentials are embedded. rclone is redistributed under the MIT License; see [`third_party/rclone-LICENSE.txt`](third_party/rclone-LICENSE.txt).

When Desktop, Documents, or Pictures moves from OneDrive to Google Drive, CloudNav can disable OneDrive Known Folder Backup before a redirect-only operation. The preview and final review explicitly list other protected folders that will return to their local locations. No OneDrive file is deleted. Copy/move plans requiring this backup release are blocked: use the migration assistant to copy the original cloud data and check the result yourself before redirecting, so releasing backup cannot change the copy source to an empty local folder.

If a scheduled task named `OneDrive-GDrive-Bidirectional-Sync` is present, CloudNav reports **external synchronization detected**. Completing migration opens the ordinary personal-folder manager without claiming verification or automatically recommending redirect-only.

Analysis shows items listed, files compared, and elapsed time with an animated activity bar. Copy shows transfer percentage, speed, and ETA when available. The analysis summary stays visible and is labeled as the pre-copy result. Progress statistics are emitted once per second at the engine's active logging level.

Only analysis uses rclone's `--fast-list` and `--onedrive-delta` to reduce directory-listing requests. Copy uses `--files-from-raw` and `--no-traverse` to restrict work to the analyzed new and changed files. It does not create unrelated empty directories.

After analysis, the assistant shows counts of new, changed, matching, and destination-only files, errors, and the estimated bytes to copy. **Voir les détails…** opens a file table with category filters and selectable full paths. The report comes from the same dry-run comparison, without a second cloud scan. Matching means equal under rclone's comparison rules; this is not a claim that every file was downloaded and compared byte for byte. Personal Vault and native Google documents are excluded before comparison and are explicitly marked as uncounted. Interrupted results are labeled partial, unknown sizes remain unavailable, and a new analysis or account connection clears the previous report.

## Usage

1. Download `CloudNav.exe` from the latest GitHub Release.
2. Make sure Google Drive for desktop and/or OneDrive is installed and running, depending on the features you need.
3. Run the executable directly; no installation is required.
4. Use **Appliquer la visibilité** for Explorer entries, **Dossiers personnels…** for folder locations, or the separate **Migration cloud** section for cloud transfer.
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

The runner keys checkpoints to the exact executable, test driver, action file, and runner hashes. It checks declared assertions, screenshot presence, process cleanup, VM shutdown, and disposable payload deletion before saving a pass. Screenshots still require visual review. The migration engine test uses the embedded rclone with synthetic local directories and an empty account configuration: it checks that analysis leaves files and timestamps untouched, copying retains destination extras and exclusions, and files added after analysis stay outside the copy list. Tests use offline fixtures and demo operations; they do not qualify real OAuth, cloud transfers, cloud performance, or actual OneDrive uninstallation.
