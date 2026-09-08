#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "../src/resource.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

std::wstring evidenceDirectory;

void Capture(HWND window, const wchar_t* name) {
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    RECT rect = {};
    GetWindowRect(window, &rect);
    HDC dc = GetDC(window);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right - rect.left, rect.bottom - rect.top);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL printed = PrintWindow(window, memory, PW_RENDERFULLCONTENT);
    Gdiplus::Status saved = Gdiplus::GenericError;
    if (printed) {
        const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        Gdiplus::Bitmap image(bitmap, nullptr);
        saved = image.Save((evidenceDirectory + name).c_str(), &png);
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, dc);
    if (!printed || saved != Gdiplus::Ok) throw std::runtime_error("window screenshot failed");
}

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Wait(const std::function<bool()>& condition) {
    const ULONGLONG deadline = GetTickCount64() + 15000;
    do { if (condition()) return true; Sleep(50); } while (GetTickCount64() < deadline);
    return false;
}

std::wstring Text(HWND window) {
    const LRESULT length = SendMessageW(window, WM_GETTEXTLENGTH, 0, 0);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    const LRESULT copied = SendMessageW(window, WM_GETTEXT, result.size(), reinterpret_cast<LPARAM>(result.data()));
    result.resize(static_cast<size_t>(copied));
    return result;
}

std::wstring Text(HWND dialog, int id) { return Text(GetDlgItem(dialog, id)); }

struct Search {
    DWORD pid;
    const wchar_t* title;
    HWND found = nullptr;
};

BOOL CALLBACK Find(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search.pid && IsWindowVisible(window) && Text(window) == search.title) {
        search.found = window; return FALSE;
    }
    return TRUE;
}

HWND Window(DWORD pid, const wchar_t* title) {
    Search search{pid, title};
    Require(Wait([&] { EnumWindows(Find, reinterpret_cast<LPARAM>(&search)); return search.found != nullptr; }),
            "expected application dialog did not open");
    return search.found;
}

void Click(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    Require(control && IsWindowEnabled(control), "expected action is unavailable");
    PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(control));
}

void Select(HWND dialog, int id, int value) {
    HWND combo = GetDlgItem(dialog, id);
    Require(combo && IsWindowEnabled(combo), "expected selection is unavailable");
    SendMessageW(combo, CB_SETCURSEL, value, 0);
    PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
}

BOOL CALLBACK Bounds(HWND child, LPARAM parameter) {
    auto& valid = *reinterpret_cast<bool*>(parameter);
    if (!IsWindowVisible(child)) return TRUE;
    HWND parent = GetParent(child);
    RECT rect = {}, client = {};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    GetClientRect(parent, &client);
    valid &= rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom;
    return TRUE;
}

void CheckBounds(HWND dialog) {
    bool valid = true;
    EnumChildWindows(dialog, Bounds, reinterpret_cast<LPARAM>(&valid));
    Require(valid, "a visible control extends outside its parent");
}

class App {
public:
    PROCESS_INFORMATION process = {};
    HWND main = nullptr;
    explicit App(const std::wstring& executable, const wchar_t* args) {
        std::wstring command = L"\"" + executable + L"\" " + args;
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(0);
        STARTUPINFOW startup = {sizeof(startup)};
        Require(CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0,
                               nullptr, nullptr, &startup, &process) != FALSE, "application launch failed");
        CloseHandle(process.hThread);
        main = Window(process.dwProcessId, L"CloudNav — test visuel");
    }
    ~App() {
        if (main) PostMessageW(main, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
    }
};

