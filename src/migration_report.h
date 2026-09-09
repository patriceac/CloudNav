#pragma once

#include "migration_logic.h"
#include <map>
#include <sstream>

namespace cloudnav {

// rclone's JSON logger escapes quotes, backslashes and control characters.
inline bool JsonString(const std::string& json, const char* key, std::string& out) {
    size_t at = json.find(std::string("\"") + key + "\"");
    if (at == std::string::npos || (at = json.find(':', at)) == std::string::npos) return false;
    at = json.find_first_not_of(" \r\n\t", at + 1);
    if (at == std::string::npos || json[at++] != '"') return false;
    out.clear();
    const auto hex = [&](size_t& pos, unsigned& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos >= json.size()) return false;
            const char c = json[pos++];
            const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (n < 0) return false;
            value = value * 16 + n;
        }
        return true;
    };
    while (at < json.size()) {
        char c = json[at++];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (at == json.size()) return false;
        c = json[at++];
        if (c == 'u') {
            unsigned cp = 0;
            if (!hex(at, cp)) return false;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (json.substr(at, 2) != "\\u") return false;
                at += 2;
                unsigned low = 0;
                if (!hex(at, low) || low < 0xdc00 || low > 0xdfff) return false;
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
            if (cp < 0x80) out += static_cast<char>(cp);
            else {
                if (cp >= 0x10000) out += static_cast<char>(0xf0 | (cp >> 18));
                if (cp >= 0x800) out += static_cast<char>((cp >= 0x10000 ? 0x80 : 0xe0) | ((cp >> 12) & 0x3f));
                out += static_cast<char>((cp >= 0x800 ? 0x80 : 0xc0) | ((cp >> 6) & 0x3f));
                out += static_cast<char>(0x80 | (cp & 0x3f));
            }
        } else if (c == 'n') out += '\n';
        else if (c == 'r') out += '\r';
        else if (c == 't') out += '\t';
        else if (c == 'b') out += '\b';
        else if (c == 'f') out += '\f';
        else if (c == '"' || c == '\\' || c == '/') out += c;
        else return false;
    }
    return false;
}

struct AnalysisFile {
    char category = '?';
    std::uint64_t bytes = 0;
    bool sizeKnown = false;
    std::string error;
};

struct AnalysisReport {
    std::map<std::string, AnalysisFile> files;
    bool available = false;
    bool complete = false;
    bool malformed = false;
    std::wstring summaryOverride;

    void Log(const std::string& line) {
        std::string action, path, level, message;
        JsonString(line, "object", path);
        if (JsonString(line, "skipped", action) && action == "copy" && !path.empty()) {
            double bytes = -1;
            if (JsonNumber(line, "size", bytes) && bytes >= 0 &&
                bytes < static_cast<double>((std::numeric_limits<std::uint64_t>::max)())) {
                auto& file = files[path];
                file.bytes = static_cast<std::uint64_t>(bytes);
                file.sizeKnown = true;
            }
        }
        if (JsonString(line, "level", level) && level == "error" && JsonString(line, "msg", message)) {
            auto& file = files[path.empty() ? "[opération] " + message : path];
            file.category = '!';
            file.error = message;
        }
    }

    void Combined(std::istream& input) {
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line.size() < 3 || line[1] != ' ' || std::string("+=*-!").find(line[0]) == std::string::npos) {
                malformed = true;
                continue;
            }
            auto& file = files[line.substr(2)];
            // An error is never overwritten by a successful retry; the report
            // stays conservative. Repeated logger events count each path once.
            if (file.category != '!') file.category = line[0];
        }
        for (const auto& item : files) if (item.second.category == '?') malformed = true;
        available = true;
    }

    size_t Count(char category) const {
        return static_cast<size_t>(std::count_if(files.begin(), files.end(), [&](const auto& item) { return item.second.category == category; }));
    }
    bool CopyList(std::string& list) const {
        list.clear();
        if (!available || !complete || malformed || Count('!')) return false;
        for (const auto& item : files) {
            if (item.second.category != '+' && item.second.category != '*') continue;
            const auto& path = item.first;
            if (path.empty() || path.front() == '/' || path.find_first_of("\r\n") != std::string::npos ||
                path.find('\0') != std::string::npos) { list.clear(); return false; }
            list += path + "\n";
        }
        return true;
    }
    std::wstring CopySize() const {
        std::uint64_t bytes = 0;
        for (const auto& item : files) {
            const auto& file = item.second;
            if (file.category != '+' && file.category != '*') continue;
            if (!file.sizeKnown || bytes > (std::numeric_limits<std::uint64_t>::max)() - file.bytes) return L"indisponible";
            bytes += file.bytes;
        }
        return FormatBytes(bytes);
    }
    std::wstring Summary() const {
        if (!summaryOverride.empty()) return summaryOverride;
        if (!available) return L"Les résultats apparaîtront après l’analyse.";
        return std::wstring(complete ? L"" : L"Résultats partiels — ") +
            L"Nouveaux : " + std::to_wstring(Count('+')) + L"    Modifiés : " + std::to_wstring(Count('*')) +
            L"    Identiques : " + std::to_wstring(Count('=')) + L"\r\n" +
            L"Conservés sur Google Drive : " + std::to_wstring(Count('-')) + L"    Erreurs : " + std::to_wstring(Count('!')) +
            L"    À copier : " + CopySize();
    }
};

inline const wchar_t* AnalysisCategory(char category) {
    switch (category) {
    case '+': return L"OneDrive seul";
    case '*': return L"Différent";
    case '=': return L"Identique";
    case '-': return L"Google Drive seul";
    case '!': return L"Erreur";
    default: return L"Non classé";
    }
}

} // namespace cloudnav
