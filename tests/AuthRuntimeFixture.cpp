#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>

// Offline subprocess fixture for production staging, validation, and logging.
int wmain(int argc, wchar_t** argv) {
    std::wstring config;
    for (int i = 1; i + 1 < argc; ++i) if (std::wstring(argv[i]) == L"--config") config = argv[i + 1];
    if (config.empty()) return 2;
    std::ifstream input(config, std::ios::binary);
    const std::string original(std::istreambuf_iterator<char>(input), {});
    input.close();
    if (argc > 1 && std::wstring(argv[1]) == L"lsd") return original.find("root-failure") != std::string::npos ? 9 : 0;
    if (original.find("provider-failure") != std::string::npos) {
        std::cout << R"({"State":"choose_type","Error":"ObjectHandle is Invalid"})"; return 0;
    }
    std::ofstream output(config, std::ios::binary | std::ios::trunc);
    output << original << "\n[cloudnav-onedrive]\ntype=onedrive\ntoken={\"access_token\":\"SYNTHETIC-PRIVATE-TOKEN\"}\n";
    if (original.find("incomplete") == std::string::npos) output << "drive_id=fixture\ndrive_type=personal\n";
    output.close();
    std::cerr << "SYNTHETIC-PRIVATE-TOKEN\n";
    std::cout << R"({"State":"","Option":null,"Error":""})";
    return 0;
}
