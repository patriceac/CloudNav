#pragma once

#include <windows.h>

#include <string>

namespace cloudnav {

struct FolderProviders {
    std::wstring oneDriveLabel;
    std::wstring oneDriveRoot;
    std::wstring googleDriveRoot;
    bool rootMirrorTaskDetected = false;
    bool oneDriveToGoogleVerified = false;
    bool preloadDemoPlan = false;
    bool simulateRepointFailure = false;
    bool simulateOneDriveBackupActive = false;
};

bool ShowFolderManagerDialog(HWND owner, HINSTANCE instance,
                             const FolderProviders& providers, bool demoMode);

int RunFolderRedirectionSelfTest(const std::wstring& resultPath);

int RunDisableOneDriveFolderBackupHelper();

}  // namespace cloudnav
