#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
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
#include "sync_logic.h"
#include "resource.h"
#include "ui.h"

#pragma comment(lib, "bcrypt.lib")

namespace cloudnav {
namespace {

constexpr wchar_t kOneDriveRemote[] = L"cloudnav-onedrive";
constexpr wchar_t kGoogleRemote[] = L"cloudnav-gdrive";
constexpr UINT WM_MIGRATION_PROGRESS = WM_APP + 41;
constexpr UINT WM_MIGRATION_COMPLETE = WM_APP + 42;

using Task = MigrationTask;

const wchar_t* SyncSettingsKey(bool demoMode) {
    return demoMode ? L"Software\\CloudNav\\Demo" : L"Software\\CloudNav";
}

SyncMode LoadSyncMode(bool demoMode) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, SyncSettingsKey(demoMode), L"SyncDirection", RRF_RT_REG_DWORD,
        nullptr, &value, &size) != ERROR_SUCCESS) return SyncMode::ToGoogle;
    return SyncModeFromSetting(value);
}

bool SaveSyncMode(bool demoMode, SyncMode mode) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SyncSettingsKey(demoMode), 0, nullptr, 0, KEY_SET_VALUE,
        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = static_cast<DWORD>(mode);
    const auto status = RegSetValueExW(key, L"SyncDirection", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

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
    SyncAnalysis sync;
};

struct DialogContext {
    HWND dialog = nullptr;
    HINSTANCE instance = nullptr;
    bool demoMode = false;
    bool running = false;
    bool closeRequested = false;
    bool analyzed = false;
    bool copied = false;
    bool oneDriveReady = false;
    bool googleReady = false;
    bool sawAnalyzeProgress = false;
    bool sawCopyProgress = false;
    bool sawVerifyProgress = false;
    bool indeterminate = false;
    bool progressConsistent = true;
    bool cancellationConsistent = true;
    AnalysisReport report;
    SyncAnalysis sync;
    SyncMode mode = SyncMode::ToGoogle;
    ui::DialogTheme theme;
    std::atomic<unsigned> statisticsUpdates = 0;
    std::atomic<unsigned> inventoryReads = 0;
    std::atomic<unsigned> listingProgressUpdates = 0;
    std::atomic<bool> listingFailed = false;
    bool parallelListing = false;
    std::wstring listingStatus[2];
    unsigned peakChildProcesses = 0;
    ui::ProviderImages images;
    Task task = Task::None;
    std::wstring demoResultPath;
    std::wstring runtimePath;
    std::wstring configPath;
    std::wstring logPath;
    std::wstring oneDrivePath = L"cloudnav-onedrive:";
    std::wstring googlePath = L"cloudnav-gdrive:";
    HANDLE worker = nullptr;
    std::vector<HANDLE> childProcesses;
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
    if (!resource) { error = L"The embedded rclone resource was not found."; return false; }
    HGLOBAL loaded = LoadResource(instance, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(instance, resource);
    if (!bytes || !size || !EnsureParentDirectory(path)) { error = L"Unable to prepare the transfer engine."; return false; }
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"Unable to extract the transfer engine."; return false; }
    DWORD written = 0;
    const bool writtenOk = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!writtenOk || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = L"Unable to update the transfer engine.";
        return false;
    }
    if (!ResourceMatchesFile(instance, path)) { error = L"Transfer engine verification failed."; return false; }
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

void PostListingProgress(DialogContext& context, bool oneDrive, MigrationStage stage, const std::wstring& text) {
    if (!context.parallelListing) { PostProgress(context, -1, stage, text); return; }
    EnterCriticalSection(&context.processLock);
    context.listingStatus[oneDrive ? 0 : 1] = text;
    if (context.dialog) {
        auto* update = new ProgressUpdate{-1, stage, context.listingStatus[0], context.listingStatus[1]};
        if (!PostMessageW(context.dialog, WM_MIGRATION_PROGRESS, 0, reinterpret_cast<LPARAM>(update))) delete update;
    }
    LeaveCriticalSection(&context.processLock);
}

