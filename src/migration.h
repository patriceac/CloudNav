#pragma once

#include <windows.h>
#include <string>

namespace cloudnav {

enum class MigrationResult { Closed, ConfigureFolders };

MigrationResult ShowMigrationDialog(HWND owner, HINSTANCE instance, bool demoMode,
                                    const std::wstring& demoResultPath = {});
int RunEmbeddedRcloneSelfTest(HINSTANCE instance, const std::wstring& resultPath);

}  // namespace cloudnav
