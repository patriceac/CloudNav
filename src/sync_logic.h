#pragma once

#include <windows.h>

#include "migration_logic.h"
#include "../third_party/nlohmann/json.hpp"
#include <map>
#include <set>
#include <sstream>
#include <regex>

#pragma comment(lib, "normaliz.lib")

namespace cloudnav {

using SyncJson = nlohmann::json;
enum class SyncMode { ToGoogle = 0, ToOneDrive = 1, Bidirectional = 2 };
inline SyncMode SyncModeFromSetting(std::uint32_t value) {
    return value <= 2 ? static_cast<SyncMode>(value) : SyncMode::ToGoogle;
}
enum class SyncAction { None, ToGoogle, ToOneDrive, DeleteGoogle, DeleteOneDrive, KeepBoth, Ignored, Blocked, MoveGoogle, MoveOneDrive };
inline bool SyncChangesFiles(SyncAction action) {
    return action == SyncAction::ToGoogle || action == SyncAction::ToOneDrive ||
        action == SyncAction::DeleteGoogle || action == SyncAction::DeleteOneDrive || action == SyncAction::KeepBoth ||
        action == SyncAction::MoveGoogle || action == SyncAction::MoveOneDrive;
}

inline std::vector<std::wstring> SyncArchiveArguments(SyncMode mode, SyncAction action) {
    // Shared OneDrive archives need streamed copies for both deletions and overwrites.
    return action == SyncAction::DeleteOneDrive || (mode == SyncMode::Bidirectional && action == SyncAction::ToOneDrive)
        ? std::vector<std::wstring>{L"--disable", L"Copy"} : std::vector<std::wstring>{};
}

inline std::pair<size_t, size_t> SyncConfigSection(const std::string& text, const std::string& remote) {
    size_t start = std::string::npos;
    for (size_t pos = 0; pos < text.size();) {
        auto end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        auto line = text.substr(pos, end - pos);
        const auto first = line.find_first_not_of(" \t\r");
        const auto last = line.find_last_not_of(" \t\r");
        if (first != std::string::npos) line = line.substr(first, last - first + 1);
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            if (start != std::string::npos) return {start, pos - start};
            if (line == "[" + remote + "]") start = pos;
        }
        pos = end == text.size() ? end : end + 1;
    }
    return {start, start == std::string::npos ? 0 : text.size() - start};
}

inline std::string MergeSyncAccountConfig(std::string current, const std::string& updated, const std::string& remote) {
    const auto from = SyncConfigSection(updated, remote), to = SyncConfigSection(current, remote);
    if (from.first == std::string::npos || to.first == std::string::npos) return current;
    auto section = updated.substr(from.first, from.second);
    if (!section.empty() && section.back() != '\n') section += '\n';
    current.replace(to.first, to.second, section);
    return current;
}

inline std::wstring SyncListingProgress(bool oneDrive, size_t files, std::uint64_t elapsedSeconds) {
    return std::wstring(oneDrive ? L"OneDrive" : L"Google Drive") + L" — " +
        (files ? std::to_wstring(files) + L" files received" : L"waiting for listing data") +
        L" — elapsed " + std::to_wstring(elapsedSeconds / 60) + L"m " +
        std::to_wstring(elapsedSeconds % 60) + L"s";
}

inline std::string SyncBindingMaterial(const std::string& config) {
    std::map<std::string, std::map<std::string, std::string>> remotes;
    std::istringstream input(config);
    std::string line, section;
    const auto trim = [](const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r");
        if (first == std::string::npos) return std::string();
        return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    };
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
        if (section != "cloudnav-onedrive" && section != "cloudnav-gdrive") continue;
        const auto equal = line.find('=');
        if (equal != std::string::npos) remotes[section][trim(line.substr(0, equal))] = trim(line.substr(equal + 1));
    }
    for (auto& entry : remotes) {
        auto& values = entry.second;
        if (values["type"] == "onedrive" && values["drive_id"].empty())
            throw std::runtime_error("Reconnect OneDrive to identify the drive.");
        const auto token = values.find("token");
        if (token != values.end()) {
            // Microsoft rotates refresh tokens. The configured drive_id is the
            // stable OneDrive data-set identity. Google refresh tokens remain
            // stable across ordinary access-token renewal.
            if (entry.first == "cloudnav-gdrive") {
                const auto json = SyncJson::parse(token->second, nullptr, false);
                if (json.is_discarded() || !json.contains("refresh_token") || !json["refresh_token"].is_string())
                    throw std::runtime_error("Reconnect Google Drive to identify the account.");
                values["refresh_identity"] = json["refresh_token"].get<std::string>();
            }
            values.erase(token);
        }
    }
    if (remotes.size() != 2) throw std::runtime_error("Both accounts must be configured");
    return SyncJson(remotes).dump();
}

