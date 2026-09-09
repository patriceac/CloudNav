#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include <thread>
#include <memory>

#include "logic.h"
#include "folder_manager.h"
#include "migration.h"
#include "resource.h"
#include "ui.h"
#include "client_management.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr wchar_t kAppName[] = L"CloudNav";
constexpr wchar_t kSettingsKey[] = L"Software\\CloudNav";
constexpr wchar_t kWindowLeftValue[] = L"WindowLeft";
constexpr wchar_t kWindowTopValue[] = L"WindowTop";
constexpr wchar_t kMyDriveClsid[] = L"{D9816E5C-C103-4592-8D91-34B7E2429602}";
constexpr wchar_t kOneDrivePersonalClsid[] = L"{018D5C66-4533-4307-9B53-224DE2ED1FE6}";
constexpr wchar_t kFolderInstanceClsid[] = L"{0E5AAE11-A475-4c5b-AB00-C66DE400274E}";
constexpr DWORD kCloudSortOrder = 0x42;  // Documented cloud-provider order; Windows 11 still groups third parties below Favorites.

constexpr int IDC_MY_DRIVE = 1001;
constexpr int IDC_BROWSE = 1002;
constexpr int IDC_ONEDRIVE = 1003;
constexpr int IDC_GOOGLE_DRIVE = 1004;
constexpr int IDC_REFRESH = 1005;
constexpr int IDC_APPLY = 1006;
constexpr int IDC_PERSONAL_FOLDERS = 1007;
constexpr int IDC_DISABLE_ONEDRIVE_STARTUP = 1008;
constexpr int IDC_UNINSTALL_ONEDRIVE = 1009;
constexpr int IDC_MIGRATE_CLOUD = 1010;
constexpr int IDC_INSTALL_ONEDRIVE = 1014;
constexpr int IDC_INSTALL_GOOGLE = 1015;
constexpr int IDC_UNINSTALL_GOOGLE = 1016;
constexpr UINT WM_CLIENT_DOWNLOAD = WM_APP + 40;
constexpr UINT_PTR kClientProcessTimer = 40;
constexpr int kMainClientWidthDip = 740;
constexpr int kMainClientHeightDip = 692;

struct OneDriveInfo {
    std::wstring clsid;
    std::wstring label;
    std::wstring path;
    bool detected = false;
    bool visible = false;
};

struct PersonalFolderUsage {
    std::vector<std::wstring> names;
    bool complete = true;
};

struct AppState {
    std::wstring myDrivePath;
    bool myDriveVisible = false;
    DWORD myDriveSort = 0;
    OneDriveInfo oneDrive;
    wchar_t googleDriveLetter = 0;
    bool googleDriveVisible = false;
    bool oneDriveAutoStart = false;
    std::wstring oneDriveUninstaller;
    std::vector<std::wstring> oneDrivePersonalFolders;
    bool personalFolderScanComplete = true;
    cloudnav::ClientInstallation oneDriveClient;
    cloudnav::ClientInstallation googleClient;
    PersonalFolderUsage googlePersonalFolders;
    bool googleRootsKnown = false;
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_title = nullptr;
HWND g_subtitle = nullptr;
HWND g_sectionTitle = nullptr;
HWND g_myDrive = nullptr;
HWND g_myDriveDetail = nullptr;
HWND g_browse = nullptr;
HWND g_oneDrive = nullptr;
HWND g_oneDriveDetail = nullptr;
HWND g_oneDriveSafety = nullptr;
HWND g_disableOneDriveStartup = nullptr;
HWND g_uninstallOneDrive = nullptr;
HWND g_googleDrive = nullptr;
HWND g_googleDriveDetail = nullptr;
HWND g_personalFolders = nullptr;
HWND g_personalFoldersDetail = nullptr;
HWND g_migrateCloud = nullptr;
HWND g_migrateCloudDetail = nullptr;
HWND g_explanation = nullptr;
HWND g_status = nullptr;
HWND g_refresh = nullptr;
HWND g_apply = nullptr;
HWND g_foldersSection = nullptr;
HWND g_migrationSection = nullptr;
HWND g_oneDriveSection = nullptr;
HWND g_startupDetail = nullptr;
HWND g_installOneDrive = nullptr;
HWND g_installGoogle = nullptr;
HWND g_uninstallGoogle = nullptr;
HWND g_googleSection = nullptr;
HWND g_googleClientDetail = nullptr;
HWND g_googleSafety = nullptr;
HWND g_providerIcons[3] = {};
cloudnav::ui::ProviderImages g_providerImages;
std::wstring g_chosenMyDrivePath;
void UpdateVisibilityPending();
HFONT g_titleFont = nullptr;
HFONT g_bodyFont = nullptr;
HFONT g_bodyBoldFont = nullptr;
HFONT g_smallFont = nullptr;
HBRUSH g_backgroundBrush = nullptr;
UINT g_uiDpi = USER_DEFAULT_SCREEN_DPI;
AppState g_state;
bool g_demoMode = false;
bool g_demoPlan = false;
bool g_demoFailure = false;
bool g_demoSafeOneDriveActions = false;
bool g_demoMigration = false;
std::wstring g_demoMigrationResult;
bool g_statusIsError = false;
bool g_oneDriveBlocked = false;
bool g_demoClientsMissing = false;
bool g_demoClientFailure = false;
bool g_demoClientUnknownRoots = false;
bool g_clientDownloading = false;
bool g_closeAfterDownload = false;
std::atomic_bool g_cancelClientDownload{false};
std::thread g_clientDownloadThread;
HANDLE g_clientProcess = nullptr;
cloudnav::CloudClient g_activeClient = cloudnav::CloudClient::OneDrive;
std::wstring g_downloadedInstaller;
struct ClientDownloadResult { std::wstring path; std::wstring error; };
bool ClientBusy() { return g_clientDownloading || g_clientProcess != nullptr; }

std::wstring FormatWindowsError(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length && buffer ? std::wstring(buffer, length) : L"Windows error " + std::to_wstring(code);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

bool EqualsInsensitive(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool ContainsInsensitive(std::wstring value, std::wstring needle) {
    std::transform(value.begin(), value.end(), value.begin(), std::towlower);
    std::transform(needle.begin(), needle.end(), needle.begin(), std::towlower);
    return value.find(needle) != std::wstring::npos;
}

bool PathIsDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ExpandEnvironment(const std::wstring& value) {
    const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }
    std::vector<wchar_t> buffer(needed);
    if (ExpandEnvironmentStringsW(value.c_str(), buffer.data(), needed) == 0) {
        return value;
    }
    return buffer.data();
}

bool RegistryKeyExists(HKEY root, const std::wstring& subkey) {
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key);
    if (status == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

bool ReadRegistryDword(HKEY root, const std::wstring& subkey, const wchar_t* valueName, DWORD& value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(root, subkey.c_str(), valueName, RRF_RT_REG_DWORD, &type, &value, &size) == ERROR_SUCCESS;
}

bool ReadRegistryString(HKEY root, const std::wstring& subkey, const wchar_t* valueName, std::wstring& value) {
    DWORD type = 0;
    DWORD size = 0;
    LSTATUS status = RegGetValueW(root, subkey.c_str(), valueName,
                                 RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                                 &type, nullptr, &size);
    if (status != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return false;
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    status = RegGetValueW(root, subkey.c_str(), valueName,
                          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                          &type, buffer.data(), &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    value.assign(buffer.data());
    if (type == REG_EXPAND_SZ) {
        value = ExpandEnvironment(value);
    }
    return true;
}

LSTATUS WriteRegistryString(HKEY root, const std::wstring& subkey, const wchar_t* valueName,
                            const std::wstring& value, DWORD type = REG_SZ) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                    nullptr, &key, &disposition);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExW(key, valueName, 0, type,
                               reinterpret_cast<const BYTE*>(value.c_str()),
                               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    return status;
}

LSTATUS WriteRegistryDword(HKEY root, const std::wstring& subkey, const wchar_t* valueName, DWORD value) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                    nullptr, &key, &disposition);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExW(key, valueName, 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
    return status;
}

LSTATUS DeleteRegistryValue(HKEY root, const std::wstring& subkey, const wchar_t* valueName) {
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return ERROR_SUCCESS;
    }
    if (status == ERROR_SUCCESS) {
        status = RegDeleteValueW(key, valueName);
        RegCloseKey(key);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    return status;
}

bool ReadSavedWindowPosition(POINT& position) {
    DWORD left = 0;
    DWORD top = 0;
    if (!ReadRegistryDword(HKEY_CURRENT_USER, kSettingsKey, kWindowLeftValue, left) ||
        !ReadRegistryDword(HKEY_CURRENT_USER, kSettingsKey, kWindowTopValue, top)) {
        return false;
    }
    position.x = static_cast<LONG>(left);
    position.y = static_cast<LONG>(top);
    return true;
}

POINT ClampWindowPositionToWorkArea(POINT position, int width, int height) {
    const HMONITOR monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return position;
    }

    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    position.x = width >= workWidth
        ? info.rcWork.left
        : std::clamp(position.x, info.rcWork.left, info.rcWork.right - width);
    position.y = height >= workHeight
        ? info.rcWork.top
        : std::clamp(position.y, info.rcWork.top, info.rcWork.bottom - height);
    return position;
}

void RestoreWindowPosition(HWND window) {
    POINT position = {};
    RECT windowRect = {};
    if (!ReadSavedWindowPosition(position) || !GetWindowRect(window, &windowRect)) {
        return;
    }

    position = ClampWindowPositionToWorkArea(
        position, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top);
    SetWindowPos(window, nullptr, position.x, position.y, 0, 0,
                 SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SaveWindowPosition(HWND window) {
    if (!window || IsIconic(window)) {
        return;
    }
    RECT windowRect = {};
    if (!GetWindowRect(window, &windowRect)) {
        return;
    }
    WriteRegistryDword(HKEY_CURRENT_USER, kSettingsKey, kWindowLeftValue,
                       static_cast<DWORD>(windowRect.left));
    WriteRegistryDword(HKEY_CURRENT_USER, kSettingsKey, kWindowTopValue,
                       static_cast<DWORD>(windowRect.top));
}

std::wstring GetModulePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring GetUserProfilePath() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = ARRAYSIZE(buffer);
    if (GetEnvironmentVariableW(L"USERPROFILE", buffer, size) > 0) {
        return buffer;
    }
    return {};
}

bool DetectRootMirrorTask() {
    std::vector<wchar_t> windowsDirectory(MAX_PATH);
    const UINT length = GetWindowsDirectoryW(windowsDirectory.data(),
                                              static_cast<UINT>(windowsDirectory.size()));
    if (length == 0 || length >= windowsDirectory.size()) {
        return false;
    }
    const std::wstring taskPath = std::wstring(windowsDirectory.data(), length) +
        L"\\System32\\Tasks\\OneDrive-GDrive-Bidirectional-Sync";
    const DWORD attributes = GetFileAttributesW(taskPath.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ResolveShortcut(const std::wstring& shortcutPath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) {
        return {};
    }
    IPersistFile* persist = nullptr;
    std::wstring result;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
        if (SUCCEEDED(persist->Load(shortcutPath.c_str(), STGM_READ))) {
            wchar_t target[32768] = {};
            WIN32_FIND_DATAW data = {};
            if (SUCCEEDED(link->GetPath(target, ARRAYSIZE(target), &data, SLGP_RAWPATH))) {
                result = ExpandEnvironment(target);
            }
        }
        persist->Release();
    }
    link->Release();
    return result;
}

std::wstring ShellDisplayName(const std::wstring& clsid, std::wstring* fileSystemPath = nullptr) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    const std::wstring parsingName = L"shell:::" + clsid;
    if (FAILED(SHParseDisplayName(parsingName.c_str(), nullptr, &pidl, 0, nullptr))) {
        return {};
    }
    std::wstring label;
    PWSTR displayName = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &displayName)) && displayName) {
        label = displayName;
        CoTaskMemFree(displayName);
    }
    if (fileSystemPath) {
        wchar_t path[32768] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
            *fileSystemPath = path;
        }
    }
    CoTaskMemFree(pidl);
    return label;
}

