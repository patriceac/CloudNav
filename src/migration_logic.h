#pragma once

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>

namespace cloudnav {

enum class MigrationTask { None, AuthenticateOneDrive, AuthenticateGoogle, Analyze, CopyAndVerify };
enum class MigrationStage { Preparing, Connecting, Analyzing, Copying, Verifying };

inline void InvalidateMigrationValidation(MigrationTask task, bool& analyzed, bool& verified) {
    verified = false;
    if (task != MigrationTask::CopyAndVerify) analyzed = false;
}

inline const wchar_t* MigrationStageTitle(MigrationStage stage) {
    switch (stage) {
    case MigrationStage::Connecting: return L"Connexion du compte…";
    case MigrationStage::Analyzing: return L"1 / 3 — Analyse des écarts";
    case MigrationStage::Copying: return L"2 / 3 — Copie OneDrive → Google Drive";
    case MigrationStage::Verifying: return L"3 / 3 — Vérification des fichiers copiés";
    default: return L"Préparation de l’opération…";
    }
}

inline const wchar_t* MigrationStageDetails(MigrationStage stage) {
    switch (stage) {
    case MigrationStage::Connecting: return L"Termine la connexion dans le navigateur, puis reviens ici.";
    case MigrationStage::Analyzing: return L"Comparaison des comptes. Aucun fichier n’est copié pendant l’analyse.";
    case MigrationStage::Copying: return L"Les fichiers identiques sont ignorés. Tu peux annuler et reprendre la copie.";
    case MigrationStage::Verifying: return L"Comparaison indépendante. La configuration des dossiers sera proposée après réussite.";
    default: return L"La progression apparaîtra au démarrage. Tu peux annuler à tout moment.";
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
