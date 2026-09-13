#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <uiautomationclient.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <algorithm>
#include <vector>
#include "onedrive_backup.h"
#include "logic.h"
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace cloudnav {
namespace {
using Microsoft::WRL::ComPtr;
using Element = ComPtr<IUIAutomationElement>;
constexpr const wchar_t* kToggleIds[] = {L"desktopToggle", L"documentsToggle", L"picturesToggle",
    L"", L"musicToggle", L"videosToggle"};

std::wstring Name(const Element& element) {
    BSTR value = nullptr;
    if (!element || FAILED(element->get_CurrentName(&value))) return {};
    std::wstring result(value ? value : L"");
    SysFreeString(value);
    return result;
}
bool Visible(const Element& element) {
    BOOL offscreen = TRUE;
    return element && SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen;
}
bool Enabled(const Element& element) {
    BOOL enabled = FALSE;
    return Visible(element) && SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled;
}
bool SingleAccountMatches(const std::wstring& root) {
    HKEY accounts = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts", 0,
        KEY_READ, &accounts) != ERROR_SUCCESS) return false;
    unsigned configured = 0, matched = 0;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(accounts, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[32768]{};
        DWORD bytes = sizeof(path);
        if (RegGetValueW(accounts, name, L"UserFolder", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr, path, &bytes) == ERROR_SUCCESS && path[0]) {
            ++configured;
            if (PathEquals(path, root)) ++matched;
        }
    }
    RegCloseKey(accounts);
    return configured == 1 && matched == 1;
}

class BackupDialog {
public:
    BackupDialog(std::wstring label, std::atomic<bool>& cancelled,
        std::function<bool()> valid, std::function<void(const std::wstring&)> progress)
        : label_(std::move(label)), cancelled_(cancelled), valid_(std::move(valid)), progress_(std::move(progress)) {}

