#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <objidl.h>
#include <propidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include "folder_manager.h"
#include "logic.h"
#include "resource.h"
#include "ui.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace cloudnav {
namespace {

constexpr wchar_t kOneDriveAccountsKey[] = L"Software\\Microsoft\\OneDrive\\Accounts";
constexpr wchar_t kOneDrivePolicyKey[] = L"Software\\Policies\\Microsoft\\OneDrive";

enum class TargetChoice : int {
    Keep = 0,
    Local = 1,
    OneDrive = 2,
    GoogleDrive = 3,
    Custom = 4
};

enum class TransferChoice : int {
    Copy = 0,
    Move = 1,
    Repoint = 2
};

struct FolderSpec {
    const KNOWNFOLDERID* id;
    const wchar_t* label;
    int pathControl;
    int targetControl;
    int providerControl;
};

const std::array<FolderSpec, 6> kFolderSpecs = {{
    {&FOLDERID_Desktop, L"Desktop", IDC_FOLDER_DESKTOP_PATH, IDC_FOLDER_DESKTOP_TARGET, IDC_FOLDER_DESKTOP_PROVIDER},
    {&FOLDERID_Documents, L"Documents", IDC_FOLDER_DOCUMENTS_PATH, IDC_FOLDER_DOCUMENTS_TARGET, IDC_FOLDER_DOCUMENTS_PROVIDER},
    {&FOLDERID_Pictures, L"Pictures", IDC_FOLDER_PICTURES_PATH, IDC_FOLDER_PICTURES_TARGET, IDC_FOLDER_PICTURES_PROVIDER},
    {&FOLDERID_Downloads, L"Downloads", IDC_FOLDER_DOWNLOADS_PATH, IDC_FOLDER_DOWNLOADS_TARGET, IDC_FOLDER_DOWNLOADS_PROVIDER},
    {&FOLDERID_Music, L"Music", IDC_FOLDER_MUSIC_PATH, IDC_FOLDER_MUSIC_TARGET, IDC_FOLDER_MUSIC_PROVIDER},
    {&FOLDERID_Videos, L"Videos", IDC_FOLDER_VIDEOS_PATH, IDC_FOLDER_VIDEOS_TARGET, IDC_FOLDER_VIDEOS_PROVIDER}
}};

struct FolderRow {
    const FolderSpec* spec = nullptr;
    std::wstring currentPath;
    std::wstring defaultPath;
    std::wstring customPath;
    TargetChoice choice = TargetChoice::Keep;
    bool redirectable = false;
};

struct FolderOperation {
    size_t rowIndex = 0;
    std::wstring source;
    std::wstring target;
};

struct PlanProfile {
    bool hasChanges = false;
    bool hasMirroredCloudChanges = false;
    bool hasOtherChanges = false;
};

struct DialogContext {
    FolderProviders providers;
    std::vector<FolderRow> rows;
    bool demoMode = false;
    bool changed = false;
    bool statusIsError = false;
    bool guidanceIsWarning = false;
    bool backupWillDisable = false;
    bool backupReleaseRequired = false;
    size_t selectedChanges = 0;
    size_t sideEffects = 0;
    ui::DialogTheme theme;
    IStream* oneDriveLogoStream = nullptr;
    IStream* googleDriveLogoStream = nullptr;
    Gdiplus::Image* oneDriveLogo = nullptr;
    Gdiplus::Image* googleDriveLogo = nullptr;
    Gdiplus::Rect oneDriveVisibleBounds;
    Gdiplus::Rect googleDriveVisibleBounds;
};

enum class ProviderIcon : LONG_PTR {
    None = 0,
    OneDrive = 1,
    GoogleDrive = 2
};

DialogContext* GetContext(HWND dialog) {
    return reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
}

std::wstring FormatWindowsError(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length && buffer
        ? std::wstring(buffer, length)
        : L"Windows error " + std::to_wstring(code);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring FormatHresult(HRESULT result) {
    std::wstring message = FormatWindowsError(static_cast<DWORD>(result));
    if (message.rfind(L"Windows error ", 0) != 0) {
        return message;
    }
    wchar_t code[24] = {};
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(result));
    return L"Windows error " + std::wstring(code);
}

bool ReadRegistryDword(HKEY root, const std::wstring& subkey,
                       const wchar_t* valueName, DWORD& value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(root, subkey.c_str(), valueName, RRF_RT_REG_DWORD,
                        &type, &value, &size) == ERROR_SUCCESS;
}

bool ReadRegistryString(HKEY root, const std::wstring& subkey,
                        const wchar_t* valueName, std::wstring& value) {
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
        const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (needed > 0) {
            std::vector<wchar_t> expanded(needed);
            if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed) > 0) {
                value.assign(expanded.data());
            }
        }
    }
    return true;
}

bool RegistryPolicyValueEnabled(HKEY root, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegGetValueW(root, kOneDrivePolicyKey, valueName, RRF_RT_ANY,
                     &type, nullptr, &size) != ERROR_SUCCESS) {
        return false;
    }
    if (type == REG_DWORD) {
        DWORD value = 0;
        return ReadRegistryDword(root, kOneDrivePolicyKey, valueName, value) && value != 0;
    }
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        std::wstring value;
        return ReadRegistryString(root, kOneDrivePolicyKey, valueName, value) &&
               !value.empty() && value != L"0";
    }
    return true;
}

bool IsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID administrators = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                 &administrators)) {
        CheckTokenMembership(nullptr, administrators, &isAdmin);
        FreeSid(administrators);
    }
    return isAdmin == TRUE;
}

std::wstring GetModulePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

int ConfigureOneDriveFolderBackupPolicy() {
    if (!IsAdministrator()) {
        return ERROR_ACCESS_DENIED;
    }

    DWORD existing = 0;
    if (ReadRegistryDword(HKEY_LOCAL_MACHINE, kOneDrivePolicyKey,
                          L"KFMBlockOptIn", existing)) {
        if (existing == 2) {
            return ERROR_SUCCESS;
        }
        return ERROR_ACCESS_DISABLED_BY_POLICY;
    }

    const std::array<const wchar_t*, 3> incompatiblePolicies = {
        L"KFMBlockOptOut", L"KFMSilentOptIn", L"KFMOptInWithWizard"
    };
    for (const wchar_t* valueName : incompatiblePolicies) {
        if (RegistryPolicyValueEnabled(HKEY_LOCAL_MACHINE, valueName) ||
            RegistryPolicyValueEnabled(HKEY_CURRENT_USER, valueName)) {
            return ERROR_ACCESS_DISABLED_BY_POLICY;
        }
    }

    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kOneDrivePolicyKey,
                                     0, nullptr, 0, KEY_SET_VALUE,
                                     nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    const DWORD value = 2;
    status = RegSetValueExW(key, L"KFMBlockOptIn", 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status;
}

bool RunElevatedOneDriveBackupDisable(HWND owner, std::wstring& error) {
    DWORD current = 0;
    if (ReadRegistryDword(HKEY_LOCAL_MACHINE, kOneDrivePolicyKey,
                          L"KFMBlockOptIn", current) && current == 2) {
        return true;
    }

    if (IsAdministrator()) {
        const int result = ConfigureOneDriveFolderBackupPolicy();
        if (result == ERROR_SUCCESS) {
            return true;
        }
        error = result == ERROR_ACCESS_DISABLED_BY_POLICY
            ? L"An existing OneDrive policy prevents CloudNav from automatically disabling backup."
            : FormatWindowsError(static_cast<DWORD>(result));
        return false;
    }

    const std::wstring executable = GetModulePath();
    if (executable.empty()) {
        error = L"CloudNav cannot find its own executable.";
        return false;
    }
    SHELLEXECUTEINFOW execute = {sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = owner;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.c_str();
    execute.lpParameters = L"--elevated-disable-onedrive-backup";
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
            ? L"Administrator approval cancelled. No folders were redirected."
            : FormatWindowsError(code);
        return false;
    }
    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    if (exitCode != ERROR_SUCCESS) {
        error = exitCode == ERROR_ACCESS_DISABLED_BY_POLICY
            ? L"An existing OneDrive policy prevents CloudNav from automatically disabling backup."
            : L"Disabling OneDrive backup failed (code " +
              std::to_wstring(exitCode) + L").";
        return false;
    }
    return true;
}

bool OneDriveFolderBackupActive() {
    HKEY accounts = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kOneDriveAccountsKey, 0,
                      KEY_ENUMERATE_SUB_KEYS, &accounts) != ERROR_SUCCESS) {
        return false;
    }
    bool active = false;
    DWORD index = 0;
    wchar_t name[256] = {};
    DWORD nameLength = ARRAYSIZE(name);
    while (RegEnumKeyExW(accounts, index++, name, &nameLength, nullptr,
                         nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        DWORD protectedFolders = 0;
        const std::wstring accountKey = std::wstring(kOneDriveAccountsKey) + L"\\" +
                                        std::wstring(name, nameLength);
        if (ReadRegistryDword(HKEY_CURRENT_USER, accountKey,
                              L"KfmFoldersProtectedNow", protectedFolders) &&
            protectedFolders != 0) {
            active = true;
            break;
        }
        nameLength = ARRAYSIZE(name);
    }
    RegCloseKey(accounts);
    return active;
}

