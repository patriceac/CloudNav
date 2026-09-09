#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "client_management.h"
#include <winhttp.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <vector>
#include <filesystem>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace cloudnav {
namespace {
bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring Expand(const std::wstring& value) {
    const DWORD length = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!length) return {};
    std::vector<wchar_t> buffer(length);
    if (!ExpandEnvironmentStringsW(value.c_str(), buffer.data(), length)) return {};
    return buffer.data();
}
std::wstring ReadString(HKEY key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
    return buffer.data();
}
bool Matches(CloudClient client, const std::wstring& name, const std::wstring& publisher) {
    if (client == CloudClient::OneDrive) return name == L"Microsoft OneDrive" && publisher == L"Microsoft Corporation";
    return (name == L"Google Drive" || name == L"Google Drive File Stream") &&
        (publisher == L"Google LLC" || publisher == L"Google, Inc.");
}
struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};
}

ClientInstallation DetectClientInstallation(CloudClient client) {
    ClientInstallation info;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY uninstall = nullptr;
            const auto status = RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                                              0, KEY_READ | view, &uninstall);
            if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) continue;
            if (status != ERROR_SUCCESS) { info.detectionComplete = false; continue; }
            for (DWORD index = 0;; ++index) {
                wchar_t name[256]; DWORD length = ARRAYSIZE(name);
                const auto enumerated = RegEnumKeyExW(uninstall, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (enumerated == ERROR_NO_MORE_ITEMS) break;
                if (enumerated != ERROR_SUCCESS) { info.detectionComplete = false; break; }
                HKEY entry = nullptr;
                if (RegOpenKeyExW(uninstall, name, 0, KEY_READ | view, &entry) != ERROR_SUCCESS) {
                    info.detectionComplete = false; continue;
                }
                if (Matches(client, ReadString(entry, L"DisplayName"), ReadString(entry, L"Publisher"))) {
                    // Registration is also evidence of a partial/broken install:
                    // don't offer a second install as if the client were absent.
                    info.installed = true;
                    std::wstring executable, arguments;
                    if (SplitClientCommand(ReadString(entry, L"UninstallString"), executable, arguments) && IsClientSetupExecutable(executable, client) && FileExists(executable)) {
                        info.uninstallExecutable = executable;
                        info.uninstallArguments = arguments;
                    }
                }
                RegCloseKey(entry);
            }
            RegCloseKey(uninstall);
        }
    }
    if (client == CloudClient::OneDrive) {
        for (const wchar_t* root : {L"%LOCALAPPDATA%\\Microsoft\\OneDrive", L"%ProgramFiles%\\Microsoft OneDrive", L"%ProgramFiles(x86)%\\Microsoft OneDrive"}) {
            const auto directory = Expand(root);
            if (!FileExists(directory + L"\\OneDrive.exe")) continue;
            info.installed = true;
            if (info.uninstallExecutable.empty() && FileExists(directory + L"\\OneDriveSetup.exe")) {
                info.uninstallExecutable = directory + L"\\OneDriveSetup.exe";
                info.uninstallArguments = L"/uninstall";
            }
        }
    } else if (!info.installed) {
        for (const wchar_t* root : {L"%ProgramFiles%\\Google\\Drive File Stream", L"%ProgramFiles(x86)%\\Google\\Drive File Stream", L"%LOCALAPPDATA%\\Google\\Drive File Stream"}) {
            std::error_code error;
            const auto directory = Expand(root);
            if (!std::filesystem::exists(directory, error)) { if (error) info.detectionComplete = false; continue; }
            std::filesystem::directory_iterator entries(directory, error);
            for (auto end = std::filesystem::directory_iterator(); !error && entries != end; entries.increment(error)) {
                if (!FileExists(entries->path().wstring() + L"\\GoogleDriveFS.exe")) continue;
                info.installed = true;
                // Missing registration: let Windows repair the installation;
                // never guess which of several versioned uninstallers is active.
            }
            if (error) info.detectionComplete = false;
        }
    }
    return info;
}

