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
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "logic.h"
#include "migration.h"
#include "migration_logic.h"
#include "resource.h"

namespace cloudnav {
namespace {

constexpr wchar_t kOneDriveRemote[] = L"cloudnav-onedrive";
constexpr wchar_t kGoogleRemote[] = L"cloudnav-gdrive";
constexpr UINT WM_MIGRATION_PROGRESS = WM_APP + 41;
constexpr UINT WM_MIGRATION_COMPLETE = WM_APP + 42;

enum class Task { None, AuthenticateOneDrive, AuthenticateGoogle, Analyze, CopyAndVerify };

struct ProgressUpdate {
    int percent = 0;
    std::wstring phase;
    std::wstring statistics;
    std::wstring details;
};

struct CompletionUpdate {
    Task task = Task::None;
    bool success = false;
    bool cancelled = false;
    std::wstring message;
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

void PostProgress(DialogContext& context, int percent, const std::wstring& phase,
                  const std::wstring& statistics, const std::wstring& details = {}) {
    auto* update = new ProgressUpdate{percent, phase, statistics, details};
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
                const std::wstring& phase, std::wstring& error) {
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) { error = L"Impossible de lire la progression."; return false; }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = QuoteArgument(context.runtimePath);
    for (const auto& argument : arguments) command += L" " + QuoteArgument(argument);
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
    LeaveCriticalSection(&context.processLock);

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
            if (line.find("\"stats\"") != std::string::npos || line.find("\"totalBytes\"") != std::string::npos) {
                double bytes = 0, total = 0, checks = 0, totalChecks = 0, speed = 0, eta = -1;
                JsonNumber(line, "bytes", bytes); JsonNumber(line, "totalBytes", total);
                JsonNumber(line, "checks", checks); JsonNumber(line, "totalChecks", totalChecks);
                JsonNumber(line, "speed", speed); JsonNumber(line, "eta", eta);
                const int percent = MigrationPercent(static_cast<std::uint64_t>(bytes), static_cast<std::uint64_t>(total),
                                                     static_cast<std::uint64_t>(checks), static_cast<std::uint64_t>(totalChecks));
                std::wstring stats = std::to_wstring(percent) + L" % — " +
                    FormatBytes(static_cast<std::uint64_t>(bytes)) + L" / " +
                    FormatBytes(static_cast<std::uint64_t>(total));
                if (speed > 0) stats += L" — " + FormatBytes(static_cast<std::uint64_t>(speed)) + L"/s";
                stats += L" — " + FormatEta(eta);
                PostProgress(context, percent, phase, stats);
            }
        }
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    CloseHandle(readPipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    EnterCriticalSection(&context.processLock);
    context.childProcess = nullptr;
    LeaveCriticalSection(&context.processLock);
    CloseHandle(process.hProcess);
    if (context.cancelRequested || exitCode == ERROR_CANCELLED) return false;
    if (exitCode != 0) {
        error = L"rclone a signalé une erreur (code " + std::to_wstring(exitCode) + L"). Journal : " + context.logPath;
        return false;
    }
    return true;
}

std::vector<std::wstring> TransferArguments(DialogContext& context, bool dryRun) {
    std::vector<std::wstring> args = {L"copy", std::wstring(kOneDriveRemote) + L":", std::wstring(kGoogleRemote) + L":",
        L"--config", context.configPath, L"--check-first", L"--create-empty-src-dirs", L"--drive-skip-gdocs",
        L"--exclude", L"/Personal Vault/**", L"--use-json-log", L"--stats", L"1s", L"--stats-log-level", L"INFO"};
    if (dryRun) args.push_back(L"--dry-run");
    return args;
}

DWORD WINAPI WorkerProc(void* parameter) {
    auto& context = *static_cast<DialogContext*>(parameter);
    bool success = false;
    std::wstring error;
    if (!ExtractRclone(context.instance, context.runtimePath, error)) {
        success = false;
    } else if (context.demoMode) {
        const wchar_t* phase = context.task == Task::Analyze ? L"Analyse des écarts…" : L"Copie OneDrive → Google Drive…";
        const int delay = context.task == Task::Analyze ? 22 : 35;
        for (int percent = 0; percent <= 100 && !context.cancelRequested; percent += 2) {
            const std::uint64_t total = 8ULL * 1024 * 1024 * 1024;
            const auto done = total * static_cast<std::uint64_t>(percent) / 100;
            PostProgress(context, percent, phase, std::to_wstring(percent) + L" % — " + FormatBytes(done) +
                         L" / " + FormatBytes(total) + L" — 42.0 Mo/s — " + FormatEta((100 - percent) * 1.9));
            Sleep(delay);
        }
        if (!context.cancelRequested && context.task == Task::CopyAndVerify) {
            for (int percent = 0; percent <= 100 && !context.cancelRequested; percent += 4) {
                PostProgress(context, percent, L"Vérification indépendante…",
                             std::to_wstring(percent) + L" % — contrôle des fichiers — " + FormatEta((100 - percent) * 0.4));
                Sleep(22);
            }
        }
        success = !context.cancelRequested;
    } else if (context.task == Task::AuthenticateOneDrive || context.task == Task::AuthenticateGoogle) {
        const bool oneDrive = context.task == Task::AuthenticateOneDrive;
        const wchar_t* remote = oneDrive ? kOneDriveRemote : kGoogleRemote;
        std::vector<std::wstring> args;
        if (HasRemote(context.configPath, remote)) args = {L"config", L"reconnect", std::wstring(remote) + L":", L"--config", context.configPath};
        else args = {L"config", L"create", remote, oneDrive ? L"onedrive" : L"drive", L"config_is_local=true", L"--config", context.configPath};
        success = RunProcess(context, args, L"Connexion du compte…", error);
    } else if (context.task == Task::Analyze) {
        success = RunProcess(context, TransferArguments(context, true), L"Analyse des écarts…", error);
    } else if (context.task == Task::CopyAndVerify) {
        success = RunProcess(context, TransferArguments(context, false), L"Copie OneDrive → Google Drive…", error);
        if (success && !context.cancelRequested) {
            PostProgress(context, 0, L"Vérification indépendante…", L"0 % — comparaison OneDrive / Google Drive");
            const std::vector<std::wstring> verify = {L"check", std::wstring(kOneDriveRemote) + L":", std::wstring(kGoogleRemote) + L":",
                L"--one-way", L"--config", context.configPath, L"--drive-skip-gdocs", L"--exclude", L"/Personal Vault/**",
                L"--use-json-log", L"--stats", L"1s", L"--stats-log-level", L"INFO"};
            success = RunProcess(context, verify, L"Vérification indépendante…", error);
        }
    }
    auto* completion = new CompletionUpdate{context.task, success, context.cancelRequested.load(), error};
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
    SetDlgItemTextW(context.dialog, IDCANCEL, context.running ? L"Annuler" : L"Fermer");
}

void StartTask(DialogContext& context, Task task) {
    if (context.running) return;
    context.running = true;
    context.task = task;
    context.cancelRequested = false;
    SendDlgItemMessageW(context.dialog, IDC_MIGRATION_PROGRESS, PBM_SETPOS, 0, 0);
    const wchar_t* phase = L"Préparation…";
    if (task == Task::Analyze) phase = L"Préparation de l’analyse…";
    else if (task == Task::CopyAndVerify) phase = L"Préparation de la copie…";
    else phase = L"Ouverture de la connexion sécurisée…";
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_PHASE, phase);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS,
                    L"CloudNav prépare son moteur intégré. Tu peux annuler sans perdre les fichiers déjà validés.");
    RefreshButtons(context);
    context.worker = CreateThread(nullptr, 0, WorkerProc, &context, 0, nullptr);
    if (!context.worker) {
        context.running = false;
        SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, L"Impossible de démarrer l’opération.");
        RefreshButtons(context);
    }
}