std::wstring FindOneDriveExecutable() {
    std::wstring executable;
    if (ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive",
                           L"OneDriveTrigger", executable) &&
        GetFileAttributesW(executable.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return executable;
    }

    const std::array<std::pair<const wchar_t*, const wchar_t*>, 3> candidates = {{
        {L"LOCALAPPDATA", L"\\Microsoft\\OneDrive\\OneDrive.exe"},
        {L"ProgramFiles", L"\\Microsoft OneDrive\\OneDrive.exe"},
        {L"ProgramFiles(x86)", L"\\Microsoft OneDrive\\OneDrive.exe"}
    }};
    for (const auto& candidate : candidates) {
        wchar_t root[MAX_PATH] = {};
        if (GetEnvironmentVariableW(candidate.first, root, ARRAYSIZE(root)) == 0) {
            continue;
        }
        executable = std::wstring(root) + candidate.second;
        const DWORD attributes = GetFileAttributesW(executable.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return executable;
        }
    }
    return {};
}

bool LaunchOneDriveCommand(const std::wstring& executable,
                           const std::wstring& arguments,
                           bool waitForExit, std::wstring& error) {
    std::wstring command = QuoteArgument(executable);
    if (!arguments.empty()) {
        command += L" " + arguments;
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startup, &process)) {
        error = FormatWindowsError(GetLastError());
        return false;
    }
    if (waitForExit) {
        WaitForSingleObject(process.hProcess, 15000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void PumpDialogMessages(HWND dialog) {
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

bool WaitForOneDriveBackupRelease(HWND dialog) {
    for (int attempt = 0; attempt < 120; ++attempt) {
        if (!OneDriveFolderBackupActive()) {
            return true;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
        PumpDialogMessages(dialog);
    }
    return !OneDriveFolderBackupActive();
}

bool ContainsInsensitive(const std::wstring& text, const std::wstring& fragment) {
    if (fragment.empty() || fragment.size() > text.size()) {
        return false;
    }
    return std::search(text.begin(), text.end(), fragment.begin(), fragment.end(),
        [](wchar_t left, wchar_t right) {
            return towlower(left) == towlower(right);
        }) != text.end();
}

std::wstring FriendlyRedirectError(const std::wstring& error) {
    if (ContainsInsensitive(error, L"folder in the same location") &&
        ContainsInsensitive(error, L"can't be redirected")) {
        return L"Another Windows special folder still uses the same location. "
               L"This can happen when OneDrive backup still protects this folder.";
    }
    return error;
}

void SetStatus(HWND dialog, const std::wstring& message, bool error = false) {
    DialogContext* context = GetContext(dialog);
    if (context) {
        context->statusIsError = error;
    }
    SetDlgItemTextW(dialog, IDC_FOLDER_STATUS, message.c_str());
    InvalidateRect(GetDlgItem(dialog, IDC_FOLDER_STATUS), nullptr, TRUE);
}

bool PathIsDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, std::wstring& result) {
    PWSTR path = nullptr;
    const HRESULT status = SHGetKnownFolderPath(id, flags, nullptr, &path);
    if (FAILED(status) || !path) {
        if (path) {
            CoTaskMemFree(path);
        }
        return false;
    }
    result = path;
    CoTaskMemFree(path);
    return true;
}

bool IsRedirectable(REFKNOWNFOLDERID id) {
    IKnownFolderManager* manager = nullptr;
    if (FAILED(CoCreateInstance(CLSID_KnownFolderManager, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&manager)))) {
        return false;
    }
    IKnownFolder* folder = nullptr;
    bool redirectable = false;
    if (SUCCEEDED(manager->GetFolder(id, &folder)) && folder) {
        KF_REDIRECTION_CAPABILITIES capabilities = KF_REDIRECTION_CAPABILITIES_ALLOW_ALL;
        if (SUCCEEDED(folder->GetRedirectionCapabilities(&capabilities))) {
            const int value = static_cast<int>(capabilities);
            redirectable = (value & 0xFF) != 0 && (value & 0xFFF00) == 0;
        }
        folder->Release();
    }
    manager->Release();
    return redirectable;
}

std::wstring PickFolder(HWND owner, const std::wstring& title) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.c_str());
    dialog->SetOkButtonLabel(L"Use this folder");
    std::wstring path;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
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

std::wstring ResolveTarget(const DialogContext& context, const FolderRow& row) {
    switch (row.choice) {
    case TargetChoice::Keep:
        return row.currentPath;
    case TargetChoice::Local:
        return row.defaultPath;
    case TargetChoice::OneDrive:
        if (context.providers.oneDriveRoot.empty()) {
            return {};
        }
        return JoinPath(context.providers.oneDriveRoot,
                        CloudRelativePath(row.currentPath,
                                          context.providers.oneDriveRoot,
                                          context.providers.googleDriveRoot,
                                          row.defaultPath));
    case TargetChoice::GoogleDrive:
        if (context.providers.googleDriveRoot.empty()) {
            return {};
        }
        return JoinPath(context.providers.googleDriveRoot,
                        CloudRelativePath(row.currentPath,
                                          context.providers.oneDriveRoot,
                                          context.providers.googleDriveRoot,
                                          row.defaultPath));
    case TargetChoice::Custom:
        return row.customPath;
    }
    return {};
}

void UpdateRowPreview(HWND dialog, const DialogContext& context, const FolderRow& row) {
    const FolderLocationKind location = ClassifyFolderLocation(
        row.currentPath, row.defaultPath,
        context.providers.oneDriveRoot, context.providers.googleDriveRoot);
    ProviderIcon icon = ProviderIcon::None;
    if (location == FolderLocationKind::OneDrive) {
        icon = ProviderIcon::OneDrive;
    } else if (location == FolderLocationKind::GoogleDrive) {
        icon = ProviderIcon::GoogleDrive;
    }
    HWND providerControl = GetDlgItem(dialog, row.spec->providerControl);
    SetWindowLongPtrW(providerControl, GWLP_USERDATA, static_cast<LONG_PTR>(icon));
    InvalidateRect(providerControl, nullptr, TRUE);

    const bool returnsLocal = FolderReturnsLocalAfterBackupRelease(context.backupWillDisable,
        row.spec == &kFolderSpecs[0] || row.spec == &kFolderSpecs[1] || row.spec == &kFolderSpecs[2],
        row.choice == TargetChoice::Keep, row.currentPath, context.providers.oneDriveRoot);
    const std::wstring target = returnsLocal ? row.defaultPath : ResolveTarget(context, row);
    const bool changes = !PathEquals(row.currentPath, target);
    const std::wstring label = std::wstring(row.spec->label) +
        (returnsLocal ? L" — also returns to local storage" : changes ? L" — will change" : L"");
    SetDlgItemTextW(dialog, row.spec->pathControl - 1, label.c_str());
    SetDlgItemTextW(dialog, row.spec->pathControl,
        row.currentPath.empty() ? L"Location unavailable" : row.currentPath.c_str());
    SetDlgItemTextW(dialog, row.spec->pathControl + 2,
        target.empty() ? L"Destination unavailable" : changes ? target.c_str() : L"Same as current location");
    SetWindowLongPtrW(GetDlgItem(dialog, row.spec->pathControl + 2), GWLP_USERDATA,
                      returnsLocal ? 2 : changes ? 1 : 0);
    SendDlgItemMessageW(dialog, row.spec->pathControl - 1, WM_SETFONT,
                       reinterpret_cast<WPARAM>(context.theme.bold), TRUE);
}

Gdiplus::Image* LoadPngResource(HINSTANCE instance, int resourceId, IStream*& stream) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return nullptr;
    }
    const DWORD size = SizeofResource(instance, resource);
    HGLOBAL loaded = LoadResource(instance, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || size == 0) {
        return nullptr;
    }
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) {
        return nullptr;
    }
    void* destination = GlobalLock(copy);
    if (!destination) {
        GlobalFree(copy);
        return nullptr;
    }
    CopyMemory(destination, bytes, size);
    GlobalUnlock(copy);
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream))) {
        GlobalFree(copy);
        stream = nullptr;
        return nullptr;
    }
    Gdiplus::Image* image = Gdiplus::Image::FromStream(stream, FALSE);
    if (!image || image->GetLastStatus() != Gdiplus::Ok) {
        delete image;
        stream->Release();
        stream = nullptr;
        return nullptr;
    }
    return image;
}

