#pragma once

// Run only as an isolated Release scenario. Override the process's predefined
// roots with three real registry subtrees: one shared machine and two profiles.
// The production writer is exercised unchanged, including initiating-user SID
// selection when the current process is acting as an elevated helper.
int RunUserNavigationScenario(const std::wstring& resultPath) {
    const auto base = L"Software\\CloudNav\\NavigationTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY machine = nullptr, users = nullptr;
    bool passed = RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\Machine").c_str(), 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &machine, nullptr) == ERROR_SUCCESS &&
        RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\Users").c_str(), 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &users, nullptr) == ERROR_SUCCESS;
    const bool machineOverride = passed && RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine) == ERROR_SUCCESS;
    const bool usersOverride = machineOverride && RegOverridePredefKey(HKEY_USERS, users) == ERROR_SUCCESS;
    passed &= machineOverride && usersOverride;
    std::wstring error;
    const auto directory = std::filesystem::path(resultPath + L".profiles");
    const auto firstPath = (directory / L"Alice" / L"My Drive").wstring();
    const auto secondPath = (directory / L"Bob" / L"My Drive").wstring();
    try {
        std::filesystem::create_directories(firstPath);
        std::filesystem::create_directories(secondPath);
        const auto cls = L"Software\\Classes\\CLSID\\" + std::wstring(kMyDriveClsid);
        const auto bag = cls + L"\\Instance\\InitPropertyBag";
        const auto ns = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace\\" + std::wstring(kMyDriveClsid);
        const auto target = [&](const std::wstring& user) {
            std::wstring path;
            ReadRegistryString(HKEY_USERS, user + L"\\" + bag, L"TargetFolderPath", path);
            return path;
        };
        const auto pinned = [&](const std::wstring& user) {
            DWORD value = 0;
            ReadRegistryDword(HKEY_USERS, user + L"\\" + cls, L"System.IsPinnedToNameSpaceTree", value);
            return value;
        };
        // Legacy state reproduces the old bug: Alice's target in HKLM.
        passed = passed && WriteRegistryString(HKEY_LOCAL_MACHINE, bag, L"TargetFolderPath", firstPath) == ERROR_SUCCESS &&
            WriteRegistryDword(HKEY_LOCAL_MACHINE, cls, L"System.IsPinnedToNameSpaceTree", 1) == ERROR_SUCCESS &&
            WriteRegistryString(HKEY_LOCAL_MACHINE, ns, nullptr, L"My Drive") == ERROR_SUCCESS;
        passed = passed && ConfigureAllElevated(true, firstPath, false, L"", false, 0, L"Alice-SID", error);
        passed = passed && MigrateMyDriveForUser(L"Bob-SID", error) && target(L"Bob-SID").empty() &&
            pinned(L"Bob-SID") == 0 && !RegistryKeyExists(HKEY_USERS, L"Bob-SID\\" + ns);
        DWORD attributes = 0;
        passed = passed && ReadRegistryDword(HKEY_USERS, L"Bob-SID\\" + cls + L"\\ShellFolder", L"Attributes", attributes) &&
            (attributes & SFGAO_NONENUMERATED) != 0;
        passed = passed && ConfigureAllElevated(true, secondPath, false, L"", false, 0, L"Bob-SID", error) &&
            target(L"Alice-SID") == firstPath && target(L"Bob-SID") == secondPath;
        // Even a new auto-detected path cannot retarget an established entry.
        passed = passed && ConfigureAllElevated(false, secondPath, false, L"", false, 0, L"Alice-SID", error) &&
            target(L"Alice-SID") == firstPath && pinned(L"Alice-SID") == 0 && pinned(L"Bob-SID") == 1 &&
            RegistryKeyExists(HKEY_USERS, L"Bob-SID\\" + ns);
        passed = passed && ConfigureAllElevated(true, secondPath, false, L"", false, 0, L"Alice-SID", error) &&
            target(L"Alice-SID") == firstPath && target(L"Bob-SID") == secondPath && pinned(L"Bob-SID") == 1;
        std::wstring legacy;
        passed = passed && ReadRegistryString(HKEY_LOCAL_MACHINE, bag, L"TargetFolderPath", legacy) && legacy == firstPath &&
            RegistryKeyExists(HKEY_LOCAL_MACHINE, ns);
        for (const auto& user : {L"Alice-SID", L"Bob-SID"}) {
            std::wstring wow;
            passed = passed && ReadRegistryString(HKEY_USERS, std::wstring(user) + L"\\Software\\Classes\\Wow6432Node\\CLSID\\" +
                kMyDriveClsid + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", wow) && wow == target(user);
        }
    } catch (...) { passed = false; }
    if (usersOverride) RegOverridePredefKey(HKEY_USERS, nullptr);
    if (machineOverride) RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    if (users) RegCloseKey(users);
    if (machine) RegCloseKey(machine);
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    std::ofstream result(resultPath, std::ios::binary);
    result << (passed ? "{\"passed\":true,\"twoUserRegistryScopes\":true,\"machineUnchanged\":true,\"legacyMaskedPerUser\":true,\"targetPreserved\":true}" : "{\"passed\":false}");
    return passed && result.good() ? 0 : 1;
}
