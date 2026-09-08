#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "logic.h"
#include "migration.h"
#include "migration_logic.h"
#include "migration_report.h"
#include "resource.h"
#include "ui.h"

namespace cloudnav {
namespace {

constexpr wchar_t kOneDriveRemote[] = L"cloudnav-onedrive";
constexpr wchar_t kGoogleRemote[] = L"cloudnav-gdrive";
constexpr UINT WM_MIGRATION_PROGRESS = WM_APP + 41;
constexpr UINT WM_MIGRATION_COMPLETE = WM_APP + 42;

using Task = MigrationTask;

struct ProgressUpdate {
    int percent = 0;
    MigrationStage stage = MigrationStage::Preparing;
    std::wstring statistics;
    std::wstring details;
};

struct CompletionUpdate {
    Task task = Task::None;
    bool success = false;
    bool cancelled = false;
    std::wstring message;
    AnalysisReport report;
};

struct DialogContext {
    HWND dialog = nullptr;
    HINSTANCE instance = nullptr;
    bool demoMode = false;
    bool running = false;
    bool closeRequested = false;
    bool analyzed = false;
    bool verified = false;
    bool oneDriveReady = false;
    bool googleReady = false;
    bool sawAnalyzeProgress = false;
    bool sawCopyProgress = false;
    bool sawVerifyProgress = false;
    bool indeterminate = false;
    bool progressConsistent = true;
    bool cancellationConsistent = true;
    AnalysisReport report;
    ui::DialogTheme theme;
    ui::ProviderImages images;
    Task task = Task::None;
    std::wstring demoResultPath;
    std::wstring runtimePath;
    std::wstring configPath;
    std::wstring logPath;
    HANDLE worker = nullptr;
    HANDLE childProcess = nullptr;
    CRITICAL_SECTION processLock = {};
    std::atomic<bool> cancelRequested = false;
};

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed);
    return result;
}

std::wstring LocalAppDataPath() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path)) && path) {
        result = path;
        CoTaskMemFree(path);
    }
    return result;
}

bool EnsureParentDirectory(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    const std::wstring directory = filePath.substr(0, slash);
    return SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr) == ERROR_SUCCESS ||
           GetLastError() == ERROR_ALREADY_EXISTS || GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ResourceMatchesFile(HINSTANCE instance, const std::wstring& path) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_RCLONE_EXE), RT_RCDATA);
    if (!resource) return false;
    const DWORD size = SizeofResource(instance, resource);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fileSize = {};
    bool matches = GetFileSizeEx(file, &fileSize) && fileSize.QuadPart == size;
    HGLOBAL loaded = LoadResource(instance, resource);
    const BYTE* expected = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    std::vector<BYTE> buffer(64 * 1024);
    DWORD offset = 0;
    while (matches && offset < size) {
        DWORD read = 0;
        const DWORD wanted = std::min<DWORD>(static_cast<DWORD>(buffer.size()), size - offset);
        if (!ReadFile(file, buffer.data(), wanted, &read, nullptr) || read != wanted ||
            !expected || memcmp(buffer.data(), expected + offset, wanted) != 0) matches = false;
        offset += read;
    }
    CloseHandle(file);
    return matches;
}

bool ExtractRclone(HINSTANCE instance, std::wstring& path, std::wstring& error) {
    const std::wstring root = LocalAppDataPath() + L"\\CloudNav\\Runtime";
    path = root + L"\\rclone-v1.75.0.exe";
    if (ResourceMatchesFile(instance, path)) return true;
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_RCLONE_EXE), RT_RCDATA);
    if (!resource) { error = L"La ressource rclone intégrée est introuvable."; return false; }
    HGLOBAL loaded = LoadResource(instance, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(instance, resource);
    if (!bytes || !size || !EnsureParentDirectory(path)) { error = L"Impossible de préparer le moteur de migration."; return false; }
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"Impossible d’extraire le moteur de migration."; return false; }
    DWORD written = 0;
    const bool writtenOk = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!writtenOk || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = L"Impossible de mettre à jour le moteur de migration.";
        return false;
    }
    if (!ResourceMatchesFile(instance, path)) { error = L"La vérification du moteur de migration a échoué."; return false; }
    return true;
}

void PostProgress(DialogContext& context, int percent, MigrationStage stage,
                  const std::wstring& statistics) {
    if (!context.dialog) return;
    auto* update = new ProgressUpdate{percent, stage, statistics, MigrationStageDetails(stage)};
    if (!PostMessageW(context.dialog, WM_MIGRATION_PROGRESS, 0, reinterpret_cast<LPARAM>(update))) delete update;
}

