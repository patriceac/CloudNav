#pragma once
#include "logic.h"
#include <string>
#include <vector>

namespace cloudnav {
struct FolderOperation {
    size_t rowIndex = 0;
    std::wstring source;
    std::wstring target;
};

enum class FolderRecoveryState { WaitingForRelease, Ready, Complete, ChangedElsewhere };

inline FolderRecoveryState FolderRecoveryPosition(const FolderOperation& approved,
    const std::wstring& current, const std::wstring& localDefault) {
    if (PathEquals(current, approved.target)) return FolderRecoveryState::Complete;
    if (PathEquals(current, approved.source)) return FolderRecoveryState::WaitingForRelease;
    if (!localDefault.empty() && PathEquals(current, localDefault)) return FolderRecoveryState::Ready;
    return FolderRecoveryState::ChangedElsewhere;
}

inline bool FolderRecoveryMatches(const std::string& savedIdentity, const std::string& currentIdentity,
    const std::vector<FolderOperation>& plan, const std::wstring& oneDrive, const std::wstring& google) {
    if (savedIdentity.empty() || savedIdentity != currentIdentity || plan.empty()) return false;
    unsigned rows = 0;
    for (const auto& item : plan) {
        if (item.rowIndex >= 6 || (rows & (1u << item.rowIndex)) ||
            !IsVerifiedCloudTransition(true, item.source, item.target, oneDrive, google)) return false;
        rows |= 1u << item.rowIndex;
    }
    return true;
}
} // namespace cloudnav