bool RunProcess(DialogContext& context, const std::vector<std::wstring>& arguments,
                MigrationStage stage, std::wstring& error, DWORD* processExitCode = nullptr, AnalysisReport* report = nullptr,
                std::string* captured = nullptr) {
    if (context.cancelRequested || (captured && context.listingFailed)) return false;
    // Serialize only process creation, so concurrent children cannot inherit
    // each other's temporary inheritable pipe handles.
    static std::mutex launchMutex;
    std::unique_lock<std::mutex> launchLock(launchMutex);
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) { error = L"Unable to read progress."; return false; }
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
    // Bind every operation to this configuration, not ambient rclone remotes,
    // filters, credentials or destructive flags inherited from another tool.
    std::vector<wchar_t> environment;
    LPWCH inherited = GetEnvironmentStringsW();
    if (inherited) {
        for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1) {
            if (_wcsnicmp(entry, L"RCLONE_", 7) != 0)
                environment.insert(environment.end(), entry, entry + wcslen(entry) + 1);
        }
        FreeEnvironmentStringsW(inherited);
    }
    environment.push_back(0);
    if (environment.size() == 1) environment.push_back(0);
    const BOOL created = CreateProcessW(context.runtimePath.c_str(), mutableCommand.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &process);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    CloseHandle(writePipe);
    if (!created) { CloseHandle(readPipe); error = L"Unable to start the transfer engine."; return false; }
    CloseHandle(process.hThread);
    EnterCriticalSection(&context.processLock);
    context.childProcesses.push_back(process.hProcess);
    context.peakChildProcesses = (std::max)(context.peakChildProcesses, static_cast<unsigned>(context.childProcesses.size()));
    if (context.cancelRequested || (captured && context.listingFailed)) TerminateProcess(process.hProcess, ERROR_CANCELLED);
    LeaveCriticalSection(&context.processLock);
    launchLock.unlock();

    context.statisticsUpdates = 0;
    const bool listingOneDrive = arguments.size() > 1 && arguments[1] == context.oneDrivePath;
    const ULONGLONG listingStarted = GetTickCount64();
    if (captured) PostListingProgress(context, listingOneDrive, stage, SyncListingProgress(listingOneDrive, 0, 0));
    else PostProgress(context, -1, stage, stage == MigrationStage::Copying
        ? L"Preparing copy — opening selected files…"
        : L"Reading accounts — waiting for initial statistics…");
    std::string pending;
    size_t receivedFiles = 0;
    ULONGLONG lastInventoryUpdate = GetTickCount64();
    char buffer[8192];
    DWORD read = 0;
    HANDLE log = captured ? INVALID_HANDLE_VALUE : CreateFileW(context.logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    for (;;) {
        if (context.cancelRequested || (captured && context.listingFailed)) TerminateProcess(process.hProcess, ERROR_CANCELLED);
        // A blocking pipe read freezes progress during provider enumeration.
        // Poll availability so even a silent listing gets an honest heartbeat.
        const auto now = GetTickCount64();
        if (captured && now - lastInventoryUpdate >= 1000) {
            PostListingProgress(context, listingOneDrive, stage, SyncListingProgress(listingOneDrive, receivedFiles, (now - listingStarted) / 1000));
            ++context.listingProgressUpdates;
            lastInventoryUpdate = now;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            if (WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0) {
                // Drain final bytes written just before the process exited.
                if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || !available) break;
            } else continue;
        }
        if (!ReadFile(readPipe, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &read, nullptr) || !read) break;
        if (captured) captured->append(buffer, read);
        if (log != INVALID_HANDLE_VALUE) {
            DWORD logged = 0;
            WriteFile(log, buffer, read, &logged, nullptr);
        }
        pending.append(buffer, read);
        size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (captured && !line.empty() && line.front() == '{') {
                auto rowText = line;
                while (!rowText.empty() && (rowText.back() == ',' || rowText.back() == '\r')) rowText.pop_back();
                const auto row = SyncJson::parse(rowText, nullptr, false);
                if (row.is_object() && row.contains("Path") && row.contains("IsDir") && row["IsDir"] == false) ++receivedFiles;
            }
            if (report) report->Log(line);
            MigrationStatistics stats;
            if (ParseMigrationStatistics(line, stats)) {
                ++context.statisticsUpdates;
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
    context.childProcesses.erase(std::remove(context.childProcesses.begin(), context.childProcesses.end(), process.hProcess), context.childProcesses.end());
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
        error = L"rclone reported an error (code " + std::to_wstring(exitCode) + L"). Log: " + context.logPath;
        return false;
    }
    if (report && !report->complete) {
        error = L"The analysis report is incomplete. Analyze again before copying.";
        return false;
    }
    return true;
}

bool WriteEvidence(const std::wstring& path, const std::string& json);

std::string ReadSyncFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

std::wstring SyncStatePath(const DialogContext& context) {
    return context.configPath + L".sync-state.json";
}