inline const wchar_t* SyncModeLabel(SyncMode mode) {
    switch (mode) {
    case SyncMode::ToGoogle: return L"OneDrive → Google Drive";
    case SyncMode::ToOneDrive: return L"Google Drive → OneDrive";
    default: return L"OneDrive ↔ Google Drive";
    }
}
inline const wchar_t* SyncActionLabel(SyncAction action) {
    switch (action) {
    case SyncAction::ToGoogle: return L"Copy → Google Drive";
    case SyncAction::ToOneDrive: return L"Copy → OneDrive";
    case SyncAction::DeleteGoogle: return L"Remove from Google Drive (archived)";
    case SyncAction::DeleteOneDrive: return L"Remove from OneDrive (archived)";
    case SyncAction::MoveGoogle: return L"Finish move on Google Drive";
    case SyncAction::MoveOneDrive: return L"Finish move on OneDrive";
    case SyncAction::KeepBoth: return L"Conflict — keep both";
    case SyncAction::Ignored: return L"Ignored — duplicate Google Drive path";
    case SyncAction::Blocked: return L"Blocked";
    default: return L"Keep";
    }
}

inline bool SafeSyncPath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find_first_of("\r\n\\") != std::string::npos ||
        path.find('\0') != std::string::npos) return false;
    std::istringstream parts(path);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return path.back() != '/';
}

inline std::wstring SyncPathKey(const std::string& path) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
    if (!count) return {};
    std::wstring wide(static_cast<size_t>(count), 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), wide.data(), count);
    const int normalizedSize = NormalizeString(NormalizationC, wide.data(), count, nullptr, 0);
    if (normalizedSize <= 0) return {};
    std::wstring normalized(static_cast<size_t>(normalizedSize), 0);
    const int normalizedCount = NormalizeString(NormalizationC, wide.data(), count, normalized.data(), normalizedSize);
    if (normalizedCount <= 0) return {};
    normalized.resize(static_cast<size_t>(normalizedCount));
    const int foldedSize = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(), normalizedCount, nullptr, 0, nullptr, nullptr, 0);
    if (!foldedSize) return {};
    std::wstring folded(static_cast<size_t>(foldedSize), 0);
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(), normalizedCount, folded.data(), foldedSize, nullptr, nullptr, 0);
    return folded;
}

struct SyncFile {
    std::uint64_t size = 0;
    std::string time;
    std::map<std::string, std::string> hashes;
    std::string id;
    bool operator==(const SyncFile& other) const { return size == other.size && time == other.time && hashes == other.hashes; }
};
using SyncInventory = std::map<std::string, SyncFile>;

inline bool SyncSnapshotMatches(const SyncFile& expected, const SyncFile& actual) {
    return expected == actual && (expected.id.empty() || expected.id == actual.id);
}

inline std::string SyncContentKey(const SyncFile& file) {
    for (const auto& hash : file.hashes) if (!hash.second.empty())
        return std::to_string(file.size) + ":" + hash.first + ":" + hash.second;
    return {}; // Size and date alone cannot establish a move.
}

struct SyncMove {
    std::string from, to;
    bool onGoogle = false;
    SyncFile original, replacement;
};

struct SyncIgnoredPath {
    size_t objectCount = 0;
    std::string reason;
    bool tree = false;
};
using SyncIgnoredPaths = std::map<std::string, SyncIgnoredPath>;

inline const SyncIgnoredPath* FindIgnoredGooglePath(const SyncIgnoredPaths& ignored, const std::string& path) {
    const auto exact = ignored.find(path);
    if (exact != ignored.end()) return &exact->second;
    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const auto parent = ignored.find(path.substr(0, slash));
        if (parent != ignored.end() && parent->second.tree) return &parent->second;
    }
    return nullptr;
}

