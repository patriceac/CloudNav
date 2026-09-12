#pragma once
#include "../src/folder_recovery.h"
#include <cassert>

inline void TestFolderRecovery() {
    using namespace cloudnav;
    const FolderOperation documents{1, L"C:\\Users\\Alice\\OneDrive\\Documents", L"G:\\My Drive\\Documents"};
    const std::wstring one = L"C:\\Users\\Alice\\OneDrive", google = L"G:\\My Drive";
    const std::string identity = "Alice-SID:OneDrive-account:Google-account:mounted-root-id";
    assert(FolderRecoveryMatches(identity, identity, {documents}, one, google));
    assert(!FolderRecoveryMatches(identity, "Bob-SID:OneDrive-account:Google-account:mounted-root-id", {documents}, one, google));
    assert(!FolderRecoveryMatches(identity, "Alice-SID:OneDrive-account:other-Google:mounted-root-id", {documents}, one, google));
    assert(!FolderRecoveryMatches(identity, "Alice-SID:OneDrive-account:Google-account:replacement-mount", {documents}, one, google));
    assert(!FolderRecoveryMatches({}, {}, {documents}, one, google));
    assert(!FolderRecoveryMatches(identity, identity, {documents, documents}, one, google));
    auto wrong = documents; wrong.target = L"G:\\My Drive\\Other";
    assert(!FolderRecoveryMatches(identity, identity, {wrong}, one, google));
    wrong = documents; wrong.rowIndex = 6;
    assert(!FolderRecoveryMatches(identity, identity, {wrong}, one, google));
    const auto local = L"C:\\Users\\Alice\\Documents";
    // Neither time passing nor a release attempt proves that OneDrive released a folder.
    for (int attempt = 0; attempt < 120; ++attempt)
        assert(FolderRecoveryPosition(documents, documents.source, local) == FolderRecoveryState::WaitingForRelease);
    assert(FolderRecoveryPosition(documents, local, local) == FolderRecoveryState::Ready);
    assert(FolderRecoveryPosition(documents, documents.target, local) == FolderRecoveryState::Complete);
    assert(FolderRecoveryPosition(documents, L"D:\\Unreviewed", local) == FolderRecoveryState::ChangedElsewhere);
    // Partial completion keeps the approved destination for remaining localized folders.
    const FolderOperation pictures{2, one + L"\\Images", google + L"\\Images"};
    assert(FolderRecoveryPosition(pictures, L"C:\\Users\\Alice\\Pictures", L"C:\\Users\\Alice\\Pictures") == FolderRecoveryState::Ready);
    assert(FolderRecoveryMatches(identity, identity, {documents, pictures}, one, google));
}