bool VerifyClientPublisher(const std::wstring& path, CloudClient client, std::wstring& error) {
    if (!IsClientSetupExecutable(path, client)) { error = L"Unexpected client setup program. Nothing was launched."; return false; }
    WINTRUST_FILE_INFO file = {sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust = {sizeof(trust)};
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    bool valid = false;
    if (result == ERROR_SUCCESS) {
        const auto provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        const auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        const auto certificate = signer && signer->csCertChain ? signer->pasCertChain[0].pCert : nullptr;
        wchar_t publisher[256] = {};
        if (certificate && CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, publisher, ARRAYSIZE(publisher))) {
            valid = client == CloudClient::OneDrive ? std::wstring(publisher) == L"Microsoft Corporation" :
                (std::wstring(publisher) == L"Google LLC" || std::wstring(publisher) == L"Google Inc");
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust);
    if (!valid) error = L"The file does not have a trusted " + std::wstring(ClientName(client)) + L" publisher signature. Nothing was launched.";
    return valid;
}

void RemoveClientDownload(const std::wstring& path) {
    if (path.empty()) return;
    DeleteFileW(path.c_str());
    const auto parent = std::filesystem::path(path).parent_path().wstring();
    RemoveDirectoryW(parent.c_str()); // only the empty per-download directory
}

bool DownloadClientInstaller(CloudClient client, const std::atomic_bool& cancel, std::wstring& path, std::wstring& error) {
    path.clear();
    SYSTEM_INFO system = {}; GetNativeSystemInfo(&system);
    const bool arm = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;
    const wchar_t* url = client == CloudClient::GoogleDrive ? L"https://dl.google.com/drive-file-stream/GoogleDriveSetup.exe" :
        arm ? L"https://go.microsoft.com/fwlink/?linkid=2282608" : L"https://go.microsoft.com/fwlink/?linkid=844652";
    URL_COMPONENTS parts = {sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) { error = L"Invalid installer address."; return false; }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring resource = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
        (parts.dwExtraInfoLength ? std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength) : L"");
    InternetHandle session{WinHttpOpen(L"CloudNav/1.4", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
    if (session.value) WinHttpSetTimeouts(session.value, 15000, 15000, 30000, 30000);
    InternetHandle connection{session.value ? WinHttpConnect(session.value, host.c_str(), parts.nPort, 0) : nullptr};
    InternetHandle request{connection.value ? WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (request.value) WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    if (!request.value || cancel || !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        error = cancel ? L"Download cancelled." : L"Unable to download the installer. Check your Internet connection and try again."; return false;
    }
    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &statusSize, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        error = L"The installer server returned HTTP " + std::to_wstring(status) + L". Try again later."; return false;
    }
    wchar_t temporary[MAX_PATH] = {}; GUID id = {}; wchar_t unique[40] = {};
    const DWORD temporaryLength = GetTempPathW(ARRAYSIZE(temporary), temporary);
    if (!temporaryLength || temporaryLength >= ARRAYSIZE(temporary) || FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, unique, ARRAYSIZE(unique))) {
        error = L"Unable to prepare the installer download."; return false;
    }
    const std::wstring directory = std::wstring(temporary) + L"CloudNav-" + unique;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) { error = L"Unable to create the download folder."; return false; }
    path = directory + (client == CloudClient::OneDrive ? L"\\OneDriveSetup.exe" : L"\\GoogleDriveSetup.exe");
    HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = output != INVALID_HANDLE_VALUE;
    unsigned long long total = 0;
    const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    char buffer[65536];
    while (ok && !cancel && GetTickCount64() < deadline) {
        DWORD read = 0, written = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &read)) { ok = false; break; }
        if (!read) break;
        total += read;
        if (total > 1024ULL * 1024 * 1024 || !WriteFile(output, buffer, read, &written, nullptr) || written != read) { ok = false; break; }
    }
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (cancel || GetTickCount64() >= deadline || total == 0) ok = false;
    if (!ok) error = cancel ? L"Download cancelled." : L"The installer download did not finish. Try again.";
    if (ok) ok = VerifyClientPublisher(path, client, error);
    if (!ok) { RemoveClientDownload(path); path.clear(); }
    return ok;
}

} // namespace cloudnav