Gdiplus::Rect MeasureVisibleBounds(Gdiplus::Image* image) {
    if (!image || image->GetWidth() == 0 || image->GetHeight() == 0) {
        return {};
    }
    Gdiplus::Rect fullBounds(
        0, 0, static_cast<INT>(image->GetWidth()), static_cast<INT>(image->GetHeight()));
    auto* bitmap = static_cast<Gdiplus::Bitmap*>(image);
    Gdiplus::BitmapData data = {};
    if (bitmap->LockBits(&fullBounds, Gdiplus::ImageLockModeRead,
                         PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return fullBounds;
    }
    int left = fullBounds.Width;
    int top = fullBounds.Height;
    int right = -1;
    int bottom = -1;
    BYTE* base = static_cast<BYTE*>(data.Scan0);
    for (int y = 0; y < fullBounds.Height; ++y) {
        BYTE* row = base + y * data.Stride;
        for (int x = 0; x < fullBounds.Width; ++x) {
            if (row[x * 4 + 3] != 0) {
                left = (std::min)(left, x);
                top = (std::min)(top, y);
                right = (std::max)(right, x);
                bottom = (std::max)(bottom, y);
            }
        }
    }
    bitmap->UnlockBits(&data);
    return right >= left && bottom >= top
        ? Gdiplus::Rect(left, top, right - left + 1, bottom - top + 1)
        : fullBounds;
}

int PathControlForProvider(int providerControl) {
    for (const FolderSpec& spec : kFolderSpecs) {
        if (spec.providerControl == providerControl) {
            return spec.pathControl;
        }
    }
    return 0;
}

int TextLineHeight(HWND control, int fallback) {
    HDC dc = GetDC(control);
    if (!dc) {
        return fallback;
    }
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics = {};
    const int height = GetTextMetricsW(dc, &metrics) ? metrics.tmHeight : fallback;
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(control, dc);
    return height;
}

void DrawProviderLogo(HDC dc, const RECT& bounds, Gdiplus::Image* image,
                      const Gdiplus::Rect& visibleBounds, int textLineHeight) {
    if (!image) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    const int availableWidth = bounds.right - bounds.left;
    const int availableHeight = bounds.bottom - bounds.top;
    const UINT sourceWidth = image->GetWidth();
    const UINT sourceHeight = image->GetHeight();
    if (sourceWidth == 0 || sourceHeight == 0 ||
        visibleBounds.Width <= 0 || visibleBounds.Height <= 0) {
        return;
    }
    const float scaleForWidth = static_cast<float>(availableWidth) / sourceWidth;
    const float scaleForVisibleHeight =
        static_cast<float>((std::min)(textLineHeight, availableHeight)) /
        visibleBounds.Height;
    const float scale = (std::min)(scaleForWidth, scaleForVisibleHeight);
    const int drawWidth = static_cast<int>(sourceWidth * scale + 0.5f);
    const int drawHeight = static_cast<int>(sourceHeight * scale + 0.5f);
    const int x = bounds.left + (availableWidth - drawWidth) / 2;
    const float visibleCenter = visibleBounds.Y + visibleBounds.Height / 2.0f;
    const int y = bounds.top + static_cast<int>(
        textLineHeight / 2.0f - visibleCenter * scale + 0.5f);
    graphics.DrawImage(image, x, y, drawWidth, drawHeight);
}

void FillTargetCombo(HWND dialog, const DialogContext& context, FolderRow& row) {
    HWND combo = GetDlgItem(dialog, row.spec->targetControl);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Keep current location"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"This PC"));
    std::wstring oneDrive = context.providers.oneDriveRoot.empty()
        ? L"OneDrive (not detected)"
        : ProviderAccountLabel(L"OneDrive", context.providers.oneDriveLabel);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(oneDrive.c_str()));
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(context.providers.googleDriveRoot.empty()
                     ? L"Google Drive (not detected)" : L"Google Drive"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Other folder…"));
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(row.choice), 0);
    EnableWindow(combo, row.redirectable ? TRUE : FALSE);
}

void LoadRows(HWND dialog, DialogContext& context) {
    context.rows.clear();
    context.rows.reserve(kFolderSpecs.size());
    const std::array<std::wstring, 6> demoCurrent = {{
        L"C:\\Users\\Example\\Desktop",
        L"C:\\Users\\Example\\OneDrive\\Documents",
        L"C:\\Users\\Example\\OneDrive\\Images",
        L"D:\\Téléchargements",
        L"C:\\Users\\Example\\My Drive\\Music",
        L"C:\\Users\\Example\\Videos"
    }};
    const std::array<std::wstring, 6> demoDefault = {{
        L"C:\\Users\\Example\\Desktop",
        L"C:\\Users\\Example\\Documents",
        L"C:\\Users\\Example\\Pictures",
        L"C:\\Users\\Example\\Downloads",
        L"C:\\Users\\Example\\Music",
        L"C:\\Users\\Example\\Videos"
    }};

    for (size_t index = 0; index < kFolderSpecs.size(); ++index) {
        FolderRow row;
        row.spec = &kFolderSpecs[index];
        if (context.demoMode) {
            row.currentPath = demoCurrent[index];
            row.defaultPath = demoDefault[index];
            row.redirectable = true;
        } else {
            ReadKnownFolderPath(*row.spec->id, KF_FLAG_DONT_VERIFY, row.currentPath);
            ReadKnownFolderPath(*row.spec->id,
                                KF_FLAG_DEFAULT_PATH | KF_FLAG_DONT_VERIFY,
                                row.defaultPath);
            row.redirectable = IsRedirectable(*row.spec->id);
        }
        if ((context.demoMode && context.providers.preloadDemoPlan && index == 1) ||
            (context.providers.oneDriveToGoogleVerified &&
             PathIsWithin(row.currentPath, context.providers.oneDriveRoot))) {
            row.choice = TargetChoice::GoogleDrive;
        }
        context.rows.push_back(row);
    }

    for (FolderRow& row : context.rows) {
        FillTargetCombo(dialog, context, row);
        UpdateRowPreview(dialog, context, row);
    }
}

void FillTransferCombo(HWND dialog) {
    HWND combo = GetDlgItem(dialog, IDC_FOLDER_TRANSFER_MODE);
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Copy files and keep the original content"));
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Move files and remove the original content"));
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Redirect only — no file transfer"));
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(TransferChoice::Copy), 0);
}

TransferChoice ReadTransferChoice(HWND dialog) {
    const LRESULT selection = SendDlgItemMessageW(dialog, IDC_FOLDER_TRANSFER_MODE,
                                                   CB_GETCURSEL, 0, 0);
    if (selection < 0 || selection > static_cast<LRESULT>(TransferChoice::Repoint)) {
        return TransferChoice::Copy;
    }
    return static_cast<TransferChoice>(selection);
}

