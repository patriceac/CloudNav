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
    assert(analysis.Plan(SyncMode::ToOneDrive)[0].action == SyncAction::ToOneDrive);
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
    analysis.google["a.txt"] = changed;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::KeepBoth);
    analysis.previousOneDrive = analysis.previousGoogle = {{"a.txt", original}};
    analysis.hasBaseline = true;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::ToOneDrive);
    analysis.oneDrive["a.txt"] = other;
    assert(analysis.Plan(SyncMode::Bidirectional)[0].action == SyncAction::KeepBoth);
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
    SyncAnalysis scheduled;
    scheduled.complete = scheduled.hasBaseline = true;
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
