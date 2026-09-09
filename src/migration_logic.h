#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace cloudnav {

enum class MigrationTask { None, AuthenticateOneDrive, AuthenticateGoogle, Analyze, Copy };
enum class MigrationStage { Preparing, Connecting, Analyzing, Copying, Verifying };

inline std::vector<std::wstring> AuthenticationArguments(bool oneDrive, bool existing,
    const std::wstring& remote, const std::wstring& configPath) {
    std::vector<std::wstring> args{L"config", existing ? L"update" : L"create", remote};
    if (!existing) args.push_back(oneDrive ? L"onedrive" : L"drive");
    args.insert(args.end(), {L"config_is_local=true", L"config_refresh_token=true",
        L"--non-interactive", L"--config", configPath});
    return args;
}

inline std::vector<std::wstring> MigrationArguments(MigrationStage stage, const std::wstring& configPath,
    const std::wstring& source = L"cloudnav-onedrive:", const std::wstring& destination = L"cloudnav-gdrive:") {
    const bool verify = stage == MigrationStage::Verifying;
    std::vector<std::wstring> args = {verify ? L"check" : L"copy", source, destination,
        L"--config", configPath, L"--fast-list", L"--onedrive-delta", L"--drive-skip-gdocs",
        L"--exclude", L"/Personal Vault/**", L"--use-json-log", L"--stats", L"1s", L"--stats-log-level", L"NOTICE"};
    // Both remotes are scanned from the root: OneDrive's delta listing and Drive's
    // recursive listing avoid a separate network round trip for every directory.
    if (stage == MigrationStage::Copying) {
        // The analyzed list already contains the exclusions. rclone rejects
        // combining files-from with other include/exclude filters.
        const auto filter = std::find(args.begin(), args.end(), L"--exclude");
        args.erase(filter, filter + 2);
        for (const auto* flag : {L"--fast-list", L"--onedrive-delta"})
            args.erase(std::remove(args.begin(), args.end(), flag), args.end());
        args.push_back(L"--no-traverse");
    }
    else if (verify) args.push_back(L"--one-way");
    else {
        args.push_back(L"--check-first");
        args.push_back(L"--create-empty-src-dirs");
        if (stage == MigrationStage::Analyzing) args.push_back(L"--dry-run");
    }
    return args;
}

inline void InvalidateMigrationValidation(MigrationTask task, bool& analyzed, bool& verified) {
    verified = false;
    if (task != MigrationTask::Copy) analyzed = false;
}

inline const wchar_t* MigrationStageTitle(MigrationStage stage) {
    switch (stage) {
    case MigrationStage::Connecting: return L"Connecting account…";
    case MigrationStage::Analyzing: return L"1 / 2 — Analyzing differences";
    case MigrationStage::Copying: return L"2 / 2 — Applying the selected plan";
    case MigrationStage::Verifying: return L"3 / 3 — Checking copied files";
    default: return L"Preparing operation…";
    }
}

inline const wchar_t* MigrationStageDetails(MigrationStage stage) {
    switch (stage) {
    case MigrationStage::Connecting: return L"Finish signing in in your browser, then return here.";
    case MigrationStage::Analyzing: return L"Comparing accounts. No files are copied during analysis.";
    case MigrationStage::Copying: return L"Applying the analyzed actions. You can cancel; analyze again to resume after a transfer.";
    case MigrationStage::Verifying: return L"Independent comparison. Folder setup will be offered after success.";
    default: return L"Progress will appear when the operation starts. You can cancel at any time.";
    }
}

inline bool JsonNumber(const std::string& json, const char* name, double& value) {
    const std::string key = std::string("\"") + name + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos) return false;
    position = json.find(':', position + key.size());
    if (position == std::string::npos) return false;
    const char* begin = json.c_str() + position + 1;
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(parsed)) return false;
    while (std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end && *end != ',' && *end != '}' && *end != ']') return false;
    value = parsed;
    return true;
}

inline std::uint64_t JsonCounter(const std::string& json, const char* name) {
    double value = 0;
    if (!JsonNumber(json, name, value) || value < 0 ||
        value >= static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) return 0;
    return static_cast<std::uint64_t>(value);
}

struct MigrationStatistics {
    std::uint64_t bytes = 0, totalBytes = 0, checks = 0, totalChecks = 0, listed = 0, transfers = 0;
    double speed = 0, eta = -1, elapsed = 0;
};