std::string SyncBinding(const DialogContext& context) {
    auto identity = SyncBindingMaterial(ReadSyncFile(context.configPath));
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Account fingerprint unavailable");
    unsigned char digest[32] = {};
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(identity.data()),
        static_cast<ULONG>(identity.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Account fingerprint unavailable");
    std::string result;
    for (auto byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return "Invalid Unicode";
    std::string result(static_cast<size_t>(count), 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

bool ReadCurrentSync(DialogContext& context, SyncAnalysis& analysis, std::wstring& error,
    MigrationStage stage = MigrationStage::Analyzing) {
    analysis.binding = SyncBinding(context);
    analysis.complete = false;
    const auto originalConfig = ReadSyncFile(context.configPath);
    const auto configPrefix = context.configPath + L".parallel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    struct ListingConfigs {
        std::wstring paths[2];
        ~ListingConfigs() { for (const auto& path : paths) DeleteFileW(path.c_str()); }
    } configs{{configPrefix + L"-onedrive.conf", configPrefix + L"-google.conf"}};
    for (const auto& path : configs.paths) if (!WriteEvidence(path, originalConfig)) {
        error = L"Unable to prepare account listing configuration."; return false;
    }
    context.listingFailed = false;
    context.parallelListing = true;
    context.listingStatus[0] = SyncListingProgress(true, 0, 0);
    context.listingStatus[1] = SyncListingProgress(false, 0, 0);
    std::wstring errors[2];
    const auto readAccount = [&](bool oneDrive) {
        auto& accountError = errors[oneDrive ? 0 : 1];
        try {
        std::string output, parseError;
        auto& inventory = oneDrive ? analysis.oneDrive : analysis.google;
        const std::wstring remote = oneDrive ? context.oneDrivePath : context.googlePath;
        ++context.inventoryReads;
        const auto log = context.logPath + (oneDrive ? L".onedrive" : L".google");
        if (!RunProcess(context, SyncInventoryArguments(configs.paths[oneDrive ? 0 : 1], remote, log),
            stage, accountError, nullptr, nullptr, &output)) {
            if (!accountError.empty()) accountError += L"\nListing log: " + log;
            context.listingFailed = true;
            return false;
        }
        if (!ReadSyncInventory(output, inventory, parseError)) {
            accountError = Utf8ToWide(parseError); context.listingFailed = true; return false;
        }
        PostListingProgress(context, oneDrive, stage, std::wstring(oneDrive ? L"OneDrive : " : L"Google Drive : ") +
            std::to_wstring(inventory.size()) + L" files read — complete");
        return true;
        } catch (const std::exception& e) {
            accountError = Utf8ToWide(e.what()); context.listingFailed = true; return false;
        }
    };
    bool oneDriveOk = false, googleOk = false;
    try {
        auto oneDrive = std::async(std::launch::async, readAccount, true);
        googleOk = readAccount(false);
        oneDriveOk = oneDrive.get();
    } catch (...) {
        context.parallelListing = false;
        throw;
    }
    context.parallelListing = false;
    // Each child can refresh its own OAuth token without racing the other.
    // Merge only its own section after both children have stopped.
    if (analysis.binding != SyncBinding(context)) { error = L"The accounts changed during analysis. Analyze again."; return false; }
    auto mergedConfig = ReadSyncFile(context.configPath);
    for (int i = 0; i < 2; ++i) {
        const auto updated = ReadSyncFile(configs.paths[i]);
        if (SyncBindingMaterial(updated) != SyncBindingMaterial(originalConfig)) {
            error = L"Account configuration changed during listing. Analyze again."; return false;
        }
        const std::string remote = i == 0 ? "cloudnav-onedrive" : "cloudnav-gdrive";
        const auto from = SyncConfigSection(updated, remote), old = SyncConfigSection(originalConfig, remote);
        if (from.first != std::string::npos && old.first != std::string::npos &&
            updated.substr(from.first, from.second) != originalConfig.substr(old.first, old.second))
            mergedConfig = MergeSyncAccountConfig(mergedConfig, updated, remote);
    }
    if (mergedConfig != ReadSyncFile(context.configPath) && !WriteEvidence(context.configPath, mergedConfig)) {
        error = L"Unable to save refreshed account connections."; return false;
    }
    if (!oneDriveOk || !googleOk) {
        error = !errors[0].empty() ? L"OneDrive: " + errors[0] : L"Google Drive: " + errors[1];
        return false;
    }
    if (analysis.binding != SyncBinding(context)) { error = L"The accounts changed during analysis. Analyze again."; return false; }
    analysis.complete = !context.cancelRequested;
    return analysis.complete;
}

void LoadCurrentBaseline(DialogContext& context, SyncAnalysis& analysis) {
    const auto state = SyncStatePath(context);
    analysis.baselineDocument = ReadSyncFile(state);
    const bool dirty = GetFileAttributesW((state + L".pending").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (dirty || (!analysis.baselineDocument.empty() && !LoadSyncBaseline(analysis.baselineDocument, analysis.binding, analysis))) {
        analysis.hasBaseline = false;
        analysis.recovery = true;
    }
}

AnalysisReport SyncReport(const SyncAnalysis& analysis, SyncMode mode) {
    AnalysisReport report;
    report.available = true;
    report.complete = analysis.complete;
    report.summaryOverride = SyncPlanSummary(analysis, mode);
    for (const auto& row : analysis.Plan(mode)) {
        auto& file = report.files[row.path];
        file.category = row.category;
        file.bytes = row.bytes;
        file.sizeKnown = true;
        if (row.action == SyncAction::Blocked) file.error = "Ambiguous name, path collision, or incomplete analysis.";
    }
    if (!analysis.error.empty()) {
        auto& file = report.files["[analysis]"];
        file.category = '!'; file.error = analysis.error;
    }
    return report;
}

bool ExecuteSyncPlan(DialogContext& context, std::wstring& error) {
    const auto rows = context.sync.Plan(context.mode);
    if (!context.sync.complete || std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.action == SyncAction::Blocked; })) {
        error = L"The plan contains blocked items. Fix them, then analyze again."; return false;
    }
    // Execute the reviewed inventories without listing either account again.
    // Only local account identity and recovery history can invalidate this plan.
    SyncAnalysis currentState;
    currentState.binding = SyncBinding(context);
    LoadCurrentBaseline(context, currentState);
    if (currentState.binding != context.sync.binding || currentState.baselineDocument != context.sync.baselineDocument ||
        currentState.recovery != context.sync.recovery) {
        error = L"Accounts or local history changed since analysis. No transfer started: analyze again to review the plan.";
        return false;
    }
    if (context.mode != SyncMode::Bidirectional && std::all_of(rows.begin(), rows.end(),
        [](const auto& row) { return row.action == SyncAction::None; })) return true;
    const auto stamp = std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(GetCurrentProcessId());
    const auto& od = context.oneDrivePath;
    const auto& gd = context.googlePath;
    const auto backup = [&](const std::wstring& remote) { return remote + L".CloudNav-history/" + stamp; };
    const auto state = SyncStatePath(context);
    // A pending marker makes interruption recovery a reviewed, non-deleting
    // merge. Never interpret a destination missing after a failed overwrite as
    // a user's deletion on the next run.
    if (!WriteEvidence(state + L".pending", "{\"pending\":true}")) {
        error = L"Unable to save the recovery journal."; return false;
    }
    const auto baseArgs = [&](const wchar_t* command, const std::wstring& source, const std::wstring& destination) {
        return std::vector<std::wstring>{command, source, destination, L"--config", context.configPath,
            L"--use-json-log", L"--stats", L"1s", L"--stats-log-level", L"NOTICE", L"--drive-skip-gdocs"};
    };
    const auto transfer = [&](SyncAction action, const std::wstring& source, const std::wstring& destination, bool remove) {
        const auto list = SyncFileList(rows, action);
        if (list.empty()) return true;
        const auto path = context.logPath + L".sync-list-" + stamp;
        if (!WriteEvidence(path, list)) { error = L"Unable to prepare the file list."; return false; }
        auto args = baseArgs(remove ? L"move" : L"copy", source, remove ? backup(source) : destination);
        args.insert(args.end(), {L"--files-from-raw", path, L"--no-traverse"});
        if (!remove) {
            if (context.mode == SyncMode::Bidirectional) args.insert(args.end(), {L"--ignore-times", L"--backup-dir", backup(destination)});
            else args.push_back(L"--update");
        }
        const bool ok = RunProcess(context, args, MigrationStage::Copying, error);
        DeleteFileW(path.c_str());
        return ok;
    };
    // Resolve conflicts before overwriting either original. Both providers keep
    // the Google variant at the same unique sibling path; the OneDrive variant
    // retains the original name. All replacements also have a history copy.
    for (const auto& row : rows) if (row.action == SyncAction::KeepBoth) {
        const auto path = Utf8ToWide(row.path);
        const auto suffix = L".conflict-Google-" + stamp;
        const auto alternate = path + suffix;
        for (const auto& destination : {od + alternate, gd + alternate}) {
            auto args = baseArgs(L"copyto", gd + path, destination);
            args.push_back(L"--immutable");
            if (!RunProcess(context, args, MigrationStage::Copying, error)) return false;
        }
        auto args = baseArgs(L"copyto", od + path, gd + path);
        args.insert(args.end(), {L"--ignore-times", L"--backup-dir", backup(gd)});
        if (!RunProcess(context, args, MigrationStage::Copying, error)) return false;
    }
    if (!transfer(SyncAction::ToGoogle, od, gd, false) || !transfer(SyncAction::ToOneDrive, gd, od, false) ||
        !transfer(SyncAction::DeleteGoogle, gd, gd, true) || !transfer(SyncAction::DeleteOneDrive, od, od, true)) return false;
    if (context.mode == SyncMode::Bidirectional) {
        SyncAnalysis after;
        if (!ReadCurrentSync(context, after, error, MigrationStage::Verifying)) return false;
        if (after.binding != context.sync.binding) { error = L"The accounts changed. History was not committed."; return false; }
        const auto remaining = after.Plan(SyncMode::Bidirectional);
        if (std::any_of(remaining.begin(), remaining.end(), [](const auto& row) { return row.action != SyncAction::None; })) {
            error = L"Differences remain after transfer. Analyze again; the previous history has been kept."; return false;
        }
        const SyncJson document = {{"version", 1}, {"binding", after.binding}, {"oneDrive", SaveSyncInventory(after.oneDrive)},
            {"google", SaveSyncInventory(after.google)}};
        if (!WriteEvidence(state, document.dump())) { error = L"History was not saved. A recovery analysis will be needed."; return false; }
        if (!DeleteFileW((state + L".pending").c_str())) { error = L"The recovery journal could not be finalized. Analyze again."; return false; }
    }
    // One-way copies intentionally leave the pair in merge/recovery mode:
    // they do not establish a successful bidirectional baseline.
    return true;
}

DWORD WINAPI WorkerProc(void* parameter) {
    auto& context = *static_cast<DialogContext*>(parameter);
    bool success = false;
    std::wstring error;
    AnalysisReport report;
    SyncAnalysis sync;
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CloudNav.CloudSync");
    const DWORD lock = mutex ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;
    const bool locked = lock == WAIT_OBJECT_0 || lock == WAIT_ABANDONED;
    try {
    if (!locked) {
        error = L"Another CloudNav operation is using these accounts. Try again when it finishes.";
    } else if (!ExtractRclone(context.instance, context.runtimePath, error)) {
        success = false;
    } else if (context.demoMode) {
        const auto stage = context.task == Task::Analyze ? MigrationStage::Analyzing :
            context.task == Task::Copy ? MigrationStage::Copying : MigrationStage::Connecting;
        context.parallelListing = stage == MigrationStage::Analyzing;
        context.listingStatus[0] = SyncListingProgress(true, 0, 0);
        context.listingStatus[1] = SyncListingProgress(false, 0, 0);
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
            if (context.parallelListing) {
                PostListingProgress(context, true, stage, SyncListingProgress(true, stats.listed, percent / 20));
                PostListingProgress(context, false, stage, SyncListingProgress(false, stats.listed / 2, percent / 20));
            } else PostProgress(context, progress.percent, stage, progress.text);
            Sleep(100);
        }
        context.parallelListing = false;
        success = !context.cancelRequested;
        if (context.task == Task::Analyze) {
            const std::string time = "2026-09-09T10:00:00Z";
            sync.oneDrive = {{"Documents/nouveau.pdf", {1048576, time, {}}}, {"Photos/vacances.jpg", {2097152, time, {}}},
                {"Documents/identique.txt", {10, time, {}}}};
            sync.google = {{"Archives/conservé.txt", {512, time, {}}}, {"Photos/vacances.jpg", {1024, time, {}}},
                {"Documents/identique.txt", {10, time, {}}}};
            sync.complete = success;
            report = SyncReport(sync, context.mode);
        }
    } else if (context.task == Task::AuthenticateOneDrive || context.task == Task::AuthenticateGoogle) {
        const bool oneDrive = context.task == Task::AuthenticateOneDrive;
        const wchar_t* remote = oneDrive ? kOneDriveRemote : kGoogleRemote;
        const auto args = AuthenticationArguments(oneDrive, HasRemote(context.configPath, remote), remote, context.configPath);
        success = RunProcess(context, args, MigrationStage::Connecting, error);
    } else if (context.task == Task::Analyze) {
        success = ReadCurrentSync(context, sync, error);
        if (success) LoadCurrentBaseline(context, sync);
        report = SyncReport(sync, context.mode);
    } else if (context.task == Task::Copy) {
        success = ExecuteSyncPlan(context, error);
    }
    } catch (const std::exception& e) { success = false; error = Utf8ToWide(e.what()); }
    if (locked) ReleaseMutex(mutex);
    if (mutex) CloseHandle(mutex);
    auto* completion = new CompletionUpdate{context.task, success, context.cancelRequested.load(), error, std::move(report), std::move(sync)};
    if (!PostMessageW(context.dialog, WM_MIGRATION_COMPLETE, 0, reinterpret_cast<LPARAM>(completion))) delete completion;
    return 0;
}

void RefreshButtons(DialogContext& context) {
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_ONEDRIVE_CONNECT), !context.running);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_GOOGLE_CONNECT), !context.running);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_ANALYZE), !context.running && context.oneDriveReady && context.googleReady);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_COPY), !context.running && context.analyzed && !context.report.Count('!'));
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_MODE), !context.running);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_COPY, context.mode == SyncMode::Bidirectional ? L"Sync" : L"Copy");
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_CUTOVER), !context.running && context.copied);
    EnableWindow(GetDlgItem(context.dialog, IDC_MIGRATION_REPORT), !context.running && context.report.available);
    const int primary = context.running ? IDCANCEL : context.copied ? IDC_MIGRATION_CUTOVER :
        context.analyzed ? IDC_MIGRATION_COPY :
        IsWindowEnabled(GetDlgItem(context.dialog, IDC_MIGRATION_ANALYZE)) ? IDC_MIGRATION_ANALYZE : IDCANCEL;
    for (int id : {IDC_MIGRATION_ANALYZE, IDC_MIGRATION_COPY, IDC_MIGRATION_CUTOVER, IDCANCEL}) {
        SendDlgItemMessageW(context.dialog, id, BM_SETSTYLE, id == primary ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
        SendDlgItemMessageW(context.dialog, id, WM_SETFONT, id == primary
            ? reinterpret_cast<WPARAM>(context.theme.bold) : SendMessageW(context.dialog, WM_GETFONT, 0, 0), TRUE);
    }
    SendMessageW(context.dialog, DM_SETDEFID, primary, 0);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_ONEDRIVE_CONNECT, context.oneDriveReady ? L"Reconnect…" : L"Connect…");
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_GOOGLE_CONNECT, context.googleReady ? L"Reconnect…" : L"Connect…");
    SetDlgItemTextW(context.dialog, IDCANCEL, context.running ? L"Cancel" : L"Close");
}

