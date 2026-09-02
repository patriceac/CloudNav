#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\CloudNav";
constexpr wchar_t kWindowLeftValue[] = L"WindowLeft";
constexpr wchar_t kWindowTopValue[] = L"WindowTop";
constexpr wchar_t kWindowClass[] = L"CloudNavWindow";

struct RegistryValue {
    bool exists = false;
    DWORD value = 0;
};

bool ReadRegistryValue(const wchar_t* name, DWORD& value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD,
                        &type, &value, &size) == ERROR_SUCCESS;
}

bool WriteRegistryValue(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegSetValueExW(
        key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void RestoreRegistryValue(const wchar_t* name, const RegistryValue& original) {
    if (original.exists) {
        WriteRegistryValue(name, original.value);
        return;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

class RegistryGuard {
public:
    RegistryGuard() {
        left_.exists = ReadRegistryValue(kWindowLeftValue, left_.value);
        top_.exists = ReadRegistryValue(kWindowTopValue, top_.value);
    }

    ~RegistryGuard() {
        RestoreRegistryValue(kWindowLeftValue, left_);
        RestoreRegistryValue(kWindowTopValue, top_);
        if (!left_.exists && !top_.exists) {
            RegDeleteKeyW(HKEY_CURRENT_USER, kSettingsKey);
        }
    }

    RegistryGuard(const RegistryGuard&) = delete;
    RegistryGuard& operator=(const RegistryGuard&) = delete;

private:
    RegistryValue left_;
    RegistryValue top_;
};

struct WindowSearch {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindWindowForProcess(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId || !IsWindowVisible(window)) {
        return TRUE;
    }
    wchar_t className[64] = {};
    if (GetClassNameW(window, className, ARRAYSIZE(className)) > 0 &&
        wcscmp(className, kWindowClass) == 0) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND WaitForWindow(DWORD processId) {
    for (int attempt = 0; attempt < 150; ++attempt) {
        WindowSearch search = {processId, nullptr};
        EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&search));
        if (search.window) {
            return search.window;
        }
        Sleep(100);
    }
    return nullptr;
}

std::wstring ModuleDirectory() {
    std::vector<wchar_t> path(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size() - 1) {
            std::wstring result(path.data(), length);
            const size_t separator = result.find_last_of(L"\\/");
            return separator == std::wstring::npos ? L"" : result.substr(0, separator);
        }
        path.resize(path.size() * 2);
    }
}

class AppProcess {
public:
    ~AppProcess() {
        Close();
    }

    bool Launch(const std::wstring& executable) {
        Close();
        std::wstring commandLine = L"\"" + executable + L"\" --demo";
        std::vector<wchar_t> writable(commandLine.begin(), commandLine.end());
        writable.push_back(L'\0');

        STARTUPINFOW startup = {sizeof(startup)};
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &startup, &process)) {
            return false;
        }
        CloseHandle(process.hThread);
        processHandle_ = process.hProcess;
        processId_ = process.dwProcessId;
        window_ = WaitForWindow(processId_);
        return window_ != nullptr;
    }

    bool Close() {
        if (!processHandle_) {
            return true;
        }
        if (window_ && IsWindow(window_)) {
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(window_, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 5000, &ignored);
        }
        const DWORD wait = WaitForSingleObject(processHandle_, 10000);
        const bool exited = wait == WAIT_OBJECT_0;
        if (!exited) {
            TerminateProcess(processHandle_, ERROR_TIMEOUT);
            WaitForSingleObject(processHandle_, 5000);
        }
        CloseHandle(processHandle_);
        processHandle_ = nullptr;
        processId_ = 0;
        window_ = nullptr;
        return exited;
    }

    HWND Window() const {
        return window_;
    }

private:
    HANDLE processHandle_ = nullptr;
    DWORD processId_ = 0;
    HWND window_ = nullptr;
};

bool WriteResult(const std::wstring& path, bool passed, const std::string& detail) {
    const std::string json = std::string("{\"passed\":") + (passed ? "true" : "false") +
        ",\"detail\":\"" + detail + "\"}";
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool success = WriteFile(file, json.data(), static_cast<DWORD>(json.size()),
                                   &written, nullptr) != FALSE && written == json.size();
    CloseHandle(file);
    return success;
}

