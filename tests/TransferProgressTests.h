#pragma once
#include "../src/transfer_progress.h"

inline void TestTransferProgress() {
    using namespace cloudnav;
    using J = nlohmann::json;
    const auto copied = [](const std::string& path) {
        return J{{"level", "info"}, {"object", path}, {"msg", "Copied (new)"}}.dump();
    };
    const auto stats = [](std::uint64_t traffic, std::uint64_t active, unsigned errors = 0) {
        return J{{"stats", {{"bytes", traffic}, {"totalBytes", traffic * 10}, {"eta", 9223372036ULL},
            {"speed", 0.5}, {"errors", errors}, {"transferring", J::array({{{"name", "large.bin"}, {"bytes", active}}})}}}}.dump();
    };
    TransferProgress p;
    const auto first = p.AddBatch({{"large.bin", 10000}, {"small.txt", 1000}, {"unchanged", 50}});
    const auto second = p.AddBatch({{"large.bin", 5000}}); // same path in the other direction
    p.BeginBatch(first, 0);
    for (unsigned t = 1; t <= 40; ++t) p.Log(stats(t * 100, t * 100), t);
    const auto running = p.Format(40);
    assert(p.TotalBytes() == 16050 && p.CompletedBytes() == 0);
    assert(running.eta > 0 && running.eta < 200);
    assert(running.text.find(L"160.5") == std::wstring::npos); // engine total is irrelevant
    assert(running.details.find(L"292") == std::wstring::npos);
    assert(p.Format(60).eta < 0 && p.Format(60).details.find(L"Waiting for transfer progress") == 0);

    p.Log(R"json({"level":"error","object":"large.bin","msg":"Failed to copy: googleapi 403 RATE_LIMIT_EXCEEDED"})json", 61);
    p.Log(stats(40000, 1000, 1), 62); // retry traffic and partial bytes cannot finish a file
    assert(p.CompletedBytes() == 0 && p.Format(62).eta < 0);
    assert(p.Format(62).details.find(L"Google Drive quota") != std::wstring::npos);
    p.Log(R"json({"level":"info","object":"small.txt","msg":"Moved (server-side)"})json", 63);
    assert(p.CompletedFiles() == 0); // a backup move is not a successful copy
    p.Log(copied("small.txt"), 64);
    p.Log(copied("small.txt"), 65);
    assert(p.CompletedFiles() == 1 && p.CompletedBytes() == 1000);
    p.Log(stats(60000, 10000), 66);
    p.Log(copied("large.bin"), 67);
    p.Log(copied("outside-plan"), 68);
    assert(p.CompletedFiles() == 2 && p.CompletedBytes() == 11000);
    p.EndBatch(true, 69);
    assert(p.SkippedFiles() == 1 && p.CopiedFiles() == 2 && p.CompletedBytes() == 11050);
    p.BeginBatch(second, 70);
    p.Log(stats(100, 100), 71); // reset counters must retain earlier completed work/traffic
    assert(p.CompletedBytes() == 11050 && p.TotalBytes() == 16050);
    assert(p.Format(71).details.find(L"58.7 KB") != std::wstring::npos);
    p.EndBatch(false, 72);
    assert(p.CompletedFiles() == 3); // failed or cancelled subprocesses cannot resolve the rest
    p.Log(copied("large.bin"), 73);
    assert(p.CompletedFiles() == 4 && p.CompletedBytes() == 16050);
    assert(p.Format(73).percent == 99); // only the worker's successful exit can show 100%
    assert(p.Format(73).details.find(L"Finishing") == 0);

    TransferProgress zero;
    zero.BeginBatch(zero.AddBatch({{"empty", 0}}), 0);
    zero.Log(copied("empty"), 1);
    assert(zero.CompletedFiles() == 1 && zero.Format(1).percent == 99);
    TransferProgress move;
    move.BeginBatch(move.AddBatch({{"archived", 0}}, true), 0);
    move.Log(R"json({"level":"info","object":"archived","msg":"Moved (server-side)"})json", 1);
    assert(move.CompletedFiles() == 1);
    TransferProgress malformed;
    malformed.BeginBatch(malformed.AddBatch({{"x", 1}}), 0);
    for (const auto* bad : {"{", "null", "[]", R"json({"stats":null})json", R"json({"stats":{"bytes":-1,"transferring":[{},null]}})json",
        R"json({"level":true,"object":[],"msg":1})json"}) malformed.Log(bad, 1);
    assert(malformed.CompletedFiles() == 0);
    bool overflow = false;
    try { TransferProgress huge; huge.AddBatch({{"a", UINT64_MAX}, {"b", 1}}); }
    catch (const std::runtime_error&) { overflow = true; }
    assert(overflow);

    SyncAnalysis plan;
    plan.complete = true;
    plan.oneDrive = {{"both.bin", {20, "2026-09-09T10:00:00Z", {}}}, {"same", {1, "2026-09-09T10:00:00Z", {}}}};
    plan.google = {{"both.bin", {30, "2026-09-09T10:00:00Z", {}}}, {"same", {1, "2026-09-09T10:00:00Z", {}}}};
    TransferProgress merge;
    const auto batches = PrepareTransferProgress(plan, SyncMode::Bidirectional, merge);
    assert(merge.TotalBytes() == 80 && merge.TotalFiles() == 3); // two preserved Google copies + OneDrive original
    for (auto batch : batches.conflicts.at("both.bin")) {
        merge.BeginBatch(batch, 1);
        merge.Log(copied("both.bin"), 2);
        merge.EndBatch(true, 3);
    }
    assert(merge.CopiedFiles() == 3 && merge.CompletedBytes() == 80);
    PrepareTransferProgress(plan, SyncMode::ToGoogle, merge);
    assert(merge.TotalBytes() == 0 && merge.TotalFiles() == 0); // equal-time destination is preserved
}