bool HasRemote(const std::wstring& configPath, const wchar_t* remote) {
    std::wifstream stream(configPath);
    if (!stream) return false;
    const std::wstring header = L"[" + std::wstring(remote) + L"]";
    std::wstring line;
    while (std::getline(stream, line)) if (_wcsicmp(line.c_str(), header.c_str()) == 0) return true;
    return false;
}

bool RunProcess(DialogContext& context, const std::vector<std::wstring>& arguments,
                MigrationStage stage, std::wstring& error, DWORD* processExitCode = nullptr, AnalysisReport* report = nullptr) {
    if (context.cancelRequested) return false;
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) { error = L"Impossible de lire la progression."; return false; }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = QuoteArgument(context.runtimePath);
    for (const auto& argument : arguments) command += L" " + QuoteArgument(argument);
    const std::wstring combinedPath = context.logPath + L".analysis-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (report) command += L" --combined " + QuoteArgument(combinedPath);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    startup.hStdInput = nullInput;
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(context.runtimePath.c_str(), mutableCommand.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    CloseHandle(writePipe);
    if (!created) { CloseHandle(readPipe); error = L"Impossible de démarrer le moteur de migration."; return false; }
    CloseHandle(process.hThread);
    EnterCriticalSection(&context.processLock);
    context.childProcess = process.hProcess;
    if (context.cancelRequested) TerminateProcess(context.childProcess, ERROR_CANCELLED);
    LeaveCriticalSection(&context.processLock);

    PostProgress(context, -1, stage, L"Progression en attente…");
    std::string pending;
    char buffer[8192];
    DWORD read = 0;
    HANDLE log = CreateFileW(context.logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read) {
        if (log != INVALID_HANDLE_VALUE) {
            DWORD logged = 0;
            WriteFile(log, buffer, read, &logged, nullptr);
        }
        pending.append(buffer, read);
        size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (report) report->Log(line);
            MigrationStatistics stats;
            if (ParseMigrationStatistics(line, stats)) {
                const auto progress = FormatMigrationProgress(stage, stats);
                PostProgress(context, progress.percent, stage, progress.text);
            }
        }
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    CloseHandle(readPipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    if (processExitCode) *processExitCode = exitCode;
    EnterCriticalSection(&context.processLock);
    context.childProcess = nullptr;
    LeaveCriticalSection(&context.processLock);
    CloseHandle(process.hProcess);
    if (report) {
        std::ifstream combined(combinedPath, std::ios::binary);
        if (combined) report->Combined(combined);
        else report->malformed = true;
        combined.close();
        DeleteFileW(combinedPath.c_str());
        report->complete = exitCode == 0 && !context.cancelRequested && !report->malformed && report->Count('!') == 0;
    }
    if (context.cancelRequested || exitCode == ERROR_CANCELLED) return false;
    if (exitCode != 0) {
        error = L"rclone a signalé une erreur (code " + std::to_wstring(exitCode) + L"). Journal : " + context.logPath;
        return false;
    }
    if (report && !report->complete) {
        error = L"Le bilan de l’analyse est incomplet. Relance l’analyse avant de copier.";
        return false;
    }
    return true;
}

DWORD WINAPI WorkerProc(void* parameter) {
    auto& context = *static_cast<DialogContext*>(parameter);
    bool success = false;
    std::wstring error;
    AnalysisReport report;
    if (!ExtractRclone(context.instance, context.runtimePath, error)) {
        success = false;
    } else if (context.demoMode) {
        const auto stage = context.task == Task::Analyze ? MigrationStage::Analyzing :
            context.task == Task::CopyAndVerify ? MigrationStage::Copying : MigrationStage::Connecting;
        for (int percent = 0; percent <= 100 && !context.cancelRequested; percent += 2) {
            MigrationStatistics stats;
            stats.totalBytes = 8ULL * 1024 * 1024 * 1024;
            stats.bytes = stage == MigrationStage::Copying ? stats.totalBytes * percent / 100 : 0;
            stats.listed = 136 * percent;
            stats.checks = stats.totalChecks = 49 * percent;
            stats.elapsed = percent * 0.05;
            stats.speed = 42.0 * 1024 * 1024;
            stats.eta = (100 - percent) * 1.9;
            const auto progress = FormatMigrationProgress(stage, stats);
            PostProgress(context, progress.percent, stage, progress.text);
            Sleep(100);
        }
        if (!context.cancelRequested && context.task == Task::CopyAndVerify) {
            for (int percent = 0; percent <= 100 && !context.cancelRequested; percent += 4) {
                MigrationStatistics stats;
                stats.listed = 13600;
                stats.checks = 49 * percent;
                stats.elapsed = percent * 0.02;
                const auto progress = FormatMigrationProgress(MigrationStage::Verifying, stats);
                PostProgress(context, progress.percent, MigrationStage::Verifying, progress.text);
                Sleep(80);
            }
        }
        success = !context.cancelRequested;
        if (context.task == Task::Analyze) {
            std::istringstream combined("+ Documents/nouveau.pdf\n* Photos/vacances.jpg\n= Documents/identique.txt\n- Archives/conservé.txt\n");
            report.Combined(combined);
            report.files["Documents/nouveau.pdf"].bytes = 1048576;
            report.files["Documents/nouveau.pdf"].sizeKnown = true;
            report.files["Photos/vacances.jpg"].bytes = 2097152;
            report.files["Photos/vacances.jpg"].sizeKnown = true;
            report.complete = success;
        }
    } else if (context.task == Task::AuthenticateOneDrive || context.task == Task::AuthenticateGoogle) {
        const bool oneDrive = context.task == Task::AuthenticateOneDrive;
        const wchar_t* remote = oneDrive ? kOneDriveRemote : kGoogleRemote;
        std::vector<std::wstring> args;
        if (HasRemote(context.configPath, remote)) args = {L"config", L"reconnect", std::wstring(remote) + L":", L"--config", context.configPath};
        else args = {L"config", L"create", remote, oneDrive ? L"onedrive" : L"drive", L"config_is_local=true", L"--config", context.configPath};
        success = RunProcess(context, args, MigrationStage::Connecting, error);
    } else if (context.task == Task::Analyze) {
        success = RunProcess(context, MigrationArguments(MigrationStage::Analyzing, context.configPath), MigrationStage::Analyzing, error, nullptr, &report);
    } else if (context.task == Task::CopyAndVerify) {
        success = RunProcess(context, MigrationArguments(MigrationStage::Copying, context.configPath), MigrationStage::Copying, error);
        if (success && !context.cancelRequested) {
            success = RunProcess(context, MigrationArguments(MigrationStage::Verifying, context.configPath), MigrationStage::Verifying, error);
        }
    }
    auto* completion = new CompletionUpdate{context.task, success, context.cancelRequested.load(), error, std::move(report)};
    if (!PostMessageW(context.dialog, WM_MIGRATION_COMPLETE, 0, reinterpret_cast<LPARAM>(completion))) delete completion;
    return 0;
}

void RefreshButtons(DialogContext& context) {
    const bool acknowledged = Button_GetCheck(GetDlgItem(context.dialog, IDC_MIGRATION_REMINDER)) == BST_CHECKED;
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_REMINDER), !context.running);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_ONEDRIVE_CONNECT), !context.running);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_GOOGLE_CONNECT), !context.running);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_ANALYZE), !context.running && acknowledged && context.oneDriveReady && context.googleReady);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_COPY), !context.running && acknowledged && context.analyzed);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_CUTOVER), !context.running && context.verified);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_REPORT), !context.running && context.report.available);
    const int primary = context.running ? IDCANCEL : context.verified ? IDC_MIGRATION_CUTOVER :
        context.analyzed ? IDC_MIGRATION_COPY :
        IsWindowEnabled(GetDlgItem(context.dialog, IDC_MIGRATION_ANALYZE)) ? IDC_MIGRATION_ANALYZE : IDCANCEL;
    for (int id : {IDC_MIGRATION_ANALYZE, IDC_MIGRATION_COPY, IDC_MIGRATION_CUTOVER, IDCANCEL}) {
        SendDlgItemMessageW(context.dialog, id, BM_SETSTYLE, id == primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
        SendDlgItemMessageW(context.dialog, id, WM_SETFONT, id == primary
            ? reinterpret_cast<WPARAM>(context.theme.bold) : SendMessageW(context.dialog, WM_GETFONT, 0, 0), TRUE);
    }
    SendMessageW(context.dialog, DM_SETDEFID, primary, 0);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_ONEDRIVE_CONNECT, context.oneDriveReady ? L"Reconnecter…" : L"Connecter…");
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_GOOGLE_CONNECT, context.googleReady ? L"Reconnecter…" : L"Connecter…");
    SetDlgItemTextW(context.dialog, IDCANCEL, context.running ? L"Annuler" : L"Fermer");
}

