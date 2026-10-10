#pragma once
#include "sync_logic.h"
#include <bcrypt.h>
#include <ctime>
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

inline std::uint64_t SyncClockSeconds() { return static_cast<std::uint64_t>(std::time(nullptr)); }
inline bool SyncRecoveryExpired(std::uint64_t since, std::uint64_t now = SyncClockSeconds()) {
    return since != 0 && now >= since && now - since >= 30 * 60;
}

inline std::string SharedSyncState(const SyncJson& baseline, const std::string& generation, bool pending,
    std::uint64_t pendingSince = SyncClockSeconds()) {
    SyncJson data = {{"baseline", baseline}, {"generation", generation}, {"pending", pending}};
    if (pending) data["pendingSince"] = pendingSince;
    return SyncJson{{"version", 1}, {"data", data}, {"sha256", SyncDigest(data.dump())}}.dump();
}

// Two matching, completed copies are required before history can authorize a removal.
inline bool LoadSharedSyncState(const std::string& oneDrive, const std::string& google,
    const std::string& binding, SyncAnalysis& analysis, std::uint64_t now = SyncClockSeconds(), std::uint64_t localPendingSince = 0) {
    analysis.hasBaseline = false;
    analysis.hasSharedBaseline = false;
    analysis.recovery = true;
    analysis.recoveryExpired = false;
    try {
        if (oneDrive.empty() || oneDrive != google) return false;
        const auto document = SyncJson::parse(oneDrive);
        const auto& data = document.at("data");
        const bool pending = data.at("pending").get<bool>();
        if (document.at("version") != 1 || document.at("sha256") != SyncDigest(data.dump()) ||
            data.at("generation").get<std::string>().empty()) return false;
        if (!LoadSyncBaseline(data.at("baseline").dump(), binding, analysis)) return false;
        if (pending) {
            // An expired journal permits a non-deleting retry, not a deletion baseline.
            analysis.hasBaseline = false;
            analysis.recoveryExpired = SyncRecoveryExpired((std::max)(data.value("pendingSince", std::uint64_t{0}), localPendingSince), now);
            return false;
        }
        analysis.hasSharedBaseline = true;
        analysis.recovery = false;
        return true;
    } catch (...) { return false; }
}

inline std::string UnattendedSyncBlocker(const SyncAnalysis& analysis, const std::vector<SyncRow>& rows, unsigned maxDeletePercent) {
    if (!analysis.complete) return "Account analysis did not finish.";
    if ((!analysis.hasBaseline || analysis.recovery) && !analysis.recoveryExpired) return "Sync history needs review. Run a two-way sync in CloudNav first.";
    size_t od = 0, gd = 0;
    for (const auto& row : rows) {
        if (analysis.recoveryExpired && (row.action == SyncAction::DeleteOneDrive || row.action == SyncAction::DeleteGoogle))
            return "Recovery must finish before files can be removed.";
        // A pending baseline cannot distinguish an interrupted overwrite from
        // a user's move or deletion. Expiry must not silently restore an old
        // destination path alongside its renamed/moved replacement.
        if (analysis.recoveryExpired &&
            ((row.action == SyncAction::ToGoogle && analysis.previousGoogle.count(row.path) && !analysis.google.count(row.path)) ||
             (row.action == SyncAction::ToOneDrive && analysis.previousOneDrive.count(row.path) && !analysis.oneDrive.count(row.path))))
            return "Recovery could restore a moved or deleted file. Open Detailed sync to review the plan.";
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

inline bool SyncAccessMarkerPresent(const SyncAnalysis& analysis, const std::string& marker) {
    return analysis.oneDrive.count(marker) && analysis.google.count(marker);
}
} // namespace cloudnav