std::wstring CurrentPlanDetails(const DialogContext& context) {
    if (!context.report.available) return L"Analyze both accounts to see the proposed actions.";
    const auto rows = context.sync.Plan(context.mode);
    size_t toGoogle = 0, toOneDrive = 0;
    std::uint64_t bytes = 0;
    for (const auto& row : rows) {
        toGoogle += row.action == SyncAction::ToGoogle || row.action == SyncAction::KeepBoth;
        toOneDrive += row.action == SyncAction::ToOneDrive || row.action == SyncAction::KeepBoth;
        if (bytes <= (std::numeric_limits<std::uint64_t>::max)() - row.bytes) bytes += row.bytes;
    }
    return std::wstring(!context.analyzed && context.sync.complete && !context.running ? L"Previous plan — new analysis required.\r\n" : L"") +
        L"→ Google Drive : " + std::to_wstring(toGoogle) + L"    → OneDrive : " + std::to_wstring(toOneDrive) +
        L"    Estimated size: " + FormatBytes(bytes) + L"\r\n" +
        (context.mode != SyncMode::Bidirectional ? L"Copy missing/newer source files directly; no archiving. Extra destination files are kept." :
        context.sync.recovery ? L"Recovery: merge without removals. New history will be created after success." :
        context.sync.hasBaseline ? L"History available: deletions are propagated with archiving." :
        L"First merge: no removals. Both versions of conflicts will be kept.");
}