std::wstring DetectGoogleDriveRoot(wchar_t& letter) {
    letter = 0;
    const DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (!needed) {
        return {};
    }
    std::vector<wchar_t> drives(needed + 2, L'\0');
    if (!GetLogicalDriveStringsW(needed + 1, drives.data())) {
        return {};
    }
    std::wstring fallback;
    for (const wchar_t* root = drives.data(); *root; root += std::wcslen(root) + 1) {
        wchar_t volumeName[MAX_PATH] = {};
        if (GetVolumeInformationW(root, volumeName, ARRAYSIZE(volumeName), nullptr, nullptr, nullptr, nullptr, 0) &&
            ContainsInsensitive(volumeName, L"Google Drive")) {
            letter = static_cast<wchar_t>(std::towupper(root[0]));
            return root;
        }
        const std::wstring shortcut = std::wstring(root) + L"My Drive.lnk";
        if (GetFileAttributesW(shortcut.c_str()) != INVALID_FILE_ATTRIBUTES) {
            fallback = root;
        }
    }
    if (!fallback.empty()) {
        letter = static_cast<wchar_t>(std::towupper(fallback[0]));
    }
    return fallback;
}

std::wstring DetectMyDrivePath() {
    std::wstring candidate;
    if (ReadRegistryString(HKEY_CURRENT_USER, L"Software\\CloudNav", L"MyDrivePath", candidate) &&
        PathIsDirectory(candidate)) {
        return candidate;
    }

    const std::wstring classKey = L"Software\\Classes\\CLSID\\" + std::wstring(kMyDriveClsid) +
                                  L"\\Instance\\InitPropertyBag";
    if (ReadRegistryString(HKEY_LOCAL_MACHINE, classKey, L"TargetFolderPath", candidate) &&
        PathIsDirectory(candidate)) {
        return candidate;
    }

    const std::wstring profile = GetUserProfilePath();
    const std::vector<std::wstring> localCandidates = {
        profile + L"\\My Drive",
        profile + L"\\Google Drive\\My Drive",
        profile + L"\\Google Drive"
    };
    for (const auto& path : localCandidates) {
        if (PathIsDirectory(path)) {
            return path;
        }
    }

    wchar_t driveLetter = 0;
    const std::wstring driveRoot = DetectGoogleDriveRoot(driveLetter);
    if (!driveRoot.empty()) {
        const std::wstring direct = driveRoot + L"My Drive";
        if (PathIsDirectory(direct)) {
            return direct;
        }
        const std::wstring resolved = ResolveShortcut(driveRoot + L"My Drive.lnk");
        if (PathIsDirectory(resolved)) {
            return resolved;
        }
    }
    return {};
}

bool ReadPinned(const std::wstring& clsid, bool defaultValue = true) {
    DWORD value = defaultValue ? 1 : 0;
    const std::wstring relative = L"Software\\Classes\\CLSID\\" + clsid;
    if (ReadRegistryDword(HKEY_CURRENT_USER, relative, L"System.IsPinnedToNameSpaceTree", value)) {
        return value != 0;
    }
    if (ReadRegistryDword(HKEY_LOCAL_MACHINE, relative, L"System.IsPinnedToNameSpaceTree", value)) {
        return value != 0;
    }
    return defaultValue;
}

std::wstring OneDriveNamespaceKey(const std::wstring& clsid) {
    return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\" + clsid;
}

bool IsOneDriveNamespaceVisible(const std::wstring& clsid) {
    return ReadPinned(clsid) ||
           RegistryKeyExists(HKEY_CURRENT_USER, OneDriveNamespaceKey(clsid)) ||
           RegistryKeyExists(HKEY_LOCAL_MACHINE, OneDriveNamespaceKey(clsid));
}

OneDriveInfo DetectOneDrive() {
    OneDriveInfo result;
    std::vector<std::wstring> accountNames;
    HKEY accounts = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts", 0,
                      KEY_ENUMERATE_SUB_KEYS, &accounts) == ERROR_SUCCESS) {
        DWORD index = 0;
        wchar_t name[256] = {};
        DWORD nameLength = ARRAYSIZE(name);
        while (RegEnumKeyExW(accounts, index++, name, &nameLength, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            accountNames.emplace_back(name, nameLength);
            nameLength = ARRAYSIZE(name);
        }
        RegCloseKey(accounts);
    }
    std::stable_sort(accountNames.begin(), accountNames.end(), [](const std::wstring& left, const std::wstring& right) {
        if (EqualsInsensitive(left, L"Personal")) return true;
        if (EqualsInsensitive(right, L"Personal")) return false;
        return left < right;
    });

    for (const auto& account : accountNames) {
        const std::wstring key = L"Software\\Microsoft\\OneDrive\\Accounts\\" + account;
        std::wstring clsid;
        std::wstring path;
        if (!ReadRegistryString(HKEY_CURRENT_USER, key, L"NamespaceRootId", clsid) || clsid.empty()) {
            continue;
        }
        ReadRegistryString(HKEY_CURRENT_USER, key, L"UserFolder", path);
        const std::wstring classKey = L"Software\\Classes\\CLSID\\" + clsid;
        if (!RegistryKeyExists(HKEY_CURRENT_USER, classKey) && !RegistryKeyExists(HKEY_LOCAL_MACHINE, classKey)) {
            continue;
        }
        std::wstring shellPath;
        std::wstring label = ShellDisplayName(clsid, &shellPath);
        if (label.empty()) {
            ReadRegistryString(HKEY_CURRENT_USER, classKey, nullptr, label);
        }
        result.clsid = clsid;
        result.label = label.empty() ? L"OneDrive" : label;
        result.path = !path.empty() ? path : shellPath;
        result.detected = true;
        result.visible = IsOneDriveNamespaceVisible(clsid);
        return result;
    }

    const std::wstring fallbackKey = L"Software\\Classes\\CLSID\\" + std::wstring(kOneDrivePersonalClsid);
    if (RegistryKeyExists(HKEY_CURRENT_USER, fallbackKey) || RegistryKeyExists(HKEY_LOCAL_MACHINE, fallbackKey)) {
        std::wstring shellPath;
        result.clsid = kOneDrivePersonalClsid;
        result.label = ShellDisplayName(result.clsid, &shellPath);
        result.path = shellPath;
        result.detected = true;
        result.visible = IsOneDriveNamespaceVisible(result.clsid);
    }
    return result;
}

PersonalFolderUsage DetectPersonalFoldersInRoots(const std::vector<std::wstring>& roots) {
    struct PersonalFolderSpec {
        const KNOWNFOLDERID* id;
        const wchar_t* label;
    };
    const std::array<PersonalFolderSpec, 6> folders = {{
        {&FOLDERID_Desktop, L"Desktop"},
        {&FOLDERID_Documents, L"Documents"},
        {&FOLDERID_Pictures, L"Pictures"},
        {&FOLDERID_Downloads, L"Downloads"},
        {&FOLDERID_Music, L"Music"},
        {&FOLDERID_Videos, L"Videos"}
    }};
    PersonalFolderUsage result;
    if (roots.empty()) {
        result.complete = false;
        return result;
    }
    for (const PersonalFolderSpec& folder : folders) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*folder.id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path) {
            if (std::any_of(roots.begin(), roots.end(), [&](const auto& root) { return cloudnav::PathIsWithin(path, root); })) {
                result.names.emplace_back(folder.label);
            }
            CoTaskMemFree(path);
        } else {
            result.complete = false;
        }
    }
    return result;
}

bool DetectOneDriveAutoStart() {
    std::wstring command;
    return ReadRegistryString(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"OneDrive", command) && !command.empty();
}

std::wstring DetectOneDriveUninstaller() {
    const std::wstring localAppData = ExpandEnvironment(L"%LOCALAPPDATA%");
    const std::wstring windows = ExpandEnvironment(L"%SystemRoot%");
    const std::array<std::wstring, 3> candidates = {{
        localAppData + L"\\Microsoft\\OneDrive\\OneDriveSetup.exe",
        windows + L"\\SysWOW64\\OneDriveSetup.exe",
        windows + L"\\System32\\OneDriveSetup.exe"
    }};
    for (const std::wstring& candidate : candidates) {
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return candidate;
        }
    }
    return {};
}