PlanProfile AnalyzePlan(const DialogContext& context) {
    PlanProfile profile;
    for (const FolderRow& row : context.rows) {
        if (row.choice == TargetChoice::Keep) {
            continue;
        }
        const std::wstring target = ResolveTarget(context, row);
        if (target.empty() || PathEquals(row.currentPath, target)) {
            continue;
        }
        profile.hasChanges = true;
        const bool mirroredCloudChange = IsVerifiedCloudTransition(context.providers.oneDriveToGoogleVerified,
            row.currentPath, target,
                                      context.providers.oneDriveRoot,
                                      context.providers.googleDriveRoot);
        profile.hasMirroredCloudChanges |= mirroredCloudChange;
        profile.hasOtherChanges |= !mirroredCloudChange;
    }
    return profile;
}

void UpdateTransferGuidance(HWND dialog, DialogContext& context, bool selectRecommendation) {
    const PlanProfile profile = AnalyzePlan(context);
    HWND combo = GetDlgItem(dialog, IDC_FOLDER_TRANSFER_MODE);
    const TransferChoice recommended = profile.hasMirroredCloudChanges ? TransferChoice::Repoint : TransferChoice::Copy;
    if (selectRecommendation || !profile.hasChanges)
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(recommended), 0);
    const TransferChoice selected = ReadTransferChoice(dialog);
    context.selectedChanges = 0;
    context.sideEffects = 0;
    context.backupWillDisable = false;
    context.backupReleaseRequired = false;
    const bool backupActive = context.demoMode ? context.providers.simulateOneDriveBackupActive : OneDriveFolderBackupActive();
    for (size_t index = 0; index < context.rows.size(); ++index) {
        const FolderRow& row = context.rows[index];
        const std::wstring target = ResolveTarget(context, row);
        if (row.choice == TargetChoice::Keep || PathEquals(row.currentPath, target)) continue;
        ++context.selectedChanges;
        context.backupReleaseRequired |= backupActive && NeedsOneDriveBackupDisable(index <= 2,
            row.currentPath, target, context.providers.oneDriveRoot, context.providers.googleDriveRoot);
    }
    context.backupWillDisable = context.backupReleaseRequired && selected == TransferChoice::Repoint;
    for (size_t index = 0; index < context.rows.size(); ++index) {
        const FolderRow& row = context.rows[index];
        if (FolderReturnsLocalAfterBackupRelease(context.backupWillDisable, index <= 2,
            row.choice == TargetChoice::Keep, row.currentPath, context.providers.oneDriveRoot)) ++context.sideEffects;
        UpdateRowPreview(dialog, context, row);
    }
    const std::wstring summary = std::to_wstring(context.selectedChanges) + L" selected change(s)" +
        (context.sideEffects ? L" + " + std::to_wstring(context.sideEffects) + L" return(s) to local storage" : L"");
    SetDlgItemTextW(dialog, IDC_FOLDER_SUMMARY, summary.c_str());
    EnableWindow(GetDlgItem(dialog, IDOK), profile.hasChanges && !(profile.hasMirroredCloudChanges && profile.hasOtherChanges));
    if (!profile.hasChanges) {
        EnableWindow(combo, FALSE);
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(TransferChoice::Copy), 0);
        context.guidanceIsWarning = false;
        SetDlgItemTextW(dialog, IDC_FOLDER_WARNING,
            context.providers.oneDriveToGoogleVerified ? L"OneDrive → Google Drive migration verified in this session. Choose folders to set up."
                : context.providers.rootMirrorTaskDetected ? L"External synchronization detected. Its status and files have not been checked."
                : L"Choose a destination to prepare a change.");
        InvalidateRect(GetDlgItem(dialog, IDC_FOLDER_WARNING), nullptr, TRUE);
        return;
    }

    EnableWindow(combo, TRUE);
    if (context.backupReleaseRequired && selected != TransferChoice::Repoint) {
        context.guidanceIsWarning = true;
        SetDlgItemTextW(dialog, IDC_FOLDER_WARNING,
            L"OneDrive backup is active: transfer and check the files before changing these locations.");
        InvalidateRect(GetDlgItem(dialog, IDC_FOLDER_WARNING), nullptr, TRUE);
        return;
    }
    if (profile.hasMirroredCloudChanges && profile.hasOtherChanges) {
        context.guidanceIsWarning = true;
        SetDlgItemTextW(dialog, IDC_FOLDER_WARNING,
            L"Mixed plan: redirect folders between clouds first, then copy the other folders separately.");
        InvalidateRect(GetDlgItem(dialog, IDC_FOLDER_WARNING), nullptr, TRUE);
        return;
    }

    context.guidanceIsWarning = selected != recommended;
    if (profile.hasMirroredCloudChanges) {
        SetDlgItemTextW(dialog, IDC_FOLDER_WARNING,
            selected == recommended
                ? L"Migration verified: redirect only to the matching Google Drive folders. No file transfer."
                : L"Migration verified: Redirect only is sufficient for this OneDrive → Google Drive change.");
    } else {
        context.guidanceIsWarning = context.providers.rootMirrorTaskDetected || selected != recommended;
        SetDlgItemTextW(dialog, IDC_FOLDER_WARNING,
            context.providers.rootMirrorTaskDetected
                ? L"Synchronization detected; files not checked. Copy is recommended; pause external synchronization first."
                : selected == recommended
                ? L"Recommendation applied: Copy, then check before removing the original content."
                : L"Recommended: Copy and keep the original content until it has been checked.");
    }
    InvalidateRect(GetDlgItem(dialog, IDC_FOLDER_WARNING), nullptr, TRUE);
}

bool BuildOperations(const DialogContext& context, std::vector<FolderOperation>& operations,
                     std::wstring& error, const std::vector<FolderOperation>* approvedPlan = nullptr) {
    operations.clear();
    for (size_t index = 0; index < context.rows.size(); ++index) {
        const FolderRow& row = context.rows[index];
        const FolderOperation* approved = nullptr;
        if (approvedPlan) {
            const auto found = std::find_if(approvedPlan->begin(), approvedPlan->end(),
                [index](const FolderOperation& operation) { return operation.rowIndex == index; });
            if (found == approvedPlan->end()) continue;
            approved = &*found;
        }
        if (row.choice == TargetChoice::Keep) {
            continue;
        }
        if (!row.redirectable) {
            error = std::wstring(row.spec->label) + L" cannot be redirected on this PC.";
            return false;
        }
        const std::wstring target = approved ? approved->target : ResolveTarget(context, row);
        if (target.empty()) {
            error = L"The destination for " + std::wstring(row.spec->label) + L" is unavailable.";
            return false;
        }
        if (target.size() >= MAX_PATH) {
            error = L"The destination for " + std::wstring(row.spec->label) + L" is too long for Windows.";
            return false;
        }
        if (PathEquals(row.currentPath, target)) {
            continue;
        }
        if (PathsOverlap(row.currentPath, target)) {
            error = L"The destination for " + std::wstring(row.spec->label) +
                    L" cannot be inside its current location, or vice versa.";
            return false;
        }
        operations.push_back({index, row.currentPath, target});
    }

    for (size_t first = 0; first < operations.size(); ++first) {
        for (size_t second = first + 1; second < operations.size(); ++second) {
            if (PathsOverlap(operations[first].target, operations[second].target)) {
                error = L"Two folders cannot share the same destination or be nested inside each other.";
                return false;
            }
            if (PathsOverlap(operations[first].target, operations[second].source) ||
                PathsOverlap(operations[second].target, operations[first].source)) {
                error = L"The selected locations would create a circular redirection or nested folder.";
                return false;
            }
        }
    }
    return true;
}

bool PathTouchesCloud(const DialogContext& context, const std::wstring& path) {
    return (!context.providers.oneDriveRoot.empty() &&
            PathIsWithin(path, context.providers.oneDriveRoot)) ||
           (!context.providers.googleDriveRoot.empty() &&
            PathIsWithin(path, context.providers.googleDriveRoot));
}

bool PlanNeedsOneDriveBackupDisable(const DialogContext& context,
                                    const std::vector<FolderOperation>& operations) {
    return std::any_of(operations.begin(), operations.end(),
        [&context](const FolderOperation& operation) {
            return NeedsOneDriveBackupDisable(
                operation.rowIndex <= 2,
                operation.source,
                operation.target,
                context.providers.oneDriveRoot,
                context.providers.googleDriveRoot);
        });
}

bool IsOneDriveBackupActive(const DialogContext& context) {
    return context.demoMode
        ? context.providers.simulateOneDriveBackupActive
        : OneDriveFolderBackupActive();
}

