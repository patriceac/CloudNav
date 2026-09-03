#include <cassert>
#include <iostream>
#include <string>

#include "../src/logic.h"
#include "../src/migration_logic.h"

int wmain() {
    using cloudnav::DriveBit;
    using cloudnav::IsDriveVisible;
    using cloudnav::CloudRelativePath;
    using cloudnav::CanDetachOneDrive;
    using cloudnav::ClassifyFolderLocation;
    using cloudnav::FolderLocationKind;
    using cloudnav::IsMirroredCloudTransition;
    using cloudnav::NeedsOneDriveBackupDisable;
    using cloudnav::JoinPath;
    using cloudnav::PathEquals;
    using cloudnav::PathIsWithin;
    using cloudnav::PathLeaf;
    using cloudnav::PathRelativeToRoot;
    using cloudnav::PathsOverlap;
    using cloudnav::QuoteArgument;
    using cloudnav::SetDriveVisible;

    assert(DriveBit(L'A') == 1UL);
    assert(DriveBit(L'G') == 64UL);
    assert(DriveBit(L'z') == 33554432UL);
    assert(DriveBit(L'?') == 0UL);

    const unsigned long alreadyHiddenC = DriveBit(L'C');
    const unsigned long hiddenCG = SetDriveVisible(alreadyHiddenC, L'G', false);
    assert(hiddenCG == DriveBit(L'C') + DriveBit(L'G'));
    assert(!IsDriveVisible(hiddenCG, L'G'));
    assert(!IsDriveVisible(hiddenCG, L'C'));
    assert(IsDriveVisible(hiddenCG, L'D'));

    const unsigned long shownG = SetDriveVisible(hiddenCG, L'G', true);
    assert(shownG == DriveBit(L'C'));
    assert(IsDriveVisible(shownG, L'G'));
    assert(!IsDriveVisible(shownG, L'C'));

    assert(QuoteArgument(L"C:\\Users\\Example\\My Drive") == L"\"C:\\Users\\Example\\My Drive\"");
    assert(QuoteArgument(L"C:\\ends-with-slash\\") == L"\"C:\\ends-with-slash\\\\\"");

    assert(PathEquals(L"C:/Users/Example/Documents/", L"c:\\users\\example\\documents"));
    assert(PathIsWithin(L"C:\\Users\\Example\\OneDrive\\Images", L"C:\\Users\\Example\\OneDrive"));
    assert(!PathIsWithin(L"C:\\Users\\Example\\OneDrive-old", L"C:\\Users\\Example\\OneDrive"));
    assert(PathRelativeToRoot(L"C:\\Users\\Example\\OneDrive\\Images",
                              L"C:\\Users\\Example\\OneDrive") == L"Images");
    assert(PathLeaf(L"C:\\Users\\Example\\Pictures") == L"Pictures");
    assert(JoinPath(L"G:\\", L"My Drive\\Documents") == L"G:\\My Drive\\Documents");
    assert(CloudRelativePath(L"C:\\Users\\Example\\OneDrive\\Images",
                             L"C:\\Users\\Example\\OneDrive",
                             L"C:\\Users\\Example\\My Drive",
                             L"C:\\Users\\Example\\Pictures") == L"Images");
    assert(CloudRelativePath(L"D:\\Netflix",
                             L"C:\\Users\\Example\\OneDrive",
                             L"C:\\Users\\Example\\My Drive",
                             L"C:\\Users\\Example\\Downloads") == L"Downloads");
    assert(PathsOverlap(L"C:\\Users\\Example\\Documents",
                        L"C:\\Users\\Example\\Documents\\Archive"));
    assert(!PathsOverlap(L"C:\\Users\\Example\\Documents",
                         L"C:\\Users\\Example\\Pictures"));

    const std::wstring oneDriveRoot = L"C:\\Users\\Example\\OneDrive";
    const std::wstring googleDriveRoot = L"C:\\Users\\Example\\My Drive";
    assert(ClassifyFolderLocation(L"C:\\Users\\Example\\OneDrive\\Documents",
                                  L"C:\\Users\\Example\\Documents",
                                  oneDriveRoot, googleDriveRoot) == FolderLocationKind::OneDrive);
    assert(ClassifyFolderLocation(L"C:\\Users\\Example\\My Drive\\Images",
                                  L"C:\\Users\\Example\\Pictures",
                                  oneDriveRoot, googleDriveRoot) == FolderLocationKind::GoogleDrive);
    assert(ClassifyFolderLocation(L"C:\\Users\\Example\\Desktop",
                                  L"C:\\Users\\Example\\Desktop",
                                  oneDriveRoot, googleDriveRoot) == FolderLocationKind::ThisComputer);
    assert(ClassifyFolderLocation(L"D:\\Netflix",
                                  L"C:\\Users\\Example\\Downloads",
                                  oneDriveRoot, googleDriveRoot) == FolderLocationKind::Other);
    assert(IsMirroredCloudTransition(L"C:\\Users\\Example\\OneDrive\\Documents",
                                     L"C:\\Users\\Example\\My Drive\\Documents",
                                     oneDriveRoot, googleDriveRoot));
    assert(IsMirroredCloudTransition(L"C:\\Users\\Example\\My Drive\\Images",
                                     L"C:\\Users\\Example\\OneDrive\\Images",
                                     oneDriveRoot, googleDriveRoot));
    assert(!IsMirroredCloudTransition(L"C:\\Users\\Example\\Desktop",
                                      L"C:\\Users\\Example\\My Drive\\Desktop",
                                      oneDriveRoot, googleDriveRoot));
    assert(NeedsOneDriveBackupDisable(
        true,
        L"C:\\Users\\Example\\OneDrive\\Documents",
        L"C:\\Users\\Example\\My Drive\\Documents",
        oneDriveRoot, googleDriveRoot));
    assert(!NeedsOneDriveBackupDisable(
        false,
        L"C:\\Users\\Example\\OneDrive\\Downloads",
        L"C:\\Users\\Example\\My Drive\\Downloads",
        oneDriveRoot, googleDriveRoot));
    assert(!NeedsOneDriveBackupDisable(
        true,
        L"C:\\Users\\Example\\My Drive\\Documents",
        L"C:\\Users\\Example\\OneDrive\\Documents",
        oneDriveRoot, googleDriveRoot));

    assert(CanDetachOneDrive(true, true, true, false));
    assert(!CanDetachOneDrive(true, true, true, true));
    assert(!CanDetachOneDrive(true, false, true, false));
    assert(!CanDetachOneDrive(true, true, false, false));
    assert(!CanDetachOneDrive(false, true, true, false));

    double value = 0;
    assert(cloudnav::JsonNumber("{\"bytes\":524288,\"totalBytes\":1048576}", "bytes", value));
    assert(value == 524288.0);
    assert(!cloudnav::JsonNumber("{\"bytes\":1}", "speed", value));
    assert(cloudnav::MigrationPercent(50, 100, 0, 0) == 50);
    assert(cloudnav::MigrationPercent(0, 0, 3, 4) == 75);
    assert(cloudnav::MigrationPercent(200, 100, 0, 0) == 100);
    assert(cloudnav::FormatBytes(1048576) == L"1.0 Mo");
    assert(cloudnav::FormatEta(125) == L"ETA 2m 05s");

    std::wcout << L"CloudNav logic tests: OK\n";
    return 0;
}