inline bool ParseMigrationStatistics(const std::string& line, MigrationStatistics& result) {
    const size_t key = line.find("\"stats\"");
    if (key == std::string::npos) return false;
    const size_t colon = line.find(':', key + 7);
    if (colon == std::string::npos) return false;
    const size_t start = line.find_first_not_of(" \t\r\n", colon + 1);
    if (start == std::string::npos || line[start] != '{') return false;
    // Restrict parsing to the stats object, ignoring braces inside quoted paths.
    size_t depth = 0, end = start;
    bool quoted = false, escaped = false;
    for (; end < line.size(); ++end) {
        const char c = line[end];
        if (escaped) { escaped = false; continue; }
        if (quoted && c == '\\') { escaped = true; continue; }
        if (c == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) break;
    }
    if (end == line.size()) return false;
    const std::string json = line.substr(start, end - start + 1);
    result = {};
    result.bytes = JsonCounter(json, "bytes");
    result.totalBytes = JsonCounter(json, "totalBytes");
    result.checks = JsonCounter(json, "checks");
    result.totalChecks = JsonCounter(json, "totalChecks");
    result.listed = JsonCounter(json, "listed");
    result.transfers = JsonCounter(json, "transfers");
    JsonNumber(json, "speed", result.speed);
    JsonNumber(json, "eta", result.eta);
    JsonNumber(json, "elapsedTime", result.elapsed);
    return true;
}

inline int MigrationPercent(std::uint64_t completedBytes, std::uint64_t totalBytes,
                            std::uint64_t completedChecks, std::uint64_t totalChecks) {
    double ratio = 0.0;
    if (totalBytes > 0) ratio = static_cast<double>(completedBytes) / totalBytes;
    else if (totalChecks > 0) ratio = static_cast<double>(completedChecks) / totalChecks;
    return std::clamp(static_cast<int>(ratio * 100.0 + 0.5), 0, 100);
}

inline std::wstring FormatBytes(std::uint64_t bytes) {
    static constexpr const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double amount = static_cast<double>(bytes);
    size_t unit = 0;
    while (amount >= 1024.0 && unit + 1 < _countof(units)) { amount /= 1024.0; ++unit; }
    wchar_t result[64] = {};
    if (unit == 0) swprintf_s(result, L"%llu %s", static_cast<unsigned long long>(bytes), units[unit]);
    else swprintf_s(result, L"%.1f %s", amount, units[unit]);
    return result;
}

inline std::wstring FormatEta(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0 ||
        seconds >= static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) return L"ETA —";
    const auto value = static_cast<unsigned long long>(seconds + 0.5);
    const auto hours = value / 3600;
    const auto minutes = (value % 3600) / 60;
    const auto remainder = value % 60;
    wchar_t result[64] = {};
    if (hours) swprintf_s(result, L"ETA %lluh %02llum", hours, minutes);
    else if (minutes) swprintf_s(result, L"ETA %llum %02llus", minutes, remainder);
    else swprintf_s(result, L"ETA %llus", remainder);
    return result;
}

struct MigrationProgress {
    // The listing totals grow while rclone discovers directories. A negative
    // percentage means activity with an unknown total, never a stalled 0%.
    int percent = -1;
    std::wstring text;
};

inline MigrationProgress FormatMigrationProgress(MigrationStage stage, const MigrationStatistics& stats) {
    MigrationProgress progress;
    const std::wstring elapsed = L"elapsed " + FormatEta((std::max)(0.0, stats.elapsed)).substr(4);
    if (stage == MigrationStage::Analyzing || stage == MigrationStage::Verifying) {
        progress.text = std::to_wstring(stats.listed) + L" items scanned — " +
            std::to_wstring(stats.checks) + (stage == MigrationStage::Analyzing
                ? L" files compared — " : L" files checked — ") + elapsed;
    } else if (stage == MigrationStage::Copying && stats.bytes == 0 && stats.transfers == 0) {
        progress.text = L"Preparing copy — " + std::to_wstring(stats.listed) +
            L" items scanned — " + std::to_wstring(stats.checks) +
            L" files compared — " + elapsed;
    } else {
        progress.percent = (std::min)(99, MigrationPercent(stats.bytes, stats.totalBytes, stats.checks, stats.totalChecks));
        progress.text = std::to_wstring(progress.percent) + L" % — " + FormatBytes(stats.bytes) +
            L" / " + FormatBytes(stats.totalBytes);
        if (std::isfinite(stats.speed) && stats.speed > 0 &&
            stats.speed < static_cast<double>((std::numeric_limits<std::uint64_t>::max)()))
            progress.text += L" — " + FormatBytes(static_cast<std::uint64_t>(stats.speed)) + L"/s";
        progress.text += L" — " + FormatEta(stats.eta);
    }
    return progress;
}

}  // namespace cloudnav
