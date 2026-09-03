#pragma once

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>

namespace cloudnav {

inline bool JsonNumber(const std::string& json, const char* name, double& value) {
    const std::string key = std::string("\"") + name + "\"";
    size_t position = json.find(key);
    if (position == std::string::npos) return false;
    position = json.find(':', position + key.size());
    if (position == std::string::npos) return false;
    const char* begin = json.c_str() + position + 1;
    char* end = nullptr;
    value = std::strtod(begin, &end);
    return end != begin;
}

inline int MigrationPercent(std::uint64_t completedBytes, std::uint64_t totalBytes,
                            std::uint64_t completedChecks, std::uint64_t totalChecks) {
    double ratio = 0.0;
    if (totalBytes > 0) ratio = static_cast<double>(completedBytes) / totalBytes;
    else if (totalChecks > 0) ratio = static_cast<double>(completedChecks) / totalChecks;
    return std::clamp(static_cast<int>(ratio * 100.0 + 0.5), 0, 100);
}

inline std::wstring FormatBytes(std::uint64_t bytes) {
    static constexpr const wchar_t* units[] = {L"o", L"Ko", L"Mo", L"Go", L"To"};
    double amount = static_cast<double>(bytes);
    size_t unit = 0;
    while (amount >= 1024.0 && unit + 1 < _countof(units)) { amount /= 1024.0; ++unit; }
    wchar_t result[64] = {};
    if (unit == 0) swprintf_s(result, L"%llu %s", static_cast<unsigned long long>(bytes), units[unit]);
    else swprintf_s(result, L"%.1f %s", amount, units[unit]);
    return result;
}

inline std::wstring FormatEta(double seconds) {
    if (seconds < 0.0) return L"ETA —";
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

}  // namespace cloudnav
