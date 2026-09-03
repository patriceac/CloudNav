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

#include "logic.h"
#include "folder_manager.h"
#include "migration.h"
#include "resource.h"

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
constexpr int kMainClientWidthDip = 680;
constexpr int kMainClientHeightDip = 568;

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

std::wstring FormatWindowsError(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length && buffer ? std::wstring(buffer, length) : L"Erreur Windows " + std::to_wstring(code);
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

PersonalFolderUsage DetectPersonalFoldersInOneDrive(const std::wstring& oneDriveRoot) {
    struct PersonalFolderSpec {
        const KNOWNFOLDERID* id;
        const wchar_t* label;
    };
    const std::array<PersonalFolderSpec, 6> folders = {{
        {&FOLDERID_Desktop, L"Bureau"},
        {&FOLDERID_Documents, L"Documents"},
        {&FOLDERID_Pictures, L"Images"},
        {&FOLDERID_Downloads, L"Téléchargements"},
        {&FOLDERID_Music, L"Musique"},
        {&FOLDERID_Videos, L"Vidéos"}
    }};
    PersonalFolderUsage result;
    if (oneDriveRoot.empty()) {
        result.complete = false;
        return result;
    }
    for (const PersonalFolderSpec& folder : folders) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*folder.id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path) {
            if (cloudnav::PathIsWithin(path, oneDriveRoot)) {
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
    state.oneDriveAutoStart = DetectOneDriveAutoStart();
    state.oneDriveUninstaller = DetectOneDriveUninstaller();
    const PersonalFolderUsage usage = DetectPersonalFoldersInOneDrive(state.oneDrive.path);
    state.oneDrivePersonalFolders = usage.names;
    state.personalFolderScanComplete = usage.complete;
}

AppState DetectState() {
    if (g_demoMode) {
        AppState demo;
        demo.myDrivePath = L"C:\\Users\\Example\\My Drive";
        demo.myDriveVisible = true;
        demo.myDriveSort = kCloudSortOrder;
        demo.oneDrive = {kOneDrivePersonalClsid, L"Compte personnel", L"C:\\Users\\Example\\OneDrive", true, true};
        demo.googleDriveLetter = L'G';
        demo.googleDriveVisible = false;
        demo.oneDriveAutoStart = true;
        demo.oneDriveUninstaller = L"C:\\Windows\\System32\\OneDriveSetup.exe";
        demo.personalFolderScanComplete = true;
        if (!g_demoSafeOneDriveActions) {
            demo.oneDrivePersonalFolders = {L"Documents", L"Images"};
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
        return L"Google Drive (non détecté)";
    }
    std::wstring label = L"Google Drive (";
    label.push_back(state.googleDriveLetter);
    label += L":)";
    return label;
}

std::wstring GoogleDriveDetail(const AppState& state) {
    if (!state.googleDriveLetter) {
        return L"Lance Google Drive pour que CloudNav retrouve sa lettre.";
    }
    return L"Masque seulement l’icône du lecteur ; les fichiers restent accessibles.";
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
            result += index + 1 == names.size() ? L" et " : L", ";
        }
        result += names[index];
    }
    return result;
}

void UpdateControlsFromState() {
    const std::wstring myDetail = g_state.myDrivePath.empty()
        ? L"Dossier non détecté : utilise « Choisir… »."
        : g_state.myDrivePath;
    SetWindowTextW(g_myDriveDetail, myDetail.c_str());
    Button_SetCheck(g_myDrive, g_state.myDriveVisible ? BST_CHECKED : BST_UNCHECKED);

    if (g_state.oneDrive.detected) {
        SetWindowTextW(g_oneDrive, g_state.oneDrive.label.c_str());
        std::wstring detail = g_state.oneDrive.path.empty()
            ? L"Compte OneDrive détecté."
            : g_state.oneDrive.path;
        detail += g_state.oneDriveAutoStart
            ? L"  •  démarrage automatique activé"
            : L"  •  démarrage automatique désactivé";
        SetWindowTextW(g_oneDriveDetail, detail.c_str());
        EnableWindow(g_oneDrive, TRUE);
        Button_SetCheck(g_oneDrive, g_state.oneDrive.visible ? BST_CHECKED : BST_UNCHECKED);
    } else {
        SetWindowTextW(g_oneDrive, L"OneDrive (non détecté)");
        SetWindowTextW(g_oneDriveDetail, L"Aucun compte OneDrive n’est enregistré sur ce PC.");
        EnableWindow(g_oneDrive, FALSE);
        Button_SetCheck(g_oneDrive, BST_UNCHECKED);
    }

    const bool anyPersonalFolderUsesOneDrive = !g_state.oneDrivePersonalFolders.empty();
    const bool canDetach = cloudnav::CanDetachOneDrive(
        g_state.oneDrive.detected, !g_state.oneDrive.path.empty(),
        g_state.personalFolderScanComplete, anyPersonalFolderUsesOneDrive);
    g_oneDriveBlocked = g_state.oneDrive.detected &&
        (anyPersonalFolderUsesOneDrive || !g_state.personalFolderScanComplete ||
         g_state.oneDrive.path.empty());
    if (!g_state.oneDrivePersonalFolders.empty()) {
        const std::wstring warning = L"⚠ OneDrive est encore utilisé par : " +
            JoinFolderNames(g_state.oneDrivePersonalFolders) +
            L". Déplace ces dossiers avant de désactiver ou désinstaller OneDrive.";
        SetWindowTextW(g_oneDriveSafety, warning.c_str());
    } else if (g_oneDriveBlocked) {
        SetWindowTextW(g_oneDriveSafety,
            L"⚠ Impossible de vérifier tous les dossiers personnels ; actions OneDrive bloquées.");
    } else if (g_state.oneDrive.detected) {
        SetWindowTextW(g_oneDriveSafety,
            L"Aucun dossier personnel ne dépend de OneDrive.");
    } else {
        SetWindowTextW(g_oneDriveSafety, L"");
    }
    EnableWindow(g_disableOneDriveStartup,
                 canDetach && g_state.oneDriveAutoStart ? TRUE : FALSE);
    EnableWindow(g_uninstallOneDrive,
                 canDetach && !g_state.oneDriveUninstaller.empty() ? TRUE : FALSE);
    InvalidateRect(g_oneDriveSafety, nullptr, TRUE);

    SetWindowTextW(g_googleDrive, GoogleDriveLabel(g_state).c_str());
    SetWindowTextW(g_googleDriveDetail, GoogleDriveDetail(g_state).c_str());
    EnableWindow(g_googleDrive, g_state.googleDriveLetter != 0);
    Button_SetCheck(g_googleDrive, g_state.googleDriveVisible ? BST_CHECKED : BST_UNCHECKED);
}

bool VerifyOneDriveActionGuard() {
    if (!g_demoMode) {
        const PersonalFolderUsage usage = DetectPersonalFoldersInOneDrive(g_state.oneDrive.path);
        g_state.oneDrivePersonalFolders = usage.names;
        g_state.personalFolderScanComplete = usage.complete;
    }
    UpdateControlsFromState();
    if (!cloudnav::CanDetachOneDrive(
            g_state.oneDrive.detected, !g_state.oneDrive.path.empty(),
            g_state.personalFolderScanComplete,
            !g_state.oneDrivePersonalFolders.empty())) {
        const std::wstring message = !g_state.oneDrivePersonalFolders.empty()
            ? L"Action bloquée : " + JoinFolderNames(g_state.oneDrivePersonalFolders) +
              L" pointe encore vers OneDrive. Déplace d’abord ce dossier dans « Dossiers personnels… »."
            : (g_state.oneDrive.detected
                ? L"CloudNav ne peut pas vérifier avec certitude tous les dossiers personnels. Action bloquée."
                : L"OneDrive n’est pas détecté sur ce PC.");
        MessageBoxW(g_window, message.c_str(), L"CloudNav — OneDrive",
                    MB_OK | MB_ICONWARNING);
        ShowStatus(L"Action OneDrive bloquée pour protéger les dossiers personnels.", true);
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
        ShowStatus(L"Mode test : démarrage désactivé ; OneDrive resterait actif.");
        return;
    }
    const LSTATUS status = DeleteRegistryValue(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"OneDrive");
    if (status != ERROR_SUCCESS) {
        ShowStatus(L"Impossible de désactiver le démarrage OneDrive : " +
                   FormatWindowsError(status), true);
        return;
    }
    g_state.oneDriveAutoStart = DetectOneDriveAutoStart();
    UpdateControlsFromState();
    ShowStatus(g_state.oneDriveAutoStart
        ? L"OneDrive est toujours configuré pour démarrer automatiquement."
        : L"Démarrage désactivé ; OneDrive reste actif pour cette session.",
        g_state.oneDriveAutoStart);
}

void UninstallOneDrive() {
    if (!VerifyOneDriveActionGuard()) {
        return;
    }
    if (g_state.oneDriveUninstaller.empty()) {
        ShowStatus(L"Programme de désinstallation OneDrive introuvable.", true);
        return;
    }
    const int confirmation = MessageBoxW(
        g_window,
        L"Désinstaller le client OneDrive de ce PC ?\n\n"
        L"CloudNav a vérifié que Bureau, Documents, Images, Téléchargements, Musique et Vidéos "
        L"ne pointent pas vers OneDrive. Les autres dossiers synchronisés ne sont pas vérifiés.\n\n"
        L"Avant de continuer, assure-toi que OneDrive indique « À jour ». Les fichiers stockés "
        L"dans le cloud ne seront pas supprimés ; ceux disponibles uniquement en ligne resteront "
        L"accessibles sur OneDrive.com.",
        L"CloudNav — désinstaller OneDrive",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (confirmation != IDYES) {
        ShowStatus(L"Désinstallation OneDrive annulée.");
        return;
    }
    if (g_demoMode) {
        ShowStatus(L"Mode test : désinstallation OneDrive simulée.");
        return;
    }
    SHELLEXECUTEINFOW execute = {sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = g_window;
    execute.lpVerb = L"runas";
    execute.lpFile = g_state.oneDriveUninstaller.c_str();
    execute.lpParameters = L"/uninstall";
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        ShowStatus(L"Impossible de lancer la désinstallation OneDrive : " +
                   FormatWindowsError(GetLastError()), true);
        return;
    }
    if (execute.hProcess) {
        CloseHandle(execute.hProcess);
    }
    ShowStatus(L"Désinstallation OneDrive lancée. Actualise ensuite l’état.");
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
    dialog->SetTitle(L"Choisir le dossier My Drive");
    dialog->SetOkButtonLabel(L"Utiliser ce dossier");
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
        error = L"Le dossier My Drive choisi n’existe pas.";
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
        error = L"L’entrée OneDrive de ce PC n’est pas modifiable pour l’utilisateur courant.";
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
        error = L"Impossible d’identifier l’utilisateur Windows courant.";
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
            ? L"Autorisation administrateur annulée. Rien n’a été modifié."
            : FormatWindowsError(code);
        return false;
    }
    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    if (exitCode != ERROR_SUCCESS) {
        error = L"La configuration a échoué (code " + std::to_wstring(exitCode) + L").";
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

    if (showMyDrive && g_state.myDrivePath.empty()) {
        g_state.myDrivePath = PickFolder(g_window);
        if (g_state.myDrivePath.empty()) {
            ShowStatus(L"Choisis d’abord le dossier My Drive.", true);
            return;
        }
        SetWindowTextW(g_myDriveDetail, g_state.myDrivePath.c_str());
    }

    if (g_demoMode) {
        g_state.myDriveVisible = showMyDrive;
        g_state.oneDrive.visible = showOneDrive;
        g_state.googleDriveVisible = showGoogleDrive;
        ShowStatus(L"Mode test : configuration simulée, interface validée.");
        return;
    }

    EnableWindow(GetDlgItem(g_window, IDC_APPLY), FALSE);
    ShowStatus(L"Application en cours…");
    std::wstring error;

    if (!RunElevatedApply(showMyDrive, g_state.myDrivePath,
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
    EnableWindow(GetDlgItem(g_window, IDC_APPLY), TRUE);
    ShowStatus(restarted
        ? L"Appliqué. L’Explorateur a été relancé."
        : L"Appliqué. Rouvre l’Explorateur pour voir le résultat.", !restarted);
}

void RefreshState() {
    if (!g_demoMode) {
        g_state = DetectState();
    }
    UpdateControlsFromState();
    ShowStatus(L"État relu depuis ce PC.");
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

void LayoutMainControls(UINT dpi) {
    MoveControl(g_title, 26, 20, 620, 36, dpi);
    MoveControl(g_subtitle, 28, 58, 624, 24, dpi);
    MoveControl(g_sectionTitle, 28, 96, 300, 18, dpi);

    MoveControl(g_myDrive, 32, 122, 430, 25, dpi);
    MoveControl(g_myDriveDetail, 55, 149, 480, 22, dpi);
    MoveControl(g_browse, 554, 120, 98, 32, dpi);

    MoveControl(g_oneDrive, 32, 190, 245, 25, dpi);
    MoveControl(g_disableOneDriveStartup, 285, 186, 229, 32, dpi);
    MoveControl(g_uninstallOneDrive, 522, 186, 130, 32, dpi);
    MoveControl(g_oneDriveDetail, 55, 219, 597, 20, dpi);
    MoveControl(g_oneDriveSafety, 55, 241, 597, 31, dpi);

    MoveControl(g_googleDrive, 32, 284, 430, 25, dpi);
    MoveControl(g_googleDriveDetail, 55, 311, 597, 22, dpi);

    MoveControl(g_personalFolders, 28, 352, 184, 32, dpi);
    MoveControl(g_personalFoldersDetail, 226, 357, 426, 22, dpi);
    MoveControl(g_migrateCloud, 28, 398, 236, 32, dpi);
    MoveControl(g_migrateCloudDetail, 278, 403, 374, 28, dpi);
    MoveControl(g_explanation, 28, 448, 624, 22, dpi);

    MoveControl(g_status, 28, 513, 403, 30, dpi);
    MoveControl(g_refresh, 446, 508, 96, 34, dpi);
    MoveControl(g_apply, 552, 508, 100, 34, dpi);
}

void CreateInterface(HWND window) {
    g_title = CreateLabel(window, L"CloudNav", g_titleFont);
    g_subtitle = CreateLabel(window,
        L"Choisis ce qui apparaît dans le volet gauche de l’Explorateur Windows.", g_bodyFont);
    g_sectionTitle = CreateLabel(window, L"ÉLÉMENTS À AFFICHER", g_smallFont);

    g_myDrive = CreateCheckbox(window, IDC_MY_DRIVE, L"My Drive");
    g_myDriveDetail = CreateLabel(window, L"", g_smallFont);
    g_browse = CreateWindowExW(0, L"BUTTON", L"Choisir…",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               0, 0, 0, 0, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE)), g_instance, nullptr);
    SetControlFont(g_browse, g_bodyFont);

    g_oneDrive = CreateCheckbox(window, IDC_ONEDRIVE, L"OneDrive");
    g_oneDriveDetail = CreateLabel(window, L"", g_smallFont);
    g_oneDriveSafety = CreateLabel(window, L"", g_smallFont);
    g_disableOneDriveStartup = CreateWindowExW(
        0, L"BUTTON", L"Ne plus lancer à la connexion",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DISABLE_ONEDRIVE_STARTUP)),
        g_instance, nullptr);
    SetControlFont(g_disableOneDriveStartup, g_bodyFont);
    g_uninstallOneDrive = CreateWindowExW(
        0, L"BUTTON", L"Désinstaller",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UNINSTALL_ONEDRIVE)),
        g_instance, nullptr);
    SetControlFont(g_uninstallOneDrive, g_bodyFont);

    g_googleDrive = CreateCheckbox(window, IDC_GOOGLE_DRIVE, L"Google Drive");
    g_googleDriveDetail = CreateLabel(window, L"", g_smallFont);

    g_personalFolders = CreateWindowExW(
        0, L"BUTTON", L"Dossiers personnels…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PERSONAL_FOLDERS)),
        g_instance, nullptr);
    SetControlFont(g_personalFolders, g_bodyBoldFont);
    g_personalFoldersDetail = CreateLabel(
        window, L"Bureau, Documents, Images, Téléchargements, Musique et Vidéos", g_smallFont);

    g_migrateCloud = CreateWindowExW(
        0, L"BUTTON", L"Migrer OneDrive → Google Drive…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MIGRATE_CLOUD)),
        g_instance, nullptr);
    SetControlFont(g_migrateCloud, g_bodyBoldFont);
    g_migrateCloudDetail = CreateLabel(
        window, L"Copie à sens unique, progression, vérification et bascule facultative", g_smallFont);

    g_explanation = CreateLabel(
        window,
        L"« Appliquer » concerne le volet Explorer. Les déplacements de fichiers sont confirmés séparément.",
        g_smallFont);
    g_status = CreateLabel(window, L"Prêt.", g_bodyFont);

    g_refresh = CreateWindowExW(0, L"BUTTON", L"Actualiser",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                0, 0, 0, 0, window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH)), g_instance, nullptr);
    SetControlFont(g_refresh, g_bodyFont);
    g_apply = CreateWindowExW(0, L"BUTTON", L"Appliquer",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 0, 0, window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_APPLY)), g_instance, nullptr);
    SetControlFont(g_apply, g_bodyBoldFont);
    LayoutMainControls(g_uiDpi);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_uiDpi = GetDpiForWindow(window);
        RecreateUiFonts(g_uiDpi);
        CreateInterface(window);
        UpdateControlsFromState();
        ShowStatus(g_demoMode ? L"Mode test : aucune modification du système." : L"Prêt.");
        if (g_demoMigration) {
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_MIGRATE_CLOUD, BN_CLICKED), 0);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BROWSE: {
            const std::wstring selected = PickFolder(window);
            if (!selected.empty()) {
                g_state.myDrivePath = selected;
                SetWindowTextW(g_myDriveDetail, selected.c_str());
                Button_SetCheck(g_myDrive, BST_CHECKED);
                ShowStatus(L"Dossier choisi. Clique sur « Appliquer ». ");
            }
            return 0;
        }
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
            providers.simulateOneDriveBackupActive = g_demoPlan;
            const bool changed = cloudnav::ShowFolderManagerDialog(
                window, g_instance, providers, g_demoMode);
            if (!g_demoMode) {
                g_state = DetectState();
                UpdateControlsFromState();
            }
            ShowStatus(changed
                ? L"Emplacements des dossiers personnels mis à jour."
                : L"Gestionnaire de dossiers fermé.");
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
                providers.oneDriveToGoogleVerified = true;
                providers.preloadDemoPlan = g_demoMode;
                cloudnav::ShowFolderManagerDialog(window, g_instance, providers, g_demoMode);
            }
            ShowStatus(result == cloudnav::MigrationResult::ConfigureFolders
                ? L"Migration vérifiée ; gestion des dossiers ouverte."
                : L"Assistant de migration fermé.");
            return 0;
        }
        case IDC_APPLY:
            ApplySelections();
            return 0;
        default:
            break;
        }
        break;
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
        SaveWindowPosition(window);
        DestroyWindow(window);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        HPEN linePen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
        HGDIOBJ oldPen = SelectObject(dc, linePen);
        MoveToEx(dc, ScaleDip(28, g_uiDpi), ScaleDip(178, g_uiDpi), nullptr);
        LineTo(dc, ScaleDip(652, g_uiDpi), ScaleDip(178, g_uiDpi));
        MoveToEx(dc, ScaleDip(28, g_uiDpi), ScaleDip(273, g_uiDpi), nullptr);
        LineTo(dc, ScaleDip(652, g_uiDpi), ScaleDip(273, g_uiDpi));
        MoveToEx(dc, ScaleDip(28, g_uiDpi), ScaleDip(340, g_uiDpi), nullptr);
        LineTo(dc, ScaleDip(652, g_uiDpi), ScaleDip(340, g_uiDpi));
        MoveToEx(dc, ScaleDip(28, g_uiDpi), ScaleDip(391, g_uiDpi), nullptr);
        LineTo(dc, ScaleDip(652, g_uiDpi), ScaleDip(391, g_uiDpi));
        MoveToEx(dc, ScaleDip(28, g_uiDpi), ScaleDip(430, g_uiDpi), nullptr);
        LineTo(dc, ScaleDip(652, g_uiDpi), ScaleDip(430, g_uiDpi));
        SelectObject(dc, oldPen);
        DeleteObject(linePen);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
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
    SetControlFont(g_sectionTitle, g_smallFont);
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
        g_demoMode ? L"CloudNav — test visuel" : L"CloudNav — volet de l’Explorateur",
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

    DeleteObject(g_titleFont);
    DeleteObject(g_bodyFont);
    DeleteObject(g_bodyBoldFont);
    DeleteObject(g_smallFont);
    DeleteObject(g_backgroundBrush);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