void DetectOneDriveClientState(AppState& state) {
    state.oneDriveClient = cloudnav::DetectClientInstallation(cloudnav::CloudClient::OneDrive);
    state.oneDriveAutoStart = DetectOneDriveAutoStart();
    state.oneDriveUninstaller = state.oneDriveClient.uninstallExecutable;
    if (state.oneDriveUninstaller.empty() && state.oneDriveClient.installed) {
        state.oneDriveUninstaller = DetectOneDriveUninstaller();
        state.oneDriveClient.uninstallExecutable = state.oneDriveUninstaller;
        state.oneDriveClient.uninstallArguments = L"/uninstall";
    }
    std::vector<std::wstring> roots;
    if (!state.oneDrive.path.empty()) roots.push_back(state.oneDrive.path);
    HKEY accounts = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts", 0, KEY_READ, &accounts) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t name[256]; DWORD length = ARRAYSIZE(name);
            if (RegEnumKeyExW(accounts, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring path;
            if (ReadRegistryString(accounts, name, L"UserFolder", path) && !path.empty()) roots.push_back(path);
        }
        RegCloseKey(accounts);
    }
    const PersonalFolderUsage usage = DetectPersonalFoldersInRoots(roots);
    state.oneDrivePersonalFolders = usage.names;
    state.personalFolderScanComplete = usage.complete;
}

void DetectGoogleClientState(AppState& state) {
    state.googleClient = cloudnav::DetectClientInstallation(cloudnav::CloudClient::GoogleDrive);
    std::vector<std::wstring> roots;
    if (!state.myDrivePath.empty()) roots.push_back(state.myDrivePath);
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        std::wstring root = L"A:\\"; root[0] += static_cast<wchar_t>(i);
        wchar_t label[MAX_PATH] = {};
        if (GetVolumeInformationW(root.c_str(), label, ARRAYSIZE(label), nullptr, nullptr, nullptr, nullptr, 0) &&
            ContainsInsensitive(label, L"Google Drive")) roots.push_back(root);
    }
    state.googleRootsKnown = !roots.empty();
    state.googlePersonalFolders = DetectPersonalFoldersInRoots(roots);
}

AppState DetectState() {
    if (g_demoMode) {
        AppState demo;
        demo.myDrivePath = L"C:\\Users\\Example\\My Drive";
        demo.myDriveVisible = true;
        demo.myDriveSort = kCloudSortOrder;
        demo.oneDrive = {kOneDrivePersonalClsid, L"Personal account", L"C:\\Users\\Example\\OneDrive", true, true};
        demo.googleDriveLetter = L'G';
        demo.googleDriveVisible = false;
        demo.oneDriveAutoStart = true;
        demo.oneDriveUninstaller = L"C:\\Windows\\System32\\OneDriveSetup.exe";
        demo.personalFolderScanComplete = true;
        demo.oneDriveClient = {true, true, demo.oneDriveUninstaller, L"/uninstall"};
        demo.googleClient = {true, true, L"C:\\Program Files\\Google\\Drive File Stream\\130.0.2.0\\uninstall.exe", L""};
        demo.googleRootsKnown = true;
        if (!g_demoSafeOneDriveActions) {
            demo.oneDrivePersonalFolders = {L"Documents", L"Pictures"};
            demo.googlePersonalFolders.names = {L"Music"};
        }
        if (g_demoClientsMissing) {
            demo.oneDriveClient = {}; demo.googleClient = {};
            demo.oneDrive.detected = false;
            demo.oneDriveAutoStart = false;
            demo.oneDrivePersonalFolders.clear(); demo.googlePersonalFolders.names.clear();
        }
        if (g_demoClientUnknownRoots) {
            demo.oneDrive.path.clear();
            demo.personalFolderScanComplete = false;
            demo.googleRootsKnown = false;
            demo.googlePersonalFolders.complete = false;
        }
        return demo;
    }

    AppState state;
    state.myDrivePath = DetectMyDrivePath();
    const std::wstring classKey = L"Software\\Classes\\CLSID\\" + std::wstring(kMyDriveClsid);
    DWORD pinned = 0;
    ReadRegistryDword(HKEY_LOCAL_MACHINE, classKey, L"System.IsPinnedToNameSpaceTree", pinned);
    ReadRegistryDword(HKEY_LOCAL_MACHINE, classKey, L"SortOrderIndex", state.myDriveSort);
    const std::wstring namespaceKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\" +
                                      std::wstring(kMyDriveClsid);
    state.myDriveVisible = pinned != 0 && RegistryKeyExists(HKEY_LOCAL_MACHINE, namespaceKey);
    state.oneDrive = DetectOneDrive();
    DetectOneDriveClientState(state);
    DetectGoogleClientState(state);

    DetectGoogleDriveRoot(state.googleDriveLetter);
    if (state.googleDriveLetter) {
        DWORD noDrives = 0;
        ReadRegistryDword(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                          L"NoDrives", noDrives);
        DWORD machineNoDrives = 0;
        ReadRegistryDword(HKEY_LOCAL_MACHINE,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                          L"NoDrives", machineNoDrives);
        noDrives |= machineNoDrives;
        state.googleDriveVisible = cloudnav::IsDriveVisible(noDrives, state.googleDriveLetter);
    }
    return state;
}

std::wstring GoogleDriveLabel(const AppState& state) {
    if (!state.googleDriveLetter) {
        return L"Google Drive (not detected)";
    }
    std::wstring label = L"Google Drive — drive ";
    label.push_back(state.googleDriveLetter);
    label += L":";
    return label;
}

std::wstring GoogleDriveDetail(const AppState& state) {
    if (!state.googleDriveLetter) {
        return L"Start Google Drive so CloudNav can detect its drive letter.";
    }
    return L"Show the virtual drive in File Explorer; files remain accessible when hidden.";
}

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void ShowStatus(const std::wstring& message, bool error = false) {
    g_statusIsError = error;
    SetWindowTextW(g_status, message.c_str());
    InvalidateRect(g_status, nullptr, TRUE);
    UpdateWindow(g_status);
}

std::wstring JoinFolderNames(const std::vector<std::wstring>& names) {
    std::wstring result;
    for (size_t index = 0; index < names.size(); ++index) {
        if (index > 0) {
            result += index + 1 == names.size() ? L" and " : L", ";
        }
        result += names[index];
    }
    return result;
}

void UpdateControlsFromState() {
    g_chosenMyDrivePath = g_state.myDrivePath;
    const std::wstring myDetail = g_state.myDrivePath.empty()
        ? L"Folder not detected: use Browse."
        : g_state.myDrivePath;
    SetWindowTextW(g_myDriveDetail, myDetail.c_str());
    Button_SetCheck(g_myDrive, g_state.myDriveVisible ? BST_CHECKED : BST_UNCHECKED);

    if (g_state.oneDrive.detected) {
        SetWindowTextW(g_oneDrive, cloudnav::ProviderAccountLabel(L"OneDrive", g_state.oneDrive.label).c_str());
        std::wstring detail = g_state.oneDrive.path.empty()
            ? L"OneDrive account detected."
            : g_state.oneDrive.path;
        const std::wstring startup = g_state.oneDriveAutoStart
            ? L"Automatic startup enabled"
            : L"Automatic startup disabled";
        SetWindowTextW(g_startupDetail, startup.c_str());
        SetWindowTextW(g_oneDriveDetail, detail.c_str());
        EnableWindow(g_oneDrive, TRUE);
        Button_SetCheck(g_oneDrive, g_state.oneDrive.visible ? BST_CHECKED : BST_UNCHECKED);
    } else {
        SetWindowTextW(g_oneDrive, L"OneDrive (not detected)");
        SetWindowTextW(g_startupDetail, L"Client not detected");
        SetWindowTextW(g_oneDriveDetail, L"No OneDrive account is registered on this PC.");
        EnableWindow(g_oneDrive, FALSE);
        Button_SetCheck(g_oneDrive, BST_UNCHECKED);
    }

    const bool anyPersonalFolderUsesOneDrive = !g_state.oneDrivePersonalFolders.empty();
    const auto oneDriveActions = cloudnav::AvailableClientActions(g_state.oneDriveClient,
        !g_state.oneDrive.path.empty(), g_state.personalFolderScanComplete, anyPersonalFolderUsesOneDrive, ClientBusy());
    const bool canDetach = g_state.oneDriveClient.detectionComplete && !ClientBusy() && cloudnav::CanDetachOneDrive(
        g_state.oneDriveClient.installed, !g_state.oneDrive.path.empty(), g_state.personalFolderScanComplete, anyPersonalFolderUsesOneDrive);
    g_oneDriveBlocked = g_state.oneDriveClient.installed &&
        (anyPersonalFolderUsesOneDrive || !g_state.personalFolderScanComplete ||
         g_state.oneDrive.path.empty());
    if (!g_state.oneDrivePersonalFolders.empty()) {
        const std::wstring warning = L"Used by: " +
            JoinFolderNames(g_state.oneDrivePersonalFolders) +
            L". Move these folders before disabling startup or uninstalling.";
        SetWindowTextW(g_oneDriveSafety, warning.c_str());
    } else if (g_oneDriveBlocked) {
        SetWindowTextW(g_oneDriveSafety,
            L"⚠ Unable to check all personal folders; OneDrive actions are blocked.");
    } else if (g_state.oneDriveClient.installed) {
        SetWindowTextW(g_oneDriveSafety,
            L"None of the six managed personal folders uses OneDrive. Other folders have not been checked.");
    } else {
        SetWindowTextW(g_oneDriveSafety, L"");
    }
    EnableWindow(g_disableOneDriveStartup,
                 canDetach && g_state.oneDriveAutoStart ? TRUE : FALSE);
    EnableWindow(g_uninstallOneDrive,
                 canDetach && !g_state.oneDriveUninstaller.empty() ? TRUE : FALSE);
    ShowWindow(g_installOneDrive, !g_state.oneDriveClient.installed ? SW_SHOW : SW_HIDE);
    ShowWindow(g_uninstallOneDrive, g_state.oneDriveClient.installed ? SW_SHOW : SW_HIDE);
    EnableWindow(g_installOneDrive, oneDriveActions.install || (g_clientDownloading && g_activeClient == cloudnav::CloudClient::OneDrive));
    SetWindowTextW(g_installOneDrive, g_clientDownloading && g_activeClient == cloudnav::CloudClient::OneDrive ? L"Cancel download" : L"Install OneDrive");
    const std::wstring oneDriveStatus = !g_state.oneDriveClient.detectionComplete ? L"Installation status unavailable" :
        !g_state.oneDriveClient.installed ? L"Not installed" :
        g_state.oneDriveAutoStart ? L"Installed · Automatic startup enabled" : L"Installed · Automatic startup disabled";
    SetWindowTextW(g_startupDetail, oneDriveStatus.c_str());
    if (g_state.oneDriveClient.installed && g_state.oneDriveClient.uninstallExecutable.empty())
        SetWindowTextW(g_oneDriveSafety, L"Uninstaller not found. Repair or remove the client using Windows Installed apps.");
    InvalidateRect(g_oneDriveSafety, nullptr, TRUE);

    const auto googleActions = cloudnav::AvailableClientActions(g_state.googleClient, g_state.googleRootsKnown,
        g_state.googlePersonalFolders.complete, !g_state.googlePersonalFolders.names.empty(), ClientBusy());
    ShowWindow(g_installGoogle, !g_state.googleClient.installed ? SW_SHOW : SW_HIDE);
    ShowWindow(g_uninstallGoogle, g_state.googleClient.installed ? SW_SHOW : SW_HIDE);
    EnableWindow(g_installGoogle, googleActions.install || (g_clientDownloading && g_activeClient == cloudnav::CloudClient::GoogleDrive));
    SetWindowTextW(g_installGoogle, g_clientDownloading && g_activeClient == cloudnav::CloudClient::GoogleDrive ? L"Cancel download" : L"Install Google Drive");
    EnableWindow(g_uninstallGoogle, googleActions.uninstall);
    SetWindowTextW(g_googleClientDetail, !g_state.googleClient.detectionComplete ? L"Installation status unavailable" :
        g_state.googleClient.installed ? L"Installed" : L"Not installed");
    const std::wstring googleSafety = !g_state.googleClient.installed ? L"" :
        !g_state.googlePersonalFolders.names.empty() ? L"Used by: " + JoinFolderNames(g_state.googlePersonalFolders.names) + L". Move these folders before uninstalling." :
        !g_state.googleRootsKnown ? L"Folder locations unknown. Start Google Drive and choose its My Drive folder before uninstalling." :
        !g_state.googlePersonalFolders.complete ? L"Unable to check all personal folders. Uninstall is blocked." :
        g_state.googleClient.uninstallExecutable.empty() ? L"Uninstaller not found. Repair or remove the client using Windows Installed apps." :
        L"None of the six managed personal folders uses the detected Google Drive locations. Other folders have not been checked.";
    SetWindowTextW(g_googleSafety, googleSafety.c_str());

    SetWindowTextW(g_googleDrive, GoogleDriveLabel(g_state).c_str());
    SetWindowTextW(g_googleDriveDetail, GoogleDriveDetail(g_state).c_str());
    EnableWindow(g_googleDrive, g_state.googleDriveLetter != 0);
    Button_SetCheck(g_googleDrive, g_state.googleDriveVisible ? BST_CHECKED : BST_UNCHECKED);
    UpdateVisibilityPending();
    for (HWND control : {g_browse, g_personalFolders, g_migrateCloud, g_refresh}) EnableWindow(control, !ClientBusy());
    if (ClientBusy()) EnableWindow(g_apply, FALSE);
}