inline std::string CanonicalSyncTime(const std::string& value) {
    static const std::regex pattern(R"(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?(Z|[+-]\d{2}:\d{2})$)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) throw std::runtime_error("Invalid file date");
    SYSTEMTIME time = {};
    time.wYear = static_cast<WORD>(std::stoi(match[1])); time.wMonth = static_cast<WORD>(std::stoi(match[2]));
    time.wDay = static_cast<WORD>(std::stoi(match[3])); time.wHour = static_cast<WORD>(std::stoi(match[4]));
    time.wMinute = static_cast<WORD>(std::stoi(match[5])); time.wSecond = static_cast<WORD>(std::stoi(match[6]));
    FILETIME fileTime;
    if (!SystemTimeToFileTime(&time, &fileTime)) throw std::runtime_error("Unsupported file date");
    ULARGE_INTEGER ticks;
    ticks.LowPart = fileTime.dwLowDateTime; ticks.HighPart = fileTime.dwHighDateTime;
    const std::string offset = match[8];
    if (offset != "Z") {
        const int hours = std::stoi(offset.substr(1, 2)), minutes = std::stoi(offset.substr(4, 2));
        if (hours > 23 || minutes > 59) throw std::runtime_error("Invalid time zone");
        const auto delta = static_cast<std::uint64_t>(hours * 60 + minutes) * 600000000ULL;
        if (ticks.QuadPart < delta) throw std::runtime_error("Unsupported file date");
        ticks.QuadPart = offset[0] == '+' ? ticks.QuadPart - delta : ticks.QuadPart + delta;
    }
    fileTime.dwLowDateTime = ticks.LowPart; fileTime.dwHighDateTime = ticks.HighPart;
    if (!FileTimeToSystemTime(&fileTime, &time)) throw std::runtime_error("Unsupported file date");
    std::string fraction = match[7]; fraction.append(9 - fraction.size(), '0');
    char output[64];
    sprintf_s(output, "%04u-%02u-%02uT%02u:%02u:%02u.%sZ", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, fraction.c_str());
    return output;
}

// Same-side comparisons use exact metadata. Across providers, prefer a common
// checksum, then size + UTC modtime at second precision (OneDrive precision).
inline bool SyncEquivalent(const SyncFile& a, const SyncFile& b) {
    if (a.size != b.size) return false;
    for (const auto& hash : a.hashes) {
        const auto found = b.hashes.find(hash.first);
        if (!hash.second.empty() && found != b.hashes.end() && !found->second.empty()) return hash.second == found->second;
    }
    return a.time.size() >= 20 && b.time.size() >= 20 && a.time.substr(0, 19) == b.time.substr(0, 19);
}

inline std::wstring SyncInventoryProgress(bool oneDrive, const std::string& data, std::uint64_t elapsedSeconds) {
    try {
        const auto progress = SyncJson::parse(data);
        const auto mode = progress.at("mode").get<std::string>();
        if ((mode != "full" && mode != "changes" && mode != "scan") ||
            !progress.at("items").is_number_unsigned() || !progress.at("pages").is_number_unsigned()) return {};
        return std::wstring(oneDrive ? L"OneDrive" : L"Google Drive") + L" — " +
            (mode == "changes" ? L"checking changes" : L"reading full inventory") + L" — " +
            std::to_wstring(progress.at("items").get<std::uint64_t>()) + L" items" +
            (mode == "scan" ? L"" : L" / " + std::to_wstring(progress.at("pages").get<std::uint64_t>()) + L" API pages") +
            L" — elapsed " + std::to_wstring(elapsedSeconds / 60) + L"m " + std::to_wstring(elapsedSeconds % 60) + L"s";
    } catch (...) { return {}; }
}

inline SyncFile ReadSyncFileRow(const SyncJson& row, const std::string& path) {
    const auto size = row.at("Size").get<std::int64_t>();
    SyncFile file;
    file.time = CanonicalSyncTime(row.at("ModTime").get<std::string>());
    if (size < 0) throw std::runtime_error("Unsupported path, size, or date: " + path);
    file.size = static_cast<std::uint64_t>(size);
    file.id = row.value("ID", std::string());
    if (row.contains("Hashes") && !row["Hashes"].is_null())
        file.hashes = row["Hashes"].get<std::map<std::string, std::string>>();
    return file;
}

inline bool ReadSyncInventoryRows(const SyncJson& json, SyncInventory& inventory, std::string& error) {
    inventory.clear();
    try {
        if (!json.is_array()) throw std::runtime_error("Invalid inventory");
        std::set<std::string> directories;
        for (const auto& row : json) {
            const auto path = row.at("Path").get<std::string>();
            if (!SafeSyncPath(path) || SyncPathKey(path).empty()) throw std::runtime_error("Unsupported path: " + path);
            if (row.at("IsDir").get<bool>()) {
                if (!directories.insert(path).second || inventory.count(path)) throw std::runtime_error("Ambiguous folder: " + path);
                continue;
            }
            if (directories.count(path)) throw std::runtime_error("File/folder collision: " + path);
            if (!inventory.emplace(path, ReadSyncFileRow(row, path)).second)
                throw std::runtime_error("Duplicate name: " + path);
        }
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

inline bool ReadSyncInventory(const std::string& text, SyncInventory& inventory, std::string& error) {
    try { return ReadSyncInventoryRows(SyncJson::parse(text), inventory, error); }
    catch (const std::exception& e) { inventory.clear(); error = e.what(); return false; }
}

// Google Drive permits files and folders with identical names in the same
// parent. OneDrive cannot represent those paths, and selecting one object by
// path would be arbitrary. Keep the rest of the inventory usable and report
// every affected path as ignored until the user gives the objects unique names.
inline bool ReadGoogleSyncInventory(const std::string& text, SyncInventory& inventory,
    SyncIgnoredPaths& ignoredPaths, std::string& error) {
    inventory.clear();
    ignoredPaths.clear();
    try {
        const auto json = SyncJson::parse(text);
        if (!json.is_array()) throw std::runtime_error("Invalid inventory");

        std::map<std::string, size_t> directories;
        std::map<std::string, std::string> directoryIds;
        struct FileGroup { size_t count = 0; const SyncJson* row = nullptr; };
        std::map<std::string, FileGroup> files;
        for (const auto& row : json) {
            const auto path = row.at("Path").get<std::string>();
            if (!SafeSyncPath(path) || SyncPathKey(path).empty())
                throw std::runtime_error("Unsupported path: " + path);
            if (row.at("IsDir").get<bool>()) {
                ++directories[path];
                directoryIds[path] = row.value("ID", std::string());
            }
            else { auto& group = files[path]; ++group.count; group.row = &row; }
        }

        // rclone identifies shortcuts as TARGET\tSHORTCUT. Only a target with
        // a real path outside every shortcut tree is already in this account's
        // inventory; shared targets reached solely through shortcuts stay valid.
        std::map<std::string, std::string> realFolders;
        for (const auto& directory : directoryIds) {
            if (directory.second.empty() || directory.second.find('\t') != std::string::npos) continue;
            bool viaShortcut = false;
            for (size_t slash = directory.first.find('/'); slash != std::string::npos; slash = directory.first.find('/', slash + 1)) {
                const auto parent = directoryIds.find(directory.first.substr(0, slash));
                if (parent != directoryIds.end() && parent->second.find('\t') != std::string::npos) viaShortcut = true;
            }
            if (!viaShortcut) realFolders.emplace(directory.second, directory.first);
        }
        for (const auto& directory : directoryIds) {
            const auto tab = directory.second.find('\t');
            if (tab == std::string::npos) continue;
            const auto target = realFolders.find(directory.second.substr(0, tab));
            if (target != realFolders.end()) ignoredPaths[directory.first] = {0,
                "This Google Drive folder shortcut points to " + target->second +
                ", which is already included. CloudNav ignored the shortcut tree to avoid syncing the same files twice.", true};
        }

        std::set<std::string> ambiguousDirectories;
        for (const auto& directory : directories)
            if (directory.second > 1 || files.count(directory.first)) ambiguousDirectories.insert(directory.first);

        const auto insideAmbiguousDirectory = [&](const std::string& path) {
            for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
                if (ambiguousDirectories.count(path.substr(0, slash))) return true;
            return ambiguousDirectories.count(path) != 0;
        };

        for (const auto& group : files) {
            const auto& path = group.first;
            const auto count = group.second.count;
            if (const auto* ignored = FindIgnoredGooglePath(ignoredPaths, path)) {
                ignoredPaths[path] = {count, ignored->reason, ignored->tree};
                continue;
            }
            if (count > 1) {
                ignoredPaths[path] = {count, "Google Drive contains " + std::to_string(count) +
                    " files with this same path. CloudNav ignored all of them; rename them to unique names to sync them with OneDrive."};
                continue;
            }
            if (insideAmbiguousDirectory(path)) {
                ignoredPaths[path] = {count, directories.count(path)
                    ? "Google Drive contains a file and a folder with this same path. CloudNav ignored the file and this folder tree; rename them to sync them with OneDrive."
                    : "This file is inside duplicate Google Drive folders. CloudNav ignored this ambiguous folder tree; rename the folders to sync it with OneDrive."};
                continue;
            }
            inventory.emplace(path, ReadSyncFileRow(*group.second.row, path));
        }
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

inline SyncJson SaveSyncInventory(const SyncInventory& inventory) {
    auto rows = SyncJson::array();
    for (const auto& item : inventory) rows.push_back({{"Path", item.first}, {"Size", item.second.size},
        {"ModTime", item.second.time}, {"Hashes", item.second.hashes}, {"ID", item.second.id}, {"IsDir", false}});
    return rows;
}

struct SyncRow {
    std::string path;
    char category = '=';
    SyncAction action = SyncAction::None;
    std::uint64_t bytes = 0;
    bool editDeleteConflict = false;
};
struct SyncAnalysis {
    SyncInventory oneDrive, google, previousOneDrive, previousGoogle;
    SyncIgnoredPaths ignoredGooglePaths;
    std::vector<SyncMove> pendingMoves;
    std::string moveGeneration;
    bool complete = false;
    bool hasBaseline = false;
    bool hasSharedBaseline = false;
    bool recovery = false;
    bool recoveryExpired = false;
    std::string binding;
    std::string error;
    std::string baselineDocument;
    std::string sharedDocument;
    std::string historyBinding;

    std::vector<SyncMove> Moves() const {
        if (!pendingMoves.empty()) return pendingMoves;
        std::vector<SyncMove> moves;
        if (!hasBaseline && !recoveryExpired) return moves;
        for (bool onGoogle : {true, false}) {
            const auto& moved = onGoogle ? google : oneDrive;
            const auto& other = onGoogle ? oneDrive : google;
            const auto& before = onGoogle ? previousGoogle : previousOneDrive;
            const auto& otherBefore = onGoogle ? previousOneDrive : previousGoogle;
            std::map<std::string, std::vector<std::string>> ids, contents;
            std::map<std::string, size_t> missingIds, missingContents;
            for (const auto& old : before) {
                if (moved.count(old.first)) continue;
                if (!old.second.id.empty()) ++missingIds[old.second.id];
                const auto key = SyncContentKey(old.second);
                if (!key.empty()) ++missingContents[key];
            }
            for (const auto& item : moved) {
                if (!item.second.id.empty()) ids[item.second.id].push_back(item.first);
                const auto key = SyncContentKey(item.second);
                if (!key.empty()) contents[key].push_back(item.first);
            }
            for (const auto& old : before) {
                if (moved.count(old.first) || FindIgnoredGooglePath(ignoredGooglePaths, old.first)) continue;
                const auto survivor = other.find(old.first), previous = otherBefore.find(old.first);
                if (survivor == other.end() || previous == otherBefore.end() || !SyncSnapshotMatches(previous->second, survivor->second)) continue;
                const auto& index = old.second.id.empty() ? contents : ids;
                const auto& missing = old.second.id.empty() ? missingContents : missingIds;
                const auto key = old.second.id.empty() ? SyncContentKey(old.second) : old.second.id;
                const auto count = missing.find(key);
                if (count == missing.end() || count->second != 1) continue;
                const auto found = index.find(key);
                if (found == index.end() || found->second.size() != 1) continue;
                const auto& path = found->second.front();
                if (before.count(path) || FindIgnoredGooglePath(ignoredGooglePaths, path) ||
                    path.find(".conflict-Google-") != std::string::npos) continue;
                const auto destination = other.find(path);
                if (destination != other.end() && !SyncEquivalent(moved.at(path), destination->second)) continue;
                moves.push_back({old.first, path, onGoogle, survivor->second, moved.at(path)});
            }
        }
        return moves;
    }

    bool MoveMatches(const SyncMove& move, bool requireDestination = false) const {
        const auto& moved = move.onGoogle ? google : oneDrive;
        const auto& other = move.onGoogle ? oneDrive : google;
        const auto source = moved.find(move.to), old = other.find(move.from), destination = other.find(move.to);
        return !moved.count(move.from) && source != moved.end() && SyncSnapshotMatches(move.replacement, source->second) &&
            (old == other.end() || SyncSnapshotMatches(move.original, old->second)) &&
            (destination == other.end() ? !requireDestination : SyncEquivalent(source->second, destination->second)) &&
            !FindIgnoredGooglePath(ignoredGooglePaths, move.from) && !FindIgnoredGooglePath(ignoredGooglePaths, move.to);
    }

    std::vector<SyncRow> Plan(SyncMode mode) const {
        std::set<std::string> paths;
        for (const auto* inventory : {&oneDrive, &google}) for (const auto& item : *inventory) paths.insert(item.first);
        for (const auto& item : ignoredGooglePaths) paths.insert(item.first);
        if (hasBaseline) for (const auto* inventory : {&previousOneDrive, &previousGoogle})
            for (const auto& item : *inventory) paths.insert(item.first);
        std::map<std::wstring, std::string> folded;
        std::map<std::wstring, std::string> folderSpellings;
        std::set<std::wstring> ambiguousFolders;
        std::set<std::string> collisions;
        for (const auto& path : paths) {
            const auto key = SyncPathKey(path);
            const auto found = folded.emplace(key, path);
            if (key.empty() || !found.second) {
                collisions.insert(path);
                if (!found.second) collisions.insert(found.first->second);
            }
            for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
                const auto prefix = path.substr(0, slash);
                const auto folderKey = SyncPathKey(prefix);
                const auto folder = folderSpellings.emplace(folderKey, prefix);
                if (!folder.second && folder.first->second != prefix) ambiguousFolders.insert(folderKey);
            }
        }
        // A file on one provider must not collide with a directory on the other.
        for (const auto& path : paths) {
            size_t slash = path.find('/');
            while (slash != std::string::npos) {
                const auto prefixKey = SyncPathKey(path.substr(0, slash));
                if (ambiguousFolders.count(prefixKey)) collisions.insert(path);
                const auto found = folded.find(prefixKey);
                if (found != folded.end()) { collisions.insert(path); collisions.insert(found->second); }
                slash = path.find('/', slash + 1);
            }
        }
        std::vector<SyncRow> rows;
        for (const auto& path : paths) {
            if (FindIgnoredGooglePath(ignoredGooglePaths, path)) {
                rows.push_back({path, '~', SyncAction::Ignored, 0});
                continue;
            }
            const auto od = oneDrive.find(path), gd = google.find(path);
            const bool a = od != oneDrive.end(), b = gd != google.end();
            if (!a && !b) continue;
            const bool same = a && b && SyncEquivalent(od->second, gd->second);
            SyncRow row{path, same ? '=' : a && b ? '*' : a ? '+' : '-', SyncAction::None, 0};
            if (!complete || collisions.count(path)) { row.action = SyncAction::Blocked; row.category = '!'; }
            else if (mode == SyncMode::ToGoogle) {
                if (a && (!b || (!same && od->second.time.substr(0, 19) > gd->second.time.substr(0, 19)))) row.action = SyncAction::ToGoogle;
            }
            else if (mode == SyncMode::ToOneDrive) {
                if (b && (!a || (!same && gd->second.time.substr(0, 19) > od->second.time.substr(0, 19)))) row.action = SyncAction::ToOneDrive;
            }
            else if (!same) {
                const auto oldA = previousOneDrive.find(path), oldB = previousGoogle.find(path);
                const bool hadA = hasBaseline && oldA != previousOneDrive.end();
                const bool hadB = hasBaseline && oldB != previousGoogle.end();
                const bool changedA = a && (!hadA || !(od->second == oldA->second));
                const bool changedB = b && (!hadB || !(gd->second == oldB->second));
                if (a && b) row.action = od->second.time.substr(0, 19) > gd->second.time.substr(0, 19) ? SyncAction::ToGoogle :
                    gd->second.time.substr(0, 19) > od->second.time.substr(0, 19) ? SyncAction::ToOneDrive : SyncAction::KeepBoth;
                else if (a) row.action = hadB && !changedA ? SyncAction::DeleteOneDrive : SyncAction::ToGoogle;
                else row.action = hadA && !changedB ? SyncAction::DeleteGoogle : SyncAction::ToOneDrive;
                // Edit-versus-delete preserves the edited file on both sides.
                row.editDeleteConflict = (a && !b && hadB && changedA) || (b && !a && hadA && changedB);
            }
            if (row.action == SyncAction::ToGoogle) row.bytes = od->second.size;
            if (row.action == SyncAction::ToOneDrive) row.bytes = gd->second.size;
            if (row.action == SyncAction::KeepBoth) row.bytes = od->second.size + gd->second.size;
            rows.push_back(std::move(row));
        }
        if (mode == SyncMode::Bidirectional) for (const auto& move : Moves()) {
            auto old = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.path == move.from; });
            const auto destination = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.path == move.to; });
            if (!MoveMatches(move) || (destination != rows.end() && destination->action == SyncAction::Blocked)) {
                if (old == rows.end()) rows.push_back({move.from, '!', SyncAction::Blocked});
                else { old->action = SyncAction::Blocked; old->category = '!'; }
            } else if (old != rows.end() && old->action != SyncAction::Blocked && old->action != SyncAction::Ignored) {
                old->action = move.onGoogle ? SyncAction::MoveOneDrive : SyncAction::MoveGoogle;
                old->bytes = 0;
                old->editDeleteConflict = false;
            }
        }
        return rows;
    }

    size_t IgnoredGoogleObjectCount() const {
        size_t count = 0;
        for (const auto& item : ignoredGooglePaths) count += item.second.objectCount;
        return count;
    }
};

