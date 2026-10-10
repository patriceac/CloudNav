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

inline SyncJson SaveSyncMoves(const std::vector<SyncMove>& moves) {
    auto result = SyncJson::array();
    for (const auto& move : moves) result.push_back({{"from", move.from}, {"to", move.to}, {"onGoogle", move.onGoogle},
        {"original", SaveSyncInventory({{move.from, move.original}}).front()},
        {"replacement", SaveSyncInventory({{move.to, move.replacement}}).front()}});
    return result;
}

inline std::vector<SyncMove> ReadSyncMoves(const SyncJson& rows) {
    if (!rows.is_array()) throw std::runtime_error("Invalid move journal");
    std::vector<SyncMove> moves;
    std::set<std::pair<bool, std::string>> paths;
    for (const auto& row : rows) {
        SyncMove move{row.at("from").get<std::string>(), row.at("to").get<std::string>(), row.at("onGoogle").get<bool>()};
        if (!SafeSyncPath(move.from) || !SafeSyncPath(move.to) || SyncPathKey(move.from) == SyncPathKey(move.to) ||
            !paths.emplace(move.onGoogle, move.from).second || !paths.emplace(move.onGoogle, move.to).second)
            throw std::runtime_error("Invalid move journal path");
        move.original = ReadSyncFileRow(row.at("original"), move.from);
        move.replacement = ReadSyncFileRow(row.at("replacement"), move.to);
        moves.push_back(std::move(move));
    }
    return moves;
}

inline std::string SharedSyncState(const SyncJson& baseline, const std::string& generation, bool pending,
    std::uint64_t pendingSince = SyncClockSeconds(), const std::vector<SyncMove>& moves = {}, const std::string& movePhase = "copy") {
    SyncJson data = {{"baseline", baseline}, {"generation", generation}, {"pending", pending}};
    if (pending) data["pendingSince"] = pendingSince;
    if (pending && !moves.empty()) { data["moves"] = SaveSyncMoves(moves); data["movePhase"] = movePhase; }
    return SyncJson{{"version", data.contains("moves") ? 2 : 1}, {"data", data}, {"sha256", SyncDigest(data.dump())}}.dump();
}

inline SyncJson ReadSharedSyncData(const std::string& text, const std::string& binding) {
    if (text.empty()) return {};
    const auto document = SyncJson::parse(text);
    const auto& data = document.at("data");
    const bool pending = data.at("pending").get<bool>();
    SyncAnalysis baseline;
    if ((document.at("version") != 1 && document.at("version") != 2) ||
        document.at("sha256") != SyncDigest(data.dump()) || data.at("generation").get<std::string>().empty() ||
        !LoadSyncBaseline(data.at("baseline").dump(), binding, baseline)) throw std::runtime_error("Invalid sync history");
    if (document.at("version") == 2) {
        const auto phase = data.at("movePhase").get<std::string>();
        if (!pending || ReadSyncMoves(data.at("moves")).empty() ||
            (phase != "copy" && phase != "remove")) throw std::runtime_error("Invalid move journal");
    } else if (data.contains("moves")) throw std::runtime_error("Invalid move journal version");
    return data;
}

inline bool SameSyncBaseline(const SyncJson& a, const SyncJson& b, const std::string& binding) {
    SyncAnalysis left, right;
    return LoadSyncBaseline(a.dump(), binding, left) && LoadSyncBaseline(b.dump(), binding, right) &&
        SaveSyncInventory(left.previousOneDrive) == SaveSyncInventory(right.previousOneDrive) &&
        SaveSyncInventory(left.previousGoogle) == SaveSyncInventory(right.previousGoogle);
}

// Completed history authorizes ordinary removals. A write-ahead move journal
// authorizes only its recorded moves, which must be reverified before deletion.
inline bool LoadSharedSyncState(const std::string& oneDrive, const std::string& google,
    const std::string& binding, SyncAnalysis& analysis, std::uint64_t now = SyncClockSeconds(), std::uint64_t localPendingSince = 0) {
    analysis.hasBaseline = false;
    analysis.hasSharedBaseline = false;
    analysis.recovery = true;
    analysis.recoveryExpired = false;
    analysis.pendingMoves.clear();
    analysis.moveGeneration.clear();
    try {
        auto data = ReadSharedSyncData(oneDrive, binding);
        auto other = oneDrive == google ? SyncJson{} : ReadSharedSyncData(google, binding);
        if (oneDrive != google) {
            // A crash can interrupt either mirrored journal upload or final
            // commit. Replay the same intent and verify the files again.
            if (!data.contains("moves")) {
                if (!other.contains("moves")) return false;
                std::swap(data, other);
            }
            const auto& peer = other;
            if (!peer.is_null() && !peer.empty()) {
                if (peer.contains("moves")) {
                    if (peer.at("generation") != data.at("generation") || peer.at("moves") != data.at("moves") ||
                        !SameSyncBaseline(peer.at("baseline"), data.at("baseline"), binding)) return false;
                } else if (peer.at("generation") != data.at("generation") &&
                    !SameSyncBaseline(peer.at("baseline"), data.at("baseline"), binding)) return false;
            }
        }
        if (data.empty()) return false;
        const bool pending = data.at("pending").get<bool>();
        if (!LoadSyncBaseline(data.at("baseline").dump(), binding, analysis)) return false;
        if (pending) {
            // An expired journal permits a non-deleting retry, not a deletion baseline.
            analysis.hasBaseline = false;
            analysis.recoveryExpired = SyncRecoveryExpired((std::max)(data.value("pendingSince", std::uint64_t{0}), localPendingSince), now);
            if (data.contains("moves")) {
                analysis.pendingMoves = ReadSyncMoves(data.at("moves"));
                analysis.moveGeneration = data.at("generation").get<std::string>();
            }
            return false;
        }
        analysis.hasSharedBaseline = true;
        analysis.recovery = false;
        return true;
    } catch (...) { return false; }
}

inline std::string UnattendedSyncBlocker(const SyncAnalysis& analysis, const std::vector<SyncRow>& rows, unsigned maxDeletePercent) {
    if (!analysis.complete) return "Account analysis did not finish.";
    if ((!analysis.hasBaseline || analysis.recovery) && !analysis.recoveryExpired && analysis.pendingMoves.empty()) return "Sync history needs review. Run a two-way sync in CloudNav first.";
    size_t od = 0, gd = 0;
    for (const auto& row : rows) {
        if (analysis.recoveryExpired && (row.action == SyncAction::DeleteOneDrive || row.action == SyncAction::DeleteGoogle))
            return "Recovery must finish before files can be removed.";
        // A pending baseline cannot distinguish an interrupted overwrite from
        // a user's move or deletion. Expiry must not silently restore an old
        // destination path alongside its renamed/moved replacement.
        if ((analysis.recoveryExpired || !analysis.pendingMoves.empty()) &&
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
