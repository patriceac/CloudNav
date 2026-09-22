#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>
#include "../src/auth_logic.h"

// Offline subprocess fixture for production staging, validation, and logging.
int wmain(int argc, wchar_t** argv) {
    std::wstring config;
    for (int i = 1; i + 1 < argc; ++i) if (std::wstring(argv[i]) == L"--config") config = argv[i + 1];
    if (config.empty()) return 2;
    std::ifstream input(config, std::ios::binary);
    const std::string original(std::istreambuf_iterator<char>(input), {});
    input.close();
    if (argc > 1 && std::wstring(argv[1]) == L"lsd") return original.find("root-failure") != std::string::npos ? 9 : 0;
    if (argc > 1 && std::wstring(argv[1]) == L"lsjson") {
        std::cout << (original.find("listing-failure") != std::string::npos ? "[invalid" : "[]");
        return 0;
    }
    if (original.find("provider-failure") != std::string::npos) {
        std::cout << R"({"State":"choose_type","Error":"ObjectHandle is Invalid"})"; return 0;
    }
    const bool google = argc > 3 && std::wstring(argv[3]) == L"cloudnav-gdrive";
    if (google) {
        std::string id, secret;
        bool cleared = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg.rfind(L"client_id=", 0) == 0) for (const auto ch : arg.substr(10)) id.push_back(static_cast<char>(ch));
            if (arg.rfind(L"client_secret=", 0) == 0) for (const auto ch : arg.substr(14)) secret.push_back(static_cast<char>(ch));
            cleared |= arg == L"token=";
        }
        auto fields = cloudnav::AuthFields(original, "cloudnav-gdrive");
        if (cloudnav::LegacyGoogleClient(id) || secret.empty() || (fields["client_id"] != id && !cleared)) return 12;
        auto updated = original;
        const auto section = cloudnav::SyncConfigSection(updated, "cloudnav-gdrive");
        if (section.first != std::string::npos) updated.erase(section.first, section.second);
        std::ofstream output(config, std::ios::binary | std::ios::trunc);
        output << updated << "\n[cloudnav-gdrive]\ntype=drive\nclient_id=" << id << "\nclient_secret=" << secret <<
            "\ntoken={\"access_token\":\"SYNTHETIC-NEW-USER-TOKEN\",\"refresh_token\":\"SYNTHETIC-REFRESH\"}\n";
        output.close();
        std::cout << R"({"State":"","Option":null,"Error":""})";
        return 0;
    }
    std::ofstream output(config, std::ios::binary | std::ios::trunc);
    output << original << "\n[cloudnav-onedrive]\ntype=onedrive\ntoken={\"access_token\":\"SYNTHETIC-PRIVATE-TOKEN\"}\n";
    if (original.find("incomplete") == std::string::npos) output << "drive_id=fixture\ndrive_type=personal\n";
    output.close();
    std::cerr << "SYNTHETIC-PRIVATE-TOKEN\n";
    std::cout << R"({"State":"","Option":null,"Error":""})";
    return 0;
}
