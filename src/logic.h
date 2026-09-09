#pragma once

#include <cwctype>
#include <string>

namespace cloudnav {

constexpr unsigned long kAllDriveBits = 0x03FFFFFFUL;

inline bool IsCloudNavigationAvailable(bool clientInstalled, bool clientDetectionComplete,
                                       bool accountRegistered, bool folderAvailable) {
    return clientInstalled && clientDetectionComplete && accountRegistered && folderAvailable;
}

inline bool CanChangeNavigationVisibility(bool available, bool currentlyVisible) {
    return available || currentlyVisible; // An unavailable, stale entry may still be hidden.
}

inline unsigned long DriveBit(wchar_t letter) {
    const wchar_t upper = static_cast<wchar_t>(std::towupper(letter));
    if (upper < L'A' || upper > L'Z') {
        return 0;
    }
    return 1UL << (upper - L'A');
}

inline bool IsDriveVisible(unsigned long noDrives, wchar_t letter) {
    const unsigned long bit = DriveBit(letter);
    return bit != 0 && (noDrives & bit) == 0;
}

inline unsigned long SetDriveVisible(unsigned long noDrives, wchar_t letter, bool visible) {
    const unsigned long bit = DriveBit(letter);
    if (bit == 0) {
        return noDrives & kAllDriveBits;
    }
    return (visible ? (noDrives & ~bit) : (noDrives | bit)) & kAllDriveBits;
}

inline std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned int backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

inline bool IsPathSeparator(wchar_t value) {
    return value == L'\\' || value == L'/';
}

inline std::wstring NormalizePath(std::wstring value, bool lowerCase = false) {
    for (wchar_t& character : value) {
        if (character == L'/') {
            character = L'\\';
        } else if (lowerCase) {
            character = static_cast<wchar_t>(std::towlower(character));
        }
    }
    while (value.size() > 3 && IsPathSeparator(value.back())) {
        value.pop_back();
    }
    if (lowerCase) {
        for (wchar_t& character : value) {
            character = static_cast<wchar_t>(std::towlower(character));
        }
    }
    return value;
}

inline bool PathEquals(const std::wstring& left, const std::wstring& right) {
    return NormalizePath(left, true) == NormalizePath(right, true);
}

inline bool PathIsWithin(const std::wstring& path, const std::wstring& root) {
    const std::wstring normalizedPath = NormalizePath(path, true);
    const std::wstring normalizedRoot = NormalizePath(root, true);
    if (normalizedPath.empty() || normalizedRoot.empty()) {
        return false;
    }
    if (normalizedPath == normalizedRoot) {
        return true;
    }
    if (normalizedPath.size() <= normalizedRoot.size() ||
        normalizedPath.compare(0, normalizedRoot.size(), normalizedRoot) != 0) {
        return false;
    }
    return IsPathSeparator(normalizedPath[normalizedRoot.size()]);
}

inline std::wstring PathRelativeToRoot(const std::wstring& path, const std::wstring& root) {
    const std::wstring normalizedPath = NormalizePath(path);
    const std::wstring normalizedRoot = NormalizePath(root);
    if (!PathIsWithin(normalizedPath, normalizedRoot) || PathEquals(normalizedPath, normalizedRoot)) {
        return {};
    }
    std::wstring relative = normalizedPath.substr(normalizedRoot.size());
    while (!relative.empty() && IsPathSeparator(relative.front())) {
        relative.erase(relative.begin());
    }
    return relative;
}

inline std::wstring PathLeaf(const std::wstring& path) {
    const std::wstring normalized = NormalizePath(path);
    const std::wstring::size_type separator = normalized.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return normalized;
    }
    return normalized.substr(separator + 1);
}

inline std::wstring JoinPath(const std::wstring& root, const std::wstring& relative) {
    std::wstring result = NormalizePath(root);
    std::wstring tail = relative;
    while (!tail.empty() && IsPathSeparator(tail.front())) {
        tail.erase(tail.begin());
    }
    if (result.empty()) {
        return tail;
    }
    if (!tail.empty() && !IsPathSeparator(result.back())) {
        result.push_back(L'\\');
    }
    result += tail;
    return result;
}

inline std::wstring CloudRelativePath(const std::wstring& currentPath,
                                      const std::wstring& oneDriveRoot,
                                      const std::wstring& googleDriveRoot,
                                      const std::wstring& defaultPath) {
    for (const std::wstring* root : {&oneDriveRoot, &googleDriveRoot}) {
        if (!root->empty()) {
            const std::wstring relative = PathRelativeToRoot(currentPath, *root);
            if (!relative.empty()) {
                return relative;
            }
        }
    }
    return PathLeaf(defaultPath);
}