void SetMigrationProgress(DialogContext& context, int percent) {
    const HWND bar = GetDlgItem(context.dialog, IDC_MIGRATION_PROGRESS);
    const bool indeterminate = percent < 0;
    if (context.indeterminate != indeterminate) {
        const LONG_PTR style = GetWindowLongPtrW(bar, GWL_STYLE);
        if (!indeterminate) SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(bar, GWL_STYLE, indeterminate ? style | PBS_MARQUEE : style & ~PBS_MARQUEE);
        if (indeterminate) SendMessageW(bar, PBM_SETMARQUEE, TRUE, 35);
        context.indeterminate = indeterminate;
    }
    if (!indeterminate) SendMessageW(bar, PBM_SETPOS, percent, 0);
}

void StartTask(DialogContext& context, Task task) {
    if (context.running) return;
    context.report = {};
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_SUMMARY, context.report.Summary().c_str());
    InvalidateMigrationValidation(task, context.analyzed, context.verified);
    if (task == Task::Analyze) context.sawAnalyzeProgress = false;
    if (task == Task::Analyze || task == Task::CopyAndVerify) {
        context.sawCopyProgress = false;
        context.sawVerifyProgress = false;
        context.progressConsistent = true;
    }
    context.running = true;
    context.task = task;
    context.cancelRequested = false;
    SendDlgItemMessageW(context.dialog, IDC_MIGRATION_PROGRESS, PBM_SETSTATE, PBST_NORMAL, 0);
    SetMigrationProgress(context, -1);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_PHASE, MigrationStageTitle(MigrationStage::Preparing));
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_STATS, L"Progression en attente…");
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, MigrationStageDetails(MigrationStage::Preparing));
    RefreshButtons(context);
    context.worker = CreateThread(nullptr, 0, WorkerProc, &context, 0, nullptr);
    if (!context.worker) {
        context.running = false;
        SetMigrationProgress(context, 0);
        SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, L"Impossible de démarrer l’opération.");
        RefreshButtons(context);
    }
}