void CancelTask(DialogContext& context) {
    context.cancelRequested = true;
    EnterCriticalSection(&context.processLock);
    if (context.childProcess) TerminateProcess(context.childProcess, ERROR_CANCELLED);
    LeaveCriticalSection(&context.processLock);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, L"Annulation… Les fichiers déjà copiés seront réutilisés à la reprise.");
}

bool WriteDemoResult(const std::wstring& path) {
    if (path.empty()) return true;
    if (!EnsureParentDirectory(path)) return false;
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char json[] = "{\"passed\":true,\"copyDirection\":\"onedrive-to-google-drive\",\"verified\":true,\"cutoverOffered\":true}\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, json, static_cast<DWORD>(sizeof(json) - 1), &written, nullptr) && written == sizeof(json) - 1;
    CloseHandle(file);
    return ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

INT_PTR CALLBACK MigrationDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<DialogContext*>(lParam);
        context->dialog = dialog;
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        SendDlgItemMessageW(dialog, IDC_MIGRATION_PROGRESS, PBM_SETRANGE32, 0, 100);
        context->configPath = LocalAppDataPath() + L"\\CloudNav\\Migration\\rclone.conf";
        context->logPath = LocalAppDataPath() + L"\\CloudNav\\Migration\\migration.log";
        EnsureParentDirectory(context->configPath);
        context->oneDriveReady = context->demoMode || HasRemote(context->configPath, kOneDriveRemote);
        context->googleReady = context->demoMode || HasRemote(context->configPath, kGoogleRemote);
        SetDlgItemTextW(dialog, IDC_MIGRATION_ONEDRIVE_STATUS, context->oneDriveReady ? L"Prêt" : L"Non connecté");
        SetDlgItemTextW(dialog, IDC_MIGRATION_GOOGLE_STATUS, context->googleReady ? L"Prêt" : L"Non connecté");
        if (context->demoMode) Button_SetCheck(GetDlgItem(dialog, IDC_MIGRATION_REMINDER), BST_CHECKED);
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
        case IDC_MIGRATION_COPY: StartTask(*context, Task::CopyAndVerify); return TRUE;
        case IDC_MIGRATION_CUTOVER:
            WriteDemoResult(context->demoResultPath);
            EndDialog(dialog, 2); return TRUE;
        case IDCANCEL:
            if (context->running) CancelTask(*context); else EndDialog(dialog, 1);
            return TRUE;
        default: break;
        }
    } else if (message == WM_CLOSE) {
        if (context->running) { context->closeRequested = true; CancelTask(*context); }
        else EndDialog(dialog, 1);
        return TRUE;
    } else if (message == WM_MIGRATION_PROGRESS) {
        auto* update = reinterpret_cast<ProgressUpdate*>(lParam);
        SendDlgItemMessageW(dialog, IDC_MIGRATION_PROGRESS, PBM_SETPOS, update->percent, 0);
        SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, update->phase.c_str());
        SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, update->statistics.c_str());
        if (!update->details.empty()) SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, update->details.c_str());
        delete update;
        return TRUE;
    } else if (message == WM_MIGRATION_COMPLETE) {
        auto* update = reinterpret_cast<CompletionUpdate*>(lParam);
        if (context->worker) { CloseHandle(context->worker); context->worker = nullptr; }
        context->running = false;
        if (update->success) {
            if (update->task == Task::AuthenticateOneDrive) {
                context->oneDriveReady = true; SetDlgItemTextW(dialog, IDC_MIGRATION_ONEDRIVE_STATUS, L"Prêt");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Compte OneDrive connecté.");
            } else if (update->task == Task::AuthenticateGoogle) {
                context->googleReady = true; SetDlgItemTextW(dialog, IDC_MIGRATION_GOOGLE_STATUS, L"Prêt");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Compte Google Drive connecté.");
            } else if (update->task == Task::Analyze) {
                context->analyzed = true;
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, L"Analyse terminée.");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Tu peux lancer la copie. Les fichiers Google Drive supplémentaires seront conservés.");
            } else if (update->task == Task::CopyAndVerify) {
                context->verified = true;
                SendDlgItemMessageW(dialog, IDC_MIGRATION_PROGRESS, PBM_SETPOS, 100, 0);
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, L"Migration copiée et vérifiée.");
                SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"100 % — vérification réussie");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Aucune suppression côté Google Drive. La bascule des dossiers Windows est maintenant facultative.");
            }
        } else {
            SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, update->cancelled ? L"Opération annulée." : L"Opération interrompue.");
            SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS,
                            update->cancelled ? L"Reprends quand tu veux : les fichiers identiques seront ignorés." : update->message.c_str());
        }
        const bool close = context->closeRequested;
        delete update;
        RefreshButtons(*context);
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
    if (passed) passed = RunProcess(context, {L"version"}, L"Vérification rclone…", error);
    DeleteCriticalSection(&context.processLock);
    if (!EnsureParentDirectory(resultPath)) return 3;
    HANDLE file = CreateFileW(resultPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 4;
    const std::string json = passed ? "{\"passed\":true,\"embeddedVersion\":\"1.75.0\"}\n" : "{\"passed\":false}\n";
    DWORD written = 0;
    const bool wrote = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();
    CloseHandle(file);
    return passed && wrote ? 0 : 1;
}

}  // namespace cloudnav
