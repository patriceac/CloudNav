<p align="center">
  <img src="assets/CloudNav-icon.png" width="132" alt="CloudNav icon">
</p>

# CloudNav 1.4.14

CloudNav is a native, portable Windows 11 utility that brings together two settings that are usually scattered across the system: cloud entries in the File Explorer navigation pane and the locations of Windows personal folders.

**[Download CloudNav for Windows](https://github.com/patriceac/CloudNav/releases/latest/download/CloudNav.exe)**

One standalone `.exe`, with no installer or additional application runtime.

Version 1.4.14 finishes a two-way sync after live delta and history checks when both catalogs still match their verified shared baseline. Unchanged runs skip history uploads and full verification listings. Changes, missing shared history, and recovery retain the complete synchronization path. Version 1.4.13 added concurrent inventory/history reads, history caching with live version checks, and parallel OneDrive shared-folder delta feeds.

Version 1.4.6 opens the matching OneDrive backup dialog when a personal-folder change needs it. The user stops the selected backups, then clicks Continue setup in CloudNav to apply the saved locations. Redirect only never reads, compares, copies, or downloads personal file contents. The title bar displays the running version.

## Features

- show or hide the **My Drive** entry in the navigation pane;
- show or hide the detected **OneDrive** account entry;
- hide the **Google Drive** virtual drive letter without blocking access to its files;
- redirect Desktop, Documents, Pictures, Downloads, Music, and Videos to their local location, OneDrive, Google Drive, or a custom folder;
- identify providers consistently across Explorer visibility, personal folders, and migration;
- disable OneDrive automatic startup or launch its uninstaller when no managed personal folder still depends on OneDrive;
- install OneDrive or Google Drive through their official setup programs, and uninstall Google Drive with personal-folder checks;
- analyze OneDrive and Google Drive together, then choose either one-way copy direction or a manual bidirectional sync without repeating the comparison;
- preview per-file actions, preserve both conflicting versions, archive overwritten/deleted files, and recover interrupted transfers through a reviewed merge;
- inspect complete current/proposed paths and selected folder changes before an action-specific final confirmation;
- apply Explorer visibility independently, with the Apply button enabled only for pending changes;
- remember the window position and bring it back onto a visible display if the monitor layout changes.

CloudNav detects the paths and account labels available on the current PC. No user name, account name, or user-specific path is hard-coded in the application.

The navigation section distinguishes installed OneDrive accounts with accessible folders from leftover registrations, missing folders, and unknown availability. Unavailable entries cannot be enabled; an already-visible stale entry can still be hidden. Google Drive's virtual-drive control requires a detected mounted volume, and a remaining My Drive folder is labeled local when the Google client is absent. My Drive visibility and its target belong to the current Windows user. An established target is preserved across visibility changes, even while offline; fresh detection requires one unambiguous candidate. Legacy machine registration is masked only for the current user, without importing its possibly foreign target or modifying other profiles. Other users migrate when they next open CloudNav. The navigation controls group both Google Drive entries before OneDrive. Refresh reloads these states; visibility checkboxes describe Explorer settings, not cloud synchronization health.

## Safety

The checkboxes in the main window only control visibility in the File Explorer navigation pane. Hiding the Google Drive letter uses the current user’s Windows `NoDrives` policy: the icon disappears, but the drive and its files remain accessible.

Personal-folder changes are shown in a separate review before anything is applied. Current and proposed paths can be selected and copied in full. CloudNav validates destinations, blocks nested paths, and attempts to restore previous locations if a redirect-only operation fails. Copy/move failures report any steps that already completed.

After a OneDrive → Google Drive copy, **Set up folders** recommends **Redirect only** for corresponding folders. Redirect only uses the existing destination and changes Windows folder locations without enumerating or reading file contents. It does not copy, download, or compare files, including on saved setup resumes. Original files stay in their current locations; CloudNav does not establish that both folders contain the same files.

CloudNav saves the approved folder pairs in the current user's registry before setup. Recovery is bound to that Windows SID, the cloud account fingerprint and both mounted root identities. Every resume validates the account, mounted root identities, current Windows locations, and any outstanding backup release without inspecting folder contents. Completed paths are recognized, unapproved path changes are rejected, and failed redirection attempts restore their starting Windows locations where possible. A rollback does not undo a backup change already made in OneDrive; the saved plan remains available to continue or explicitly discard.

For protected folders, CloudNav opens the matching OneDrive account's backup dialog. Stop backup for the selected folders, choose **Only in OneDrive** to keep their files there, then return to CloudNav and click **Continue setup**. CloudNav reads the selected backup switches and applies the saved Windows locations only after they are off. It does not click backup switches or make file-retention choices. The dialog opener requires one unambiguous configured OneDrive account and recognized controls. Unavailable settings, cancellation, account/path changes, or an administrator policy keep the saved plan for retry. CloudNav does not change OneDrive's internal registry state or reset/restart OneDrive.

OneDrive client actions stay disabled while Desktop, Documents, Pictures, Downloads, Music, or Videos points anywhere inside the detected OneDrive root. CloudNav names the folders that must be moved and checks their locations again immediately before disabling automatic startup or launching the uninstaller. This check covers those six managed personal folders only, not every other folder that OneDrive may synchronize. Before uninstalling, CloudNav asks the user to verify that OneDrive is up to date and warns that online-only files will remain accessible through OneDrive.com.

## Install or uninstall cloud clients

The main window shows separate **OneDrive client settings** and **Google Drive client settings** sections. Installation detection is independent of account connection and mounted-drive visibility. Each section offers **Install** when its client is absent, or **Uninstall** when it is installed. Unknown installation status disables actions; a missing uninstaller is reported rather than treated as an absent application.

Install downloads the setup program from the links published by [Microsoft](https://support.microsoft.com/en-us/onedrive/choose-between-the-64-bit-32-bit-and-arm-version-of-onedrive) or [Google](https://support.google.com/drive/answer/10838124?hl=en), checks its Authenticode signature and publisher, and opens the normal vendor setup interface. Downloads run in the background and can be cancelled. Follow the installer instructions and sign in afterward. Windows may request elevation. CloudNav refreshes detection when the launched process exits; use **Refresh** if a vendor bootstrapper leaves another setup window running. Exiting setup is not itself reported as successful installation.

Google Drive removal checks the same six personal folders against the detected mounted Google Drive volumes and detected My Drive path, and checks again after confirmation. Unknown roots or unreadable folder locations block removal. Other synced folders, Google Photos backups, and other users are outside this check. Complete synchronization before uninstalling; streamed files will stop being available through the virtual drive, while cloud files remain on the provider's website. Client management does not delete cloud data or change the separate rclone account connections used by CloudNav.

Uninstall uses the registered vendor executable, never a shell command or guessed version folder. Both setup and uninstall programs must have the expected executable name and a trusted vendor signature. The signature check uses the local Windows trust store and cached trust information; it does not perform an online revocation check.

**Analyze** reads an inventory from each account and compares their union. The same comparison supports **OneDrive → Google Drive**, **Google Drive → OneDrive**, and **OneDrive ↔ Google Drive**. Changing the direction only rebuilds the action plan in memory. The report distinguishes OneDrive-only, Google-Drive-only, different, identical, and blocked files, with a proposed action for each path.

During analysis, OneDrive and Google Drive are listed concurrently, without separate preliminary root probes. Each complete inventory validates access to that account. Each account has its own file count and elapsed time, updated once per second even while its engine is silent. Cancel stops both processes; an incomplete listing cannot authorize a transfer. Separate temporary configurations prevent simultaneous token refreshes from overwriting each other. The file counter reports only listing entries received so far; the total and percentage remain unknown until enumeration completes.

During transfer, the denominator comes from the reviewed plan and stays fixed. Successfully completed file operations are counted once; retries do not inflate the planned total. Network traffic is shown separately and explicitly includes retries. ETA uses recent logical progress across the whole job, waits for enough samples, and disappears during stalls or unresolved retry errors. Google quota responses show a retry state. A dedicated Google client has its own quotas and does not guarantee uninterrupted service.

The selected direction is saved immediately for the current Windows user and restored when the assistant or application is reopened. A missing or invalid preference defaults to OneDrive → Google Drive. Restoring a direction does not start a transfer or bypass analysis.

One-way modes copy missing files and directly overwrite older destination files when the source is newer (UTC timestamps at whole-second precision). Equal-date or newer destination files and destination-only files are kept. One-way copies do not archive overwritten files. Bidirectional mode compares both inventories against the last successful two-way baseline. Without a baseline it merges files without propagating deletions. If both versions changed, both accounts retain the OneDrive version under the original name and the Google version under a unique `.conflict-Google-…` sibling name. When an edit conflicts with a deletion, the edited version is restored to the other account.

The completed analysis is the transfer plan. Copy and Sync start from that plan without listing either account again; View details remains available before starting. CloudNav checks the account identity and local and cloud history before transfer; changes invalidate the plan. Files added after analysis are outside the plan; run Analyze again to include them. Bidirectional sync still compares both accounts after transfer before saving its baseline. This is not an atomic cloud snapshot: files should remain idle during transfer. Copies use literal file lists with targeted lookups or directory listings selected from the batch size. In bidirectional mode, overwritten files and propagated deletions are retained under `.CloudNav-history/<run>/` on the affected account, outside the compared set. These archives consume cloud storage and are not automatically purged; restore files from them using the provider's ordinary file tools.

Bidirectional completion requires another comparison and saves the new baseline only when both inventories match under the comparison rules. The same checksummed history is saved in both accounts under `.CloudNav-history/.sync/`, identified by stable provider root IDs rather than login tokens. A pending generation is saved before mutations. Missing, mismatched, incomplete or corrupt cloud history never authorizes deletions. A valid existing local baseline can be upgraded when neither cloud has history yet. Failed/interrupted transfers and one-way changes require a reviewed, non-deleting recovery merge. Reanalyze after any real transfer attempt.

An exclusive OneDrive metadata-folder lock prevents overlapping CloudNav operations across PCs. Locks do not expire while a writer may still be active. The originating PC can recover its own interrupted run using its local lock journal; an unknown lock on another PC requires recovery there. Child transfer processes stop if CloudNav exits. External sync tools do not participate in this lock.

`CloudNav.exe --sync` runs a windowless two-way sync using the current user's saved CloudNav accounts. It requires valid history and stops before changing files if conflicts need review or removals exceed 10% of either account. Options: `--preview` analyzes without transferring user files; `--max-delete-percent N` adjusts the guard; `--check-access filename` requires an existing marker on both accounts; `--publish-history` backs up an existing verified baseline without transferring user files. Results are written to `%LOCALAPPDATA%\CloudNav\Migration\scheduled-sync-result.json` (override with `--result path`). Exit codes are 0 for success, 1 for an error, 2 for a skipped overlapping local run, and 3 for required review. Authentication problems never open a sign-in window during a scheduled run.

`tools/Update-SyncScheduledTask.ps1` updates the existing Windows task to the Release executable while preserving its triggers and user context. It retains network/missed-run settings, prevents overlapping scheduled instances, and backs up the previous task XML. It does not start a sync.

Comparison prefers a shared checksum when available; otherwise it uses size and UTC modification time at second precision. OneDrive and Google Drive do not always expose a common hash. Equality is therefore not a promise of a downloaded byte-for-byte comparison. When Google Drive contains several files at the same path, CloudNav lists that path as ignored and skips every ambiguous object while continuing with the rest of the plan; rename those objects to unique names to include them. Files inside duplicate Google Drive folder paths are skipped for the same reason. Case/Unicode collisions across accounts, unsupported paths, and unreadable inventories still block execution.

CloudNav embeds rclone **1.75.0-cloudnav.8**, built from pinned upstream 1.75.0 sources with OAuth callback, incremental inventory and shared-history support, so the distributed application remains one portable executable. The engine is extracted into the current user's local application-data directory and byte-verified before use. Google Drive uses CloudNav's dedicated installed-app OAuth credentials; they identify CloudNav but do not authenticate a user. Upgrades detect both implicit and explicitly configured rclone shared Google clients and require **Reconnect after upgrade**. Reconnect uses the packaged CloudNav client and a fresh sign-in for that user; valid intentional custom clients are retained. A failed connection leaves the previous configuration intact. Every user signs in separately and receives a per-user token in `%LOCALAPPDATA%\CloudNav`; tokens and migration state are never embedded. rclone is redistributed under the MIT License; see [`third_party/rclone-LICENSE.txt`](third_party/rclone-LICENSE.txt).

If Windows reserves rclone's usual callback port 53682, Google Drive and OneDrive sign-in fall back to an available port on `127.0.0.1`. The same callback URI is used for authorization and token exchange; state validation remains enabled. Other providers retain their registered callback behavior. This follows the loopback redirect rules documented by [Google](https://developers.google.com/identity/protocols/oauth2/native-app) and [Microsoft](https://learn.microsoft.com/en-us/entra/identity-platform/reply-url#localhost-exceptions). The reproducible engine build recipe, patch, and regression test are in `third_party/Build-Rclone.ps1`, `third_party/rclone-cloudnav.patch`, and `third_party/rclone-cloudnav_test.go`. This Windows engine uses a non-CGO build; CloudNav does not use rclone's optional FUSE mount commands.

Account setup uses rclone's [non-interactive continuation protocol](https://rclone.org/commands/rclone_config_create/) without opening CloudNav question dialogs. OneDrive reconnect keeps the configured drive when the signed-in account still offers it; otherwise CloudNav selects the account's personal or business root. Google reconnect keeps an existing Shared Drive choice, while a new connection defaults to My Drive. Existing custom Google developer credentials are preserved; otherwise CloudNav supplies its dedicated desktop client before opening the per-user browser sign-in. Configurations that still depend on rclone's retired shared client are treated as disconnected and must reconnect once.

Setup stages changes beside the configuration and replaces the saved file only after all required fields and a read-only root listing succeed. Incomplete OneDrive configurations require reconnecting and selecting a drive. Provider errors stop the attempt; reconnect starts discovery again. Setup is limited to 32 continuation steps and five minutes per subprocess. Authentication and readiness-probe output are excluded from diagnostic logs. Both accounts are checked before comparison. Existing logs from older versions may contain credentials: do not share them unredacted, and revoke any credentials exposed through those logs.

When Desktop, Documents, or Pictures moves from OneDrive to Google Drive, use the selected-folder backup-release flow described above. If files were already copied, choose **Redirect only** to use the existing destination without a file transfer or content check.

If a scheduled task named `OneDrive-GDrive-Bidirectional-Sync` is present, CloudNav reports **external synchronization detected**. Task detection alone never proves that files match. A completed OneDrive → Google Drive transfer recommends redirect-only for corresponding folders, without an automatic content check before the location change.

Analysis shows each provider's full scan or change check, API pages and items read, and elapsed time while the inventory is being fetched. Completed provider counts remain visible while CloudNav reads shared sync history and releases its lock. After the first complete listing, ordinary account-root analyses apply OneDrive's delta feed and Google Drive's changes feed to an account-bound inventory cache. Each OneDrive shared folder has its own persistent delta token and catalog, including folders shared from another drive. All feeds are saved together only after a successful listing; removing a shortcut removes its cached target when no other shortcut uses it. Folder moves and renames retain child identities; deleted items disappear. Expired tokens or invalid caches trigger a full scan of the affected feed. The first analysis after upgrading to 1.4.11 rebuilds the OneDrive catalog once. Google shortcut targets and special views retain live conventional listings. Errors and cancellation never authorize a plan from stale cached data. Post-transfer verification always reads a fresh full inventory. Transfers show the current operation's percentage, speed, and ETA when available; these are not an overall multi-operation ETA. The pre-transfer comparison remains available for consultation. Progress statistics are emitted once per second during active transfer operations.

Inventories use rclone's `lsjson --recursive --hash`, with `--fast-list` and `--onedrive-delta` to reduce directory-listing requests. Directory entries are checked for ambiguity; transfer plans contain files only. Transfers always use `--files-from-raw` to restrict work to the plan. Small or sparse batches use `--no-traverse`; batches of at least 64 files covering half of each inventory use directory listings. Conflict preservation and archival moves retain targeted lookups. Conflicts use three batches: preserve Google variants under sibling names on OneDrive, rename the Google originals to those sibling names without downloading them again, then copy the OneDrive originals to Google. The pending journal also protects interruption between these phases. Empty directories, OneDrive Personal Vault, native Google documents, and `.CloudNav-history` are outside the compared file set and are not counted. Native Google document export is not supported by this sync flow.

**View details…** opens the comparison table with category filters, proposed actions, and selectable full paths. Interrupted results are marked partial, and a new analysis or account connection clears the previous report. Two-way baseline metadata is stored per user beside the rclone configuration; it contains file paths, sizes, times, available hashes, and a digest binding it to the configured account pair. OAuth tokens are not copied into this baseline.

## Usage

1. Download `CloudNav.exe` from the latest GitHub Release.
2. Make sure Google Drive for desktop and/or OneDrive is installed and running, depending on the features you need.
3. Run the executable directly; no installation is required.
4. Use **Apply visibility** for Explorer entries, **Personal folders…** for folder locations, or **Transfer or sync files…** in **Cloud sync** for cloud analysis and transfer.
5. In the folder manager, select **Review…**, review the selected folders, and confirm the action. If OneDrive still protects them, follow its per-folder backup instructions and return to **Continue setup**.

File Explorer may restart once to reload its configuration. Any open folder windows will close when that happens.

## Requirements

- Windows 11 x64;
- Google Drive for desktop and/or OneDrive, depending on the selected features;
- no additional library or application runtime.

The CloudNav user interface is in English, including reports, confirmations, progress messages, and application errors. User file paths and account names are displayed as stored.

Official OneDrive and Google Drive artwork is used only to identify the corresponding storage provider. Source and trademark details are documented in [`assets/BRAND_ASSETS.md`](assets/BRAND_ASSETS.md).

## Build from source

Visual Studio Build Tools 2022 with the x64 C++ toolchain is required. From PowerShell:

```powershell
.\build.ps1 -Configuration Release -GoogleOAuthConfigPath C:\secure\rclone.conf -GoogleOAuthRemote gdrive
```

The selected Google Drive remote must contain CloudNav's dedicated desktop `client_id` and `client_secret`. The source configuration stays outside the repository; the generated header remains under the ignored `build` directory. Debug builds may omit the OAuth configuration, but Release builds fail closed without it.

The build downloads the official pinned rclone archive, verifies its SHA-256 checksum, and embeds the engine as a resource. The Release build uses the static C++ runtime (`/MT`) and produces:

- `build\Release\CloudNav.exe`;
- `build\Release\CloudNavTests.exe`;
- `build\Release\CloudNavWindowPositionTests.exe`;
- `build\Release\CloudNavUiTests.exe`;
- `build\Release\CloudNavClientTests.exe`.

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
# Check client buttons, confirmations, folder guards, and installation detection:
.\tests\Invoke-ReleaseChecks.ps1 -SkipBuild -Scenario Clients,ClientRuntime
# Check per-user Explorer state, saved setup, and real Windows path redirection/rollback:
.\tests\Invoke-ReleaseChecks.ps1 -SkipBuild -Scenario UserNavigation,KnownFolders
# Check account selection/cancellation and atomic setup failure recovery:
.\tests\Invoke-ReleaseChecks.ps1 -SkipBuild -Scenario Auth,MigrationEngine
```

The runner keys checkpoints to the exact executable, test driver, action file, and runner hashes. It checks declared assertions, screenshot presence, process cleanup, VM shutdown, and disposable payload deletion before saving a pass. Screenshots still require visual review. The engine test exercises the embedded rclone against synthetic local directories with no cloud credentials: read-only analysis, both copy directions, first merge, conflict preservation, archived deletion, reviewed-plan reuse without pre-transfer listings, local-history change rejection, and interrupted recovery. UI tests check switching modes without discarding the comparison. Offline fixtures and demo operations do not qualify real OAuth, provider-specific cloud behavior, cloud performance, or actual OneDrive uninstallation.

Client tests use disposable registry/filesystem fixtures to cover unquoted uninstaller paths, installed clients without connected accounts, missing uninstallers, and rejection of unsigned or unexpected programs. UI client installation/removal is simulated. The optional `CloudNavClientTests.exe <result.json> --downloads` mode downloads both official installers, validates their publisher signatures, and deletes the downloads without executing them. Run it only through the isolated harness with its approved `InternetOnly` profile; it is deliberately excluded from the offline release suite.

Backup-state unit tests cover active and released selections, unknown states, unsupported folders, and unchanged unselected backups. Opening the real OneDrive dialog is checked separately on the supported client. Folder tests exercise real registry subtrees representing two user scopes, plus actual known-folder redirection and restoration in an isolated guest. These are not two simultaneous interactive Windows logons. They check saved plan restoration, identity mismatch rejection, wrong destinations, and delayed/failed release states. Redirect-only fixtures use exclusively locked source files absent from the destination to exercise initial setup and saved-plan resumes without reading or transferring their contents. The account tests cover fresh Google setup, implicit and explicit shared-client upgrades, custom-client preservation, the native drive selector, and cancellation. The engine test runs the real non-interactive rclone protocol with stdin closed against a local backend, then uses a synthetic subprocess to check provider errors, incomplete metadata, failed root checks, successful atomic replacement, recovery after failed listing, temporary-file cleanup, and exclusion of authentication output from logs. `CloudNavAuthFixture.exe` is test-only and is not part of the portable application distribution.

The JSON parser is vendored from nlohmann/json v3.12.0 under the MIT License; see [`third_party/nlohmann/LICENSE.MIT`](third_party/nlohmann/LICENSE.MIT).
