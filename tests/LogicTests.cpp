#include <cassert>
#include <iostream>
#include <string>

#include "../src/logic.h"
#include "../src/migration_logic.h"
#include "../src/migration_report.h"
#include "SyncLogicTests.h"

int wmain() {
    RunSyncLogicTests();
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

    const auto documentsSource = JoinPath(oneDriveRoot, L"Documents");
    const auto documentsTarget = JoinPath(googleDriveRoot, L"Documents");
    // Task detection must never substitute for successful migration verification.
    assert(!cloudnav::IsVerifiedCloudTransition(false, documentsSource, documentsTarget, oneDriveRoot, googleDriveRoot));
    assert(cloudnav::IsVerifiedCloudTransition(true, documentsSource, documentsTarget, oneDriveRoot, googleDriveRoot));
    assert(!cloudnav::IsVerifiedCloudTransition(true, documentsTarget, documentsSource, oneDriveRoot, googleDriveRoot));
    assert(!cloudnav::IsVerifiedCloudTransition(true, documentsSource, JoinPath(googleDriveRoot, L"Other"), oneDriveRoot, googleDriveRoot));
    assert(!cloudnav::IsVerifiedCloudTransition(true, documentsSource, documentsTarget, L"", googleDriveRoot));
    assert(cloudnav::FolderReturnsLocalAfterBackupRelease(true, true, true, documentsSource, oneDriveRoot));
    assert(!cloudnav::FolderReturnsLocalAfterBackupRelease(false, true, true, documentsSource, oneDriveRoot));
    assert(!cloudnav::FolderReturnsLocalAfterBackupRelease(true, false, true, documentsSource, oneDriveRoot));
    assert(!cloudnav::FolderReturnsLocalAfterBackupRelease(true, true, false, documentsSource, oneDriveRoot));
    assert(!cloudnav::FolderReturnsLocalAfterBackupRelease(true, true, true, documentsTarget, oneDriveRoot));
    assert(cloudnav::ProviderAccountLabel(L"OneDrive", L"Personal account") == L"OneDrive — Personal account");
    assert(cloudnav::ProviderAccountLabel(L"OneDrive", L"OneDrive - Work") == L"OneDrive - Work");
    assert(cloudnav::ProviderAccountLabel(L"OneDrive", L"") == L"OneDrive");

    for (auto task : {cloudnav::MigrationTask::Analyze, cloudnav::MigrationTask::AuthenticateGoogle,
                      cloudnav::MigrationTask::AuthenticateOneDrive, cloudnav::MigrationTask::Copy}) {
        bool analyzed = true, verified = true;
        cloudnav::InvalidateMigrationValidation(task, analyzed, verified);
        assert(!verified);
        assert(analyzed == (task == cloudnav::MigrationTask::Copy));
    }
    assert(std::wstring(cloudnav::MigrationStageTitle(cloudnav::MigrationStage::Copying)).find(L"2 / 2") != std::wstring::npos);
    assert(std::wstring(cloudnav::MigrationStageTitle(cloudnav::MigrationStage::Verifying)).find(L"3 / 3") != std::wstring::npos);
    assert(std::wstring(cloudnav::MigrationStageDetails(cloudnav::MigrationStage::Copying)).find(L"engine") == std::wstring::npos);
    assert(std::wstring(cloudnav::MigrationStageDetails(cloudnav::MigrationStage::Verifying)).find(L"Independent") != std::wstring::npos);

    double value = 0;
    assert(cloudnav::JsonNumber("{\"bytes\":524288,\"totalBytes\":1048576}", "bytes", value));
    assert(value == 524288.0);
    assert(!cloudnav::JsonNumber("{\"bytes\":1}", "speed", value));
    assert(cloudnav::MigrationPercent(50, 100, 0, 0) == 50);
    assert(cloudnav::MigrationPercent(0, 0, 3, 4) == 75);
    assert(cloudnav::MigrationPercent(200, 100, 0, 0) == 100);
    assert(cloudnav::FormatBytes(1048576) == L"1.0 MB");
    assert(cloudnav::FormatBytes(1024) == L"1.0 KB");
    assert(cloudnav::FormatBytes(0) == L"0 B");
    assert(std::wstring(cloudnav::AnalysisCategory('!')) == L"Error");
    assert(cloudnav::FormatEta(125) == L"ETA 2m 05s");

    // A null ETA from a real scan must remain unknown, not become zero seconds.
    value = -1;
    for (const char* json : {"{\"eta\":null}", "{\"eta\":\"12\"}", "{\"eta\":1e999}", "{\"eta\":12oops}"}) {
        assert(!cloudnav::JsonNumber(json, "eta", value));
        assert(value == -1);
    }
    assert(cloudnav::JsonNumber("{\"eta\":0}", "eta", value) && value == 0);
    assert(cloudnav::JsonCounter("{\"checks\":-1}", "checks") == 0);
    assert(cloudnav::JsonCounter("{\"bytes\":1e30}", "bytes") == 0);

    using cloudnav::MigrationStage;
    cloudnav::MigrationStatistics stats;
    // Sanitized statistics from the reported 0% scan: all *known* checks were
    // done, but more directories were still being discovered. No reliable %.
    assert(cloudnav::ParseMigrationStatistics(R"({"stats":{"bytes":0,"checks":4933,"elapsedTime":438.0,"eta":null,"listed":13567,"speed":0,"totalBytes":283945,"totalChecks":4933,"transfers":0},"level":"info"})", stats));
    assert(stats.eta == -1 && stats.checks == 4933 && stats.listed == 13567);
    const auto analysis = cloudnav::FormatMigrationProgress(MigrationStage::Analyzing, stats);
    assert(analysis.percent == -1);
    assert(analysis.text == L"13567 items scanned — 4933 files compared — elapsed 7m 18s");
    const auto verification = cloudnav::FormatMigrationProgress(MigrationStage::Verifying, stats);
    assert(verification.percent == -1);
    assert(verification.text.find(L"4933 files checked") != std::wstring::npos);
    assert(verification.text.find(L"ETA") == std::wstring::npos);
    const auto preparingCopy = cloudnav::FormatMigrationProgress(MigrationStage::Copying, stats);
    assert(preparingCopy.percent == -1);
    assert(preparingCopy.text.find(L"Preparing copy") == 0);

    assert(cloudnav::ParseMigrationStatistics(R"({"stats":{"bytes":524288,"totalBytes":1048576,"eta":2,"speed":262144,"transferring":[{"name":"folder/{file}","bytes":1,"speed":1}]}})", stats));
    const auto copying = cloudnav::FormatMigrationProgress(MigrationStage::Copying, stats);
    assert(copying.percent == 50);
    assert(copying.text == L"50 % — 512.0 KB / 1.0 MB — 256.0 KB/s — ETA 2s");
    stats.bytes = stats.totalBytes;
    assert(cloudnav::FormatMigrationProgress(MigrationStage::Copying, stats).percent == 99);
    assert(!cloudnav::ParseMigrationStatistics(R"({"msg":"not a statistics event"})", stats));
    assert(!cloudnav::ParseMigrationStatistics(R"({"stats":null})", stats));
    assert(!cloudnav::ParseMigrationStatistics(R"({"stats":{"bytes":12)", stats));
    assert(cloudnav::ParseMigrationStatistics(R"({"stats":{},"eta":0,"listed":999})", stats));
    assert(stats.eta == -1 && stats.listed == 0);

    // Fast enumeration must retain the same dry-run/copy/one-way-check contract.
    for (auto stage : {MigrationStage::Analyzing, MigrationStage::Copying, MigrationStage::Verifying}) {
        const auto args = cloudnav::MigrationArguments(stage, L"C:\\test config\\rclone.conf");
        const auto has = [&](const wchar_t* arg) { return std::find(args.begin(), args.end(), arg) != args.end(); };
        assert(has(L"--fast-list") == (stage != MigrationStage::Copying));
        assert(has(L"--no-traverse") == (stage == MigrationStage::Copying));
        assert(has(L"NOTICE"));
        assert(has(L"/Personal Vault/**") == (stage != MigrationStage::Copying));
        assert(has(L"--drive-skip-gdocs"));
        assert(args[1] == L"cloudnav-onedrive:" && args[2] == L"cloudnav-gdrive:");
        assert(has(L"--dry-run") == (stage == MigrationStage::Analyzing));
        assert(has(L"--one-way") == (stage == MigrationStage::Verifying));
        assert(args[0] == (stage == MigrationStage::Verifying ? L"check" : L"copy"));
        assert(!has(L"--size-only") && !has(L"--ignore-existing") && !has(L"--ignore-errors"));
    }

    cloudnav::AnalysisReport plan;
    std::istringstream planInput("+ new.txt\n* changed.txt\n= same.txt\n- extra.txt\n");
    plan.Combined(planInput);
    std::string copyList;
    assert(!plan.CopyList(copyList));
    plan.complete = true;
    assert(plan.CopyList(copyList) && copyList == "changed.txt\nnew.txt\n");
    plan.files["bad\npath"].category = '+';
    assert(!plan.CopyList(copyList));
    cloudnav::AnalysisReport report;
    report.Log(R"({"object":"Documents/été, \"copie\".txt","skipped":"copy","size":123})");
    report.Log(R"({"object":"changed.txt","skipped":"copy","size":456})");
    std::istringstream combined("+ Documents/été, \"copie\".txt\n* changed.txt\n= same.txt\n- extra.txt\n+ Documents/été, \"copie\".txt\n");
    report.Combined(combined);
    assert(!report.malformed && report.Count('+') == 1 && report.Count('*') == 1 && report.Count('=') == 1 && report.Count('-') == 1);
    assert(report.CopySize() == L"579 B");
    assert(report.Summary().find(L"Partial results") == 0);
    report.complete = true;
    assert(report.Summary().find(L"New: 1") == 0);
    report.Log(R"({"level":"error","object":"changed.txt","msg":"access denied"})");
    report.Log(R"({"level":"error","object":"changed.txt","msg":"access denied"})");
    assert(report.Count('!') == 1 && report.Count('*') == 0);
    cloudnav::AnalysisReport emptyReport;
    std::istringstream emptyCombined("");
    emptyReport.Combined(emptyCombined);
    assert(emptyReport.available && emptyReport.CopySize() == L"0 B" && emptyReport.files.empty());
    std::istringstream unknownSize("+ new.txt\n");
    emptyReport.Combined(unknownSize);
    assert(emptyReport.CopySize() == L"unavailable");
    std::istringstream malformed("unexpected filename continuation\n");
    emptyReport.Combined(malformed);
    assert(emptyReport.malformed);
    std::string decoded;
    assert(cloudnav::JsonString("{\"object\":\"\\u00e9\\ud83d\\ude80\\\\file\"}", "object", decoded));
    assert(decoded == "é🚀\\file");
    assert(!cloudnav::JsonString("{\"object\":\"\\ud800\"}", "object", decoded));

    std::wcout << L"CloudNav logic tests: OK\n";
    return 0;
}