    bool Initialize() {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation_)))) return false;
        ComPtr<IUIAutomation2> bounded;
        if (SUCCEEDED(automation_.As(&bounded))) {
            bounded->put_ConnectionTimeout(2000);
            bounded->put_TransactionTimeout(3000);
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{sizeof(entry)};
        if (Process32FirstW(snapshot, &entry)) do {
            if (_wcsicmp(entry.szExeFile, L"OneDrive.exe") == 0) processes_.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        return !processes_.empty();
    }

    bool Open() {
        progress_(L"Opening OneDrive backup controls…");
        if (!Valid()) return false;
        // Never dismiss an existing file-retention choice made outside this run.
        if (Find(L"optOutDialogRadioButton1")) return false;
        if (!Find(L"kfmMoveHeaderSubText")) {
            if (!Find(L"kfmBackupButton")) {
                if (!Find(L"contextMenuHeaderButton")) {
                    stage_ = L"Finding the selected OneDrive notification icon";
                    Element tray = TrayAccount();
                    if (!tray) {
                        auto taskbar = FromWindow(FindWindowW(L"Shell_TrayWnd", nullptr));
                        if (!Click(FindIn(taskbar, L"1502"))) return false;
                        if (!Wait([&] { tray = TrayAccount(); return !!tray; }, 5000)) return false;
                    }
                    stage_ = L"Opening the selected OneDrive notification panel";
                    // The legacy notification toolbar advertises Invoke, but
                    // some Windows versions only focus its icon when invoked.
                    if (!Click(tray, true, true) || !Wait([&] { return !!Find(L"contextMenuHeaderButton"); }, 8000)) return false;
                }
                const auto header = Name(Find(L"headerText"));
                stage_ = L"Matching OneDrive account: " + label_ + L" / " + header;
                if (header != label_) return false;
                stage_ = L"Opening OneDrive's Help and Settings menu";
                if (!Click(Find(L"contextMenuHeaderButton")) ||
                    !Wait([&] { return !!Find(L"settings"); }, 4000)) return false;
                stage_ = L"Selecting OneDrive Settings";
                if (!Click(Find(L"settings")) || !Wait([&] { return !!Find(L"syncTab"); }, 10000)) return false;
            }
            stage_ = L"Opening the OneDrive folder backup controls";
            if (auto tab = Find(L"syncTab"); tab && !Click(tab)) return false;
            if (!Wait([&] { return Enabled(Find(L"kfmBackupButton")); }, 5000) ||
                !Click(Find(L"kfmBackupButton")) ||
                !Wait([&] { return !!Find(L"kfmMoveHeaderSubText"); }, 10000)) return false;
        }
        // This visible account label is independent of the process and registry.
        stage_ = L"Confirming the OneDrive account and folder backup states";
        return !label_.empty() && Name(Find(L"kfmMoveHeaderSubText")).find(label_) != std::wstring::npos &&
            Wait([&] { return Read()[0] != BackupState::Unknown && Read()[1] != BackupState::Unknown; }, 8000);
    }

    BackupStates Read() {
        BackupStates result{};
        for (size_t index = 0; index < result.size(); ++index) {
            if (!kToggleIds[index][0]) continue;
            const auto element = Find(kToggleIds[index]);
            ComPtr<IUIAutomationTogglePattern> toggle;
            ToggleState state{};
            if (Visible(element) && SUCCEEDED(element->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) && toggle &&
                SUCCEEDED(toggle->get_CurrentToggleState(&state))) {
                if (state == ToggleState_On) result[index] = BackupState::On;
                else if (state == ToggleState_Off) result[index] = BackupState::Off;
            }
        }
        return result;
    }

    const std::wstring& Stage() const { return stage_; }

private:
    bool Valid() const { return !cancelled_.load() && valid_(); }
    bool Wait(const std::function<bool()>& condition, DWORD milliseconds) {
        const auto deadline = GetTickCount64() + milliseconds;
        do { if (!Valid()) return false; if (condition()) return true; Sleep(150); } while (GetTickCount64() < deadline);
        return false;
    }
    Element FromWindow(HWND window) {
        Element result;
        if (window && IsWindowVisible(window)) automation_->ElementFromHandle(window, &result);
        return result;
    }
    Element FindIn(const Element& root, const wchar_t* id) {
        if (!root) return {};
        VARIANT value{}; value.vt = VT_BSTR; value.bstrVal = SysAllocString(id);
        ComPtr<IUIAutomationCondition> condition;
        const auto status = automation_->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition);
        VariantClear(&value);
        if (FAILED(status) || !condition) return {};
        ComPtr<IUIAutomationElementArray> elements;
        if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &elements)) || !elements) return {};
        int count = 0; elements->get_Length(&count);
        Element result;
        for (int index = 0; index < count; ++index) {
            Element candidate; elements->GetElement(index, &candidate);
            if (!Visible(candidate)) continue;
            if (result) {
                BOOL same = FALSE;
                automation_->CompareElements(result.Get(), candidate.Get(), &same);
                if (!same) return {}; // Never act on distinct, ambiguous controls.
                continue;
            }
            result = candidate;
        }
        return result;
    }
    Element Find(const wchar_t* id) {
        Element result;
        struct Windows { const std::vector<DWORD>* pids; std::vector<HWND> handles; } windows{&processes_, {}};
        EnumWindows([](HWND window, LPARAM param) -> BOOL {
            auto& list = *reinterpret_cast<Windows*>(param);
            DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            if (IsWindowVisible(window) && std::find(list.pids->begin(), list.pids->end(), pid) != list.pids->end()) list.handles.push_back(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));
        for (HWND window : windows.handles) {
            auto candidate = FindIn(FromWindow(window), id);
            if (!candidate) continue;
            if (result) {
                BOOL same = FALSE;
                automation_->CompareElements(result.Get(), candidate.Get(), &same);
                if (!same) return {};
                continue;
            }
            result = candidate;
        }
        return result;
    }
    Element TrayAccount() {
        Element found;
        for (const auto name : {L"Shell_TrayWnd", L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland"}) {
            auto root = FromWindow(FindWindowW(name, nullptr));
            if (!root) continue;
            VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ButtonControlTypeId;
            ComPtr<IUIAutomationCondition> condition;
            if (FAILED(automation_->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)) || !condition) continue;
            ComPtr<IUIAutomationElementArray> items;
            if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &items)) || !items) continue;
            int count = 0; items->get_Length(&count);
            for (int index = 0; index < count; ++index) {
                Element item; items->GetElement(index, &item);
                const auto text = Name(item);
                if (!Visible(item) || (text != label_ && text.rfind(label_ + L"\r", 0) != 0 && text.rfind(label_ + L"\n", 0) != 0)) continue;
                if (found) return {};
                found = item;
            }
        }
        return found;
    }
    bool Click(const Element& element, bool requireValid = true, bool pointer = false) {
        if ((requireValid && !Valid()) || !Enabled(element)) return false;
        ComPtr<IUIAutomationInvokePattern> invoke;
        // Unsupported patterns can return S_OK with a null interface.
        if (!pointer && SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke) return SUCCEEDED(invoke->Invoke());
        POINT point{}; BOOL available = FALSE;
        if (FAILED(element->GetClickablePoint(&point, &available)) || !available) return false;
        HWND window = GetAncestor(WindowFromPoint(point), GA_ROOT);
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        DWORD shellPid = 0; GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &shellPid);
        if (pid != shellPid && std::find(processes_.begin(), processes_.end(), pid) == processes_.end()) return false;
        // Activate the legacy tray before opening its notification panel.
        // Do not activate XAML menu items: activation dismisses their popup.
        if (pointer) SetForegroundWindow(window);
        SetCursorPos(point.x, point.y);
        INPUT input[2]{};
        input[0].type = input[1].type = INPUT_MOUSE;
        input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        return SendInput(2, input, sizeof(INPUT)) == 2;
    }
    ComPtr<IUIAutomation> automation_;
    std::vector<DWORD> processes_;
    std::wstring label_;
    std::wstring stage_ = L"Connecting to Windows UI Automation";
    std::atomic<bool>& cancelled_;
    std::function<bool()> valid_;
    std::function<void(const std::wstring&)> progress_;
};
} // namespace

bool OpenOneDriveBackupDialog(const std::wstring& root, const std::wstring& accountLabel,
    unsigned selected, std::atomic<bool>& cancelled, const std::function<bool()>& valid,
    const std::function<void(const std::wstring&)>& progress, BackupReadiness& readiness, std::wstring& error) {
    if (!SingleAccountMatches(root)) {
        error = L"Opening backup settings requires one unambiguous OneDrive account matching the selected source.";
        return false;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { error = L"Windows UI Automation is unavailable."; return false; }
    readiness = BackupReadiness::Unknown;
    bool passed = false;
    {
        BackupDialog automation(accountLabel, cancelled, valid, progress);
        passed = automation.Initialize() && automation.Open() && valid();
        if (passed) readiness = SelectedBackupReadiness(automation.Read(), selected);
        passed = passed && readiness != BackupReadiness::Unknown;
        if (!passed) error = L"CloudNav could not open or read the selected OneDrive backup settings. The saved setup is kept.";
        if (!passed) error += L"\n\nStopped at: " + automation.Stage();
    }
    CoUninitialize();
    if (!passed && error.empty()) error = L"OneDrive backup settings could not be opened. The saved setup is kept.";
    return passed;
}
} // namespace cloudnav
