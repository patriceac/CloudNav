#pragma once

#include <windows.h>
#include <string>
#include <atomic>

struct CloudFolderHandoff {
    bool copyCompleted = false;
    std::string accountBinding;
};

namespace cloudnav {

enum class MigrationResult { Closed, ConfigureFolders };

MigrationResult ShowMigrationDialog(HWND owner, HINSTANCE instance, bool demoMode,
                                    const std::wstring& demoResultPath = {},
                                    CloudFolderHandoff* handoff = nullptr);
std::string CurrentCloudAccountBinding();
bool VerifyFolderCopy(const std::wstring& source, const std::wstring& destination,
                      std::atomic<bool>& cancelled, std::wstring& error);
int RunEmbeddedRcloneSelfTest(HINSTANCE instance, const std::wstring& resultPath);

}  // namespace cloudnav
