#pragma once

#include <windows.h>
#include <string>

struct CloudFolderHandoff {
    bool copyCompleted = false;
    std::string accountBinding;
};

namespace cloudnav {

enum class MigrationResult { Closed, ConfigureFolders, Synced };

MigrationResult ShowMigrationDialog(HWND owner, HINSTANCE instance, bool demoMode,
                                    const std::wstring& demoResultPath = {},
                                    CloudFolderHandoff* handoff = nullptr, bool quickSync = false);
std::string CurrentCloudAccountBinding();
int RunScheduledCloudSync(HINSTANCE instance, int count, wchar_t** arguments);
int RunEmbeddedRcloneSelfTest(HINSTANCE instance, const std::wstring& resultPath);

}  // namespace cloudnav
