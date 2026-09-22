#pragma once
#include "sync_logic.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace cloudnav {
inline std::string SyncDigest(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    unsigned char digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Sync history checksum unavailable");
    const auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Sync history checksum failed");
    std::string result;
    for (auto byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}

inline std::string SharedSyncState(const SyncJson& baseline, const std::string& generation, bool pending) {
    const SyncJson data = {{"baseline", baseline}, {"generation", generation}, {"pending", pending}};
    return SyncJson{{"version", 1}, {"data", data}, {"sha256", SyncDigest(data.dump())}}.dump();
}

// Two matching, completed copies are required before history can authorize a removal.
inline bool LoadSharedSyncState(const std::string& oneDrive, const std::string& google,
    const std::string& binding, SyncAnalysis& analysis) {
    analysis.hasBaseline = false;
    analysis.recovery = true;
    try {
        if (oneDrive.empty() || oneDrive != google) return false;
        const auto document = SyncJson::parse(oneDrive);
        const auto& data = document.at("data");
        if (document.at("version") != 1 || document.at("sha256") != SyncDigest(data.dump()) ||
            data.at("pending").get<bool>() || data.at("generation").get<std::string>().empty()) return false;
        if (!LoadSyncBaseline(data.at("baseline").dump(), binding, analysis)) return false;
        analysis.recovery = false;
        return true;
    } catch (...) { return false; }
}

inline std::string UnattendedSyncBlocker(const SyncAnalysis& analysis, const std::vector<SyncRow>& rows, unsigned maxDeletePercent) {
    if (!analysis.complete) return "Account analysis did not finish.";
    if (!analysis.hasBaseline || analysis.recovery) return "Sync history needs review. Run a two-way sync in CloudNav first.";
    size_t od = 0, gd = 0;
    for (const auto& row : rows) {
        if (row.action == SyncAction::Blocked || row.action == SyncAction::KeepBoth || row.editDeleteConflict)
            return "Conflicts need review in CloudNav. No files were changed.";
        od += row.action == SyncAction::DeleteOneDrive;
        gd += row.action == SyncAction::DeleteGoogle;
    }
    if (od * 100.0 > analysis.oneDrive.size() * static_cast<double>(maxDeletePercent) ||
        gd * 100.0 > analysis.google.size() * static_cast<double>(maxDeletePercent))
        return "Removal limit exceeded. Review the plan in CloudNav.";
    return {};
}
} // namespace cloudnav
