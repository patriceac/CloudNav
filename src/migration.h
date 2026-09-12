#pragma once

#include <windows.h>
#include <string>

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
int RunEmbeddedRcloneSelfTest(HINSTANCE instance, const std::wstring& resultPath);

}  // namespace cloudnav