void RefreshPlan(DialogContext& context) {
    if (context.report.available) {
        context.report = SyncReport(context.sync, context.mode);
        SetDlgItemTextW(context.dialog, IDC_MIGRATION_SUMMARY, context.report.Summary().c_str());
    }
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_PLAN, CurrentPlanDetails(context).c_str());
    RefreshButtons(context);
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
    if (task != Task::Copy) { context.report = {}; context.sync = {}; }
    const auto summary = (task == Task::Copy && context.report.available
        ? L"Before copy — " : std::wstring()) + context.report.Summary();
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_SUMMARY, summary.c_str());
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_PLAN, CurrentPlanDetails(context).c_str());
    InvalidateMigrationValidation(task, context.analyzed, context.copied);
    if (task == Task::Analyze) context.sawAnalyzeProgress = false;
    if (task == Task::Analyze || task == Task::Copy) {
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
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_STATS, L"Waiting for progress…");
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, MigrationStageDetails(MigrationStage::Preparing));
    RefreshButtons(context);
    context.worker = CreateThread(nullptr, 0, WorkerProc, &context, 0, nullptr);
    if (!context.worker) {
        context.running = false;
        SetMigrationProgress(context, 0);
        SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, L"Unable to start the operation.");
        RefreshButtons(context);
    }
}