void CancelTask(DialogContext& context) {
    context.cancelRequested = true;
    EnterCriticalSection(&context.processLock);
    if (context.childProcess) TerminateProcess(context.childProcess, ERROR_CANCELLED);
    LeaveCriticalSection(&context.processLock);
    SetMigrationProgress(context, 0);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, L"Annulation… Les fichiers déjà copiés seront réutilisés à la reprise.");
}

bool WriteEvidence(const std::wstring& path, const std::string& json) {
    if (path.empty()) return true;
    if (!EnsureParentDirectory(path)) return false;
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();
    CloseHandle(file);
    return ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void WriteMilestone(const DialogContext& context, const wchar_t* name, bool passed) {
    if (!context.demoMode || context.demoResultPath.empty()) return;
    const size_t slash = context.demoResultPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    WriteEvidence(context.demoResultPath.substr(0, slash + 1) + name, passed ? "{\"passed\":true}" : "{\"passed\":false}");
}

struct ReportRow {
    char category;
    std::wstring status, path, size, error;
};

struct ReportDialog {
    DialogContext* owner;
    ui::DialogTheme theme;
    std::vector<ReportRow> rows;
    std::vector<size_t> visible;
};

void FilterReport(HWND dialog, ReportDialog& context) {
    const int selection = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_REPORT_FILTER));
    const char category = selection > 0 && selection <= 5 ? "+*=-!"[selection - 1] : 0;
    context.visible.clear();
    for (size_t i = 0; i < context.rows.size(); ++i)
        if (!category || context.rows[i].category == category) context.visible.push_back(i);
    const HWND list = GetDlgItem(dialog, IDC_REPORT_LIST);
    ListView_SetItemCountEx(list, static_cast<int>(context.visible.size()), 0);
    InvalidateRect(list, nullptr, TRUE);
    SetDlgItemTextW(dialog, IDC_REPORT_SELECTED, L"Sélectionne un fichier pour lire et copier son chemin complet.");
    if (selection == 1) WriteMilestone(*context.owner, L"report-filtered.json",
        context.visible.size() == context.owner->report.Count('+'));
}

