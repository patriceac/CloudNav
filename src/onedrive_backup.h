#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <string>

namespace cloudnav {
// Read backup switches only. The user makes every backup/retention choice in OneDrive.
enum class BackupState { Unknown, Off, On };
using BackupStates = std::array<BackupState, 6>;
enum class BackupReadiness { Unknown, Active, Released };

inline BackupReadiness SelectedBackupReadiness(const BackupStates& states, unsigned selected) {
    if (!selected || (selected & ~0x37u)) return BackupReadiness::Unknown;
    bool active = false;
    for (size_t row = 0; row < states.size(); ++row) {
        if (!(selected & (1u << row))) continue;
        if (states[row] == BackupState::Unknown) return BackupReadiness::Unknown;
        active |= states[row] == BackupState::On;
    }
    return active ? BackupReadiness::Active : BackupReadiness::Released;
}

bool OpenOneDriveBackupDialog(const std::wstring& root, const std::wstring& accountLabel,
    unsigned selected, std::atomic<bool>& cancelled, const std::function<bool()>& valid,
    const std::function<void(const std::wstring&)>& progress, BackupReadiness& readiness, std::wstring& error);
} // namespace cloudnav