bool IsFullyInNearestWorkArea(const RECT& windowRect) {
    POINT position = {windowRect.left, windowRect.top};
    const HMONITOR monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    return windowRect.left >= info.rcWork.left && windowRect.top >= info.rcWork.top &&
        (width >= workWidth || windowRect.right <= info.rcWork.right) &&
        (height >= workHeight || windowRect.bottom <= info.rcWork.bottom);
}

bool RunTest(const std::wstring& executable, std::string& detail) {
    RegistryGuard registryGuard;
    AppProcess app;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kWindowLeftValue);
        RegDeleteValueW(key, kWindowTopValue);
        RegCloseKey(key);
    }

    if (!app.Launch(executable)) {
        detail = "first launch failed";
        return false;
    }
    RECT firstRect = {};
    if (!GetWindowRect(app.Window(), &firstRect)) {
        detail = "first window bounds unavailable";
        return false;
    }
    MONITORINFO monitorInfo = {sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromWindow(app.Window(), MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
        detail = "monitor work area unavailable";
        return false;
    }
    const int width = firstRect.right - firstRect.left;
    const int height = firstRect.bottom - firstRect.top;
    const int maxLeft = monitorInfo.rcWork.right - width;
    const int maxTop = monitorInfo.rcWork.bottom - height;
    const int targetLeft = maxLeft < monitorInfo.rcWork.left
        ? monitorInfo.rcWork.left
        : std::min(static_cast<int>(monitorInfo.rcWork.left) + 137, maxLeft);
    const int targetTop = maxTop < monitorInfo.rcWork.top
        ? monitorInfo.rcWork.top
        : std::min(static_cast<int>(monitorInfo.rcWork.top) + 113, maxTop);
    if (!SetWindowPos(app.Window(), nullptr, targetLeft, targetTop, 0, 0,
                      SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE)) {
        detail = "window move failed";
        return false;
    }
    RECT movedRect = {};
    if (!GetWindowRect(app.Window(), &movedRect) || !app.Close()) {
        detail = "first close failed";
        return false;
    }

    DWORD storedLeft = 0;
    DWORD storedTop = 0;
    if (!ReadRegistryValue(kWindowLeftValue, storedLeft) ||
        !ReadRegistryValue(kWindowTopValue, storedTop) ||
        static_cast<LONG>(storedLeft) != movedRect.left ||
        static_cast<LONG>(storedTop) != movedRect.top) {
        detail = "saved coordinates are incorrect";
        return false;
    }

    if (!app.Launch(executable)) {
        detail = "second launch failed";
        return false;
    }
    RECT restoredRect = {};
    if (!GetWindowRect(app.Window(), &restoredRect) ||
        std::abs(restoredRect.left - movedRect.left) > 2 ||
        std::abs(restoredRect.top - movedRect.top) > 2) {
        detail = "position was not restored";
        return false;
    }
    if (!app.Close()) {
        detail = "second close failed";
        return false;
    }

    if (!WriteRegistryValue(kWindowLeftValue, 2000000) ||
        !WriteRegistryValue(kWindowTopValue, 2000000) ||
        !app.Launch(executable)) {
        detail = "offscreen launch failed";
        return false;
    }
    RECT clampedRect = {};
    if (!GetWindowRect(app.Window(), &clampedRect) || !IsFullyInNearestWorkArea(clampedRect)) {
        detail = "offscreen position was not clamped";
        return false;
    }
    if (!app.Close()) {
        detail = "offscreen close failed";
        return false;
    }

    detail = "position restored and offscreen coordinates clamped";
    return true;
}

}  // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount != 2) {
        return ERROR_INVALID_PARAMETER;
    }
    const std::wstring executable = ModuleDirectory() + L"\\CloudNav.exe";
    std::string detail;
    const bool passed = RunTest(executable, detail);
    if (!WriteResult(arguments[1], passed, detail)) {
        return ERROR_WRITE_FAULT;
    }
    return passed ? 0 : 1;
}