INT_PTR CALLBACK ReportDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<ReportDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<ReportDialog*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        context->theme.Initialize(dialog, IDC_DIALOG_HEADING);
        SetDlgItemTextW(dialog, IDC_MIGRATION_SUMMARY, context->owner->report.Summary().c_str());
        for (const auto& item : context->owner->report.files) {
            const auto& file = item.second;
            context->rows.push_back({file.category, AnalysisCategory(file.category), Utf8ToWide(item.first),
                file.sizeKnown ? FormatBytes(file.bytes) : L"—", Utf8ToWide(file.error)});
        }
        for (const wchar_t* label : {L"Tous les fichiers", L"Nouveaux", L"Modifiés", L"Identiques", L"Conservés sur Google Drive", L"Erreurs"})
            ComboBox_AddString(GetDlgItem(dialog, IDC_REPORT_FILTER), label);
        ComboBox_SetCurSel(GetDlgItem(dialog, IDC_REPORT_FILTER), 0);
        const HWND list = GetDlgItem(dialog, IDC_REPORT_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        RECT bounds = {};
        GetClientRect(list, &bounds);
        const int width = bounds.right;
        int index = 0;
        for (const wchar_t* title : {L"Résultat", L"Chemin du fichier", L"Taille à copier"}) {
            LVCOLUMNW column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(title);
            column.cx = index == 0 ? width * 25 / 100 : index == 1 ? width * 57 / 100 : width * 16 / 100;
            ListView_InsertColumn(list, index++, &column);
        }
        FilterReport(dialog, *context);
        WriteMilestone(*context->owner, L"report-open.json", context->rows.size() == context->owner->report.files.size() &&
            ListView_GetItemCount(list) == static_cast<int>(context->rows.size()) &&
            ui::ControlText(dialog, IDC_MIGRATION_SUMMARY) == context->owner->report.Summary());
        return TRUE;
    }
    if (!context) return FALSE;
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, 0); return TRUE; }
        if (LOWORD(wParam) == IDC_REPORT_FILTER && HIWORD(wParam) == CBN_SELCHANGE) { FilterReport(dialog, *context); return TRUE; }
    } else if (message == WM_NOTIFY) {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == IDC_REPORT_LIST && header->code == LVN_GETDISPINFOW) {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);
            if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 && static_cast<size_t>(info->item.iItem) < context->visible.size()) {
                const auto& row = context->rows[context->visible[info->item.iItem]];
                const auto& text = info->item.iSubItem == 0 ? row.status : info->item.iSubItem == 1 ? row.path : row.size;
                wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
            }
            return TRUE;
        }
        if (header->idFrom == IDC_REPORT_LIST && header->code == LVN_ITEMCHANGED) {
            const int selected = ListView_GetNextItem(header->hwndFrom, -1, LVNI_SELECTED);
            if (selected >= 0 && static_cast<size_t>(selected) < context->visible.size()) {
                const auto& row = context->rows[context->visible[selected]];
                SetDlgItemTextW(dialog, IDC_REPORT_SELECTED, (row.path + (row.error.empty() ? L"" : L"\r\n" + row.error)).c_str());
            }
        }
    } else if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORDLG) {
        return context->theme.Color(reinterpret_cast<HDC>(wParam));
    } else if (message == WM_CLOSE) { EndDialog(dialog, 0); return TRUE; }
    return FALSE;
}