void Run(const std::wstring& executable) {
    App app(executable, L"--demo-plan");
    CheckBounds(app.main);
    Require(!IsWindowEnabled(GetDlgItem(app.main, 1006)), "visibility apply should start disabled");
    Require(Text(app.main, 1001) == L"Google Drive — dossier My Drive", "folder provider label is ambiguous");
    Require(Text(app.main, 1003).find(L"OneDrive") != std::wstring::npos, "account label omits OneDrive");
    Require(Text(app.main, 1004) == L"Google Drive — lecteur G:", "drive provider label is ambiguous");
    Capture(app.main, L"ui-main.png");
    SendDlgItemMessageW(app.main, 1001, BM_SETCHECK, BST_UNCHECKED, 0);
    Click(app.main, 1001);
    Require(Wait([&] { return IsWindowEnabled(GetDlgItem(app.main, 1006)) != FALSE; }), "visibility changes were not detected");
    Click(app.main, 1006);
    Require(Wait([&] { return !IsWindowEnabled(GetDlgItem(app.main, 1006)); }), "applied visibility stays dirty");

    Click(app.main, 1007);
    HWND folders = Window(app.process.dwProcessId, L"CloudNav — dossiers personnels");
    CheckBounds(folders);
    Require(SendDlgItemMessageW(folders, IDC_FOLDER_TRANSFER_MODE, CB_GETCURSEL, 0, 0) == 0,
            "sync task detection incorrectly recommends redirect-only");
    const std::wstring originalDocuments = Text(folders, IDC_FOLDER_DOCUMENTS_PATH);
    const std::wstring originalPictures = Text(folders, IDC_FOLDER_PICTURES_PATH);
    Require(Text(folders, IDC_FOLDER_DOCUMENTS_PATH + 2) == L"C:\\Users\\Example\\My Drive\\Documents",
            "full proposed path is not independently readable");
    Capture(folders, L"ui-unverified.png");
    Select(folders, IDC_FOLDER_PICTURES_TARGET, 2);
    Require(Wait([&] { return SendDlgItemMessageW(folders, IDC_FOLDER_PICTURES_TARGET, CB_GETCURSEL, 0, 0) == 0; }),
            "an unchanged target would introduce an unreviewed operation after backup release");
    // Copy must not release backup and accidentally switch its source to an empty local folder.
    Click(folders, IDOK);
    HWND blocked = Window(app.process.dwProcessId, L"CloudNav — migrer avant de repointer");
    HWND safeButton = GetDlgItem(blocked, IDOK);
    Click(blocked, safeButton ? IDOK : IDCANCEL);
    Require(Wait([&] { return !IsWindow(blocked); }), "backup guard could not be dismissed");
    Require(Text(folders, IDC_FOLDER_DOCUMENTS_PATH) == originalDocuments, "backup guard changed the folder");

    Select(folders, IDC_FOLDER_TRANSFER_MODE, 2);
    Require(Wait([&] { return Text(folders, IDC_FOLDER_PICTURES_PATH + 2) == L"C:\\Users\\Example\\Pictures"; }),
            "implicit return to local is missing from preview");
    Require(Text(folders, IDC_FOLDER_SUMMARY).find(L"+ 1 retour") != std::wstring::npos, "side-effect count is missing");
    Require(Text(folders, IDC_FOLDER_STATUS).find(L"Mode de transfert mis à jour") != std::wstring::npos,
            "changing transfer mode leaves a stale error");
    Capture(folders, L"ui-folder-preview.png");
    Click(folders, IDOK);
    HWND review = Window(app.process.dwProcessId, L"CloudNav — vérifier les changements");
    CheckBounds(review);
    Require(Text(review, IDC_REVIEW_PLAN).find(originalDocuments) != std::wstring::npos, "review omits full source path");
    Require(Text(review, IDC_REVIEW_EFFECTS).find(originalPictures) != std::wstring::npos, "review omits affected Pictures path");
    Require(Text(review, IDC_REVIEW_EFFECTS).find(L"C:\\Users\\Example\\Pictures") != std::wstring::npos,
            "review omits Pictures final destination");
    Require(LOWORD(SendMessageW(review, DM_GETDEFID, 0, 0)) == IDCANCEL, "confirmation default is not safe");
    Require(Text(review, IDOK) == L"Confirmer le repointage", "confirmation action is ambiguous");
    Capture(review, L"ui-folder-confirm.png");
    Click(review, IDCANCEL);
    Require(Wait([&] { return !IsWindow(review); }), "review cancellation did not close");
    Require(Text(folders, IDC_FOLDER_DOCUMENTS_PATH) == originalDocuments &&
            Text(folders, IDC_FOLDER_PICTURES_PATH) == originalPictures, "cancelling the review changed a folder");
    Click(folders, IDOK);
    review = Window(app.process.dwProcessId, L"CloudNav — vérifier les changements");
    Click(review, IDOK);
    Require(Wait([&] { return !IsWindow(review) && !IsWindowEnabled(GetDlgItem(folders, IDOK)); }),
            "completed plan was not reset");
    Require(Text(folders, IDC_FOLDER_DOCUMENTS_PATH) == L"C:\\Users\\Example\\My Drive\\Documents" &&
            Text(folders, IDC_FOLDER_PICTURES_PATH) == L"C:\\Users\\Example\\Pictures",
            "applied results differ from the explicit preview");
    Capture(folders, L"ui-folder-applied.png");
    Click(folders, IDCANCEL);
    Require(Wait([&] { return !IsWindow(folders); }), "folder manager did not close");

    // A localized OneDrive leaf (Images) must keep its approved destination after
    // backup release changes the Windows source to the local leaf (Pictures).
    Click(app.main, 1007);
    folders = Window(app.process.dwProcessId, L"CloudNav — dossiers personnels");
    Select(folders, IDC_FOLDER_PICTURES_TARGET, 3);
    const std::wstring approvedPictures = L"C:\\Users\\Example\\My Drive\\Images";
    Require(Wait([&] { return Text(folders, IDC_FOLDER_PICTURES_PATH + 2) == approvedPictures; }),
            "localized picture destination was not previewed");
    Select(folders, IDC_FOLDER_TRANSFER_MODE, 2);
    Require(Wait([&] { return Text(folders, IDC_FOLDER_STATUS).find(L"Mode de transfert mis à jour") != std::wstring::npos; }),
            "redirect-only selection was not processed");
    Click(folders, IDOK);
    review = Window(app.process.dwProcessId, L"CloudNav — vérifier les changements");
    Require(Text(review, IDC_REVIEW_PLAN).find(approvedPictures) != std::wstring::npos,
            "approved picture destination is missing");
    Click(review, IDOK);
    Require(Wait([&] { return !IsWindow(review) && !IsWindowEnabled(GetDlgItem(folders, IDOK)); }),
            "second plan did not finish");
    Require(Text(folders, IDC_FOLDER_PICTURES_PATH) == approvedPictures,
            "backup release changed an approved destination");
    Click(folders, IDCANCEL);
    Require(Wait([&] { return !IsWindow(folders); }), "second folder manager did not close");
}