bool VerifyOneDriveActionGuard() {
    if (!g_demoMode) {
        g_state = DetectState();
    }
    UpdateControlsFromState();
    if (!g_state.oneDriveClient.detectionComplete || !cloudnav::CanDetachOneDrive(
            g_state.oneDriveClient.installed, !g_state.oneDrive.path.empty(),
            g_state.personalFolderScanComplete,
            !g_state.oneDrivePersonalFolders.empty())) {
        const std::wstring message = !g_state.oneDrivePersonalFolders.empty()
            ? L"Action blocked: " + JoinFolderNames(g_state.oneDrivePersonalFolders) +
              L" still points to OneDrive. Move it first using Personal folders."
            : (g_state.oneDrive.detected
                ? L"CloudNav cannot reliably check all personal folders. Action blocked."
                : L"OneDrive is not detected on this PC.");
        MessageBoxW(g_window, message.c_str(), L"CloudNav — OneDrive",
                    MB_OK | MB_ICONWARNING);
        ShowStatus(L"OneDrive action blocked to protect personal folders.", true);
        return false;
    }
    return true;
}

void DisableOneDriveStartup() {
    if (!VerifyOneDriveActionGuard()) {
        return;
    }
    if (g_demoMode) {
        g_state.oneDriveAutoStart = false;
        UpdateControlsFromState();
        ShowStatus(L"Test mode: startup disabled; OneDrive would remain active.");
        return;
    }
    const LSTATUS status = DeleteRegistryValue(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"OneDrive");
    if (status != ERROR_SUCCESS) {
        ShowStatus(L"Unable to disable OneDrive startup: " +
                   FormatWindowsError(status), true);
        return;
    }
    g_state.oneDriveAutoStart = DetectOneDriveAutoStart();
    UpdateControlsFromState();
    ShowStatus(g_state.oneDriveAutoStart
        ? L"OneDrive is still configured to start automatically."
        : L"Startup disabled; OneDrive remains active for this session.",
        g_state.oneDriveAutoStart);
}

bool LaunchClientProgram(cloudnav::CloudClient client, const std::wstring& executable,
                         const std::wstring& arguments, bool uninstall) {
    std::wstring error;
    // Hold the file against replacement between verification and process launch.
    HANDLE locked = CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) { ShowStatus(L"Unable to open the client setup program.", true); return false; }
    const bool trusted = cloudnav::VerifyClientPublisher(executable, client, error);
    SHELLEXECUTEINFOW execute = {sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = g_window;
    execute.lpVerb = uninstall ? L"runas" : L"open";
    execute.lpFile = executable.c_str();
    execute.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    execute.nShow = SW_SHOWNORMAL;
    const bool launched = trusted && ShellExecuteExW(&execute);
    const DWORD launchError = GetLastError();
    CloseHandle(locked);
    if (!launched) {
        ShowStatus(trusted ? (launchError == ERROR_CANCELLED ? L"Setup cancelled. No installation change confirmed." :
            L"Unable to start setup: " + FormatWindowsError(launchError)) : error, true);
        return false;
    }
    g_activeClient = client;
    g_clientProcess = execute.hProcess;
    if (g_clientProcess) SetTimer(g_window, kClientProcessTimer, 1000, nullptr);
    ShowStatus(std::wstring(cloudnav::ClientName(client)) + (uninstall ? L" uninstaller opened. Follow its instructions." : L" installer opened. Follow its instructions, then sign in."));
    UpdateControlsFromState();
    return true;
}