inline bool PathsOverlap(const std::wstring& first, const std::wstring& second) {
    return PathIsWithin(first, second) || PathIsWithin(second, first);
}

enum class FolderLocationKind {
    Unavailable,
    ThisComputer,
    OneDrive,
    GoogleDrive,
    Other
};

inline FolderLocationKind ClassifyFolderLocation(const std::wstring& currentPath,
                                                  const std::wstring& defaultPath,
                                                  const std::wstring& oneDriveRoot,
                                                  const std::wstring& googleDriveRoot) {
    if (currentPath.empty()) {
        return FolderLocationKind::Unavailable;
    }
    if (!oneDriveRoot.empty() && PathIsWithin(currentPath, oneDriveRoot)) {
        return FolderLocationKind::OneDrive;
    }
    if (!googleDriveRoot.empty() && PathIsWithin(currentPath, googleDriveRoot)) {
        return FolderLocationKind::GoogleDrive;
    }
    if (!defaultPath.empty() && PathEquals(currentPath, defaultPath)) {
        return FolderLocationKind::ThisComputer;
    }
    return FolderLocationKind::Other;
}

inline bool IsMirroredCloudTransition(const std::wstring& source,
                                      const std::wstring& target,
                                      const std::wstring& oneDriveRoot,
                                      const std::wstring& googleDriveRoot) {
    if (oneDriveRoot.empty() || googleDriveRoot.empty()) {
        return false;
    }
    const bool sourceIsOneDrive = PathIsWithin(source, oneDriveRoot);
    const bool sourceIsGoogleDrive = PathIsWithin(source, googleDriveRoot);
    const bool targetIsOneDrive = PathIsWithin(target, oneDriveRoot);
    const bool targetIsGoogleDrive = PathIsWithin(target, googleDriveRoot);
    return (sourceIsOneDrive && targetIsGoogleDrive) ||
           (sourceIsGoogleDrive && targetIsOneDrive);
}

// Verification is directional and only covers the corresponding relative folder.
// A scheduled task, a reverse move, or a different destination is not proof.
inline bool IsVerifiedCloudTransition(bool oneDriveToGoogleVerified,
                                      const std::wstring& source,
                                      const std::wstring& target,
                                      const std::wstring& oneDriveRoot,
                                      const std::wstring& googleDriveRoot) {
    return oneDriveToGoogleVerified && PathIsWithin(source, oneDriveRoot) &&
           PathIsWithin(target, googleDriveRoot) &&
           PathEquals(PathRelativeToRoot(source, oneDriveRoot),
                      PathRelativeToRoot(target, googleDriveRoot));
}

inline bool FolderReturnsLocalAfterBackupRelease(bool backupWillDisable, bool managedFolder,
                                                 bool keepLocation,
                                                 const std::wstring& currentPath,
                                                 const std::wstring& oneDriveRoot) {
    return backupWillDisable && managedFolder && keepLocation &&
           PathIsWithin(currentPath, oneDriveRoot);
}

inline std::wstring ProviderAccountLabel(const std::wstring& provider, const std::wstring& account) {
    if (account.empty()) return provider;
    std::wstring normalized = account;
    std::wstring normalizedProvider = provider;
    for (auto& ch : normalized) ch = static_cast<wchar_t>(std::towlower(ch));
    for (auto& ch : normalizedProvider) ch = static_cast<wchar_t>(std::towlower(ch));
    return normalized.find(normalizedProvider) != std::wstring::npos
        ? account : provider + L" — " + account;
}

inline bool NeedsOneDriveBackupDisable(bool isManagedFolder,
                                       const std::wstring& source,
                                       const std::wstring& target,
                                       const std::wstring& oneDriveRoot,
                                       const std::wstring& googleDriveRoot) {
    return isManagedFolder &&
           !oneDriveRoot.empty() &&
           !googleDriveRoot.empty() &&
           PathIsWithin(source, oneDriveRoot) &&
           PathIsWithin(target, googleDriveRoot);
}

inline bool CanDetachOneDrive(bool oneDriveDetected,
                              bool oneDriveRootKnown,
                              bool personalFolderScanComplete,
                              bool anyPersonalFolderUsesOneDrive) {
    return oneDriveDetected && oneDriveRootKnown &&
           personalFolderScanComplete && !anyPersonalFolderUsesOneDrive;
}

}  // namespace cloudnav