bool RefreshCurrentPaths(HWND dialog, DialogContext& context, std::wstring& error) {
    for (FolderRow& row : context.rows) {
        std::wstring current;
        if (!ReadKnownFolderPath(*row.spec->id, KF_FLAG_DONT_VERIFY, current)) {
            error = L"Windows could not read " +
                    std::wstring(row.spec->label) + L" after OneDrive backup stopped.";
            return false;
        }
        row.currentPath = current;
        UpdateRowPreview(dialog, context, row);
    }
    return true;
}

bool PrepareOneDriveBackupForGoogle(HWND dialog, DialogContext& context,
                                    std::vector<FolderOperation>& operations) {
    if (!PlanNeedsOneDriveBackupDisable(context, operations) ||
        !IsOneDriveBackupActive(context)) {
        return true;
    }

    // Backup release changes sources, never the destinations or rows the user approved.
    const std::vector<FolderOperation> approvedPlan = operations;

    if (context.demoMode) {
        context.providers.simulateOneDriveBackupActive = false;
        for (size_t index = 0; index < context.rows.size() && index <= 2; ++index) {
            FolderRow& row = context.rows[index];
            if (!context.providers.oneDriveRoot.empty() &&
                PathIsWithin(row.currentPath, context.providers.oneDriveRoot)) {
                row.currentPath = row.defaultPath;
                UpdateRowPreview(dialog, context, row);
            }
        }
        std::wstring error;
        operations.clear();
        if (!BuildOperations(context, operations, error, &approvedPlan)) {
            SetStatus(dialog, error, true);
            return false;
        }
        context.changed = true;
        SetStatus(dialog, L"Test mode: OneDrive backup automatically disabled.");
        return true;
    }

    SetStatus(dialog, L"Disabling OneDrive folder backup…");
    std::wstring error;
    if (!RunElevatedOneDriveBackupDisable(dialog, error)) {
        SetStatus(dialog, L"Failed: OneDrive backup is still active.", true);
        MessageBoxW(dialog, error.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::wstring oneDriveExecutable = FindOneDriveExecutable();
    if (oneDriveExecutable.empty()) {
        const std::wstring message =
            L"CloudNav has blocked automatic OneDrive backup but cannot find "
            L"OneDrive.exe to finish releasing the folders. No folders were redirected.";
        SetStatus(dialog, L"OneDrive.exe not found: no folders redirected.", true);
        MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
        return false;
    }

    std::wstring processError;
    LaunchOneDriveCommand(oneDriveExecutable, L"/shutdown", true, processError);
    if (!LaunchOneDriveCommand(oneDriveExecutable, L"", false, processError)) {
        const std::wstring message =
            L"Automatic OneDrive backup is now blocked, but the "
            L"OneDrive client did not restart:\n\n" + processError +
            L"\n\nNo folders were redirected.";
        SetStatus(dialog, L"OneDrive did not restart: no folders redirected.", true);
        MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
        return false;
    }

    SetStatus(dialog, L"OneDrive is releasing Documents and Pictures…");
    if (!WaitForOneDriveBackupRelease(dialog)) {
        const std::wstring message =
            L"CloudNav disabled automatic OneDrive backup, but OneDrive did not "
            L"confirm the folders were released within the time limit. No folders were redirected.\n\n"
            L"Try again after OneDrive finishes synchronizing.";
        SetStatus(dialog, L"OneDrive did not release the folders: no redirection.", true);
        MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!RefreshCurrentPaths(dialog, context, error)) {
        SetStatus(dialog, L"OneDrive backup stopped; refresh the locations.", true);
        MessageBoxW(dialog, error.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
        context.changed = true;
        return false;
    }

    operations.clear();
    if (!BuildOperations(context, operations, error, &approvedPlan)) {
        SetStatus(dialog, L"OneDrive backup stopped; refresh the plan.", true);
        MessageBoxW(dialog, error.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
        context.changed = true;
        return false;
    }
    context.changed = true;
    return true;
}

struct ReviewContext {
    std::wstring plan;
    std::wstring effects;
    std::wstring notes;
    std::wstring action;
    ui::DialogTheme theme;
};

INT_PTR CALLBACK ReviewDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<ReviewContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<ReviewContext*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        context->theme.Initialize(dialog, IDC_DIALOG_HEADING);
        SetDlgItemTextW(dialog, IDC_REVIEW_PLAN, context->plan.c_str());
        SetDlgItemTextW(dialog, IDC_REVIEW_EFFECTS, context->effects.c_str());
        SetDlgItemTextW(dialog, IDC_REVIEW_NOTES, context->notes.c_str());
        SetDlgItemTextW(dialog, IDOK, context->action.c_str());
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wParam)); return TRUE;
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (context && (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORDLG))
        return context->theme.Color(reinterpret_cast<HDC>(wParam));
    return FALSE;
}

bool ConfirmOperations(HWND dialog, const DialogContext& context,
                       const std::vector<FolderOperation>& operations,
                       TransferChoice transfer) {
    ReviewContext review;
    std::wstring& message = review.notes;
    for (const FolderOperation& operation : operations) {
        const FolderRow& row = context.rows[operation.rowIndex];
        review.plan += std::wstring(row.spec->label) + L"\r\n    Current: " + operation.source +
                       L"\r\n    Planned: " + operation.target + L"\r\n\r\n";
    }

    switch (transfer) {
    case TransferChoice::Copy:
        message = L"Files will be copied. The original content will be kept.";
        review.action = L"Copy and apply";
        break;
    case TransferChoice::Move:
        message = L"Files will be moved and removed from their original locations.";
        review.action = L"Move and apply";
        break;
    case TransferChoice::Repoint:
        message = L"No file transfer. The destinations must already contain the data.";
        review.action = L"Confirm redirection";
        break;
    }

    if (PlanNeedsOneDriveBackupDisable(context, operations) &&
        IsOneDriveBackupActive(context)) {
        review.effects = L"OneDrive backup will be disabled for all protected folders.\r\n";
        for (size_t index = 0; index < context.rows.size(); ++index) {
            const FolderRow& row = context.rows[index];
            if (FolderReturnsLocalAfterBackupRelease(true, index <= 2, row.choice == TargetChoice::Keep,
                row.currentPath, context.providers.oneDriveRoot)) {
                review.effects += std::wstring(row.spec->label) + L" — also returns to local storage\r\n    Current: " +
                    row.currentPath + L"\r\n    Planned: " + row.defaultPath + L"\r\n";
            }
        }
        review.effects += L"Files remain in OneDrive. Administrator approval may be requested.";
    } else {
        review.effects = L"No other folders will change. OneDrive backup will remain unchanged.";
    }
    const bool crossesMirroredClouds = context.providers.rootMirrorTaskDetected &&
        std::any_of(operations.begin(), operations.end(),
            [&context](const FolderOperation& operation) {
                return IsMirroredCloudTransition(operation.source, operation.target,
                    context.providers.oneDriveRoot, context.providers.googleDriveRoot);
            });
    if (crossesMirroredClouds && transfer != TransferChoice::Repoint) {
        message += L"\r\nPause the detected external synchronization; its files have not been checked.";
    }
    if (transfer == TransferChoice::Move && context.providers.rootMirrorTaskDetected) {
        const bool touchesCloud = std::any_of(operations.begin(), operations.end(),
            [&context](const FolderOperation& operation) {
                return PathTouchesCloud(context, operation.source) ||
                       PathTouchesCloud(context, operation.target);
            });
        if (touchesCloud) {
            message += L"\r\nWARNING: the detected OneDrive ↔ Google Drive synchronization may "
                       L"propagate deletions. Stop it before moving files out of a cloud.";
        }
    }

    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FOLDER_REVIEW), dialog,
        ReviewDialogProc, reinterpret_cast<LPARAM>(&review)) == IDOK;
}

bool EnsureTargetDirectory(HWND dialog, const std::wstring& path, std::wstring& error) {
    if (PathIsDirectory(path)) {
        return true;
    }
    const int result = SHCreateDirectoryExW(dialog, path.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS) {
        return PathIsDirectory(path);
    }
    error = FormatWindowsError(static_cast<DWORD>(result));
    return false;
}