void InstallClient(cloudnav::CloudClient client) {
    if (g_clientDownloading && client == g_activeClient) {
        g_cancelClientDownload = true;
        ShowStatus(L"Cancelling download…");
        return;
    }
    if (ClientBusy()) return;
    if (!g_demoMode) g_state = DetectState();
    const auto& info = client == cloudnav::CloudClient::OneDrive ? g_state.oneDriveClient : g_state.googleClient;
    UpdateControlsFromState();
    if (!cloudnav::AvailableClientActions(info, false, false, false, false).install) {
        ShowStatus(info.installed ? L"This client is already installed." : L"Unable to determine whether the client is installed.", true);
        return;
    }
    const std::wstring name = cloudnav::ClientName(client);
    if (!cloudnav::ui::Confirm(g_window, (L"CloudNav — install " + name).c_str(), (L"Install " + name + L" on this PC?").c_str(),
        L"CloudNav will download the official installer and verify its publisher signature. Follow the setup instructions; Windows may request administrator approval. Sign in to your account after installation.",
        (L"Install " + name).c_str())) { ShowStatus(name + L" installation cancelled."); return; }
    if (!g_demoMode) {
        const auto current = cloudnav::DetectClientInstallation(client);
        if (current.installed || !current.detectionComplete) { g_state = DetectState(); UpdateControlsFromState(); ShowStatus(L"Client status changed. Review it before installing.", true); return; }
    }
    if (g_demoMode) {
        if (g_demoClientFailure) { ShowStatus(L"Test mode: installer download failed. Nothing was launched.", true); return; }
        auto& simulated = client == cloudnav::CloudClient::OneDrive ? g_state.oneDriveClient : g_state.googleClient;
        simulated = {true, true, L"C:\\Example\\setup.exe", L"/uninstall"};
        UpdateControlsFromState();
        ShowStatus(L"Test mode: " + name + L" installation simulated.");
        return;
    }
    g_activeClient = client;
    g_clientDownloading = true;
    g_cancelClientDownload = false;
    UpdateControlsFromState();
    ShowStatus(L"Downloading the official " + name + L" installer…");
    try {
        g_clientDownloadThread = std::thread([client, owner = g_window] {
            auto result = std::make_unique<ClientDownloadResult>();
            try { cloudnav::DownloadClientInstaller(client, g_cancelClientDownload, result->path, result->error); }
            catch (...) { result->error = L"Unable to prepare the installer."; cloudnav::RemoveClientDownload(result->path); result->path.clear(); }
            if (PostMessageW(owner, WM_CLIENT_DOWNLOAD, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
            else cloudnav::RemoveClientDownload(result->path);
        });
    } catch (...) {
        g_clientDownloading = false;
        UpdateControlsFromState();
        ShowStatus(L"Unable to start the installer download.", true);
    }
}

bool VerifyGoogleActionGuard() {
    if (!g_demoMode) g_state = DetectState();
    UpdateControlsFromState();
    const auto actions = cloudnav::AvailableClientActions(g_state.googleClient, g_state.googleRootsKnown,
        g_state.googlePersonalFolders.complete, !g_state.googlePersonalFolders.names.empty(), ClientBusy());
    if (actions.uninstall) return true;
    MessageBoxW(g_window, L"Google Drive removal is blocked. Review the client status and move any personal folders that still use Google Drive before trying again.",
        L"CloudNav — Google Drive", MB_OK | MB_ICONWARNING);
    ShowStatus(L"Google Drive uninstall blocked to protect personal folders.", true);
    return false;
}

void UninstallGoogleDrive() {
    if (ClientBusy() || !VerifyGoogleActionGuard()) return;
    if (!cloudnav::ui::Confirm(g_window, L"CloudNav — uninstall Google Drive", L"Uninstall Google Drive from this PC?",
        L"The six managed personal folders do not use the detected Google Drive locations. Other synced or backed-up folders have not been checked.\n\n"
        L"Make sure Google Drive has finished syncing. Streamed files will no longer be available through its virtual drive. Cloud files remain accessible at drive.google.com.",
        L"Uninstall Google Drive", true)) { ShowStatus(L"Google Drive uninstall cancelled."); return; }
    if (!VerifyGoogleActionGuard()) return;
    if (g_demoMode) {
        g_state.googleClient = {};
        UpdateControlsFromState();
        ShowStatus(L"Test mode: Google Drive uninstall simulated.");
        return;
    }
    LaunchClientProgram(cloudnav::CloudClient::GoogleDrive, g_state.googleClient.uninstallExecutable,
                        g_state.googleClient.uninstallArguments, true);
}

void UninstallOneDrive() {
    if (ClientBusy()) return;
    if (!VerifyOneDriveActionGuard()) {
        return;
    }
    if (g_state.oneDriveUninstaller.empty()) {
        ShowStatus(L"OneDrive uninstaller not found.", true);
        return;
    }
    const bool confirmation = cloudnav::ui::Confirm(
        g_window, L"CloudNav — uninstall OneDrive", L"Uninstall the OneDrive client from this PC?",
        L""
        L"CloudNav has checked that Desktop, Documents, Pictures, Downloads, Music, and Videos "
        L"do not point to OneDrive. Other synchronized folders have not been checked.\n\n"
        L"Before continuing, make sure OneDrive says Up to date. Files stored "
        L"in the cloud will not be deleted; online-only files will remain "
        L"accessible on OneDrive.com.",
        L"Uninstall OneDrive", true);
    if (!confirmation) {
        ShowStatus(L"OneDrive uninstall cancelled.");
        return;
    }
    if (g_demoMode) {
        g_state.oneDriveClient = {};
        g_state.oneDriveAutoStart = false;
        UpdateControlsFromState();
        ShowStatus(L"Test mode: OneDrive uninstall simulated.");
        return;
    }
    if (!VerifyOneDriveActionGuard()) return;
    LaunchClientProgram(cloudnav::CloudClient::OneDrive, g_state.oneDriveClient.uninstallExecutable,
                        g_state.oneDriveClient.uninstallArguments, true);
}

std::wstring PickFolder(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose the My Drive folder");
    dialog->SetOkButtonLabel(L"Use this folder");
    std::wstring path;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR selected = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) && selected) {
                path = selected;
                CoTaskMemFree(selected);
            }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

bool WriteMyDriveClass(const std::wstring& classRoot, bool show, const std::wstring& path,
                       std::wstring& error) {
    const std::wstring classKey = classRoot + L"\\" + kMyDriveClsid;
    if (!show) {
        if (RegistryKeyExists(HKEY_LOCAL_MACHINE, classKey)) {
            const LSTATUS status = WriteRegistryDword(HKEY_LOCAL_MACHINE, classKey,
                                                      L"System.IsPinnedToNameSpaceTree", 0);
            if (status != ERROR_SUCCESS) {
                error = FormatWindowsError(status);
                return false;
            }
        }
        return true;
    }

    std::wstring icon = L"C:\\Program Files\\Google\\Drive File Stream\\drive_fs.ico";
    if (GetFileAttributesW(icon.c_str()) == INVALID_FILE_ATTRIBUTES) {
        icon = L"%SystemRoot%\\System32\\imageres.dll,-1043";
    }

    struct StringValue { std::wstring key; const wchar_t* name; std::wstring value; DWORD type; };
    const std::vector<StringValue> strings = {
        {classKey, nullptr, L"My Drive", REG_SZ},
        {classKey + L"\\DefaultIcon", nullptr, icon, REG_EXPAND_SZ},
        {classKey + L"\\InProcServer32", nullptr, L"%SystemRoot%\\System32\\shell32.dll", REG_EXPAND_SZ},
        {classKey + L"\\Instance", L"CLSID", kFolderInstanceClsid, REG_SZ},
        {classKey + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", path, REG_EXPAND_SZ}
    };
    for (const auto& item : strings) {
        const LSTATUS status = WriteRegistryString(HKEY_LOCAL_MACHINE, item.key, item.name, item.value, item.type);
        if (status != ERROR_SUCCESS) {
            error = FormatWindowsError(status);
            return false;
        }
    }
    struct DwordValue { std::wstring key; const wchar_t* name; DWORD value; };
    const std::vector<DwordValue> dwords = {
        {classKey, L"System.IsPinnedToNameSpaceTree", 1},
        {classKey, L"SortOrderIndex", kCloudSortOrder},
        {classKey + L"\\Instance\\InitPropertyBag", L"Attributes", 0x11},
        {classKey + L"\\ShellFolder", L"FolderValueFlags", 0x28},
        {classKey + L"\\ShellFolder", L"Attributes", 0xF080004D}
    };
    for (const auto& item : dwords) {
        const LSTATUS status = WriteRegistryDword(HKEY_LOCAL_MACHINE, item.key, item.name, item.value);
        if (status != ERROR_SUCCESS) {
            error = FormatWindowsError(status);
            return false;
        }
    }
    return true;
}

bool ConfigureMyDriveMachine(bool show, const std::wstring& path, std::wstring& error) {
    if (show && !PathIsDirectory(path)) {
        error = L"The selected My Drive folder does not exist.";
        return false;
    }
    if (!WriteMyDriveClass(L"Software\\Classes\\CLSID", show, path, error) ||
        !WriteMyDriveClass(L"Software\\Classes\\Wow6432Node\\CLSID", show, path, error)) {
        return false;
    }

    const std::wstring namespaceKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\" +
        std::wstring(kMyDriveClsid);
    if (show) {
        const LSTATUS status = WriteRegistryString(HKEY_LOCAL_MACHINE, namespaceKey, nullptr, L"My Drive");
        if (status != ERROR_SUCCESS) {
            error = FormatWindowsError(status);
            return false;
        }
    } else {
        const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, namespaceKey.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) {
            error = FormatWindowsError(status);
            return false;
        }
    }
    return true;
}

bool IsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID administrators = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                 &administrators)) {
        CheckTokenMembership(nullptr, administrators, &isAdmin);
        FreeSid(administrators);
    }
    return isAdmin == TRUE;
}

std::wstring GetCurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (!size || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t* sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText) || !sidText) {
        return {};
    }
    const std::wstring result = sidText;
    LocalFree(sidText);
    return result;
}

bool SetOneDriveVisibleForUser(const std::wstring& userSid, const std::wstring& clsid,
                               bool visible, std::wstring& error) {
    if (clsid.empty()) {
        return true;
    }
    const std::wstring userSoftwareRoot = userSid + L"\\Software\\";
    const std::wstring classKey = userSoftwareRoot + L"Classes\\CLSID\\" + clsid;
    if (!RegistryKeyExists(HKEY_USERS, classKey)) {
        error = L"The current user cannot change this PC's OneDrive entry.";
        return false;
    }
    const LSTATUS status = WriteRegistryDword(HKEY_USERS, classKey,
                                              L"System.IsPinnedToNameSpaceTree", visible ? 1 : 0);
    if (status != ERROR_SUCCESS) {
        error = FormatWindowsError(status);
        return false;
    }
    const std::wstring wowKey = userSoftwareRoot + L"Classes\\Wow6432Node\\CLSID\\" + clsid;
    if (RegistryKeyExists(HKEY_USERS, wowKey)) {
        const LSTATUS wowStatus = WriteRegistryDword(
            HKEY_USERS, wowKey, L"System.IsPinnedToNameSpaceTree", visible ? 1 : 0);
        if (wowStatus != ERROR_SUCCESS) {
            error = FormatWindowsError(wowStatus);
            return false;
        }
    }

    const std::wstring namespaceKey = userSid + L"\\" + OneDriveNamespaceKey(clsid);
    if (visible) {
        std::wstring label;
        ReadRegistryString(HKEY_USERS, classKey, nullptr, label);
        if (label.empty()) {
            label = L"OneDrive";
        }
        const LSTATUS namespaceStatus = WriteRegistryString(
            HKEY_USERS, namespaceKey, nullptr, label);
        if (namespaceStatus != ERROR_SUCCESS) {
            error = FormatWindowsError(namespaceStatus);
            return false;
        }
    } else {
        const LSTATUS namespaceStatus = RegDeleteTreeW(HKEY_USERS, namespaceKey.c_str());
        if (namespaceStatus != ERROR_SUCCESS &&
            namespaceStatus != ERROR_FILE_NOT_FOUND &&
            namespaceStatus != ERROR_PATH_NOT_FOUND) {
            error = FormatWindowsError(namespaceStatus);
            return false;
        }
    }
    return true;
}

bool SetGoogleDriveVisibleForUser(const std::wstring& userSid, wchar_t letter, bool visible,
                                  std::wstring& error) {
    if (!letter) {
        return true;
    }
    const std::wstring policyKey = userSid +
        L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    DWORD current = 0;
    ReadRegistryDword(HKEY_USERS, policyKey, L"NoDrives", current);
    const DWORD updated = cloudnav::SetDriveVisible(current, letter, visible);
    LSTATUS status = updated == 0
        ? DeleteRegistryValue(HKEY_USERS, policyKey, L"NoDrives")
        : WriteRegistryDword(HKEY_USERS, policyKey, L"NoDrives", updated);
    if (status != ERROR_SUCCESS) {
        error = FormatWindowsError(status);
        return false;
    }

    const std::wstring machinePolicyKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    DWORD machineCurrent = 0;
    ReadRegistryDword(HKEY_LOCAL_MACHINE, machinePolicyKey, L"NoDrives", machineCurrent);
    const DWORD machineUpdated = cloudnav::SetDriveVisible(machineCurrent, letter, visible);
    status = machineUpdated == 0
        ? DeleteRegistryValue(HKEY_LOCAL_MACHINE, machinePolicyKey, L"NoDrives")
        : WriteRegistryDword(HKEY_LOCAL_MACHINE, machinePolicyKey, L"NoDrives", machineUpdated);
    if (status != ERROR_SUCCESS) {
        error = FormatWindowsError(status);
        return false;
    }
    return true;
}

bool ConfigureAllElevated(bool showMyDrive, const std::wstring& myDrivePath,
                          bool showOneDrive, const std::wstring& oneDriveClsid,
                          bool showGoogleDrive, wchar_t googleDriveLetter,
                          const std::wstring& userSid, std::wstring& error) {
    if (!ConfigureMyDriveMachine(showMyDrive, myDrivePath, error)) {
        return false;
    }
    if (!myDrivePath.empty()) {
        WriteRegistryString(HKEY_USERS, userSid + L"\\Software\\CloudNav", L"MyDrivePath", myDrivePath);
    }
    const std::wstring hideDesktopKey = userSid +
        L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel";
    WriteRegistryDword(HKEY_USERS, hideDesktopKey, kMyDriveClsid, 1);
    return SetOneDriveVisibleForUser(userSid, oneDriveClsid, showOneDrive, error) &&
           SetGoogleDriveVisibleForUser(userSid, googleDriveLetter, showGoogleDrive, error);
}

