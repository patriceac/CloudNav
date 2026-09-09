#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "../src/client_management.h"
#include "../third_party/nlohmann/json.hpp"

namespace {
void Require(bool condition) { if (!condition) throw std::runtime_error("Client runtime assertion failed"); }
void WriteRegistry(HKEY key, const wchar_t* name, const std::wstring& value) {
    Require(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
}
struct Fixture {
    HKEY user = nullptr, machine = nullptr;
    std::wstring keyName, directory;
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    Fixture() {
        keyName = L"Software\\CloudNavClientTest-" + std::to_wstring(GetCurrentProcessId());
        Require(RegCreateKeyExW(HKEY_CURRENT_USER, (keyName + L"\\User").c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr) == ERROR_SUCCESS);
        Require(RegCreateKeyExW(HKEY_CURRENT_USER, (keyName + L"\\Machine").c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &machine, nullptr) == ERROR_SUCCESS);
        wchar_t temp[MAX_PATH] = {}; GetTempPathW(ARRAYSIZE(temp), temp);
        directory = std::wstring(temp) + L"CloudNav client runtime " + std::to_wstring(GetCurrentProcessId());
        std::filesystem::create_directories(directory);
        for (const wchar_t* name : {L"LOCALAPPDATA", L"ProgramFiles", L"ProgramFiles(x86)"}) {
            wchar_t old[32768] = {}; GetEnvironmentVariableW(name, old, ARRAYSIZE(old));
            environment.emplace_back(name, old);
            SetEnvironmentVariableW(name, directory.c_str());
        }
        Require(RegOverridePredefKey(HKEY_CURRENT_USER, user) == ERROR_SUCCESS);
        Require(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine) == ERROR_SUCCESS);
    }
    ~Fixture() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        for (const auto& value : environment) SetEnvironmentVariableW(value.first.c_str(), value.second.empty() ? nullptr : value.second.c_str());
        if (user) RegCloseKey(user);
        if (machine) RegCloseKey(machine);
        RegDeleteTreeW(HKEY_CURRENT_USER, keyName.c_str());
        std::error_code error;
        std::filesystem::remove_all(directory, error); // unique disposable guest fixture
    }
};
void LocalTests() {
    using namespace cloudnav;
    Fixture fixture;
    Require(!DetectClientInstallation(CloudClient::OneDrive).installed);
    Require(!DetectClientInstallation(CloudClient::GoogleDrive).installed);
    const auto oneDrive = fixture.directory + L"\\Microsoft\\OneDrive";
    std::filesystem::create_directories(oneDrive);
    std::ofstream(std::filesystem::path(oneDrive + L"\\OneDriveSetup.exe")) << "unsigned setup fixture";
    Require(!DetectClientInstallation(CloudClient::OneDrive).installed); // bundled installer alone != installed
    std::ofstream(std::filesystem::path(oneDrive + L"\\OneDrive.exe")) << "client fixture";
    auto info = DetectClientInstallation(CloudClient::OneDrive);
    Require(info.installed && info.detectionComplete && info.uninstallArguments == L"/uninstall");
    std::wstring error;
    Require(!VerifyClientPublisher(info.uninstallExecutable, CloudClient::OneDrive, error) && !error.empty());

    const auto uninstaller = fixture.directory + L"\\uninstall.exe";
    std::ofstream(std::filesystem::path(uninstaller)) << "unsigned uninstaller fixture";
    HKEY registration = nullptr;
    Require(RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DriveFS", 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &registration, nullptr) == ERROR_SUCCESS);
    WriteRegistry(registration, L"DisplayName", L"Google Drive");
    WriteRegistry(registration, L"Publisher", L"Google LLC");
    WriteRegistry(registration, L"UninstallString", uninstaller);
    info = DetectClientInstallation(CloudClient::GoogleDrive);
    Require(info.installed && info.detectionComplete && info.uninstallExecutable == uninstaller && info.uninstallArguments.empty());
    Require(!VerifyClientPublisher(uninstaller, CloudClient::GoogleDrive, error));
    WriteRegistry(registration, L"UninstallString", L"C:\\Windows\\System32\\cmd.exe /c whoami");
    info = DetectClientInstallation(CloudClient::GoogleDrive);
    Require(info.installed && info.uninstallExecutable.empty());
    WriteRegistry(registration, L"Publisher", L"Unrelated vendor");
    Require(!DetectClientInstallation(CloudClient::GoogleDrive).installed);
    RegCloseKey(registration);
}
void DownloadTests() {
    using namespace cloudnav;
    std::atomic_bool cancel{false};
    for (const auto client : {CloudClient::OneDrive, CloudClient::GoogleDrive}) {
        std::wstring path, error;
        if (!DownloadClientInstaller(client, cancel, path, error)) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), nullptr, 0, nullptr, nullptr);
            std::string message(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), message.data(), size, nullptr, nullptr);
            throw std::runtime_error(message);
        }
        const bool trusted = VerifyClientPublisher(path, client, error);
        RemoveClientDownload(path);
        Require(trusted && !std::filesystem::exists(path));
    }
    cancel = true;
    std::wstring path, error;
    Require(!DownloadClientInstaller(CloudClient::OneDrive, cancel, path, error) && path.empty());
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3) return 2;
    const bool downloads = argc == 3 && std::wstring(argv[2]) == L"--downloads";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool passed = false;
    std::string error;
    try { if (downloads) DownloadTests(); else LocalTests(); passed = true; } catch (const std::exception& exception) { error = exception.what(); }
    std::ofstream output{std::filesystem::path(argv[1])};
    output << nlohmann::json({{"passed", passed}, {"error", error}, {"officialDownloads", downloads},
        {"installersExecuted", false}, {"registryFixtures", !downloads}}).dump();
    output.close();
    CoUninitialize();
    return passed ? 0 : 1;
}
