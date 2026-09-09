#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <cwctype>

namespace cloudnav {

enum class CloudClient { OneDrive, GoogleDrive };

inline const wchar_t* ClientName(CloudClient client) {
    return client == CloudClient::OneDrive ? L"OneDrive" : L"Google Drive";
}

struct ClientInstallation {
    bool installed = false;
    bool detectionComplete = true;
    std::wstring uninstallExecutable;
    std::wstring uninstallArguments;
};

struct ClientActions { bool install; bool uninstall; };
inline ClientActions AvailableClientActions(const ClientInstallation& info, bool rootsKnown,
                                           bool scanComplete, bool foldersInUse, bool busy) {
    return {info.detectionComplete && !info.installed && !busy,
            info.detectionComplete && info.installed && !info.uninstallExecutable.empty() &&
                rootsKnown && scanComplete && !foldersInUse && !busy};
}

// Registry commands may have an unquoted executable containing spaces. Split at
// the .exe boundary, never at the first space, and never dispatch via a shell.
inline bool SplitClientCommand(const std::wstring& command, std::wstring& executable, std::wstring& arguments) {
    executable.clear(); arguments.clear();
    const auto start = command.find_first_not_of(L" \t");
    if (start == std::wstring::npos) return false;
    size_t end = std::wstring::npos, next = 0;
    if (command[start] == L'"') {
        end = command.find(L'"', start + 1);
        if (end == std::wstring::npos) return false;
        executable = command.substr(start + 1, end - start - 1);
        next = end + 1;
        if (next < command.size() && command[next] != L' ' && command[next] != L'\t') return false;
    } else {
        std::wstring lower = command;
        for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
        end = lower.find(L".exe", start);
        while (end != std::wstring::npos && end + 4 < command.size() &&
               command[end + 4] != L' ' && command[end + 4] != L'\t') end = lower.find(L".exe", end + 4);
        if (end == std::wstring::npos) return false;
        next = end + 4;
        executable = command.substr(start, next - start);
    }
    if (executable.size() < 7 || !std::iswalpha(executable[0]) || executable[1] != L':' || executable[2] != L'\\' ||
        _wcsicmp(executable.c_str() + executable.size() - 4, L".exe") != 0) return false;
    next = command.find_first_not_of(L" \t", next);
    if (next != std::wstring::npos) arguments = command.substr(next);
    return true;
}

inline bool IsClientSetupExecutable(const std::wstring& path, CloudClient client) {
    const auto slash = path.find_last_of(L"\\/");
    const auto name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    return client == CloudClient::OneDrive ? _wcsicmp(name.c_str(), L"OneDriveSetup.exe") == 0 :
        (_wcsicmp(name.c_str(), L"GoogleDriveSetup.exe") == 0 || _wcsicmp(name.c_str(), L"uninstall.exe") == 0);
}

ClientInstallation DetectClientInstallation(CloudClient client);
bool VerifyClientPublisher(const std::wstring& path, CloudClient client, std::wstring& error);
// Downloads only. Caller opens the verified installer with the normal vendor UI.
bool DownloadClientInstaller(CloudClient client, const std::atomic_bool& cancel,
                             std::wstring& path, std::wstring& error);
void RemoveClientDownload(const std::wstring& path);

} // namespace cloudnav