bool RunElevatedApply(bool showMyDrive, const std::wstring& myDrivePath,
                      bool showOneDrive, const std::wstring& oneDriveClsid,
                      bool showGoogleDrive, wchar_t googleDriveLetter,
                      std::wstring& error) {
    const std::wstring userSid = GetCurrentUserSid();
    if (userSid.empty()) {
        error = L"Unable to identify the current Windows user.";
        return false;
    }
    if (IsAdministrator()) {
        return ConfigureAllElevated(showMyDrive, myDrivePath, showOneDrive, oneDriveClsid,
                                    showGoogleDrive, googleDriveLetter, userSid, error);
    }

    const std::wstring executable = GetModulePath();
    std::wstring parameters = L"--elevated-apply ";
    parameters += showMyDrive ? L"1 " : L"0 ";
    parameters += cloudnav::QuoteArgument(myDrivePath) + L" ";
    parameters += showOneDrive ? L"1 " : L"0 ";
    parameters += cloudnav::QuoteArgument(oneDriveClsid) + L" ";
    parameters += showGoogleDrive ? L"1 " : L"0 ";
    parameters += std::to_wstring(static_cast<unsigned int>(googleDriveLetter)) + L" ";
    parameters += cloudnav::QuoteArgument(userSid);

    SHELLEXECUTEINFOW execute = {sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = g_window;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
            ? L"Administrator approval cancelled. Nothing was changed."
            : FormatWindowsError(code);
        return false;
    }
    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    if (exitCode != ERROR_SUCCESS) {
        error = L"Configuration failed (code " + std::to_wstring(exitCode) + L").";
        return false;
    }
    return true;
}

void NotifyShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Policy"), SMTO_ABORTIFHUNG, 2000, nullptr);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"ShellState"), SMTO_ABORTIFHUNG, 2000, nullptr);
}

bool RestartExplorer() {
    DWORD currentSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSession);
    std::vector<HANDLE> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = {sizeof(entry)};
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0) {
                    continue;
                }
                DWORD session = 0;
                if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != currentSession) {
                    continue;
                }
                HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
                if (process && TerminateProcess(process, 0)) {
                    processes.push_back(process);
                } else if (process) {
                    CloseHandle(process);
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    for (HANDLE process : processes) {
        WaitForSingleObject(process, 2500);
        CloseHandle(process);
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (FindWindowW(L"Shell_TrayWnd", nullptr)) {
            return true;
        }
        Sleep(100);
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr,
                                                   SW_SHOWNORMAL)) > 32;
}

void ApplySelections() {
    bool showMyDrive = Button_GetCheck(g_myDrive) == BST_CHECKED;
    const bool showOneDrive = Button_GetCheck(g_oneDrive) == BST_CHECKED;
    const bool showGoogleDrive = Button_GetCheck(g_googleDrive) == BST_CHECKED;

    if (showMyDrive && g_chosenMyDrivePath.empty()) {
        g_chosenMyDrivePath = PickFolder(g_window);
        if (g_chosenMyDrivePath.empty()) {
            ShowStatus(L"Choose the My Drive folder first.", true);
            return;
        }
        SetWindowTextW(g_myDriveDetail, g_chosenMyDrivePath.c_str());
    }

    if (!IsWindowEnabled(g_apply)) return;
    if (g_demoMode) {
        g_state.myDrivePath = g_chosenMyDrivePath;
        g_state.myDriveVisible = showMyDrive;
        g_state.oneDrive.visible = showOneDrive;
        g_state.googleDriveVisible = showGoogleDrive;
        UpdateVisibilityPending();
        ShowStatus(L"Test mode: visibility changes simulated successfully.");
        return;
    }

    EnableWindow(GetDlgItem(g_window, IDC_APPLY), FALSE);
    ShowStatus(L"Applying changes…");
    std::wstring error;

    if (!RunElevatedApply(showMyDrive, g_chosenMyDrivePath,
                          showOneDrive,
                          g_state.oneDrive.detected ? g_state.oneDrive.clsid : L"",
                          showGoogleDrive, g_state.googleDriveLetter, error)) {
        EnableWindow(GetDlgItem(g_window, IDC_APPLY), TRUE);
        ShowStatus(error, true);
        return;
    }

    NotifyShell();
    const bool restarted = RestartExplorer();
    g_state = DetectState();
    UpdateControlsFromState();
    UpdateVisibilityPending();
    ShowStatus(restarted
        ? L"Applied. File Explorer was restarted."
        : L"Applied. Reopen File Explorer to see the result.", !restarted);
}

void RefreshState() {
    if (!g_demoMode) {
        g_state = DetectState();
    }
    UpdateControlsFromState();
    ShowStatus(L"Status refreshed from this PC.");
}

int ScaleDip(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

void MoveControl(HWND control, int x, int y, int width, int height, UINT dpi) {
    if (control) {
        MoveWindow(control, ScaleDip(x, dpi), ScaleDip(y, dpi),
                   ScaleDip(width, dpi), ScaleDip(height, dpi), TRUE);
    }
}

HFONT CreateUiFont(int pointSize, int weight, UINT dpi);
void RecreateUiFonts(UINT dpi);
void ResizeMainWindowForDpi(HWND window, UINT dpi, const RECT* suggested);

HWND CreateLabel(HWND parent, const wchar_t* text, HFONT font) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, parent, nullptr, g_instance, nullptr);
    SetControlFont(control, font);
    return control;
}

HWND CreateCheckbox(HWND parent, int id, const wchar_t* text) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   0, 0, 0, 0, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SetControlFont(control, g_bodyBoldFont);
    return control;
}

void UpdateVisibilityPending() {
    if (!g_apply) return;
    const bool myDrive = Button_GetCheck(g_myDrive) == BST_CHECKED;
    int changes = myDrive != g_state.myDriveVisible ||
        (myDrive && !cloudnav::PathEquals(g_chosenMyDrivePath, g_state.myDrivePath)) ? 1 : 0;
    if ((Button_GetCheck(g_oneDrive) == BST_CHECKED) != g_state.oneDrive.visible) ++changes;
    if ((Button_GetCheck(g_googleDrive) == BST_CHECKED) != g_state.googleDriveVisible) ++changes;
    EnableWindow(g_apply, changes > 0);
    const std::wstring summary = changes
        ? std::to_wstring(changes) + L" visibility change(s). File Explorer will restart."
        : L"Visibility is up to date. Select an entry to show it.";
    SetWindowTextW(g_explanation, summary.c_str());
}

void LayoutMainControls(UINT dpi) {
    MoveControl(g_title, 28, 16, 684, 32, dpi);
    MoveControl(g_subtitle, 28, 50, 684, 24, dpi);
    MoveControl(g_sectionTitle, 28, 86, 684, 22, dpi);
    MoveControl(g_myDrive, 56, 112, 540, 25, dpi);
    MoveControl(g_myDriveDetail, 78, 138, 520, 20, dpi);
    MoveControl(g_browse, 614, 112, 98, 32, dpi);
    MoveControl(g_oneDrive, 56, 160, 656, 25, dpi);
    MoveControl(g_oneDriveDetail, 78, 186, 634, 20, dpi);
    MoveControl(g_googleDrive, 56, 208, 656, 25, dpi);
    MoveControl(g_googleDriveDetail, 78, 234, 634, 20, dpi);
    for (int i = 0; i < 3; ++i) MoveControl(g_providerIcons[i], 28, 114 + i * 48, 22, 22, dpi);
    MoveControl(g_explanation, 28, 268, 476, 34, dpi);
    MoveControl(g_apply, 520, 264, 192, 34, dpi);
    MoveControl(g_foldersSection, 28, 314, 350, 22, dpi);
    MoveControl(g_personalFoldersDetail, 28, 338, 400, 18, dpi);
    MoveControl(g_personalFolders, 456, 318, 256, 34, dpi);
    MoveControl(g_migrationSection, 28, 370, 400, 22, dpi);
    MoveControl(g_migrateCloudDetail, 28, 394, 410, 26, dpi);
    MoveControl(g_migrateCloud, 456, 374, 256, 34, dpi);
    MoveControl(g_oneDriveSection, 28, 448, 324, 22, dpi);
    MoveControl(g_startupDetail, 28, 476, 324, 24, dpi);
    MoveControl(g_oneDriveSafety, 28, 504, 324, 56, dpi);
    MoveControl(g_installOneDrive, 28, 568, 324, 32, dpi);
    MoveControl(g_uninstallOneDrive, 28, 568, 324, 32, dpi);
    MoveControl(g_disableOneDriveStartup, 28, 606, 324, 30, dpi);
    MoveControl(g_googleSection, 388, 448, 324, 22, dpi);
    MoveControl(g_googleClientDetail, 388, 476, 324, 24, dpi);
    MoveControl(g_googleSafety, 388, 504, 324, 56, dpi);
    MoveControl(g_installGoogle, 388, 568, 324, 32, dpi);
    MoveControl(g_uninstallGoogle, 388, 568, 324, 32, dpi);
    MoveControl(g_status, 28, 652, 570, 34, dpi);
    MoveControl(g_refresh, 614, 648, 98, 32, dpi);
}

