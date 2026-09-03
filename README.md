<p align="center">
  <img src="assets/CloudNav-icon.png" width="132" alt="CloudNav icon">
</p>

# CloudNav 1.3.0

CloudNav is a native, portable Windows 11 utility that brings together two settings that are usually scattered across the system: cloud entries in the File Explorer navigation pane and the locations of Windows personal folders.

**[Download CloudNav for Windows](https://github.com/patriceac/CloudNav/releases/latest/download/CloudNav.exe)**

One standalone `.exe`, with no installer or additional application runtime.

## Features

- show or hide the **My Drive** entry in the navigation pane;
- show or hide the detected **OneDrive** account entry;
- hide the **Google Drive** virtual drive letter without blocking access to its files;
- redirect Desktop, Documents, Pictures, Downloads, Music, and Videos to their local location, OneDrive, Google Drive, or a custom folder;
- identify OneDrive and Google Drive locations at a glance with provider icons in the personal-folder manager;
- disable OneDrive automatic startup or launch its uninstaller when no managed personal folder still depends on OneDrive;
- copy files, move files, or redirect the folder only, with an explicit final confirmation;
- remember the window position and bring it back onto a visible display if the monitor layout changes.

CloudNav detects the paths and account labels available on the current PC. No user name, account name, or user-specific path is hard-coded in the application.

## Safety

The checkboxes in the main window only control visibility in the File Explorer navigation pane. Hiding the Google Drive letter uses the Windows `NoDrives` policy: the icon disappears, but the drive and its files remain accessible.

Personal-folder changes are shown in a separate review before anything is applied. CloudNav validates every destination, blocks nested paths, and restores the previous locations if a grouped operation fails.

OneDrive client actions stay disabled while Desktop, Documents, Pictures, Downloads, Music, or Videos points anywhere inside the detected OneDrive root. CloudNav names the folders that must be moved and checks their locations again immediately before disabling automatic startup or launching the uninstaller.

When Desktop, Documents, or Pictures moves from OneDrive to Google Drive, CloudNav can disable OneDrive Known Folder Backup before redirecting the folder. The confirmation dialog explains the scope of this strategy, and no OneDrive file is deleted.

If a scheduled task named `OneDrive-GDrive-Bidirectional-Sync` is present, CloudNav assumes that the OneDrive and Google Drive roots are already synchronized and recommends **Redirect only**. Without that signal, **Copy** remains the cautious default.

## Usage

1. Download `CloudNav.exe` from the latest GitHub Release.
2. Make sure Google Drive for desktop and/or OneDrive is installed and running, depending on the features you need.
3. Run the executable directly; no installation is required.
4. Choose the navigation-pane entries, or open **Dossiers personnels…** to manage personal folders.
5. Review the summary, then accept the Windows elevation prompt when required.

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

The Release build uses the static C++ runtime (`/MT`) and produces:

- `build\Release\CloudNav.exe`;
- `build\Release\CloudNavTests.exe`;
- `build\Release\CloudNavWindowPositionTests.exe`.

The build script runs the logic tests automatically. UI and integration scenarios for the isolated Windows test harness are available in `tests/`.