INT_PTR CALLBACK MigrationDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<DialogContext*>(lParam);
        context->dialog = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        context->theme.Initialize(dialog, IDC_DIALOG_HEADING);
        context->images.Load(context->instance);
        SendDlgItemMessageW(dialog, IDC_MIGRATION_PROGRESS, PBM_SETRANGE32, 0, 100);
        context->configPath = LocalAppDataPath() + L"\\CloudNav\\Migration\\rclone.conf";
        context->logPath = LocalAppDataPath() + L"\\CloudNav\\Migration\\migration.log";
        EnsureParentDirectory(context->configPath);
        context->oneDriveReady = context->demoMode || HasRemote(context->configPath, kOneDriveRemote);
        context->googleReady = context->demoMode || HasRemote(context->configPath, kGoogleRemote);
        SetDlgItemTextW(dialog, IDC_MIGRATION_ONEDRIVE_STATUS, context->demoMode ? L"Compte de démonstration" : context->oneDriveReady ? L"Connexion enregistrée" : L"Non connecté");
        SetDlgItemTextW(dialog, IDC_MIGRATION_GOOGLE_STATUS, context->demoMode ? L"Compte de démonstration" : context->googleReady ? L"Connexion enregistrée" : L"Non connecté");
        if (context->demoMode) Button_SetCheck(GetDlgItem(dialog, IDC_MIGRATION_REMINDER), BST_CHECKED);
        if (context->oneDriveReady && context->googleReady)
            SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"L’analyse vérifie l’accès aux comptes et estime la copie. Aucun fichier n’est transféré.");
        SendDlgItemMessageW(dialog, IDC_MIGRATION_PHASE, WM_SETFONT, reinterpret_cast<WPARAM>(context->theme.bold), TRUE);
        RefreshButtons(*context);
        return TRUE;
    }
    if (!context) return FALSE;
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDC_MIGRATION_REMINDER: RefreshButtons(*context); return TRUE;
        case IDC_MIGRATION_ONEDRIVE_CONNECT: StartTask(*context, Task::AuthenticateOneDrive); return TRUE;
        case IDC_MIGRATION_GOOGLE_CONNECT: StartTask(*context, Task::AuthenticateGoogle); return TRUE;
        case IDC_MIGRATION_ANALYZE: StartTask(*context, Task::Analyze); return TRUE;
        case IDC_MIGRATION_REPORT: {
            if (context->running || !context->report.available) return TRUE;
            ReportDialog report;
            report.owner = context;
            DialogBoxParamW(context->instance, MAKEINTRESOURCEW(IDD_MIGRATION_REPORT), dialog,
                ReportDialogProc, reinterpret_cast<LPARAM>(&report));
            return TRUE;
        }
        case IDC_MIGRATION_COPY: StartTask(*context, Task::CopyAndVerify); return TRUE;
        case IDC_MIGRATION_CUTOVER: {
            if (context->running || !context->verified) return TRUE;
            const bool passed = context->analyzed && context->verified && context->sawAnalyzeProgress && context->sawCopyProgress &&
                context->sawVerifyProgress && context->progressConsistent && context->cancellationConsistent &&
                LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDC_MIGRATION_CUTOVER;
            if (context->demoMode) WriteEvidence(context->demoResultPath, passed
                ? "{\"passed\":true,\"verified\":true,\"progressConsistent\":true,\"cutoverPrimary\":true}"
                : "{\"passed\":false}");
            EndDialog(dialog, 2); return TRUE;
        }
        case IDCANCEL:
            if (context->running) CancelTask(*context); else EndDialog(dialog, 1);
            return TRUE;
        default: break;
        }
    } else if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORDLG) {
        return context->theme.Color(reinterpret_cast<HDC>(wParam));
    } else if (message == WM_DRAWITEM) {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && (item->CtlID == IDC_MIGRATION_ONEDRIVE_ICON || item->CtlID == IDC_MIGRATION_GOOGLE_ICON)) {
            FillRect(item->hDC, &item->rcItem, context->theme.background);
            context->images.Draw(item->hDC, item->rcItem, item->CtlID == IDC_MIGRATION_ONEDRIVE_ICON);
            return TRUE;
        }
    } else if (message == WM_CLOSE) {
        if (context->running) { context->closeRequested = true; CancelTask(*context); }
        else EndDialog(dialog, 1);
        return TRUE;
    } else if (message == WM_MIGRATION_PROGRESS) {
        auto* update = reinterpret_cast<ProgressUpdate*>(lParam);
        if (context->cancelRequested) { delete update; return TRUE; }
        SetMigrationProgress(*context, update->percent);
        SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, MigrationStageTitle(update->stage));
        SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, update->statistics.c_str());
        SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, update->details.c_str());
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        const bool consistent = ui::ControlText(dialog, IDC_MIGRATION_PHASE) == MigrationStageTitle(update->stage) &&
            ui::ControlText(dialog, IDC_MIGRATION_DETAILS) == MigrationStageDetails(update->stage);
        context->progressConsistent &= consistent;
        if (update->stage == MigrationStage::Analyzing && !context->sawAnalyzeProgress &&
            update->statistics.find(L"fichiers comparés") != std::wstring::npos) {
            context->sawAnalyzeProgress = consistent && context->indeterminate &&
                (GetWindowLongPtrW(GetDlgItem(dialog, IDC_MIGRATION_PROGRESS), GWL_STYLE) & PBS_MARQUEE) != 0 &&
                ui::ControlText(dialog, IDC_MIGRATION_STATS) == update->statistics &&
                update->statistics.find(L'%') == std::wstring::npos && update->statistics.find(L"ETA") == std::wstring::npos;
            WriteMilestone(*context, L"analysis-progress.json", context->sawAnalyzeProgress);
        }
        if (update->stage == MigrationStage::Copying && update->percent >= 30 && !context->sawCopyProgress) {
            context->sawCopyProgress = true;
            WriteMilestone(*context, L"copy-progress.json", consistent);
        }
        if (update->stage == MigrationStage::Verifying && !context->sawVerifyProgress) {
            context->sawVerifyProgress = true;
            WriteMilestone(*context, L"verify-progress.json", consistent);
        }
        delete update;
        return TRUE;
    } else if (message == WM_MIGRATION_COMPLETE) {
        auto* update = reinterpret_cast<CompletionUpdate*>(lParam);
        if (context->worker) { CloseHandle(context->worker); context->worker = nullptr; }
        context->running = false;
        if (update->task == Task::Analyze) {
            context->report = std::move(update->report);
            SetDlgItemTextW(dialog, IDC_MIGRATION_SUMMARY, context->report.Summary().c_str());
        }
        SetMigrationProgress(*context, 0);
        if (update->success) {
            if (update->task == Task::AuthenticateOneDrive || update->task == Task::AuthenticateGoogle) {
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, L"1 / 3 — Analyser avant de copier");
                SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"Analyse requise après une reconnexion");
            }
            if (update->task == Task::AuthenticateOneDrive) {
                context->oneDriveReady = true; SetDlgItemTextW(dialog, IDC_MIGRATION_ONEDRIVE_STATUS, L"Connexion enregistrée");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Compte OneDrive connecté.");
            } else if (update->task == Task::AuthenticateGoogle) {
                context->googleReady = true; SetDlgItemTextW(dialog, IDC_MIGRATION_GOOGLE_STATUS, L"Connexion enregistrée");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Compte Google Drive connecté.");
            } else if (update->task == Task::Analyze) {
                context->analyzed = true;
                SetMigrationProgress(*context, 100);
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, L"1 / 3 — Analyse terminée : prête pour la copie");
                SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"Analyse terminée — aucun fichier transféré");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Tu peux lancer la copie. Les fichiers Google Drive supplémentaires seront conservés.");
            } else if (update->task == Task::CopyAndVerify) {
                context->verified = true;
                SetMigrationProgress(*context, 100);
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, L"3 / 3 — Migration copiée et vérifiée");
                SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"100 % — vérification réussie");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Tu peux maintenant configurer les dossiers Windows, ou fermer l’assistant.");
            }
        } else {
            SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, update->cancelled ? L"Opération annulée." : L"Opération interrompue.");
            SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"Opération arrêtée — aucun transfert en cours");
            SendDlgItemMessageW(dialog, IDC_MIGRATION_PROGRESS, PBM_SETSTATE, update->cancelled ? PBST_PAUSED : PBST_ERROR, 0);
            SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS,
                            update->cancelled ? L"Reprends quand tu veux : les fichiers identiques seront ignorés." : update->message.c_str());
        }
        const bool close = context->closeRequested;
        delete update;
        RefreshButtons(*context);
        if (context->verified || context->analyzed)
            SendMessageW(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dialog,
                context->verified ? IDC_MIGRATION_CUTOVER : IDC_MIGRATION_COPY)), TRUE);
        if (context->analyzed && !context->verified)
            WriteMilestone(*context, L"analysis-ready.json", context->sawAnalyzeProgress && !context->indeterminate &&
                IsWindowEnabled(GetDlgItem(dialog, IDC_MIGRATION_COPY)) != FALSE);
        if (context->report.complete) WriteMilestone(*context, L"analysis-summary.json",
            ui::ControlText(dialog, IDC_MIGRATION_SUMMARY) == context->report.Summary() &&
            IsWindowEnabled(GetDlgItem(dialog, IDC_MIGRATION_REPORT)) != FALSE);
        if (context->verified)
            WriteMilestone(*context, L"verified.json", LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDC_MIGRATION_CUTOVER);
        if (context->cancelRequested) {
            context->cancellationConsistent &= !context->verified &&
                !IsWindowEnabled(GetDlgItem(dialog, IDC_MIGRATION_CUTOVER)) &&
                ui::ControlText(dialog, IDC_MIGRATION_STATS) == L"Opération arrêtée — aucun transfert en cours";
            WriteMilestone(*context, L"cancelled.json", context->cancellationConsistent);
        }
        if (close) EndDialog(dialog, 1);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