void CreateInterface(HWND window) {
    g_title = CreateLabel(window, L"CloudNav", g_titleFont);
    g_subtitle = CreateLabel(window,
        L"File Explorer visibility, personal folders, and cloud synchronization.", g_bodyFont);
    g_sectionTitle = CreateLabel(window, L"File Explorer navigation pane", g_bodyBoldFont);

    g_myDrive = CreateCheckbox(window, IDC_MY_DRIVE, L"Google Drive — My Drive folder");
    g_myDriveDetail = CreateLabel(window, L"", g_smallFont);
    g_browse = CreateWindowExW(0, L"BUTTON", L"Browse…",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               0, 0, 0, 0, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE)), g_instance, nullptr);
    SetControlFont(g_browse, g_bodyFont);

    g_oneDrive = CreateCheckbox(window, IDC_ONEDRIVE, L"OneDrive");
    g_oneDriveDetail = CreateLabel(window, L"", g_smallFont);
    g_oneDriveSafety = CreateLabel(window, L"", g_smallFont);
    g_disableOneDriveStartup = CreateWindowExW(
        0, L"BUTTON", L"Disable startup at sign-in",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DISABLE_ONEDRIVE_STARTUP)),
        g_instance, nullptr);
    SetControlFont(g_disableOneDriveStartup, g_bodyFont);
    g_uninstallOneDrive = CreateWindowExW(
        0, L"BUTTON", L"Uninstall OneDrive",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UNINSTALL_ONEDRIVE)),
        g_instance, nullptr);
    SetControlFont(g_uninstallOneDrive, g_bodyFont);

    g_googleDrive = CreateCheckbox(window, IDC_GOOGLE_DRIVE, L"Google Drive");
    g_googleDriveDetail = CreateLabel(window, L"", g_smallFont);

    g_personalFolders = CreateWindowExW(
        0, L"BUTTON", L"Personal folders…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PERSONAL_FOLDERS)),
        g_instance, nullptr);
    SetControlFont(g_personalFolders, g_bodyBoldFont);
    g_personalFoldersDetail = CreateLabel(
        window, L"Choose where Windows stores your six personal folders.", g_smallFont);

    g_migrateCloud = CreateWindowExW(
        0, L"BUTTON", L"Compare accounts…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MIGRATE_CLOUD)),
        g_instance, nullptr);
    SetControlFont(g_migrateCloud, g_bodyBoldFont);
    g_migrateCloudDetail = CreateLabel(
        window, L"Compare both accounts and choose a transfer direction.", g_smallFont);

    g_explanation = CreateLabel(
        window,
        L"Visibility is up to date.",
        g_smallFont);
    g_status = CreateLabel(window, L"Ready.", g_bodyFont);

    g_refresh = CreateWindowExW(0, L"BUTTON", L"Refresh",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH)), g_instance, nullptr);
    SetControlFont(g_refresh, g_bodyFont);
    g_apply = CreateWindowExW(0, L"BUTTON", L"Apply visibility",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 0, 0, window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_APPLY)), g_instance, nullptr);
    SetControlFont(g_apply, g_bodyBoldFont);
    g_foldersSection = CreateLabel(window, L"Personal folders", g_bodyBoldFont);
    g_migrationSection = CreateLabel(window, L"Cloud sync", g_bodyBoldFont);
    g_oneDriveSection = CreateLabel(window, L"OneDrive client settings", g_bodyBoldFont);
    g_startupDetail = CreateLabel(window, L"", g_smallFont);
    SetWindowLongPtrW(g_startupDetail, GWLP_ID, 1017);
    g_googleSection = CreateLabel(window, L"Google Drive client settings", g_bodyBoldFont);
    g_googleClientDetail = CreateLabel(window, L"", g_smallFont);
    SetWindowLongPtrW(g_googleClientDetail, GWLP_ID, 1018);
    g_googleSafety = CreateLabel(window, L"", g_smallFont);
    SetWindowLongPtrW(g_googleSafety, GWLP_ID, 1019);
    SetWindowLongPtrW(g_status, GWLP_ID, 1020);
    const auto clientButton = [&](int id, const wchar_t* text) {
        HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
        SetControlFont(button, g_bodyFont);
        return button;
    };
    g_installOneDrive = clientButton(IDC_INSTALL_ONEDRIVE, L"Install OneDrive");
    g_installGoogle = clientButton(IDC_INSTALL_GOOGLE, L"Install Google Drive");
    g_uninstallGoogle = clientButton(IDC_UNINSTALL_GOOGLE, L"Uninstall Google Drive");
    g_providerImages.Load(g_instance);
    for (int i = 0; i < 3; ++i) {
        g_providerIcons[i] = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1011 + i)), g_instance, nullptr);
    }
    LayoutMainControls(g_uiDpi);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLIENT_DOWNLOAD: {
        std::unique_ptr<ClientDownloadResult> result(reinterpret_cast<ClientDownloadResult*>(lParam));
        if (g_clientDownloadThread.joinable()) g_clientDownloadThread.join();
        g_clientDownloading = false;
        if (g_closeAfterDownload || g_cancelClientDownload) {
            cloudnav::RemoveClientDownload(result->path);
            UpdateControlsFromState();
            ShowStatus(L"Download cancelled. Nothing was launched.");
            if (g_closeAfterDownload) PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        UpdateControlsFromState();
        if (!result->error.empty() || result->path.empty()) { ShowStatus(result->error.empty() ? L"Installer download failed." : result->error, true); return 0; }
        if (LaunchClientProgram(g_activeClient, result->path, L"", false)) g_downloadedInstaller = result->path;
        else cloudnav::RemoveClientDownload(result->path);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kClientProcessTimer && g_clientProcess && WaitForSingleObject(g_clientProcess, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0; GetExitCodeProcess(g_clientProcess, &exitCode);
            CloseHandle(g_clientProcess); g_clientProcess = nullptr;
            KillTimer(window, kClientProcessTimer);
            cloudnav::RemoveClientDownload(g_downloadedInstaller); g_downloadedInstaller.clear();
            g_state = DetectState(); UpdateControlsFromState();
            const auto& client = g_activeClient == cloudnav::CloudClient::OneDrive ? g_state.oneDriveClient : g_state.googleClient;
            ShowStatus(L"Setup exited (code " + std::to_wstring(exitCode) + L"). " + cloudnav::ClientName(g_activeClient) +
                (!client.detectionComplete ? L" status unavailable. Refresh after setup finishes." : client.installed ?
                    L" is detected as installed. Refresh if setup is still open." : L" is not detected as installed. Refresh if setup is still open."), exitCode != 0);
            return 0;
        }
        break;
    case WM_CREATE:
        g_uiDpi = GetDpiForWindow(window);
        RecreateUiFonts(g_uiDpi);
        CreateInterface(window);
        UpdateControlsFromState();
        ShowStatus(g_demoMode ? L"Test mode: no system changes." : L"Ready.");
        if (g_demoMigration) {
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_MIGRATE_CLOUD, BN_CLICKED), 0);
        }
        return 0;
    case WM_COMMAND:
        if (ClientBusy() && LOWORD(wParam) != IDC_INSTALL_ONEDRIVE && LOWORD(wParam) != IDC_INSTALL_GOOGLE) return 0;
        switch (LOWORD(wParam)) {
        case IDC_INSTALL_ONEDRIVE: InstallClient(cloudnav::CloudClient::OneDrive); return 0;
        case IDC_INSTALL_GOOGLE: InstallClient(cloudnav::CloudClient::GoogleDrive); return 0;
        case IDC_UNINSTALL_GOOGLE: UninstallGoogleDrive(); return 0;
        case IDC_BROWSE: {
            const std::wstring selected = PickFolder(window);
            if (!selected.empty()) {
                g_chosenMyDrivePath = selected;
                SetWindowTextW(g_myDriveDetail, selected.c_str());
                Button_SetCheck(g_myDrive, BST_CHECKED);
                UpdateVisibilityPending();
                ShowStatus(L"Folder selected. Apply visibility to save this entry.");
            }
            return 0;
        }
        case IDC_MY_DRIVE:
        case IDC_ONEDRIVE:
        case IDC_GOOGLE_DRIVE:
            UpdateVisibilityPending();
            return 0;
        case IDC_REFRESH:
            RefreshState();
            return 0;
        case IDC_DISABLE_ONEDRIVE_STARTUP:
            DisableOneDriveStartup();
            return 0;
        case IDC_UNINSTALL_ONEDRIVE:
            UninstallOneDrive();
            return 0;
        case IDC_PERSONAL_FOLDERS: {
            cloudnav::FolderProviders providers;
            providers.oneDriveLabel = g_state.oneDrive.label;
            providers.oneDriveRoot = g_state.oneDrive.path;
            providers.googleDriveRoot = g_state.myDrivePath;
            providers.rootMirrorTaskDetected = DetectRootMirrorTask() || g_demoMode;
            providers.preloadDemoPlan = g_demoPlan;
            providers.simulateRepointFailure = g_demoFailure;
            providers.simulateOneDriveBackupActive = g_demoPlan && !g_demoFailure;
            const bool changed = cloudnav::ShowFolderManagerDialog(
                window, g_instance, providers, g_demoMode);
            if (!g_demoMode) {
                g_state = DetectState();
                UpdateControlsFromState();
            }
            ShowStatus(changed
                ? L"Personal folder locations updated."
                : L"Folder manager closed.");
            return 0;
        }
        case IDC_MIGRATE_CLOUD: {
            const cloudnav::MigrationResult result = cloudnav::ShowMigrationDialog(
                window, g_instance, g_demoMode, g_demoMigrationResult);
            if (result == cloudnav::MigrationResult::ConfigureFolders) {
                cloudnav::FolderProviders providers;
                providers.oneDriveLabel = g_state.oneDrive.label;
                providers.oneDriveRoot = g_state.oneDrive.path;
                providers.googleDriveRoot = g_state.myDrivePath;
                providers.oneDriveToGoogleVerified = false;
                providers.preloadDemoPlan = g_demoMode;
                cloudnav::ShowFolderManagerDialog(window, g_instance, providers, g_demoMode);
                if (!g_demoMode) { g_state = DetectState(); UpdateControlsFromState(); }
            }
            ShowStatus(result == cloudnav::MigrationResult::ConfigureFolders
                ? L"Copy complete; folder manager opened."
                : L"Cloud sync window closed.");
            return 0;
        }
        case IDC_APPLY:
            ApplySelections();
            return 0;
        default:
            break;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && item->CtlID >= 1011 && item->CtlID <= 1013) {
            FillRect(item->hDC, &item->rcItem, g_backgroundBrush);
            g_providerImages.Draw(item->hDC, item->rcItem, item->CtlID == 1012);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        if (control == g_status) {
            SetTextColor(dc, g_statusIsError ? RGB(176, 32, 37) : RGB(20, 111, 78));
        } else if (control == g_oneDriveSafety && g_oneDriveBlocked) {
            SetTextColor(dc, RGB(146, 64, 14));
        } else if (control == g_myDriveDetail || control == g_oneDriveDetail ||
                   control == g_oneDriveSafety ||
                   control == g_googleDriveDetail || control == g_migrateCloudDetail) {
            SetTextColor(dc, RGB(91, 101, 116));
        } else {
            SetTextColor(dc, RGB(30, 41, 59));
        }
        return reinterpret_cast<LRESULT>(g_backgroundBrush);
    }
    case WM_ERASEBKGND: {
        RECT rect = {};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, g_backgroundBrush);
        return 1;
    }
    case WM_DPICHANGED: {
        g_uiDpi = HIWORD(wParam);
        RecreateUiFonts(g_uiDpi);
        LayoutMainControls(g_uiDpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        ResizeMainWindowForDpi(window, g_uiDpi, suggested);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_EXITSIZEMOVE:
        SaveWindowPosition(window);
        return 0;
    case WM_CLOSE:
        if (g_clientDownloading) {
            g_closeAfterDownload = true;
            g_cancelClientDownload = true;
            ShowStatus(L"Cancelling download before closing…");
            return 0;
        }
        SaveWindowPosition(window);
        DestroyWindow(window);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        HPEN linePen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
        HGDIOBJ oldPen = SelectObject(dc, linePen);
        for (int y : {304, 360, 432}) {
            MoveToEx(dc, ScaleDip(28, g_uiDpi), ScaleDip(y, g_uiDpi), nullptr);
            LineTo(dc, ScaleDip(712, g_uiDpi), ScaleDip(y, g_uiDpi));
        }
        SelectObject(dc, oldPen);
        DeleteObject(linePen);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (g_clientProcess) { CloseHandle(g_clientProcess); g_clientProcess = nullptr; }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HFONT CreateUiFont(int pointSize, int weight, UINT dpi) {
    return CreateFontW(-MulDiv(pointSize, static_cast<int>(dpi), 72),
                       0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void RecreateUiFonts(UINT dpi) {
    HFONT oldTitle = g_titleFont;
    HFONT oldBody = g_bodyFont;
    HFONT oldBodyBold = g_bodyBoldFont;
    HFONT oldSmall = g_smallFont;

    g_titleFont = CreateUiFont(21, FW_SEMIBOLD, dpi);
    g_bodyFont = CreateUiFont(10, FW_NORMAL, dpi);
    g_bodyBoldFont = CreateUiFont(10, FW_SEMIBOLD, dpi);
    g_smallFont = CreateUiFont(9, FW_NORMAL, dpi);

    SetControlFont(g_title, g_titleFont);
    SetControlFont(g_subtitle, g_bodyFont);
    SetControlFont(g_sectionTitle, g_bodyBoldFont);
    SetControlFont(g_foldersSection, g_bodyBoldFont);
    SetControlFont(g_migrationSection, g_bodyBoldFont);
    SetControlFont(g_oneDriveSection, g_bodyBoldFont);
    SetControlFont(g_startupDetail, g_smallFont);
    SetControlFont(g_googleSection, g_bodyBoldFont);
    SetControlFont(g_googleClientDetail, g_smallFont);
    SetControlFont(g_googleSafety, g_smallFont);
    for (HWND button : {g_installOneDrive, g_installGoogle, g_uninstallGoogle}) SetControlFont(button, g_bodyFont);
    SetControlFont(g_migrateCloud, g_bodyBoldFont);
    SetControlFont(g_migrateCloudDetail, g_smallFont);
    SetControlFont(g_myDrive, g_bodyBoldFont);
    SetControlFont(g_myDriveDetail, g_smallFont);
    SetControlFont(g_browse, g_bodyFont);
    SetControlFont(g_oneDrive, g_bodyBoldFont);
    SetControlFont(g_oneDriveDetail, g_smallFont);
    SetControlFont(g_oneDriveSafety, g_smallFont);
    SetControlFont(g_disableOneDriveStartup, g_bodyFont);
    SetControlFont(g_uninstallOneDrive, g_bodyFont);
    SetControlFont(g_googleDrive, g_bodyBoldFont);
    SetControlFont(g_googleDriveDetail, g_smallFont);
    SetControlFont(g_personalFolders, g_bodyBoldFont);
    SetControlFont(g_personalFoldersDetail, g_smallFont);
    SetControlFont(g_explanation, g_smallFont);
    SetControlFont(g_status, g_bodyFont);
    SetControlFont(g_refresh, g_bodyFont);
    SetControlFont(g_apply, g_bodyBoldFont);

    if (oldTitle) DeleteObject(oldTitle);
    if (oldBody) DeleteObject(oldBody);
    if (oldBodyBold) DeleteObject(oldBodyBold);
    if (oldSmall) DeleteObject(oldSmall);
}

void ResizeMainWindowForDpi(HWND window, UINT dpi, const RECT* suggested) {
    RECT dimensions = {
        0, 0,
        ScaleDip(kMainClientWidthDip, dpi),
        ScaleDip(kMainClientHeightDip, dpi)
    };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&dimensions, style, FALSE, extendedStyle, dpi);

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    int x = 0;
    int y = 0;
    if (suggested) {
        x = suggested->left;
        y = suggested->top;
    } else {
        flags |= SWP_NOMOVE;
    }
    SetWindowPos(window, nullptr, x, y,
                 dimensions.right - dimensions.left,
                 dimensions.bottom - dimensions.top, flags);
}

int RunElevatedHelper(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 9 || !EqualsInsensitive(arguments[1], L"--elevated-apply")) {
        return ERROR_INVALID_PARAMETER;
    }
    if (!IsAdministrator()) {
        return ERROR_ACCESS_DENIED;
    }
    const bool showMyDrive = wcstol(arguments[2], nullptr, 10) != 0;
    const std::wstring myDrivePath = arguments[3];
    const bool showOneDrive = wcstol(arguments[4], nullptr, 10) != 0;
    const std::wstring oneDriveClsid = arguments[5];
    const bool showGoogleDrive = wcstol(arguments[6], nullptr, 10) != 0;
    const wchar_t googleDriveLetter = static_cast<wchar_t>(wcstoul(arguments[7], nullptr, 10));
    const std::wstring userSid = arguments[8];
    std::wstring error;
    return ConfigureAllElevated(showMyDrive, myDrivePath, showOneDrive, oneDriveClsid,
                                showGoogleDrive, googleDriveLetter, userSid, error)
        ? ERROR_SUCCESS
        : ERROR_WRITE_FAULT;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    SetThreadUILanguage(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments && argumentCount == 3 &&
        EqualsInsensitive(arguments[1], L"--self-test-known-folders")) {
        const int result = cloudnav::RunFolderRedirectionSelfTest(arguments[2]);
        LocalFree(arguments);
        CoUninitialize();
        return result;
    }
    if (arguments && argumentCount == 3 &&
        EqualsInsensitive(arguments[1], L"--self-test-embedded-rclone")) {
        const int result = cloudnav::RunEmbeddedRcloneSelfTest(instance, arguments[2]);
        LocalFree(arguments);
        CoUninitialize();
        return result;
    }
    if (arguments && argumentCount == 2 &&
        EqualsInsensitive(arguments[1], L"--elevated-disable-onedrive-backup")) {
        const int result = cloudnav::RunDisableOneDriveFolderBackupHelper();
        LocalFree(arguments);
        CoUninitialize();
        return result;
    }
    if (arguments && argumentCount > 1 && EqualsInsensitive(arguments[1], L"--elevated-apply")) {
        const int result = RunElevatedHelper(argumentCount, arguments);
        LocalFree(arguments);
        CoUninitialize();
        return result;
    }
    for (int index = 1; arguments && index < argumentCount; ++index) {
        if (EqualsInsensitive(arguments[index], L"--demo")) {
            g_demoMode = true;
        } else if (EqualsInsensitive(arguments[index], L"--demo-plan")) {
            g_demoMode = true;
            g_demoPlan = true;
        } else if (EqualsInsensitive(arguments[index], L"--demo-repoint-failure")) {
            g_demoMode = true;
            g_demoPlan = true;
            g_demoFailure = true;
        } else if (EqualsInsensitive(arguments[index], L"--demo-safe-onedrive")) {
            g_demoMode = true;
            g_demoSafeOneDriveActions = true;
        } else if (EqualsInsensitive(arguments[index], L"--demo-clients-missing") || EqualsInsensitive(arguments[index], L"--demo-client-failure")) {
            g_demoMode = true; g_demoClientsMissing = true; g_demoSafeOneDriveActions = true;
            g_demoClientFailure = EqualsInsensitive(arguments[index], L"--demo-client-failure");
        } else if (EqualsInsensitive(arguments[index], L"--demo-clients-unknown-roots")) {
            g_demoMode = true; g_demoSafeOneDriveActions = true; g_demoClientUnknownRoots = true;
        } else if (EqualsInsensitive(arguments[index], L"--demo-migration")) {
            g_demoMode = true;
            g_demoMigration = true;
            if (index + 1 < argumentCount) g_demoMigrationResult = arguments[++index];
        }
    }
    if (arguments) {
        LocalFree(arguments);
    }

    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    g_backgroundBrush = CreateSolidBrush(RGB(248, 250, 252));
    g_state = DetectState();

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CLOUDNAV), IMAGE_ICON, 32, 32, LR_SHARED));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = g_backgroundBrush;
    windowClass.lpszClassName = L"CloudNavWindow";
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CLOUDNAV), IMAGE_ICON, 16, 16, LR_SHARED));
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        return 1;
    }

    RECT dimensions = {0, 0, kMainClientWidthDip, kMainClientHeightDip};
    AdjustWindowRectEx(&dimensions, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);
    g_window = CreateWindowExW(
        0, windowClass.lpszClassName,
        g_demoMode ? L"CloudNav — visual test" : L"CloudNav",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, dimensions.right - dimensions.left, dimensions.bottom - dimensions.top,
        nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        CoUninitialize();
        return 1;
    }
    const UINT actualDpi = GetDpiForWindow(g_window);
    if (actualDpi != g_uiDpi) {
        g_uiDpi = actualDpi;
        RecreateUiFonts(g_uiDpi);
        LayoutMainControls(g_uiDpi);
    }
    ResizeMainWindowForDpi(g_window, g_uiDpi, nullptr);
    RestoreWindowPosition(g_window);
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    g_cancelClientDownload = true;
    if (g_clientDownloadThread.joinable()) g_clientDownloadThread.join();

    DeleteObject(g_titleFont);
    DeleteObject(g_bodyFont);
    DeleteObject(g_bodyBoldFont);
    DeleteObject(g_smallFont);
    DeleteObject(g_backgroundBrush);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
