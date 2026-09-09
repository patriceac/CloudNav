#pragma once
#include "../src/sync_logic.h"

inline void RunSyncLogicTests() {
    using namespace cloudnav;
    assert(SyncListingProgress(true, 0, 0) == L"OneDrive — waiting for listing data — elapsed 0m 0s");
    assert(SyncListingProgress(false, 12, 65) == L"Google Drive — 12 files received — elapsed 1m 5s");
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
    assert(SyncEquivalent(original, original) && !SyncEquivalent(original, changed));
    auto sameSizeTime = original;
    sameSizeTime.hashes["md5"] = "different";
    assert(!SyncEquivalent(original, sameSizeTime));
    sameSizeTime.hashes = {{"QuickXorHash", "different-provider-hash"}};
    assert(SyncEquivalent(original, sameSizeTime));
    const auto args = SyncInventoryArguments(L"config", L"remote:", L"log");
    assert(args[0] == L"lsjson" && std::find(args.begin(), args.end(), L"/.CloudNav-history/**") != args.end());
}