void RunMigrationReport(const std::wstring& executable) {
    App app(executable, L"--demo-migration");
    const HWND migration = Window(app.process.dwProcessId, L"CloudNav — migration OneDrive vers Google Drive");
    Click(migration, IDC_MIGRATION_ANALYZE);
    Require(Wait([&] { return IsWindowEnabled(GetDlgItem(migration, IDC_MIGRATION_COPY)) != FALSE; }), "analysis did not complete");
    const auto summary = Text(migration, IDC_MIGRATION_SUMMARY);
    Require(summary.find(L"Nouveaux : 1") != std::wstring::npos && summary.find(L"Modifiés : 1") != std::wstring::npos &&
        summary.find(L"Identiques : 1") != std::wstring::npos && summary.find(L"Erreurs : 0") != std::wstring::npos &&
        summary.find(L"À copier : 3.0 Mo") != std::wstring::npos, "analysis KPIs are incorrect");
    CheckBounds(migration);
    Capture(migration, L"report-summary.png");
    Click(migration, IDC_MIGRATION_REPORT);
    const HWND report = Window(app.process.dwProcessId, L"CloudNav — résultats de l’analyse");
    const HWND list = GetDlgItem(report, IDC_REPORT_LIST);
    Require(ListView_GetItemCount(list) == 4, "report omits files");
    CheckBounds(report);
    Capture(report, L"report-all.png");
    for (int selection = 1; selection <= 5; ++selection) {
        Select(report, IDC_REPORT_FILTER, selection);
        Require(Wait([&] { return ListView_GetItemCount(list) == (selection == 5 ? 0 : 1); }), "category filter is incorrect");
        if (selection == 1) Capture(report, L"report-new.png");
    }
    Click(report, IDCANCEL);
    Require(Wait([&] { return !IsWindow(report); }), "report did not close");
    Click(migration, IDC_MIGRATION_ANALYZE);
    Require(Wait([&] { return !IsWindowEnabled(GetDlgItem(migration, IDC_MIGRATION_REPORT)); }), "stale report remains available");
    Require(Text(migration, IDC_MIGRATION_SUMMARY).find(L"Nouveaux") == std::wstring::npos, "stale KPIs remain visible");
    Click(migration, IDCANCEL);
    Require(Wait([&] { return IsWindowEnabled(GetDlgItem(migration, IDC_MIGRATION_ANALYZE)) != FALSE; }), "analysis did not cancel");
    Require(Text(migration, IDC_MIGRATION_SUMMARY).find(L"Résultats partiels") == 0 &&
        !IsWindowEnabled(GetDlgItem(migration, IDC_MIGRATION_COPY)), "cancelled analysis looks complete");
    Capture(migration, L"report-partial.png");
    Click(migration, IDCANCEL);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) return 2;
    const bool migrationReport = argc == 3 && std::wstring(argv[2]) == L"--migration-report";
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t module[32768] = {};
    GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const std::wstring executable = std::wstring(module).substr(0, std::wstring(module).find_last_of(L"\\/")) + L"\\CloudNav.exe";
    std::string error;
    evidenceDirectory = std::wstring(argv[1]).substr(0, std::wstring(argv[1]).find_last_of(L"\\/") + 1);
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput graphicsInput;
    if (Gdiplus::GdiplusStartup(&token, &graphicsInput, nullptr) != Gdiplus::Ok) return 4;
    try { if (migrationReport) RunMigrationReport(executable); else Run(executable); } catch (const std::exception& exception) { error = exception.what(); }
    Gdiplus::GdiplusShutdown(token);
    const std::string json = error.empty()
        ? (migrationReport ? "{\"passed\":true,\"summary\":true,\"filters\":true,\"partialResults\":true,\"staleReportCleared\":true,\"bounds\":true}" : "{\"passed\":true,\"visibility\":true,\"providerLabels\":true,\"unverifiedCopyDefault\":true,\"backupCopyGuard\":true,\"fullPaths\":true,\"collateralPreview\":true,\"safeConfirmation\":true,\"cancelPreservesPaths\":true,\"bounds\":true}")
        : "{\"passed\":false,\"error\":\"" + error + "\"}";
    HANDLE file = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    DWORD written = 0;
    const bool wrote = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size();
    CloseHandle(file);
    return wrote && error.empty() ? 0 : 1;
}