bool RedirectFolder(IKnownFolderManager* manager, HWND dialog,
                    const FolderRow& row, const std::wstring& target,
                    TransferChoice transfer, bool checkOnly,
                    std::wstring& error) {
    int flagValue = checkOnly ? static_cast<int>(KF_REDIRECT_CHECK_ONLY)
                              : static_cast<int>(KF_REDIRECT_WITH_UI);
    if (!checkOnly && transfer != TransferChoice::Repoint) {
        flagValue |= static_cast<int>(KF_REDIRECT_COPY_CONTENTS);
        if (transfer == TransferChoice::Move) {
            flagValue |= static_cast<int>(KF_REDIRECT_DEL_SOURCE_CONTENTS);
        }
    }
    PWSTR shellError = nullptr;
    const HRESULT result = manager->Redirect(*row.spec->id, dialog,
        static_cast<KF_REDIRECT_FLAGS>(flagValue), target.c_str(), 0, nullptr, &shellError);
    if (FAILED(result)) {
        error = shellError && *shellError ? shellError : FormatHresult(result);
        if (shellError) {
            CoTaskMemFree(shellError);
        }
        return false;
    }
    if (shellError) {
        CoTaskMemFree(shellError);
    }
    return true;
}

bool SetKnownFolderPathAndVerify(REFKNOWNFOLDERID id, const std::wstring& target,
                                 std::wstring& error) {
    const HRESULT result = SHSetKnownFolderPath(id, 0, nullptr, target.c_str());
    if (FAILED(result)) {
        error = FriendlyRedirectError(FormatHresult(result));
        return false;
    }

    std::wstring observed;
    if (!ReadKnownFolderPath(id, KF_FLAG_DONT_VERIFY, observed)) {
        error = L"Windows could not read the new location.";
        return false;
    }
    if (!PathEquals(observed, target)) {
        error = L"Windows kept a different location: " + observed;
        return false;
    }
    return true;
}

bool RestoreRepointedFolders(const DialogContext& context,
                             const std::vector<FolderOperation>& operations,
                             size_t attemptedCount) {
    bool restored = true;
    const size_t boundedCount = (std::min)(attemptedCount, operations.size());
    for (size_t position = boundedCount; position > 0; --position) {
        const FolderOperation& operation = operations[position - 1];
        const FolderRow& row = context.rows[operation.rowIndex];
        std::wstring observed;
        if (ReadKnownFolderPath(*row.spec->id, KF_FLAG_DONT_VERIFY, observed) &&
            PathEquals(observed, operation.source)) {
            continue;
        }
        std::wstring restoreError;
        if (!SetKnownFolderPathAndVerify(*row.spec->id, operation.source, restoreError)) {
            restored = false;
        }
    }
    return restored;
}

void NotifyKnownFolderChange() {
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"User Shell Folders"),
                        SMTO_ABORTIFHUNG, 3000, nullptr);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
}

void CommitRows(HWND dialog, DialogContext& context,
                const std::vector<FolderOperation>& operations) {
    for (const FolderOperation& operation : operations) {
        FolderRow& row = context.rows[operation.rowIndex];
        row.currentPath = operation.target;
        row.choice = TargetChoice::Keep;
        row.customPath.clear();
        FillTargetCombo(dialog, context, row);
        UpdateRowPreview(dialog, context, row);
    }
}