inline void LimitSyncVerification(const SyncAnalysis& planned, SyncAnalysis& after,
    const std::vector<SyncRow>& rows, const std::string& conflictSuffix) {
    std::set<std::string> paths;
    for (const auto* inventory : {&planned.oneDrive, &planned.google})
        for (const auto& item : *inventory) paths.insert(item.first);
    for (const auto& row : rows) if (row.action == SyncAction::KeepBoth) paths.insert(row.path + conflictSuffix);
    // New arrivals belong to the next run and must not enter this run's baseline.
    for (auto* inventory : {&after.oneDrive, &after.google})
        for (auto item = inventory->begin(); item != inventory->end();)
            if (!paths.count(item->first)) item = inventory->erase(item);
            else ++item;
}

inline SyncInventory SyncBaselineInventory(const SyncInventory& inventory, const SyncIgnoredPaths& ignoredPaths) {
    auto result = inventory;
    for (auto item = result.begin(); item != result.end();) {
        if (FindIgnoredGooglePath(ignoredPaths, item->first)) item = result.erase(item);
        else ++item;
    }
    return result;
}

inline bool SyncBaselineUnchanged(const SyncAnalysis& analysis) {
    // Upgrade legacy history with provider identities even when contents match.
    for (const auto& inventories : {std::make_pair(&analysis.oneDrive, &analysis.previousOneDrive),
        std::make_pair(&analysis.google, &analysis.previousGoogle)})
        for (const auto& item : *inventories.first) {
            const auto previous = inventories.second->find(item.first);
            if (previous != inventories.second->end() && item.second.id != previous->second.id) return false;
        }
    return analysis.complete && analysis.hasBaseline && analysis.hasSharedBaseline && !analysis.recovery &&
        SyncBaselineInventory(analysis.oneDrive, analysis.ignoredGooglePaths) == analysis.previousOneDrive &&
        analysis.google == analysis.previousGoogle;
}

