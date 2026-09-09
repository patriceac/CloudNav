#pragma once
#include <array>
#include "../src/client_management.h"

inline void TestClientManagementLogic() {
    using namespace cloudnav;
    ClientInstallation missing;
    auto actions = AvailableClientActions(missing, false, false, false, false);
    assert(actions.install && !actions.uninstall); // no account needed to install
    missing.detectionComplete = false;
    actions = AvailableClientActions(missing, true, true, false, false);
    assert(!actions.install && !actions.uninstall);
    ClientInstallation installed{true, true, L"C:\\Vendor\\uninstall.exe", L""};
    actions = AvailableClientActions(installed, true, true, false, false);
    assert(!actions.install && actions.uninstall);
    for (const auto& args : {std::array<bool, 4>{false, true, false, false},
                            std::array<bool, 4>{true, false, false, false},
                            std::array<bool, 4>{true, true, true, false},
                            std::array<bool, 4>{true, true, false, true}}) {
        actions = AvailableClientActions(installed, args[0], args[1], args[2], args[3]);
        assert(!actions.install && !actions.uninstall);
    }
    installed.uninstallExecutable.clear();
    actions = AvailableClientActions(installed, true, true, false, false);
    assert(!actions.install && !actions.uninstall); // broken install is not absent
    std::wstring executable, arguments;
    assert(SplitClientCommand(L"C:\\Program Files\\Google\\Drive File Stream\\130.0.2.0\\uninstall.exe", executable, arguments));
    assert(executable == L"C:\\Program Files\\Google\\Drive File Stream\\130.0.2.0\\uninstall.exe" && arguments.empty());
    assert(SplitClientCommand(L"\"C:\\Program Files\\Microsoft OneDrive\\OneDriveSetup.exe\" /uninstall /allusers", executable, arguments));
    assert(executable == L"C:\\Program Files\\Microsoft OneDrive\\OneDriveSetup.exe" && arguments == L"/uninstall /allusers");
    for (const wchar_t* bad : {L"setup.exe", L"cmd /c setup.exe", L"https://example.org/setup.exe", L"\\\\server\\setup.exe",
        L"C:\\app.exe.old", L"\"C:\\app.exe\"bad", L"\"C:\\app.exe", L""}) assert(!SplitClientCommand(bad, executable, arguments));
    assert(IsClientSetupExecutable(L"C:\\Microsoft OneDrive\\OneDriveSetup.exe", CloudClient::OneDrive));
    assert(IsClientSetupExecutable(L"C:\\Google\\uninstall.exe", CloudClient::GoogleDrive));
    assert(IsClientSetupExecutable(L"C:\\GoogleDriveSetup.exe", CloudClient::GoogleDrive));
    assert(!IsClientSetupExecutable(L"C:\\Windows\\System32\\cmd.exe", CloudClient::OneDrive));
    assert(!IsClientSetupExecutable(L"C:\\GoogleDriveSetup.exe", CloudClient::OneDrive));
}