void CancelTask(DialogContext& context) {
    context.cancelRequested = true;
    EnterCriticalSection(&context.processLock);
    for (HANDLE process : context.childProcesses) TerminateProcess(process, ERROR_CANCELLED);
    LeaveCriticalSection(&context.processLock);
    SetMigrationProgress(context, 0);
    SetDlgItemTextW(context.dialog, IDC_MIGRATION_DETAILS, L"Cancelling… Files already copied will be reused when resuming.");
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
    std::wstring status, path, size, error, action;
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
    SetDlgItemTextW(dialog, IDC_REPORT_SELECTED, L"Select a file to read and copy its full path.");
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
        std::map<std::string, std::wstring> actions;
        for (const auto& row : context->owner->sync.Plan(context->owner->mode)) actions[row.path] = std::wstring(SyncActionLabel(row.action)) +
            (row.editDeleteConflict ? L" — restore the edited version" : L"");
        for (const auto& item : context->owner->report.files) {
            const auto& file = item.second;
            context->rows.push_back({file.category, AnalysisCategory(file.category), Utf8ToWide(item.first),
                file.sizeKnown ? FormatBytes(file.bytes) : L"—", Utf8ToWide(file.error), actions[item.first]});
        }
        for (const wchar_t* label : {L"All files", L"OneDrive only", L"Different", L"Identical", L"Google Drive only", L"Errors"})
            ComboBox_AddString(GetDlgItem(dialog, IDC_REPORT_FILTER), label);
        ComboBox_SetCurSel(GetDlgItem(dialog, IDC_REPORT_FILTER), 0);
        const HWND list = GetDlgItem(dialog, IDC_REPORT_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        RECT bounds = {};
        GetClientRect(list, &bounds);
        const int width = bounds.right;
        int index = 0;
        for (const wchar_t* title : {L"Result", L"File path", L"Proposed action", L"Size"}) {
            LVCOLUMNW column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(title);
            column.cx = width * (index == 0 ? 20 : index == 1 ? 35 : index == 2 ? 33 : 10) / 100;
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
                const auto& text = info->item.iSubItem == 0 ? row.status : info->item.iSubItem == 1 ? row.path :
                    info->item.iSubItem == 2 ? row.action : row.size;
                wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
            }
            return TRUE;
        }
        if (header->idFrom == IDC_REPORT_LIST && header->code == LVN_ITEMCHANGED) {
            const int selected = ListView_GetNextItem(header->hwndFrom, -1, LVNI_SELECTED);
            if (selected >= 0 && static_cast<size_t>(selected) < context->visible.size()) {
                const auto& row = context->rows[context->visible[selected]];
                SetDlgItemTextW(dialog, IDC_REPORT_SELECTED, (row.path + L"\r\n" + row.action +
                    (row.error.empty() ? L"" : L" — " + row.error)).c_str());
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
        for (auto mode : {SyncMode::ToGoogle, SyncMode::ToOneDrive, SyncMode::Bidirectional})
            ComboBox_AddString(GetDlgItem(dialog, IDC_MIGRATION_MODE), SyncModeLabel(mode));
        context->mode = LoadSyncMode(context->demoMode);
        ComboBox_SetCurSel(GetDlgItem(dialog, IDC_MIGRATION_MODE), static_cast<int>(context->mode));
        SendDlgItemMessageW(dialog, IDC_MIGRATION_PROGRESS, PBM_SETRANGE32, 0, 100);
        context->configPath = LocalAppDataPath() + L"\\CloudNav\\Migration\\rclone.conf";
        context->logPath = LocalAppDataPath() + L"\\CloudNav\\Migration\\migration.log";
        EnsureParentDirectory(context->configPath);
        context->oneDriveReady = context->demoMode || HasRemote(context->configPath, kOneDriveRemote);
        context->googleReady = context->demoMode || HasRemote(context->configPath, kGoogleRemote);
        SetDlgItemTextW(dialog, IDC_MIGRATION_ONEDRIVE_STATUS, context->demoMode ? L"Demo account" : context->oneDriveReady ? L"Connection saved" : L"Not connected");
        SetDlgItemTextW(dialog, IDC_MIGRATION_GOOGLE_STATUS, context->demoMode ? L"Demo account" : context->googleReady ? L"Connection saved" : L"Not connected");
        if (context->oneDriveReady && context->googleReady)
            SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Analysis checks account access and estimates the copy. No files are transferred.");
        SendDlgItemMessageW(dialog, IDC_MIGRATION_PHASE, WM_SETFONT, reinterpret_cast<WPARAM>(context->theme.bold), TRUE);
        RefreshPlan(*context);
        return TRUE;
    }
    if (!context) return FALSE;
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDC_MIGRATION_MODE:
            if (!context->running && HIWORD(wParam) == CBN_SELCHANGE) {
                const int selection = ComboBox_GetCurSel(GetDlgItem(dialog, IDC_MIGRATION_MODE));
                if (selection < 0 || selection > 2) return TRUE;
                context->mode = static_cast<SyncMode>(selection);
                const bool saved = SaveSyncMode(context->demoMode, context->mode);
                context->copied = false;
                RefreshPlan(*context);
                if (!saved) SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS,
                    L"Transfer direction applied to this window, but the preference could not be saved.");
            }
            return TRUE;
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
        case IDC_MIGRATION_COPY: {
            if (context->running || !context->analyzed || context->report.Count('!')) return TRUE;
            const std::wstring review = std::wstring(SyncModeLabel(context->mode)) + L"\r\n\r\n" + context->report.Summary() +
                L"\r\n\r\n" + CurrentPlanDetails(*context) +
                L"\r\n\r\nReplaced and removed files are archived in .CloudNav-history on the affected account." +
                (context->mode == SyncMode::Bidirectional ? L"\r\nConflicts: the OneDrive version keeps the original name; the Google version gets a suffix on both accounts." : L"") +
                L"\r\n\r\nApply this plan after checking the accounts again?";
            if (context->demoMode || MessageBoxW(dialog, review.c_str(), L"CloudNav — review transfer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                StartTask(*context, Task::Copy);
            return TRUE;
        }
        case IDC_MIGRATION_CUTOVER: {
            if (context->running || !context->copied) return TRUE;
            const bool passed = context->analyzed && context->copied && context->sawAnalyzeProgress && context->sawCopyProgress &&
                !context->sawVerifyProgress && context->progressConsistent && context->cancellationConsistent &&
                LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDC_MIGRATION_CUTOVER;
            if (context->demoMode) WriteEvidence(context->demoResultPath, passed
                ? "{\"passed\":true,\"copied\":true,\"progressConsistent\":true,\"cutoverPrimary\":true}"
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
            ui::ControlText(dialog, IDC_MIGRATION_DETAILS) == update->details;
        context->progressConsistent &= consistent;
        if (update->stage == MigrationStage::Analyzing && !context->sawAnalyzeProgress &&
            (update->statistics.find(L"files compared") != std::wstring::npos || update->statistics.find(L"OneDrive") == 0)) {
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
            context->sync = std::move(update->sync);
            context->report = std::move(update->report);
            SetDlgItemTextW(dialog, IDC_MIGRATION_SUMMARY, context->report.Summary().c_str());
            SetDlgItemTextW(dialog, IDC_MIGRATION_PLAN, CurrentPlanDetails(*context).c_str());
        }
        SetMigrationProgress(*context, 0);
        if (update->success) {
            if (update->task == Task::AuthenticateOneDrive || update->task == Task::AuthenticateGoogle) {
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, L"1 / 2 — Analyze before copying");
                SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"Analysis required after reconnecting");
            }
            if (update->task == Task::AuthenticateOneDrive) {
                context->oneDriveReady = true; SetDlgItemTextW(dialog, IDC_MIGRATION_ONEDRIVE_STATUS, L"Connection saved");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"OneDrive account connected.");
            } else if (update->task == Task::AuthenticateGoogle) {
                context->googleReady = true; SetDlgItemTextW(dialog, IDC_MIGRATION_GOOGLE_STATUS, L"Connection saved");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Google Drive account connected.");
            } else if (update->task == Task::Analyze) {
                context->analyzed = true;
                SetDlgItemTextW(dialog, IDC_MIGRATION_PLAN, CurrentPlanDetails(*context).c_str());
                SetMigrationProgress(*context, 100);
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, L"1 / 2 — Analysis complete: ready to copy");
                SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"Analysis complete — no files transferred");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"Both accounts have been compared. Choose a transfer direction and review the actions in View details.");
            } else if (update->task == Task::Copy) {
                context->copied = true;
                SetMigrationProgress(*context, 100);
                SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, context->mode == SyncMode::Bidirectional ? L"Synchronization complete" : L"2 / 2 — Copy complete");
                SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, context->mode == SyncMode::Bidirectional ?
                    L"100 % — accounts compared after transfer, history saved" : L"100 % — copy complete with no errors reported");
                SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS, L"You can now set up Windows folders or close this window.");
            }
        } else {
            SetDlgItemTextW(dialog, IDC_MIGRATION_PHASE, update->cancelled ? L"Operation cancelled." : L"Operation interrupted.");
            SetDlgItemTextW(dialog, IDC_MIGRATION_STATS, L"Operation stopped — no transfer running");
            SendDlgItemMessageW(dialog, IDC_MIGRATION_PROGRESS, PBM_SETSTATE, update->cancelled ? PBST_PAUSED : PBST_ERROR, 0);
            SetDlgItemTextW(dialog, IDC_MIGRATION_DETAILS,
                            update->cancelled ? L"Analyze again to resume. Files already copied and archives are kept." : update->message.c_str());
        }
        if (update->task == Task::Copy && !context->demoMode) {
            context->analyzed = false;
            SetDlgItemTextW(dialog, IDC_MIGRATION_PLAN, L"Previous plan kept for reference. Analyze again before another transfer.");
        }
        const bool close = context->closeRequested;
        delete update;
        RefreshButtons(*context);
        if (context->copied || context->analyzed)
            SendMessageW(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dialog,
                context->copied ? IDC_MIGRATION_CUTOVER : IDC_MIGRATION_COPY)), TRUE);
        if (context->analyzed && !context->copied)
            WriteMilestone(*context, L"analysis-ready.json", context->sawAnalyzeProgress && !context->indeterminate &&
                IsWindowEnabled(GetDlgItem(dialog, IDC_MIGRATION_COPY)) != FALSE);
        if (context->report.complete) WriteMilestone(*context, L"analysis-summary.json",
            ui::ControlText(dialog, IDC_MIGRATION_SUMMARY) == context->report.Summary() &&
            IsWindowEnabled(GetDlgItem(dialog, IDC_MIGRATION_REPORT)) != FALSE);
        if (context->copied)
            WriteMilestone(*context, L"copied.json", LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDC_MIGRATION_CUTOVER);
        if (context->cancelRequested) {
            context->cancellationConsistent &= !context->copied &&
                !IsWindowEnabled(GetDlgItem(dialog, IDC_MIGRATION_CUTOVER)) &&
                ui::ControlText(dialog, IDC_MIGRATION_STATS) == L"Operation stopped — no transfer running";
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
    context.logPath = resultPath + L".log";
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
        if (passed) {
            step = "silentListingProgress";
            const auto fixture = root + L"\\delayed-listing.ps1";
            passed = WriteEvidence(fixture, "Start-Sleep -Seconds 3\r\nWrite-Output '['\r\n"
                "Write-Output '{\"Path\":\"sample.txt\",\"Size\":1,\"IsDir\":false,\"ModTime\":\"2026-09-09T10:00:00Z\"}'\r\nWrite-Output ']'\r\n");
            wchar_t systemDirectory[MAX_PATH] = {};
            passed = passed && GetSystemDirectoryW(systemDirectory, MAX_PATH) != 0;
            context.runtimePath = std::wstring(systemDirectory) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
            const auto updates = context.listingProgressUpdates.load();
            std::string output, parseError;
            SyncInventory inventory;
            passed = passed && RunProcess(context, {L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File", fixture}, MigrationStage::Analyzing,
                error, nullptr, nullptr, &output) && context.listingProgressUpdates >= updates + 2 &&
                ReadSyncInventory(output, inventory, parseError) && inventory.size() == 1;
            if (passed) {
                step = "cancelBothListings";
                const auto delayed = [&] {
                    std::wstring childError; std::string childOutput;
                    return RunProcess(context, {L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File", fixture},
                        MigrationStage::Analyzing, childError, nullptr, nullptr, &childOutput);
                };
                auto first = std::async(std::launch::async, delayed);
                auto second = std::async(std::launch::async, delayed);
                bool bothRunning = false;
                const auto deadline = GetTickCount64() + 2000;
                while (GetTickCount64() < deadline) {
                    EnterCriticalSection(&context.processLock);
                    bothRunning = context.childProcesses.size() == 2;
                    LeaveCriticalSection(&context.processLock);
                    if (bothRunning) break;
                    Sleep(10);
                }
                CancelTask(context);
                const bool firstOk = first.get(), secondOk = second.get();
                passed = bothRunning && !firstOk && !secondOk && context.childProcesses.empty();
                context.cancelRequested = false;
            }
            context.runtimePath = runtime;
        }
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
            auto args = MigrationArguments(stage, context.configPath, source, destination);
            if (stage == MigrationStage::Copying) {
                std::string list;
                if (!analysisReport.CopyList(list) || !WriteEvidence(root + L"\\copy-list.txt", list)) return false;
                args.insert(args.end(), {L"--files-from-raw", root + L"\\copy-list.txt"});
            }
            return RunProcess(context, args, stage, error, exitCode,
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
                    analysisReport.CopySize() != L"unavailable" && analysisReport.files.count("Personal Vault/excluded.txt") == 0;
            }
            if (passed) {
                step = "copyAnalyzedListOnly";
                passed = WriteEvidence(source + L"\\late-file.txt", "not in the plan") && run(MigrationStage::Copying) &&
                    !std::filesystem::exists(destination + L"\\late-file.txt") && read(destination + L"\\changed.txt") == "replacement contents" &&
                    read(destination + L"\\extra.txt") == "keep this file" &&
                    read(destination + L"\\nested 31\\été.txt") == "fixture 31" &&
                    !std::filesystem::exists(destination + L"\\empty directory") &&
                    !std::filesystem::exists(destination + L"\\Personal Vault");
            }

        }
        if (passed) {
            step = "sharedSyncAnalysis";
            context.oneDrivePath = root + L"\\sync-one\\";
            context.googlePath = root + L"\\sync-google\\";
            context.configPath = root + L"\\sync-fixture.conf";
            passed = WriteEvidence(context.configPath, "[cloudnav-onedrive]\ntype = local\n[cloudnav-gdrive]\ntype = local\n") &&
                WriteEvidence(context.oneDrivePath + L"one.txt", "one only") &&
                WriteEvidence(context.googlePath + L"google.txt", "google only") &&
                WriteEvidence(context.oneDrivePath + L"both.txt", "OneDrive version") &&
                WriteEvidence(context.googlePath + L"both.txt", "Google version");
            if (passed) {
                const auto now = std::filesystem::file_time_type::clock::now();
                std::filesystem::last_write_time(context.oneDrivePath + L"both.txt", now - std::chrono::hours(2));
                std::filesystem::last_write_time(context.googlePath + L"both.txt", now - std::chrono::hours(1));
            }
            const auto odBefore = snapshot(context.oneDrivePath), gdBefore = snapshot(context.googlePath);
            const auto analyzeSync = [&] {
                context.sync = {};
                if (!ReadCurrentSync(context, context.sync, error)) return false;
                LoadCurrentBaseline(context, context.sync);
                return true;
            };
            context.peakChildProcesses = 0;
            passed = passed && analyzeSync() && context.peakChildProcesses == 2 &&
                snapshot(context.oneDrivePath) == odBefore && snapshot(context.googlePath) == gdBefore;
            if (passed) {
                step = "reverseSyncCopy";
                context.mode = SyncMode::ToOneDrive;
                const auto reads = context.inventoryReads.load();
                passed = WriteEvidence(context.googlePath + L"late-file.txt", "outside reviewed plan") &&
                    ExecuteSyncPlan(context, error) && context.inventoryReads == reads &&
                    !std::filesystem::exists(context.oneDrivePath + L"late-file.txt") &&
                    read(context.oneDrivePath + L"both.txt") == "Google version" &&
                    read(context.oneDrivePath + L"google.txt") == "google only" && read(context.oneDrivePath + L"one.txt") == "one only" &&
                    !std::filesystem::exists(context.oneDrivePath + L".CloudNav-history");
                passed = DeleteFileW((context.googlePath + L"late-file.txt").c_str()) && passed;
            }
            if (passed) {
                step = "forwardDirectOverwrite";
                context.mode = SyncMode::ToGoogle;
                passed = WriteEvidence(context.oneDrivePath + L"oneway.txt", "new source") &&
                    WriteEvidence(context.googlePath + L"oneway.txt", "old destination");
                const auto now = std::filesystem::file_time_type::clock::now();
                std::filesystem::last_write_time(context.oneDrivePath + L"oneway.txt", now - std::chrono::hours(1));
                std::filesystem::last_write_time(context.googlePath + L"oneway.txt", now - std::chrono::hours(2));
                passed = passed && analyzeSync() && ExecuteSyncPlan(context, error) &&
                    read(context.googlePath + L"oneway.txt") == "new source" &&
                    !std::filesystem::exists(context.googlePath + L".CloudNav-history");
                if (passed) {
                    step = "preserveNewerDestination";
                    passed = WriteEvidence(context.googlePath + L"oneway.txt", "destination edited since analysis");
                    std::filesystem::last_write_time(context.googlePath + L"oneway.txt", now);
                    passed = passed && ExecuteSyncPlan(context, error) &&
                        read(context.googlePath + L"oneway.txt") == "destination edited since analysis";
                }
            }
            if (passed) {
                step = "firstBidirectionalMerge";
                context.mode = SyncMode::Bidirectional;
                passed = analyzeSync() && !context.sync.hasBaseline && context.sync.recovery;
                const auto reads = context.inventoryReads.load();
                passed = passed && ExecuteSyncPlan(context, error) && context.inventoryReads == reads + 2 &&
                    read(context.googlePath + L"one.txt") == "one only" && analyzeSync() && context.sync.hasBaseline;
            }
            if (passed) {
                step = "changedLocalHistoryRejected";
                const auto state = context.sync.baselineDocument;
                const auto reads = context.inventoryReads.load();
                passed = WriteEvidence(SyncStatePath(context), state + " ") && !ExecuteSyncPlan(context, error) &&
                    context.inventoryReads == reads && WriteEvidence(SyncStatePath(context), state);
            }
            if (passed) {
                step = "bidirectionalConflicts";
                passed = WriteEvidence(context.oneDrivePath + L"one.txt", "updated during preview") &&
                    WriteEvidence(context.googlePath + L"one.txt", "also edited on Google") && analyzeSync() && ExecuteSyncPlan(context, error) &&
                    read(context.googlePath + L"one.txt") == "updated during preview";
                bool preserved = false;
                for (const auto& entry : std::filesystem::directory_iterator(context.oneDrivePath)) {
                    if (entry.path().filename().wstring().find(L"one.txt.conflict-Google-") == 0) {
                        preserved = read(entry.path()) == "also edited on Google" &&
                            read(std::filesystem::path(context.googlePath) / entry.path().filename()) == "also edited on Google";
                    }
                }
                passed = passed && preserved;
            }
            if (passed) {
                step = "bidirectionalArchivedDeletion";
                passed = DeleteFileW((context.oneDrivePath + L"google.txt").c_str()) && analyzeSync() && context.sync.hasBaseline &&
                    ExecuteSyncPlan(context, error) && !std::filesystem::exists(context.googlePath + L"google.txt");
                bool archived = false;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(context.googlePath + L".CloudNav-history"))
                    if (entry.is_regular_file() && entry.path().filename() == L"google.txt") archived |= read(entry.path()) == "google only";
                passed = passed && archived;
            }
            if (passed) {
                step = "interruptedSyncRecovery";
                passed = WriteEvidence(SyncStatePath(context) + L".pending", "pending") &&
                    DeleteFileW((context.googlePath + L"both.txt").c_str()) && analyzeSync() && context.sync.recovery && !context.sync.hasBaseline;
                if (passed) {
                    for (const auto& row : context.sync.Plan(SyncMode::Bidirectional))
                        passed = passed && row.action != SyncAction::DeleteGoogle && row.action != SyncAction::DeleteOneDrive;
                    passed = passed && ExecuteSyncPlan(context, error) && read(context.googlePath + L"both.txt") == "Google version";
                }
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
        ? "{\"passed\":true,\"embeddedVersion\":\"1.75.0\",\"readOnlyAnalysis\":true,\"copyAnalyzedListOnly\":true,\"sharedSyncAnalysis\":true,\"reverseCopy\":true,\"bidirectionalConflicts\":true,\"archivedDeletion\":true,\"parallelListings\":true,\"cancelBothListings\":true,\"silentListingProgress\":true,\"reviewedPlanReused\":true,\"changedLocalHistoryRejected\":true,\"interruptedRecovery\":true}\n"
        : SyncJson({{"passed", false}, {"failedStep", step}, {"error", WideToUtf8(error)}}).dump();
    DWORD written = 0;
    const bool wrote = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();
    CloseHandle(file);
    return passed && wrote ? 0 : 1;
}

}  // namespace cloudnav