inline std::vector<std::wstring> SyncInventoryArguments(const std::wstring& config, const std::wstring& remote,
    const std::wstring& log, const std::wstring& cache = {}, bool full = false) {
    std::vector<std::wstring> args = {L"lsjson", remote, L"--config", config, L"--recursive", L"--hash", L"--fast-list",
        L"--onedrive-delta", L"--drive-skip-gdocs", L"--exclude", L"/Personal Vault/**", L"--exclude",
        L"/.CloudNav-history/**", L"--exclude", L"**/desktop.ini", L"--exclude", L"/.849C9593-D756-4E56-8D6E-42412F2A707B",
        L"--exclude", L"/Personal Vault.lnk", L"--use-json-log", L"--log-file", log};
    if (!cache.empty()) {
        args.insert(args.end(), {L"--onedrive-cloudnav-cache", cache, L"--drive-cloudnav-cache", cache});
        if (full) args.insert(args.end(), {L"--onedrive-cloudnav-full", L"--drive-cloudnav-full"});
    }
    return args;
}

inline std::string SyncFileList(const std::vector<SyncRow>& rows, SyncAction action) {
    std::string list;
    for (const auto& row : rows) if (row.action == action) {
        if (!SafeSyncPath(row.path)) throw std::runtime_error("Invalid transfer path");
        list += row.path + "\n";
    }
    return list;
}