MigrationResult ShowMigrationDialog(HWND owner, HINSTANCE instance, bool demoMode,
                                    const std::wstring& demoResultPath) {
    DialogContext context;
    context.instance = instance;
    context.demoMode = demoMode;
    context.demoResultPath = demoResultPath;
    InitializeCriticalSection(&context.processLock);
    const INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CLOUD_MIGRATION), owner,
                                           MigrationDialogProc, reinterpret_cast<LPARAM>(&context));
    if (context.worker) { CancelTask(context); WaitForSingleObject(context.worker, 5000); CloseHandle(context.worker); }
    DeleteCriticalSection(&context.processLock);
    return result == 2 ? MigrationResult::ConfigureFolders : MigrationResult::Closed;
}

int RunEmbeddedRcloneSelfTest(HINSTANCE instance, const std::wstring& resultPath) {
    std::wstring runtime;
    std::wstring error;
    DialogContext context;
    context.instance = instance;
    context.runtimePath = runtime;
    context.logPath = LocalAppDataPath() + L"\\CloudNav\\Migration\\self-test.log";
    EnsureParentDirectory(context.logPath);
    InitializeCriticalSection(&context.processLock);
    bool passed = ExtractRclone(instance, runtime, error) && ResourceMatchesFile(instance, runtime);
    context.runtimePath = runtime;
    if (passed) passed = RunProcess(context, {L"version"}, MigrationStage::Preparing, error);
    std::string step = "embeddedEngine";
    try {
        const std::wstring root = resultPath + L".fixtures";
        const std::wstring source = root + L"\\source";
        const std::wstring destination = root + L"\\destination";
        context.configPath = root + L"\\isolated-rclone.conf";
        // Only synthetic local paths and an empty config: no account credentials
        // or cloud requests are used by this integration test.
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("fixture read failed");
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
        const auto snapshot = [&](const std::wstring& path) {
            std::map<std::wstring, std::pair<std::filesystem::file_time_type, std::string>> result;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                result[entry.path().lexically_relative(path).wstring()] = {
                    entry.last_write_time(), entry.is_directory() ? "directory" : "file:" + read(entry.path())};
            }
            return result;
        };
        AnalysisReport analysisReport;
        const auto run = [&](MigrationStage stage, DWORD* exitCode = nullptr) {
            error.clear();
            return RunProcess(context, MigrationArguments(stage, context.configPath, source, destination), stage, error, exitCode,
                stage == MigrationStage::Analyzing ? &analysisReport : nullptr);
        };
        if (passed) {
            step = "fixtures";
            passed = WriteEvidence(context.configPath, "") &&
                WriteEvidence(source + L"\\same.txt", "identical") &&
                WriteEvidence(destination + L"\\same.txt", "identical") &&
                WriteEvidence(source + L"\\changed.txt", "replacement contents") &&
                WriteEvidence(destination + L"\\changed.txt", "old") &&
                WriteEvidence(source + L"\\Personal Vault\\excluded.txt", "excluded") &&
                WriteEvidence(destination + L"\\extra.txt", "keep this file");
            for (int i = 0; passed && i < 32; ++i)
                passed = WriteEvidence(source + L"\\nested " + std::to_wstring(i) + L"\\été.txt", "fixture " + std::to_string(i));
            if (passed) std::filesystem::create_directories(source + L"\\empty directory");
        }
        if (passed) {
            const auto beforeSource = snapshot(source), beforeDestination = snapshot(destination);
            step = "readOnlyAnalysis";
            passed = run(MigrationStage::Analyzing) && snapshot(source) == beforeSource && snapshot(destination) == beforeDestination;
            if (passed) {
                step = "analysisReportCounts";
                passed = analysisReport.complete && analysisReport.Count('+') == 32 && analysisReport.Count('*') == 1 &&
                    analysisReport.Count('=') == 1 && analysisReport.Count('-') == 1 && analysisReport.Count('!') == 0 &&
                    analysisReport.CopySize() != L"indisponible" && analysisReport.files.count("Personal Vault/excluded.txt") == 0;
            }
            if (passed) {
                step = "copyAndVerify";
                passed = run(MigrationStage::Copying) && run(MigrationStage::Verifying) &&
                    snapshot(source) == beforeSource && read(destination + L"\\changed.txt") == "replacement contents" &&
                    read(destination + L"\\extra.txt") == "keep this file" &&
                    read(destination + L"\\nested 31\\été.txt") == "fixture 31" &&
                    std::filesystem::is_directory(destination + L"\\empty directory") &&
                    !std::filesystem::exists(destination + L"\\Personal Vault");
            }
            if (passed) {
                step = "verificationDetectsMismatch";
                // Same length and timestamp: verification must detect changed
                // contents, rather than accidentally becoming a size-only check.
                const std::wstring changed = destination + L"\\changed.txt";
                const auto stamp = std::filesystem::last_write_time(changed);
                passed = WriteEvidence(changed, "Replacement contents");
                std::filesystem::last_write_time(changed, stamp);
                DWORD exitCode = ERROR_GEN_FAILURE;
                if (passed) passed = !run(MigrationStage::Verifying, &exitCode) && exitCode == 1;
            }
        }
    } catch (const std::exception&) {
        passed = false;
    }
    DeleteCriticalSection(&context.processLock);
    if (!EnsureParentDirectory(resultPath)) return 3;
    HANDLE file = CreateFileW(resultPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 4;
    const std::string json = passed
        ? "{\"passed\":true,\"embeddedVersion\":\"1.75.0\",\"readOnlyAnalysis\":true,\"copyAndVerify\":true,\"verificationDetectsMismatch\":true}\n"
        : "{\"passed\":false,\"failedStep\":\"" + step + "\"}\n";
    DWORD written = 0;
    const bool wrote = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();
    CloseHandle(file);
    return passed && wrote ? 0 : 1;
}

}  // namespace cloudnav