bool ApplyRepointOperations(HWND dialog, DialogContext& context,
                            const std::vector<FolderOperation>& operations) {
    for (const FolderOperation& operation : operations) {
        const FolderRow& row = context.rows[operation.rowIndex];
        std::wstring current;
        if (!ReadKnownFolderPath(*row.spec->id, KF_FLAG_DONT_VERIFY, current) ||
            !PathEquals(current, operation.source)) {
            const std::wstring message = L"The location of " + std::wstring(row.spec->label) +
                L" has changed since this window opened. Refresh before trying again.";
            SetStatus(dialog, L"Check failed: refresh the locations.", true);
            MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    size_t completed = 0;
    for (const FolderOperation& operation : operations) {
        const FolderRow& row = context.rows[operation.rowIndex];
        std::wstring error;
        if (!SetKnownFolderPathAndVerify(*row.spec->id, operation.target, error)) {
            const bool restored = RestoreRepointedFolders(context, operations, completed + 1);
            NotifyKnownFolderChange();
            std::wstring message = L"Windows refused to redirect " +
                std::wstring(row.spec->label) + L".\n\n" + FriendlyRedirectError(error);
            if (restored) {
                message += L"\n\nCloudNav restored the previous locations: nothing changed.";
                SetStatus(dialog, L"Failed: no locations were changed.", true);
            } else {
                message += L"\n\nRollback is incomplete. Click Refresh before taking any further action.";
                SetStatus(dialog, L"Failed: refresh to check the locations.", true);
                context.changed = true;
            }
            MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
            return false;
        }
        ++completed;
    }

    NotifyKnownFolderChange();
    CommitRows(dialog, context, operations);
    context.changed = true;
    SetStatus(dialog, std::to_wstring(completed) + L" location(s) redirected without file transfers.");
    return true;
}

bool ApplyOperations(HWND dialog, DialogContext& context,
                     const std::vector<FolderOperation>& operations,
                     TransferChoice transfer) {
    if (context.demoMode) {
        if (context.providers.simulateRepointFailure) {
            const std::wstring message =
                L"Windows refused to redirect Documents.\n\n"
                L"Another Windows special folder still uses the same location. "
                L"This can happen when OneDrive backup still protects this folder.\n\n"
                L"CloudNav restored the previous locations: nothing changed.";
            SetStatus(dialog, L"Failed: no locations were changed.", true);
            MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
            return false;
        }
        for (const FolderOperation& operation : operations) {
            FolderRow& row = context.rows[operation.rowIndex];
            row.currentPath = operation.target;
            row.choice = TargetChoice::Keep;
            row.customPath.clear();
            FillTargetCombo(dialog, context, row);
            UpdateRowPreview(dialog, context, row);
        }
        context.changed = true;
        SetStatus(dialog, L"Test mode: redirections simulated successfully.");
        return true;
    }

    std::wstring error;
    for (const FolderOperation& operation : operations) {
        if (transfer == TransferChoice::Repoint && !PathIsDirectory(operation.target)) {
            error = L"The destination does not exist: " + operation.target;
            SetStatus(dialog, L"Failed: a destination does not exist.", true);
            MessageBoxW(dialog, error.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
            return false;
        }
        if (!EnsureTargetDirectory(dialog, operation.target, error)) {
            SetStatus(dialog, L"Failed: unable to prepare a destination.", true);
            const std::wstring message = L"Unable to create the destination:\n" +
                operation.target + L"\n\n" + error;
            MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    if (transfer == TransferChoice::Repoint) {
        return ApplyRepointOperations(dialog, context, operations);
    }

    IKnownFolderManager* manager = nullptr;
    const HRESULT createResult = CoCreateInstance(CLSID_KnownFolderManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(createResult) || !manager) {
        SetStatus(dialog, L"The Windows folder manager is unavailable.", true);
        return false;
    }

    for (const FolderOperation& operation : operations) {
        const FolderRow& row = context.rows[operation.rowIndex];
        if (!RedirectFolder(manager, dialog, row, operation.target,
                            transfer, true, error)) {
            manager->Release();
            const std::wstring message = L"Windows cannot prepare " +
                std::wstring(row.spec->label) + L".\n\n" + FriendlyRedirectError(error) +
                L"\n\nNo locations were changed.";
            SetStatus(dialog, L"Check failed: no locations changed.", true);
            MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    size_t completed = 0;
    for (const FolderOperation& operation : operations) {
        FolderRow& row = context.rows[operation.rowIndex];
        if (!RedirectFolder(manager, dialog, row, operation.target,
                            transfer, false, error)) {
            manager->Release();
            const std::wstring message = L"Windows refused to change " +
                std::wstring(row.spec->label) + L".\n\n" + FriendlyRedirectError(error) +
                L"\n\n" + std::to_wstring(completed) + L" change(s) had already been completed.";
            SetStatus(dialog, L"Transfer failed: see the displayed details.", true);
            MessageBoxW(dialog, message.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
            return false;
        }
        ++completed;
        row.currentPath = operation.target;
        row.choice = TargetChoice::Keep;
        row.customPath.clear();
        FillTargetCombo(dialog, context, row);
        UpdateRowPreview(dialog, context, row);
    }
    manager->Release();

    NotifyKnownFolderChange();
    context.changed = true;
    SetStatus(dialog, std::to_wstring(completed) + L" location(s) updated.");
    return true;
}

void HandleTargetSelection(HWND dialog, DialogContext& context, int controlId) {
    auto iterator = std::find_if(context.rows.begin(), context.rows.end(),
        [controlId](const FolderRow& row) { return row.spec->targetControl == controlId; });
    if (iterator == context.rows.end()) {
        return;
    }
    FolderRow& row = *iterator;
    const LRESULT selected = SendDlgItemMessageW(dialog, controlId, CB_GETCURSEL, 0, 0);
    if (selected < 0 || selected > static_cast<LRESULT>(TargetChoice::Custom)) {
        return;
    }
    const TargetChoice previous = row.choice;
    row.choice = static_cast<TargetChoice>(selected);
    if (row.choice == TargetChoice::OneDrive && context.providers.oneDriveRoot.empty()) {
        MessageBoxW(dialog, L"OneDrive is not detected on this PC.", L"CloudNav",
                    MB_OK | MB_ICONINFORMATION);
        row.choice = previous;
    } else if (row.choice == TargetChoice::GoogleDrive && context.providers.googleDriveRoot.empty()) {
        MessageBoxW(dialog, L"Choose the My Drive folder in the main window first.",
                    L"CloudNav", MB_OK | MB_ICONINFORMATION);
        row.choice = previous;
    } else if (row.choice == TargetChoice::Custom) {
        const std::wstring selectedPath = PickFolder(dialog,
            L"Choose the new location for " + std::wstring(row.spec->label));
        if (selectedPath.empty()) {
            row.choice = previous;
        } else {
            row.customPath = selectedPath;
        }
    }
    // Equivalent destinations stay outside the confirmed plan, including backup side effects.
    if (row.choice != TargetChoice::Keep && PathEquals(row.currentPath, ResolveTarget(context, row))) {
        row.choice = TargetChoice::Keep;
        row.customPath.clear();
    }
    SendDlgItemMessageW(dialog, controlId, CB_SETCURSEL,
                        static_cast<WPARAM>(row.choice), 0);
    UpdateRowPreview(dialog, context, row);
    UpdateTransferGuidance(dialog, context, true);
    const std::wstring target = ResolveTarget(context, row);
    SetStatus(dialog, row.choice == TargetChoice::Keep
        ? L"No change for this folder."
        : L"Planned destination: " + target, target.empty());
}

INT_PTR CALLBACK FolderDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* context = reinterpret_cast<DialogContext*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
        context->theme.Initialize(dialog);
        HINSTANCE instance = GetModuleHandleW(nullptr);
        SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(
            LoadImageW(instance, MAKEINTRESOURCEW(IDI_CLOUDNAV),
                       IMAGE_ICON, 32, 32, LR_SHARED)));
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(
            LoadImageW(instance, MAKEINTRESOURCEW(IDI_CLOUDNAV),
                       IMAGE_ICON, 16, 16, LR_SHARED)));
        FillTransferCombo(dialog);
        LoadRows(dialog, *context);
        UpdateTransferGuidance(dialog, *context, true);
        SetStatus(dialog, context->demoMode ? L"Test mode: no system changes." : L"Ready.");
        return TRUE;
    }
    case WM_COMMAND: {
        DialogContext* context = GetContext(dialog);
        if (!context) {
            return FALSE;
        }
        const int controlId = LOWORD(wParam);
        if (HIWORD(wParam) == CBN_SELCHANGE) {
            if (controlId == IDC_FOLDER_TRANSFER_MODE) {
                UpdateTransferGuidance(dialog, *context, false);
                SetStatus(dialog, L"Transfer mode updated. Review the summary before applying.");
                return TRUE;
            }
            HandleTargetSelection(dialog, *context, controlId);
            return TRUE;
        }
        switch (controlId) {
        case IDOK: {
            const PlanProfile profile = AnalyzePlan(*context);
            if (profile.hasMirroredCloudChanges && profile.hasOtherChanges) {
                const std::wstring mixedPlanMessage =
                    L"This plan needs two separate operations. First apply folders "
                    L"between OneDrive and Google Drive using Redirect only, then the other "
                    L"folders using Copy.";
                SetStatus(dialog, L"Apply this plan in two separate steps.", true);
                MessageBoxW(dialog, mixedPlanMessage.c_str(), L"CloudNav",
                            MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            std::vector<FolderOperation> operations;
            std::wstring error;
            if (!BuildOperations(*context, operations, error)) {
                SetStatus(dialog, error, true);
                MessageBoxW(dialog, error.c_str(), L"CloudNav", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (operations.empty()) {
                SetStatus(dialog, L"No locations changed.");
                return TRUE;
            }
            const TransferChoice transfer = ReadTransferChoice(dialog);
            if (PlanNeedsOneDriveBackupDisable(*context, operations) && IsOneDriveBackupActive(*context) &&
                transfer != TransferChoice::Repoint) {
                SetStatus(dialog, L"Copy required: open Transfer or sync files from the main window.", true);
                MessageBoxW(dialog,
                    L"OneDrive backup protects these folders. To keep all files, open Transfer or sync files "
                    L"from the main window, then copy and check the data before redirecting folders. No locations were changed.",
                    L"CloudNav — copy before redirecting", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (!ConfirmOperations(dialog, *context, operations, transfer)) {
                SetStatus(dialog, L"Cancelled. Nothing was changed.");
                return TRUE;
            }
            EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
            SetStatus(dialog, L"Checking and applying…");
            if (PrepareOneDriveBackupForGoogle(dialog, *context, operations)) {
                if (operations.empty()) {
                    SetStatus(dialog, L"OneDrive backup disabled; locations are already up to date.");
                } else {
                    ApplyOperations(dialog, *context, operations, transfer);
                }
            }
            UpdateTransferGuidance(dialog, *context, false);
            return TRUE;
        }
        case IDC_FOLDER_REFRESH:
            LoadRows(dialog, *context);
            UpdateTransferGuidance(dialog, *context, true);
            SetStatus(dialog, L"Emplacements relus depuis Windows.");
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, context->changed ? IDOK : IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    }
    case WM_CLOSE: {
        DialogContext* context = GetContext(dialog);
        EndDialog(dialog, context && context->changed ? IDOK : IDCANCEL);
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        DialogContext* context = GetContext(dialog);
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        COLORREF color = ui::Text;
        if (control == GetDlgItem(dialog, IDC_FOLDER_STATUS)) {
            color = context && context->statusIsError ? RGB(176, 32, 37) : ui::Muted;
        } else if (control == GetDlgItem(dialog, IDC_FOLDER_WARNING)) {
            color = context && context->guidanceIsWarning ? ui::Warning : ui::Muted;
        } else if (context) {
            for (const auto& row : context->rows) {
                if (control == GetDlgItem(dialog, row.spec->pathControl + 2)) {
                    const LONG_PTR state = GetWindowLongPtrW(control, GWLP_USERDATA);
                    color = state == 2 ? ui::Warning : state == 1 ? ui::Accent : ui::Muted;
                }
            }
        }
        return context ? context->theme.Color(dc, color) : FALSE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (!item || item->CtlType != ODT_STATIC) {
            return FALSE;
        }
        DialogContext* context = GetContext(dialog);
        FillRect(item->hDC, &item->rcItem, context ? context->theme.background : GetSysColorBrush(COLOR_WINDOW));
        const ProviderIcon icon = static_cast<ProviderIcon>(
            GetWindowLongPtrW(item->hwndItem, GWLP_USERDATA));
        RECT iconBounds = item->rcItem;
        const int pathControlId = PathControlForProvider(static_cast<int>(item->CtlID));
        HWND pathControl = pathControlId && context
            ? GetDlgItem(GetParent(item->hwndItem), pathControlId)
            : nullptr;
        const int textLineHeight = pathControl
            ? TextLineHeight(pathControl, iconBounds.bottom - iconBounds.top)
            : iconBounds.bottom - iconBounds.top;
        if (context && icon == ProviderIcon::OneDrive) {
            DrawProviderLogo(item->hDC, iconBounds, context->oneDriveLogo,
                             context->oneDriveVisibleBounds, textLineHeight);
        } else if (context && icon == ProviderIcon::GoogleDrive) {
            DrawProviderLogo(item->hDC, iconBounds, context->googleDriveLogo,
                             context->googleDriveVisibleBounds, textLineHeight);
        }
        return TRUE;
    }
    default:
        break;
    }
    return FALSE;
}

}  // namespace

int RunDisableOneDriveFolderBackupHelper() {
    return ConfigureOneDriveFolderBackupPolicy();
}

int RunFolderRedirectionSelfTest(const std::wstring& resultPath) {
    bool managerRedirected = false;
    bool managerRestored = false;
    bool directRedirected = false;
    bool directRestored = false;
    bool localAliasUnchangedDuringRedirect = false;
    bool localAliasRestored = false;
    std::wstring originalDownloads;
    std::wstring originalDocuments;
    std::wstring originalLocalDocuments;
    const bool readDownloads = ReadKnownFolderPath(FOLDERID_Downloads,
        KF_FLAG_DONT_VERIFY, originalDownloads);
    const bool readDocuments = ReadKnownFolderPath(FOLDERID_Documents,
        KF_FLAG_DONT_VERIFY, originalDocuments);
    const bool readLocalDocuments = ReadKnownFolderPath(FOLDERID_LocalDocuments,
        KF_FLAG_DONT_VERIFY, originalLocalDocuments);

    const std::wstring::size_type separator = resultPath.find_last_of(L"\\/");
    const std::wstring outputDirectory = separator == std::wstring::npos
        ? L"." : resultPath.substr(0, separator);
    const std::wstring downloadsTarget = JoinPath(outputDirectory,
        L"CloudNav-KnownFolder-Test-Downloads");
    const std::wstring documentsTarget = JoinPath(outputDirectory,
        L"CloudNav-KnownFolder-Test-Documents");
    std::wstring error;
    const bool downloadsTargetReady = readDownloads &&
        EnsureTargetDirectory(nullptr, downloadsTarget, error);
    const bool documentsTargetReady = readDocuments && readLocalDocuments &&
        EnsureTargetDirectory(nullptr, documentsTarget, error);

    IKnownFolderManager* manager = nullptr;
    if (downloadsTargetReady && SUCCEEDED(CoCreateInstance(CLSID_KnownFolderManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager))) && manager) {
        PWSTR shellError = nullptr;
        HRESULT result = manager->Redirect(FOLDERID_Downloads, nullptr,
            KF_REDIRECT_CHECK_ONLY, downloadsTarget.c_str(), 0, nullptr, &shellError);
        if (shellError) {
            CoTaskMemFree(shellError);
            shellError = nullptr;
        }
        if (SUCCEEDED(result)) {
            result = manager->Redirect(FOLDERID_Downloads, nullptr,
                static_cast<KF_REDIRECT_FLAGS>(0), downloadsTarget.c_str(), 0, nullptr,
                &shellError);
            if (shellError) {
                CoTaskMemFree(shellError);
                shellError = nullptr;
            }
            std::wstring observed;
            managerRedirected = SUCCEEDED(result) &&
                ReadKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, observed) &&
                PathEquals(observed, downloadsTarget);
        }

        result = manager->Redirect(FOLDERID_Downloads, nullptr,
            static_cast<KF_REDIRECT_FLAGS>(0), originalDownloads.c_str(), 0, nullptr,
            &shellError);
        if (shellError) {
            CoTaskMemFree(shellError);
        }
        std::wstring restoredPath;
        managerRestored = SUCCEEDED(result) &&
            ReadKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, restoredPath) &&
            PathEquals(restoredPath, originalDownloads);
        manager->Release();
    }

    if (documentsTargetReady) {
        directRedirected = SetKnownFolderPathAndVerify(
            FOLDERID_Documents, documentsTarget, error);

        std::wstring observedLocalDocuments;
        localAliasUnchangedDuringRedirect = directRedirected &&
            ReadKnownFolderPath(FOLDERID_LocalDocuments, KF_FLAG_DONT_VERIFY,
                                observedLocalDocuments) &&
            PathEquals(observedLocalDocuments, originalLocalDocuments);

        std::wstring restoreError;
        directRestored = SetKnownFolderPathAndVerify(
            FOLDERID_Documents, originalDocuments, restoreError);

        if (ReadKnownFolderPath(FOLDERID_LocalDocuments, KF_FLAG_DONT_VERIFY,
                                observedLocalDocuments) &&
            !PathEquals(observedLocalDocuments, originalLocalDocuments)) {
            std::wstring aliasRestoreError;
            SetKnownFolderPathAndVerify(FOLDERID_LocalDocuments,
                                        originalLocalDocuments, aliasRestoreError);
        }
        localAliasRestored = ReadKnownFolderPath(FOLDERID_LocalDocuments,
                                                  KF_FLAG_DONT_VERIFY,
                                                  observedLocalDocuments) &&
            PathEquals(observedLocalDocuments, originalLocalDocuments);
    }

    RemoveDirectoryW(downloadsTarget.c_str());
    RemoveDirectoryW(documentsTarget.c_str());
    const bool passed = readDownloads && readDocuments && readLocalDocuments &&
        downloadsTargetReady && documentsTargetReady && managerRedirected &&
        managerRestored && directRedirected && directRestored && localAliasRestored;
    const auto jsonBool = [](bool value) { return value ? "true" : "false"; };
    const std::string json = std::string("{\"passed\":") + jsonBool(passed) +
        ",\"managerRedirected\":" + jsonBool(managerRedirected) +
        ",\"managerRestored\":" + jsonBool(managerRestored) +
        ",\"directRedirected\":" + jsonBool(directRedirected) +
        ",\"directRestored\":" + jsonBool(directRestored) +
        ",\"localAliasUnchangedDuringRedirect\":" +
            jsonBool(localAliasUnchangedDuringRedirect) +
        ",\"localAliasRestored\":" + jsonBool(localAliasRestored) + "}\n";
    const std::wstring temporaryResultPath = resultPath + L".tmp";
    HANDLE file = CreateFileW(temporaryResultPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 2;
    }
    DWORD written = 0;
    const BOOL writeSucceeded = WriteFile(file, json.data(), static_cast<DWORD>(json.size()),
                                          &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!writeSucceeded || written != json.size()) {
        DeleteFileW(temporaryResultPath.c_str());
        return 3;
    }
    if (!MoveFileExW(temporaryResultPath.c_str(), resultPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryResultPath.c_str());
        return 4;
    }
    return passed ? 0 : 1;
}

bool ShowFolderManagerDialog(HWND owner, HINSTANCE instance,
                             const FolderProviders& providers, bool demoMode) {
    DialogContext context;
    context.providers = providers;
    context.demoMode = demoMode;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    const bool gdiplusStarted = Gdiplus::GdiplusStartup(
        &gdiplusToken, &gdiplusInput, nullptr) == Gdiplus::Ok;
    if (gdiplusStarted) {
        context.oneDriveLogo = LoadPngResource(
            instance, IDR_ONEDRIVE_LOGO, context.oneDriveLogoStream);
        context.googleDriveLogo = LoadPngResource(
            instance, IDR_GOOGLE_DRIVE_LOGO, context.googleDriveLogoStream);
        context.oneDriveVisibleBounds = MeasureVisibleBounds(context.oneDriveLogo);
        context.googleDriveVisibleBounds = MeasureVisibleBounds(context.googleDriveLogo);
    }
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_FOLDER_MANAGER),
                                            owner, FolderDialogProc,
                                            reinterpret_cast<LPARAM>(&context));
    delete context.oneDriveLogo;
    delete context.googleDriveLogo;
    if (context.oneDriveLogoStream) {
        context.oneDriveLogoStream->Release();
    }
    if (context.googleDriveLogoStream) {
        context.googleDriveLogoStream->Release();
    }
    if (gdiplusStarted) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
    if (result == -1) {
        MessageBoxW(owner, L"Unable to open the folder manager.",
                    L"CloudNav", MB_OK | MB_ICONERROR);
        return false;
    }
    return context.changed;
}

}  // namespace cloudnav