// Target individual objects for small/sparse changes; match large batches
// covering at least half of each account using directory listings.
inline bool SyncUseTraversal(size_t selected, size_t sourceFiles, size_t destinationFiles) {
    return selected >= 64 && selected >= sourceFiles / 2 + sourceFiles % 2 &&
        selected >= destinationFiles / 2 + destinationFiles % 2;
}

inline std::uint64_t SyncPlannedBytes(const SyncAnalysis& analysis, const std::vector<SyncRow>& rows) {
    std::uint64_t total = 0;
    for (const auto& row : rows) {
        const auto add = [&](std::uint64_t bytes) {
            if (bytes > UINT64_MAX - total) throw std::runtime_error("Transfer size overflow");
            total += bytes;
        };
        add(row.bytes);
        if (row.action == SyncAction::KeepBoth) add(analysis.google.at(row.path).size);
    }
    return total;
}

inline bool LoadSyncBaseline(const std::string& document, const std::string& binding, SyncAnalysis& analysis) {
    analysis.hasSharedBaseline = false;
    try {
        const auto json = SyncJson::parse(document);
        std::string error;
        if (json.at("version") != 1 || json.at("binding") != binding ||
            !ReadSyncInventoryRows(json.at("oneDrive"), analysis.previousOneDrive, error) ||
            !ReadSyncInventoryRows(json.at("google"), analysis.previousGoogle, error)) return false;
        if (analysis.previousOneDrive.size() != analysis.previousGoogle.size()) return false;
        for (const auto& item : analysis.previousOneDrive) {
            const auto other = analysis.previousGoogle.find(item.first);
            if (other == analysis.previousGoogle.end() || !SyncEquivalent(item.second, other->second)) return false;
        }
        analysis.hasBaseline = true;
        return true;
    } catch (...) { return false; }
}

