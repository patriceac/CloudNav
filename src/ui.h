#pragma once

#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include "resource.h"

#pragma comment(lib, "gdiplus.lib")

namespace cloudnav::ui {

constexpr COLORREF Background = RGB(248, 250, 252);
constexpr COLORREF Text = RGB(30, 41, 59);
constexpr COLORREF Muted = RGB(91, 101, 116);
constexpr COLORREF Accent = RGB(0, 95, 184);
constexpr COLORREF Warning = RGB(146, 64, 14);

struct DialogTheme {
    HBRUSH background = CreateSolidBrush(Background);
    HFONT heading = nullptr;
    HFONT bold = nullptr;
    DialogTheme() = default;
    DialogTheme(const DialogTheme&) = delete;
    ~DialogTheme() {
        DeleteObject(background);
        if (heading) DeleteObject(heading);
        if (bold) DeleteObject(bold);
    }
    void Initialize(HWND dialog, int headingId = 0) {
        LOGFONTW font = {};
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0)), sizeof(font), &font);
        font.lfWeight = FW_SEMIBOLD;
        bold = CreateFontIndirectW(&font);
        font.lfHeight = -MulDiv(14, static_cast<int>(GetDpiForWindow(dialog)), 72);
        heading = CreateFontIndirectW(&font);
        if (headingId) SendDlgItemMessageW(dialog, headingId, WM_SETFONT, reinterpret_cast<WPARAM>(heading), TRUE);
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_CLOUDNAV), IMAGE_ICON, 16, 16, LR_SHARED)));
    }
    INT_PTR Color(HDC dc, COLORREF color = Text) const {
        SetTextColor(dc, color);
        SetBkColor(dc, Background);
        SetBkMode(dc, OPAQUE);
        return reinterpret_cast<INT_PTR>(background);
    }
};

// Native task dialogs provide explicit action labels and a safe default.
inline bool Confirm(HWND owner, const wchar_t* title, const wchar_t* instruction,
                    const std::wstring& content, const wchar_t* action, bool warning = false) {
    const TASKDIALOG_BUTTON buttons[] = {{IDYES, action}, {IDCANCEL, L"Cancel"}};
    TASKDIALOGCONFIG config = {sizeof(config)};
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = title;
    config.pszMainInstruction = instruction;
    config.pszContent = content.c_str();
    config.pszMainIcon = warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    config.cButtons = ARRAYSIZE(buttons);
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    int clicked = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&config, &clicked, nullptr, nullptr)) && clicked == IDYES;
}

struct ProviderImages {
    ULONG_PTR token = 0;
    IStream* streams[2] = {};
    Gdiplus::Image* images[2] = {};
    ProviderImages() = default;
    ProviderImages(const ProviderImages&) = delete;
    ~ProviderImages() {
        for (int i = 0; i < 2; ++i) {
            delete images[i];
            if (streams[i]) streams[i]->Release();
        }
        if (token) Gdiplus::GdiplusShutdown(token);
    }
    void Load(HINSTANCE instance) {
        if (token) return;
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { token = 0; return; }
        const int resources[] = {IDR_ONEDRIVE_LOGO, IDR_GOOGLE_DRIVE_LOGO};
        for (int i = 0; i < 2; ++i) {
            HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resources[i]), RT_RCDATA);
            if (!resource) continue;
            const DWORD size = SizeofResource(instance, resource);
            HGLOBAL loaded = LoadResource(instance, resource);
            const void* data = loaded ? LockResource(loaded) : nullptr;
            if (!data || !size) continue;
            HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
            if (!copy) continue;
            void* bytes = GlobalLock(copy);
            if (!bytes) { GlobalFree(copy); continue; }
            CopyMemory(bytes, data, size);
            GlobalUnlock(copy);
            if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &streams[i]))) { GlobalFree(copy); continue; }
            images[i] = Gdiplus::Image::FromStream(streams[i], FALSE);
            if (images[i] && images[i]->GetLastStatus() != Gdiplus::Ok) { delete images[i]; images[i] = nullptr; }
        }
    }
    void Draw(HDC dc, const RECT& rect, bool oneDrive) const {
        auto* image = images[oneDrive ? 0 : 1];
        if (!image || !image->GetWidth() || !image->GetHeight()) return;
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        const float scale = (std::min)(static_cast<float>(rect.right - rect.left) / image->GetWidth(),
                                       static_cast<float>(rect.bottom - rect.top) / image->GetHeight());
        const int width = static_cast<int>(image->GetWidth() * scale);
        const int height = static_cast<int>(image->GetHeight() * scale);
        graphics.DrawImage(image, rect.left + (rect.right - rect.left - width) / 2,
                           rect.top + (rect.bottom - rect.top - height) / 2, width, height);
    }
};

inline std::wstring ControlText(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const int length = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(length));
    return text;
}

} // namespace cloudnav::ui
