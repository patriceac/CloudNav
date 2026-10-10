#pragma once
#include "../src/sync_logic.h"
#include "../src/sync_state.h"

inline void RunSyncLogicTests() {
    using namespace cloudnav;
    const std::string configBefore = "# keep\n[cloudnav-onedrive]\ntoken = oldOD\n[unrelated]\nx = keep\n[cloudnav-gdrive]\ntoken = oldGD\n";
    auto merged = MergeSyncAccountConfig(configBefore, "[cloudnav-onedrive]\ntoken = newOD\n[cloudnav-gdrive]\ntoken = staleGD\n", "cloudnav-onedrive");
    merged = MergeSyncAccountConfig(merged, "[cloudnav-onedrive]\ntoken = staleOD\n[cloudnav-gdrive]\ntoken = newGD", "cloudnav-gdrive");
    assert(merged == "# keep\n[cloudnav-onedrive]\ntoken = newOD\n[unrelated]\nx = keep\n[cloudnav-gdrive]\ntoken = newGD\n");
    assert(MergeSyncAccountConfig(configBefore, "", "cloudnav-onedrive") == configBefore);
    assert(SyncListingProgress(true, 0, 0) == L"OneDrive — waiting for listing data — elapsed 0m 0s");
    assert(SyncListingProgress(false, 12, 65) == L"Google Drive — 12 files received — elapsed 1m 5s");
    assert(SyncInventoryProgress(true, R"({"mode":"changes","items":3,"pages":1})", 65) ==
        L"OneDrive — checking changes — 3 items / 1 API pages — elapsed 1m 5s");
    assert(SyncInventoryProgress(false, R"({"mode":"full","items":2000,"pages":2})", 2).find(L"2000 items / 2 API pages") != std::wstring::npos);
    assert(SyncInventoryProgress(false, "partial", 0).empty());
    assert(SyncInventoryProgress(false, R"({"mode":"changes","items":-1,"pages":0})", 0).empty());
    assert(SyncModeFromSetting(0) == SyncMode::ToGoogle);
    assert(SyncModeFromSetting(1) == SyncMode::ToOneDrive);
    assert(SyncModeFromSetting(2) == SyncMode::Bidirectional);
    assert(SyncModeFromSetting(3) == SyncMode::ToGoogle);
    assert(SyncModeFromSetting(0xffffffffU) == SyncMode::ToGoogle);
    const std::vector<std::wstring> streamedArchive{L"--disable", L"Copy"};
    assert(SyncArchiveArguments(SyncMode::Bidirectional, SyncAction::ToOneDrive) == streamedArchive);
    assert(SyncArchiveArguments(SyncMode::Bidirectional, SyncAction::DeleteOneDrive) == streamedArchive);
    assert(SyncArchiveArguments(SyncMode::ToOneDrive, SyncAction::ToOneDrive).empty());
    assert(SyncArchiveArguments(SyncMode::Bidirectional, SyncAction::ToGoogle).empty());
    assert(SyncArchiveArguments(SyncMode::Bidirectional, SyncAction::DeleteGoogle).empty());
    const std::string config = "[cloudnav-onedrive]\ntype = onedrive\ndrive_id = sample-drive\ntoken = {\"refresh_token\":\"one\"}\n"
        "[cloudnav-gdrive]\ntype = drive\ntoken = {\"refresh_token\":\"google\",\"access_token\":\"old\"}\n";
    auto rotated = config;
    rotated.replace(rotated.find("\"one\""), 5, "\"rotated\"");
    rotated.replace(rotated.find("\"old\""), 5, "\"new\"");
    assert(SyncBindingMaterial(config) == SyncBindingMaterial(rotated));
    rotated.replace(rotated.find("sample-drive"), 12, "another-drive");
    assert(SyncBindingMaterial(config) != SyncBindingMaterial(rotated));
    const SyncFile original{10, "2026-09-09T10:00:00.000000000Z", {{"md5", "original"}}};
    const SyncFile changed{20, "2026-09-09T11:00:00.000000000Z", {{"md5", "changed"}}};
    const SyncFile other{30, "2026-09-09T12:00:00.000000000Z", {{"md5", "other"}}};
    assert(CanonicalSyncTime("2026-09-09T12:00:00+02:00") == original.time);
    assert(CanonicalSyncTime("2026-09-09T05:00:00-05:00") == original.time);
    SyncAnalysis planned;
    planned.complete = true;
    planned.oneDrive = {{"a.txt", original}};
    planned.google = {{"a.txt", original}, {"queued.txt", changed}};
    auto verified = planned;
    verified.oneDrive["queued.txt"] = changed;
    verified.google["late.txt"] = other;
    auto next = verified;
    const auto plannedRows = planned.Plan(SyncMode::Bidirectional);
    LimitSyncVerification(planned, verified, plannedRows, ".conflict-test");
    assert(!verified.google.count("late.txt"));
    for (const auto& row : verified.Plan(SyncMode::Bidirectional)) assert(row.action == SyncAction::None);
    const SyncJson verifiedState = {{"version", 1}, {"binding", "pair"},
        {"oneDrive", SaveSyncInventory(verified.oneDrive)}, {"google", SaveSyncInventory(verified.google)}};
    assert(LoadSyncBaseline(verifiedState.dump(), "pair", next));
    assert(next.Plan(SyncMode::Bidirectional)[1].path == "late.txt" &&
        next.Plan(SyncMode::Bidirectional)[1].action == SyncAction::ToOneDrive);
    auto failed = verified;
    failed.oneDrive.erase("queued.txt");
    failed.google["a.txt"] = changed;
    LimitSyncVerification(planned, failed, plannedRows, ".conflict-test");
    assert(failed.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToOneDrive &&
        failed.Plan(SyncMode::Bidirectional)[1].action == SyncAction::ToOneDrive);
    verified.oneDrive["a.txt.conflict-test"] = verified.google["a.txt.conflict-test"] = other;
    LimitSyncVerification(planned, verified, {{"a.txt", '*', SyncAction::KeepBoth}}, ".conflict-test");
    assert(verified.oneDrive.count("a.txt.conflict-test") && verified.google.count("a.txt.conflict-test"));
    SyncAnalysis analysis;
    analysis.complete = true;
    analysis.oneDrive = {{"a.txt", original}};
    analysis.google = {{"b.txt", original}};
    const auto before = analysis.oneDrive;
    assert(analysis.Plan(SyncMode::ToGoogle)[0].action == SyncAction::ToGoogle);
    assert(analysis.Plan(SyncMode::ToGoogle)[1].action == SyncAction::None);
    assert(analysis.Plan(SyncMode::ToOneDrive)[0].action == SyncAction::None);
    assert(analysis.Plan(SyncMode::ToOneDrive)[1].action == SyncAction::ToOneDrive);
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToGoogle);
    assert(analysis.Plan(SyncMode::Bidirectional)[1].action == SyncAction::ToOneDrive);
    assert(analysis.oneDrive == before); // changing modes cannot mutate inventories
    analysis.google = {{"a.txt", changed}};
    assert(analysis.Plan(SyncMode::ToGoogle)[0].action == SyncAction::None);
    assert(!SyncChangesFiles(analysis.Plan(SyncMode::ToGoogle)[0].action)); // different but destination is newer
    assert(analysis.Plan(SyncMode::ToOneDrive)[0].action == SyncAction::ToOneDrive);
    assert(SyncChangesFiles(analysis.Plan(SyncMode::ToOneDrive)[0].action));
    analysis.oneDrive["a.txt"] = changed;
    analysis.google["a.txt"] = original;
    assert(analysis.Plan(SyncMode::ToGoogle)[0].action == SyncAction::ToGoogle);
    assert(analysis.Plan(SyncMode::ToOneDrive)[0].action == SyncAction::None);
    analysis.oneDrive["a.txt"] = original;
    auto equalDate = changed;
    equalDate.time = original.time;
    analysis.google["a.txt"] = equalDate;
    assert(analysis.Plan(SyncMode::ToGoogle)[0].action == SyncAction::None);
    assert(analysis.Plan(SyncMode::ToOneDrive)[0].action == SyncAction::None);
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::KeepBoth);
    assert(SyncChangesFiles(SyncAction::KeepBoth) && SyncChangesFiles(SyncAction::DeleteGoogle) &&
        SyncChangesFiles(SyncAction::DeleteOneDrive) && !SyncChangesFiles(SyncAction::Ignored) && !SyncChangesFiles(SyncAction::Blocked));
    analysis.google["a.txt"] = changed;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToOneDrive);
    analysis.previousOneDrive = analysis.previousGoogle = {{"a.txt", original}};
    analysis.hasBaseline = true;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToOneDrive);
    analysis.oneDrive["a.txt"] = other;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToGoogle);
    analysis.previousOneDrive = analysis.previousGoogle = {{"a.txt", other}};
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToGoogle); // newer unchanged side beats older edit
    analysis.previousOneDrive = analysis.previousGoogle = {{"a.txt", original}};
    auto sameSecond = other;
    sameSecond.time = changed.time;
    sameSecond.time.replace(20, 9, "900000000");
    analysis.oneDrive["a.txt"] = sameSecond;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::KeepBoth);
    analysis.oneDrive["a.txt"] = other;
    analysis.google["a.txt"] = original;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToGoogle);
    analysis.google.clear();
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToGoogle); // edit beats delete
    assert(analysis.Plan(SyncMode::Bidirectional)[0].editDeleteConflict);
    analysis.oneDrive["a.txt"] = original;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::DeleteOneDrive);
    analysis.google = analysis.oneDrive;
    analysis.oneDrive.clear();
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::DeleteGoogle);
    analysis.google["a.txt"] = changed;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToOneDrive);
    analysis.google.clear();
    assert(analysis.Plan(SyncMode::Bidirectional).empty());
    analysis.hasBaseline = false;
    analysis.google = {{"a.txt", original}};
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToOneDrive); // no history never deletes
    analysis.oneDrive = {{"A.txt", original}};
    for (const auto& row : analysis.Plan(SyncMode::Bidirectional)) assert(row.action == SyncAction::Blocked);
    analysis.oneDrive = {{"Folder/a.txt", original}};
    analysis.google = {{"folder/b.txt", original}};
    for (const auto& row : analysis.Plan(SyncMode::Bidirectional)) assert(row.action == SyncAction::Blocked);
    analysis.oneDrive = {{"a.txt/nested.txt", original}};
    analysis.google = {{"a.txt", original}};
    for (const auto& row : analysis.Plan(SyncMode::Bidirectional)) assert(row.action == SyncAction::Blocked);
    analysis.oneDrive = {{"é.txt", original}};
    analysis.google = {{"e\xcc\x81.txt", original}};
    for (const auto& row : analysis.Plan(SyncMode::Bidirectional)) assert(row.action == SyncAction::Blocked);
    analysis.google = analysis.oneDrive;
    analysis.complete = false;
    assert(analysis.Plan(SyncMode::ToGoogle)[0].action == SyncAction::Blocked);
    SyncInventory inventory;
    std::string error;
    const auto document = SaveSyncInventory(analysis.oneDrive).dump();
    assert(ReadSyncInventory(document, inventory, error) && inventory == analysis.oneDrive);
    assert(!ReadSyncInventory("{}", inventory, error));
    assert(!ReadSyncInventory("[{\"IsDir\":false}]", inventory, error));
    assert(!ReadSyncInventory(R"([{"Path":"folder","IsDir":true},{"Path":"folder","IsDir":true}])", inventory, error));
    auto duplicate = SaveSyncInventory(analysis.oneDrive);
    duplicate.push_back(duplicate[0]);
    assert(!ReadSyncInventory(duplicate.dump(), inventory, error));
    SyncJson googleRows = SyncJson::array({
        {{"Path", "duplicate.jpg"}, {"IsDir", false}, {"Size", 10}, {"ModTime", "2026-09-09T10:00:00Z"}},
        {{"Path", "duplicate.jpg"}, {"IsDir", false}, {"Size", 20}, {"ModTime", "2026-09-09T11:00:00Z"}},
        {{"Path", "safe.jpg"}, {"IsDir", false}, {"Size", 30}, {"ModTime", "2026-09-09T12:00:00Z"}}
    });
    SyncIgnoredPaths ignored;
    assert(ReadGoogleSyncInventory(googleRows.dump(), inventory, ignored, error));
    assert(inventory.size() == 1 && inventory.count("safe.jpg") == 1);
    assert(ignored.size() == 1 && ignored.at("duplicate.jpg").objectCount == 2);
    SyncAnalysis ignoredAnalysis;
    ignoredAnalysis.complete = true;
    ignoredAnalysis.oneDrive = {{"duplicate.jpg", original}, {"one-only.txt", original}};
    ignoredAnalysis.google = inventory;
    ignoredAnalysis.ignoredGooglePaths = ignored;
    const auto ignoredPlan = ignoredAnalysis.Plan(SyncMode::Bidirectional);
    const auto ignoredRow = std::find_if(ignoredPlan.begin(), ignoredPlan.end(), [](const auto& row) {
        return row.path == "duplicate.jpg";
    });
    assert(ignoredRow != ignoredPlan.end() && ignoredRow->category == '~' && ignoredRow->action == SyncAction::Ignored);
    assert(ignoredAnalysis.IgnoredGoogleObjectCount() == 2);
    assert(SyncPlanSummary(ignoredAnalysis, SyncMode::Bidirectional).find(L"Ignored: 1") != std::wstring::npos);
    const auto baseline = SyncBaselineInventory(ignoredAnalysis.oneDrive, ignoredAnalysis.ignoredGooglePaths);
    assert(baseline.count("duplicate.jpg") == 0 && baseline.count("one-only.txt") == 1);
    googleRows.erase(googleRows.begin() + 1);
    assert(ReadGoogleSyncInventory(googleRows.dump(), inventory, ignored, error));
    assert(inventory.size() == 2 && ignored.empty());

    const SyncJson duplicateFolders = SyncJson::array({
        {{"Path", "photos"}, {"IsDir", true}, {"Size", -1}, {"ModTime", "2026-09-09T10:00:00Z"}},
        {{"Path", "photos"}, {"IsDir", true}, {"Size", -1}, {"ModTime", "2026-09-09T10:00:00Z"}},
        {{"Path", "photos/one.jpg"}, {"IsDir", false}, {"Size", 1}, {"ModTime", "2026-09-09T10:00:00Z"}},
        {{"Path", "photos2/one.jpg"}, {"IsDir", false}, {"Size", 1}, {"ModTime", "2026-09-09T10:00:00Z"}},
        {{"Path", "photos-old/one.jpg"}, {"IsDir", false}, {"Size", 1}, {"ModTime", "2026-09-09T10:00:00Z"}},
        {{"Path", "outside.jpg"}, {"IsDir", false}, {"Size", 1}, {"ModTime", "2026-09-09T10:00:00Z"}}
    });
    assert(ReadGoogleSyncInventory(duplicateFolders.dump(), inventory, ignored, error));
    assert(inventory.size() == 3 && inventory.count("outside.jpg") && inventory.count("photos2/one.jpg") && inventory.count("photos-old/one.jpg"));
    assert(ignored.size() == 1 && ignored.count("photos/one.jpg") == 1);

    const SyncJson shortcutRows = SyncJson::array({
        {{"Path", "administratif France"}, {"IsDir", true}, {"ID", "france-id\tinternal-shortcut"}},
        {{"Path", "Documents/France"}, {"IsDir", true}, {"ID", "france-id"}},
        {{"Path", "administratif France/document.pdf"}, {"IsDir", false}, {"Size", 10}, {"ModTime", original.time}},
        {{"Path", "Documents/France/document.pdf"}, {"IsDir", false}, {"Size", 10}, {"ModTime", original.time}},
        {{"Path", "administratif France-old/document.pdf"}, {"IsDir", false}, {"Size", 10}, {"ModTime", original.time}},
        {{"Path", "Shared"}, {"IsDir", true}, {"ID", "external-id\tshared-shortcut"}},
        {{"Path", "Shared/nested"}, {"IsDir", true}, {"ID", "external-nested-id"}},
        {{"Path", "Shared/nested/file.txt"}, {"IsDir", false}, {"Size", 10}, {"ModTime", original.time}},
        {{"Path", "Other shared shortcut"}, {"IsDir", true}, {"ID", "external-nested-id\tother-shortcut"}},
        {{"Path", "Other shared shortcut/file.txt"}, {"IsDir", false}, {"Size", 10}, {"ModTime", original.time}}
    });
    assert(ReadGoogleSyncInventory(shortcutRows.dump(), inventory, ignored, error));
    assert(inventory.size() == 4 && inventory.count("Documents/France/document.pdf") &&
        inventory.count("Shared/nested/file.txt") && inventory.count("Other shared shortcut/file.txt"));
    assert(ignored.size() == 2 && ignored.at("administratif France").tree);
    SyncAnalysis shortcuts;
    shortcuts.complete = shortcuts.hasBaseline = true;
    shortcuts.google = inventory;
    shortcuts.ignoredGooglePaths = ignored;
    shortcuts.oneDrive = {{"administratif France/document.pdf", original},
        {"administratif France/only-on-onedrive.txt", changed}};
    shortcuts.previousOneDrive = shortcuts.previousGoogle = shortcuts.oneDrive;
    for (const auto mode : {SyncMode::ToGoogle, SyncMode::ToOneDrive, SyncMode::Bidirectional}) {
        for (const auto& row : shortcuts.Plan(mode)) {
            if (row.path == "administratif France" || row.path.find("administratif France/") == 0)
                assert(row.action == SyncAction::Ignored); // never upload through the alias or propagate a false deletion
            if (row.path == "Shared/nested/file.txt" || row.path == "Other shared shortcut/file.txt")
                assert(row.action == (mode == SyncMode::ToGoogle ? SyncAction::None : SyncAction::ToOneDrive));
        }
    }
    assert(SyncBaselineInventory(shortcuts.oneDrive, shortcuts.ignoredGooglePaths).empty());
    assert(!FindIgnoredGooglePath(ignored, "administratif France-old/document.pdf"));
    auto shortcutCollision = shortcutRows;
    shortcutCollision.push_back({{"Path", "administratif France"}, {"IsDir", false}, {"Size", 10}, {"ModTime", original.time}});
    assert(ReadGoogleSyncInventory(shortcutCollision.dump(), inventory, ignored, error));
    assert(ignored.at("administratif France").tree && FindIgnoredGooglePath(ignored, "administratif France/only-on-onedrive.txt"));

    duplicate = SaveSyncInventory(analysis.oneDrive);
    duplicate[0]["Size"] = -1;
    assert(!ReadSyncInventory(duplicate.dump(), inventory, error));
    assert(!SafeSyncPath("../file") && !SafeSyncPath("/file") && !SafeSyncPath("a\nfile") && !SafeSyncPath("a/../b"));
    assert(SafeSyncPath("folder/été, file.txt"));
    const SyncJson state = {{"version", 1}, {"binding", "account-pair"}, {"oneDrive", SaveSyncInventory(analysis.oneDrive)},
        {"google", SaveSyncInventory(analysis.google)}};
    SyncAnalysis loaded;
    assert(LoadSyncBaseline(state.dump(), "account-pair", loaded) && loaded.hasBaseline);
    loaded = {};
    assert(!LoadSyncBaseline(state.dump(), "different-account", loaded) && !loaded.hasBaseline);
    assert(!LoadSyncBaseline("{bad", "account-pair", loaded));
    auto mismatch = state;
    mismatch["google"] = SyncJson::array();
    assert(!LoadSyncBaseline(mismatch.dump(), "account-pair", loaded));
    const auto shared = SharedSyncState(state, "run-one", false);
    assert(LoadSharedSyncState(shared, shared, "account-pair", loaded) && loaded.hasBaseline && !loaded.recovery);
    for (const auto& broken : {std::string(), std::string("{bad"), SharedSyncState(state, "run-two", false),
        SharedSyncState(state, "run-one", true)}) {
        assert(!LoadSharedSyncState(shared, broken, "account-pair", loaded) && !loaded.hasBaseline && loaded.recovery);
    }
    auto corruptShared = SyncJson::parse(shared);
    corruptShared["data"]["generation"] = "tampered";
    assert(!LoadSharedSyncState(corruptShared.dump(), corruptShared.dump(), "account-pair", loaded));
    assert(!LoadSharedSyncState(shared, shared, "another-root", loaded));
    const auto pending = SharedSyncState(state, "interrupted", true, 1000);
    assert(!LoadSharedSyncState(pending, pending, "account-pair", loaded, 2799) && !loaded.recoveryExpired);
    assert(!LoadSharedSyncState(pending, pending, "account-pair", loaded, 999) && !loaded.recoveryExpired);
    assert(!LoadSharedSyncState(pending, pending, "account-pair", loaded, 2800) && loaded.recoveryExpired && !loaded.hasBaseline);
    loaded.complete = true; loaded.oneDrive = loaded.previousOneDrive; loaded.google.clear();
    const auto recoveryPlan = loaded.Plan(SyncMode::Bidirectional);
    assert(!UnattendedSyncBlocker(loaded, recoveryPlan, 10).empty()); // do not restore historical paths unattended
    for (auto remove : {SyncAction::DeleteGoogle, SyncAction::DeleteOneDrive})
        assert(!UnattendedSyncBlocker(loaded, {{"file", '+', remove}}, 10).empty());
    assert(!UnattendedSyncBlocker(loaded, {{"file", '*', SyncAction::KeepBoth}}, 10).empty());
    assert(!LoadSharedSyncState(pending, pending, "account-pair", loaded, 2800, 2000) && !loaded.recoveryExpired);
    auto legacyPending = SyncJson::parse(pending);
    legacyPending["data"].erase("pendingSince");
    legacyPending["sha256"] = SyncDigest(legacyPending["data"].dump());
    assert(!LoadSharedSyncState(legacyPending.dump(), legacyPending.dump(), "account-pair", loaded, 2800) && !loaded.recoveryExpired);
    assert(!LoadSharedSyncState(legacyPending.dump(), legacyPending.dump(), "account-pair", loaded, 2800, 1000) && loaded.recoveryExpired);
    assert(!LoadSharedSyncState(pending, shared, "account-pair", loaded, 2800) && !loaded.recoveryExpired);
    assert(!LoadSharedSyncState(pending, pending, "another-root", loaded, 2800) && !loaded.recoveryExpired);
    auto tamperedPending = SyncJson::parse(pending);
    tamperedPending["data"]["pendingSince"] = 1;
    assert(!LoadSharedSyncState(tamperedPending.dump(), tamperedPending.dump(), "account-pair", loaded, 2800) && !loaded.recoveryExpired);

    // Moving/renaming on either account during recovery must not resurrect
    // the old path, even when the move preserves every byte and timestamp.
    const SyncInventory beforeMove{{"scan.pdf", original}};
    const SyncJson moveState = {{"version", 1}, {"binding", "move-pair"},
        {"oneDrive", SaveSyncInventory(beforeMove)}, {"google", SaveSyncInventory(beforeMove)}};
    const auto movePending = SharedSyncState(moveState, "interrupted-move", true, 1000);
    const auto moveComplete = SharedSyncState(moveState, "verified-move", false);
    const auto rowAction = [](const std::vector<SyncRow>& rows, const std::string& path) {
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.path == path; });
        assert(row != rows.end());
        return row->action;
    };
    for (const bool movedOnGoogle : {true, false}) {
        for (const auto* newPath : {"renamed.pdf", "Health/scan.pdf", "Health/renamed.pdf"}) {
            SyncAnalysis moved;
            moved.complete = true;
            moved.oneDrive = moved.google = beforeMove;
            auto& movedSide = movedOnGoogle ? moved.google : moved.oneDrive;
            movedSide.erase("scan.pdf"); movedSide.emplace(newPath, original);
            assert(!LoadSharedSyncState(movePending, movePending, "move-pair", moved, 2800) && moved.recoveryExpired);
            const auto merge = moved.Plan(SyncMode::Bidirectional);
            const auto finishMove = movedOnGoogle ? SyncAction::MoveOneDrive : SyncAction::MoveGoogle;
            assert(rowAction(merge, "scan.pdf") == finishMove);
            assert(rowAction(merge, newPath) == (movedOnGoogle ? SyncAction::ToOneDrive : SyncAction::ToGoogle));
            assert(UnattendedSyncBlocker(moved, merge, 10).empty());

            const auto moves = moved.Moves();
            const auto journal = SharedSyncState(moveState, "moving", true, 1000, moves);
            const auto verifiedJournal = SharedSyncState(moveState, "moving", true, 1001, moves, "remove");
            assert(SyncJson::parse(journal).at("version") == 2); // older readers cannot replay it as a merge
            for (const auto& peer : {journal, verifiedJournal, moveComplete, std::string()}) {
                auto resumed = moved;
                assert(!LoadSharedSyncState(journal, peer, "move-pair", resumed, 1002) && resumed.pendingMoves.size() == 1);
                assert(UnattendedSyncBlocker(resumed, resumed.Plan(SyncMode::Bidirectional), 10).empty());
                assert(!LoadSharedSyncState(peer, journal, "move-pair", resumed, 1002) && resumed.pendingMoves.size() == 1);
                auto& target = movedOnGoogle ? resumed.oneDrive : resumed.google;
                target[newPath] = original; // copy finished, old path still present
                assert(rowAction(resumed.Plan(SyncMode::Bidirectional), "scan.pdf") == finishMove);
                target["scan.pdf"] = changed;
                assert(rowAction(resumed.Plan(SyncMode::Bidirectional), "scan.pdf") == SyncAction::Blocked);
                target.erase("scan.pdf"); // deletion finished, final commit interrupted
                assert(UnattendedSyncBlocker(resumed, resumed.Plan(SyncMode::Bidirectional), 10).empty());
                (movedOnGoogle ? resumed.google : resumed.oneDrive)[newPath] = changed;
                assert(!UnattendedSyncBlocker(resumed, resumed.Plan(SyncMode::Bidirectional), 10).empty());
            }

            // Two missing identical files cannot both be inferred to be the same move.
            auto twoOldPaths = moved;
            twoOldPaths.previousGoogle["also-old.pdf"] = twoOldPaths.previousOneDrive["also-old.pdf"] = original;
            (movedOnGoogle ? twoOldPaths.oneDrive : twoOldPaths.google)["also-old.pdf"] = original;
            assert(twoOldPaths.Moves().empty());
            const auto ambiguousPlan = twoOldPaths.Plan(SyncMode::Bidirectional);
            assert(rowAction(ambiguousPlan, "scan.pdf") != finishMove);
            assert(rowAction(ambiguousPlan, "also-old.pdf") != finishMove);
            assert(!UnattendedSyncBlocker(twoOldPaths, ambiguousPlan, 100).empty());
            (movedOnGoogle ? twoOldPaths.previousGoogle : twoOldPaths.previousOneDrive)["scan.pdf"].id = "moved-object";
            (movedOnGoogle ? twoOldPaths.google : twoOldPaths.oneDrive)[newPath].id = "moved-object";
            assert(twoOldPaths.Moves().size() == 1 && twoOldPaths.Moves().front().from == "scan.pdf");
            // An edited identity-matched move must not also claim an ID-less file with its new hash.
            twoOldPaths.previousGoogle["scan.pdf"] = twoOldPaths.previousOneDrive["scan.pdf"] = changed;
            (movedOnGoogle ? twoOldPaths.previousGoogle : twoOldPaths.previousOneDrive)["scan.pdf"].id = "moved-object";
            (movedOnGoogle ? twoOldPaths.oneDrive : twoOldPaths.google)["scan.pdf"] = changed;
            assert(twoOldPaths.Moves().empty());
            assert(!UnattendedSyncBlocker(twoOldPaths, twoOldPaths.Plan(SyncMode::Bidirectional), 100).empty());

            auto ambiguous = moved;
            (movedOnGoogle ? ambiguous.google : ambiguous.oneDrive)["unrelated-copy.pdf"] = original;
            assert(!UnattendedSyncBlocker(ambiguous, ambiguous.Plan(SyncMode::Bidirectional), 100).empty());
            // Stable provider identity distinguishes an actual move from existing identical copies.
            (movedOnGoogle ? ambiguous.previousGoogle : ambiguous.previousOneDrive)["scan.pdf"].id = "moved-object";
            (movedOnGoogle ? ambiguous.google : ambiguous.oneDrive)[newPath].id = "moved-object";
            assert(ambiguous.Moves().size() == 1);
            assert(rowAction(ambiguous.Plan(SyncMode::Bidirectional), "scan.pdf") == finishMove);
            ambiguous.ignoredGooglePaths["scan.pdf"] = {2, "duplicate Google path"};
            assert(rowAction(ambiguous.Plan(SyncMode::Bidirectional), "scan.pdf") == SyncAction::Ignored);

            // Normal moves use the same verified cleanup, without archiving.
            assert(LoadSharedSyncState(moveComplete, moveComplete, "move-pair", moved));
            const auto normal = moved.Plan(SyncMode::Bidirectional);
            assert(rowAction(normal, "scan.pdf") == finishMove);
            assert(UnattendedSyncBlocker(moved, normal, 100).empty());

            // A local pending marker over completed shared history has the same
            // risk and uses the same guard as a matching cloud pending journal.
            moved.hasBaseline = false; moved.recovery = moved.recoveryExpired = true;
            assert(UnattendedSyncBlocker(moved, moved.Plan(SyncMode::Bidirectional), 10).empty());
        }
    }
    SyncAnalysis retry;
    retry.complete = true; retry.oneDrive = retry.google = beforeMove;
    assert(!LoadSharedSyncState(movePending, movePending, "move-pair", retry, 2800) && retry.recoveryExpired);
    retry.oneDrive.emplace("new-one.txt", changed);
    retry.google.emplace("new-google.txt", changed);
    assert(UnattendedSyncBlocker(retry, retry.Plan(SyncMode::Bidirectional), 10).empty());
    // Existing destination edits and first-merge additions remain retryable.
    retry.oneDrive["scan.pdf"] = changed;
    assert(UnattendedSyncBlocker(retry, retry.Plan(SyncMode::Bidirectional), 10).empty());
    const SyncJson firstState = {{"version", 1}, {"binding", "first-pair"},
        {"oneDrive", SyncJson::array()}, {"google", SyncJson::array()}};
    const auto firstPending = SharedSyncState(firstState, "first-merge", true, 1000);
    assert(!LoadSharedSyncState(firstPending, firstPending, "first-pair", retry, 2800) && retry.recoveryExpired);
    retry.google.erase("scan.pdf");
    assert(UnattendedSyncBlocker(retry, retry.Plan(SyncMode::Bidirectional), 10).empty());
    // Ambiguous Google objects are ignored, never restored through a path copy.
    assert(!LoadSharedSyncState(movePending, movePending, "move-pair", retry, 2800));
    retry.ignoredGooglePaths["scan.pdf"] = {2, "ambiguous"};
    assert(UnattendedSyncBlocker(retry, retry.Plan(SyncMode::Bidirectional), 10).empty());
    SyncAnalysis unchanged;
    unchanged.complete = true;
    unchanged.oneDrive = unchanged.google = {{"a.txt", original}};
    const SyncJson unchangedState = {{"version", 1}, {"binding", "pair"},
        {"oneDrive", SaveSyncInventory(unchanged.oneDrive)}, {"google", SaveSyncInventory(unchanged.google)}};
    const auto unchangedShared = SharedSyncState(unchangedState, "verified", false);
    assert(LoadSharedSyncState(unchangedShared, unchangedShared, "pair", unchanged) && SyncBaselineUnchanged(unchanged));
    for (auto required : {&SyncAnalysis::complete, &SyncAnalysis::hasBaseline, &SyncAnalysis::hasSharedBaseline}) {
        auto incomplete = unchanged;
        incomplete.*required = false;
        assert(!SyncBaselineUnchanged(incomplete));
    }
    auto different = unchanged;
    different.recovery = true;
    assert(!SyncBaselineUnchanged(different));
    different = unchanged;
    different.oneDrive["new.txt"] = different.google["new.txt"] = original;
    assert(!SyncBaselineUnchanged(different)); // Equal additions still need a new baseline.
    different.oneDrive.clear(); different.google.clear();
    assert(!SyncBaselineUnchanged(different)); // Deletion on both sides must retire the old baseline entry.
    different = unchanged;
    different.oneDrive["a.txt"].time = changed.time;
    assert(!SyncBaselineUnchanged(different));
    assert(LoadSyncBaseline(unchangedState.dump(), "pair", unchanged) && !SyncBaselineUnchanged(unchanged));
    assert(LoadSharedSyncState(unchangedShared, unchangedShared, "pair", unchanged));
    unchanged.oneDrive["duplicate.jpg"] = original;
    unchanged.ignoredGooglePaths["duplicate.jpg"] = {2, "ambiguous"};
    assert(SyncBaselineUnchanged(unchanged));
    SyncAnalysis scheduled;
    scheduled.complete = scheduled.hasBaseline = true;
    scheduled.oneDrive[".onedrive-gdrive-sync-access"] = original;
    assert(!SyncAccessMarkerPresent(scheduled, ".onedrive-gdrive-sync-access"));
    scheduled.google[".onedrive-gdrive-sync-access"] = original;
    assert(SyncAccessMarkerPresent(scheduled, ".onedrive-gdrive-sync-access"));
    assert(!SyncAccessMarkerPresent(scheduled, "other-marker"));
    for (int i = 0; i < 10; ++i) scheduled.oneDrive[std::to_string(i)] = original;
    assert(UnattendedSyncBlocker(scheduled, {{"0", '+', SyncAction::DeleteOneDrive}}, 10).empty());
    assert(!UnattendedSyncBlocker(scheduled, {{"0", '+', SyncAction::DeleteOneDrive}, {"1", '+', SyncAction::DeleteOneDrive}}, 10).empty());
    assert(!UnattendedSyncBlocker(scheduled, {{"0", '*', SyncAction::KeepBoth}}, 10).empty());
    scheduled.recovery = true;
    assert(!UnattendedSyncBlocker(scheduled, {}, 10).empty());
    assert(SyncEquivalent(original, original) && !SyncEquivalent(original, changed));
    auto sameSizeTime = original;
    sameSizeTime.hashes["md5"] = "different";
    assert(!SyncEquivalent(original, sameSizeTime));
    sameSizeTime.hashes = {{"QuickXorHash", "different-provider-hash"}};
    assert(SyncEquivalent(original, sameSizeTime));
    const auto args = SyncInventoryArguments(L"config", L"remote:", L"log");
    assert(args[0] == L"lsjson" && std::find(args.begin(), args.end(), L"/.CloudNav-history/**") != args.end());
    const auto incrementalArgs = SyncInventoryArguments(L"config", L"remote:", L"log", L"account-cache");
    const auto verificationArgs = SyncInventoryArguments(L"config", L"remote:", L"log", L"account-cache", true);
    assert(std::find(incrementalArgs.begin(), incrementalArgs.end(), L"--onedrive-cloudnav-cache") != incrementalArgs.end());
    assert(std::find(incrementalArgs.begin(), incrementalArgs.end(), L"--drive-cloudnav-cache") != incrementalArgs.end());
    assert(std::find(incrementalArgs.begin(), incrementalArgs.end(), L"--onedrive-cloudnav-full") == incrementalArgs.end());
    assert(std::find(verificationArgs.begin(), verificationArgs.end(), L"--onedrive-cloudnav-full") != verificationArgs.end());
    assert(std::find(verificationArgs.begin(), verificationArgs.end(), L"--drive-cloudnav-full") != verificationArgs.end());
    assert(!SyncUseTraversal(63, 64, 64));
    assert(SyncUseTraversal(64, 128, 128));
    assert(!SyncUseTraversal(64, 129, 128) && !SyncUseTraversal(64, 128, 129));
    assert(SyncUseTraversal(65, 129, 129) && SyncUseTraversal(64, 64, 0));
}