inline std::wstring SyncPlanSummary(const std::vector<SyncRow>& rows, bool complete) {
    size_t a = 0, b = 0, changes = 0, same = 0, deletions = 0, conflicts = 0, ignored = 0, blocked = 0;
    for (const auto& row : rows) {
        a += row.category == '+'; b += row.category == '-'; changes += row.category == '*';
        same += row.category == '=';
        deletions += row.action == SyncAction::DeleteGoogle || row.action == SyncAction::DeleteOneDrive ||
            row.action == SyncAction::MoveGoogle || row.action == SyncAction::MoveOneDrive;
        conflicts += row.action == SyncAction::KeepBoth || row.editDeleteConflict;
        ignored += row.action == SyncAction::Ignored; blocked += row.action == SyncAction::Blocked;
    }
    return std::wstring(complete ? L"" : L"Partial results — ") + L"OneDrive only: " + std::to_wstring(a) +
        L"    Google Drive only: " + std::to_wstring(b) + L"    Different: " + std::to_wstring(changes) +
        L"\r\nIdentical: " + std::to_wstring(same) + L"    Removals: " + std::to_wstring(deletions) + L"    Conflicts: " + std::to_wstring(conflicts) +
        L"    Ignored: " + std::to_wstring(ignored) + L"    Blocked: " + std::to_wstring(blocked);
}

inline std::wstring SyncPlanSummary(const SyncAnalysis& analysis, SyncMode mode) {
    return SyncPlanSummary(analysis.Plan(mode), analysis.complete);
}

} // namespace cloudnav
