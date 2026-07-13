#include "AppWindow.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "resource.h"

namespace {

using Microsoft::WRL::ComPtr;

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect{};
    rect.left = left;
    rect.top = top;
    rect.right = right;
    rect.bottom = bottom;
    return rect;
}

D2D1_COLOR_F ToColor(COLORREF value, float alpha = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>(GetRValue(value)) / 255.0f,
        static_cast<float>(GetGValue(value)) / 255.0f,
        static_cast<float>(GetBValue(value)) / 255.0f,
        alpha);
}

D2D1_ROUNDED_RECT ToRoundedRect(const RECT& rect, float radius) {
    return D2D1::RoundedRect(
        D2D1::RectF(
            static_cast<float>(rect.left),
            static_cast<float>(rect.top),
            static_cast<float>(rect.right),
            static_cast<float>(rect.bottom)),
        radius,
        radius);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

std::wstring TimestampNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[16]{};
    wsprintfW(buffer, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring HitToToken(const AppWindow::HitRegion& region) {
    return std::to_wstring(static_cast<int>(region.type)) + L"|" + region.key + L"|" + (region.cautious ? L"1" : L"0");
}

const CleanItemDefinition* FindItemByKey(const std::wstring& key) {
    const auto& items = DiskCleanerCore::AllItems();
    const auto it = std::find_if(items.begin(), items.end(), [&](const CleanItemDefinition& item) {
        return item.key == key;
    });
    return it == items.end() ? nullptr : &(*it);
}

UINT GetSystemDpiValue() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForSystemFn = UINT(WINAPI*)();
        const auto fn = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
        if (fn != nullptr) {
            return (std::max)(96u, fn());
        }
    }
    return 96;
}

UINT GetWindowDpiValue(HWND hwnd) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto fn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (fn != nullptr) {
            return (std::max)(96u, fn(hwnd));
        }
    }
    return GetSystemDpiValue();
}

int CALLBACK EnumFontFamiliesProc(const LOGFONTW* font, const TEXTMETRICW*, DWORD, LPARAM param) {
    auto* found = reinterpret_cast<bool*>(param);
    if (font != nullptr) {
        *found = true;
    }
    return 1;
}

bool FontExists(const wchar_t* family_name) {
    LOGFONTW logfont{};
    lstrcpynW(logfont.lfFaceName, family_name, LF_FACESIZE);
    bool found = false;
    const HDC hdc = GetDC(nullptr);
    if (hdc != nullptr) {
        EnumFontFamiliesExW(hdc, &logfont, EnumFontFamiliesProc, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, hdc);
    }
    return found;
}

std::wstring ChooseUIFontFamily() {
    for (const wchar_t* family : {L"SF Pro Display", L"PingFang SC", L"Microsoft YaHei UI", L"Segoe UI"}) {
        if (FontExists(family)) {
            return family;
        }
    }
    return L"Segoe UI";
}

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, void*);

struct AccentPolicy {
    int accent_state;
    int accent_flags;
    unsigned int gradient_color;
    int animation_id;
};

struct WindowCompositionAttribData {
    int attribute;
    void* data;
    size_t size_of_data;
};

void TryEnableAcrylic(HWND hwnd) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }

    const auto set_attribute =
        reinterpret_cast<SetWindowCompositionAttributeFn>(GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (set_attribute == nullptr) {
        return;
    }

    AccentPolicy accent{};
    accent.accent_state = 4;
    accent.accent_flags = 2;
    accent.gradient_color = 0x96F7F7FA;

    WindowCompositionAttribData data{};
    data.attribute = 19;
    data.data = &accent;
    data.size_of_data = sizeof(accent);
    set_attribute(hwnd, &data);
}

void ApplySystemBackdrop(HWND hwnd) {
    const MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    const DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference, sizeof(corner_preference));

    const DWORD border_color = 0xFFFFFFFE;
    DwmSetWindowAttribute(hwnd, 34, &border_color, sizeof(border_color));

    for (const int attribute : {38, 1029}) {
        const int transient_window = 3;
        if (SUCCEEDED(DwmSetWindowAttribute(hwnd, attribute, &transient_window, sizeof(transient_window)))) {
            return;
        }
    }

    TryEnableAcrylic(hwnd);
}

bool CreateBitmapFromIconHandle(IWICImagingFactory* wic_factory, ID2D1RenderTarget* render_target,
                                HICON icon_handle, ID2D1Bitmap** bitmap) {
    if (wic_factory == nullptr || render_target == nullptr || icon_handle == nullptr || bitmap == nullptr) {
        return false;
    }

    ComPtr<IWICBitmap> wic_bitmap;
    if (FAILED(wic_factory->CreateBitmapFromHICON(icon_handle, &wic_bitmap))) {
        return false;
    }

    return SUCCEEDED(render_target->CreateBitmapFromWicBitmap(wic_bitmap.Get(), bitmap));
}

bool LoadTitleIconBitmapFromResource(IWICImagingFactory* wic_factory, ID2D1RenderTarget* render_target,
                                     ID2D1Bitmap** bitmap) {
    for (const int size : {256, 128, 64, 48, 32, 16}) {
        const HICON icon_handle = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (icon_handle == nullptr) {
            continue;
        }

        const bool loaded = CreateBitmapFromIconHandle(wic_factory, render_target, icon_handle, bitmap);
        DestroyIcon(icon_handle);
        if (loaded) {
            return true;
        }
    }
    return false;
}

std::wstring ButtonText(bool busy, const wchar_t* idle_text) {
    return busy ? L"处理中..." : idle_text;
}

} // namespace

AppWindow::AppWindow(HINSTANCE instance) : instance_(instance) {
    InitializeState();
}

AppWindow::~AppWindow() {
    JoinWorker();
    ShutdownVisuals();
}

void AppWindow::InitializeState() {
    is_admin_ = IsAdminUser();
    for (const auto& item : DiskCleanerCore::AllItems()) {
        size_labels_[item.key] = L"等待扫描";
    }
    LoadConfig();
    AppendLog(L"[" + TimestampNow() + L"] 准备就绪");
    if (!is_admin_) {
        AppendLog(L"[" + TimestampNow() + L"] 当前不是管理员，部分项目可能受限");
    }
    RecalculateContentHeight();
}

bool AppWindow::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &AppWindow::WndProcThunk;
    wc.hInstance = instance_;
    wc.lpszClassName = L"DiskCleanerCppModernWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, 0));
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    InitializeVisuals();
    UpdateDpi(GetSystemDpiValue());

    const int width = ScaleInt(static_cast<float>(kWindowWidth));
    const int height = ScaleInt(static_cast<float>(kWindowHeight));
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"DiskCleaner C++",
        WS_POPUP | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        return false;
    }

    UpdateDpi(GetWindowDpiValue(hwnd_));
    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screen_width - width) / 2;
    const int y = (screen_height - height) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_FRAMECHANGED);
    ApplyWindowEffects();
    return true;
}

LRESULT CALLBACK AppWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<AppWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT AppWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wparam == TRUE) {
            return 0;
        }
        break;
    case WM_CREATE:
        return 0;
    case WM_NCHITTEST: {
        const POINT screen_point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        POINT client_point = screen_point;
        ScreenToClient(hwnd_, &client_point);
        if (IsPointInRect(GetCloseRect(), client_point.x, client_point.y)) {
            return HTCLIENT;
        }
        if (client_point.y >= 0 && client_point.y < ScaleInt(54.0f)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const int width = ScaleInt(static_cast<float>(kWindowWidth));
        const int height = ScaleInt(static_cast<float>(kWindowHeight));
        info->ptMinTrackSize.x = width;
        info->ptMinTrackSize.y = height;
        info->ptMaxTrackSize.x = width;
        info->ptMaxTrackSize.y = height;
        return 0;
    }
    case WM_DPICHANGED: {
        UpdateDpi(HIWORD(wparam));
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        DiscardDeviceResources();
        RecalculateContentHeight();
        Invalidate();
        return 0;
    }
    case WM_SIZE:
        if (render_target_) {
            render_target_->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
        }
        ClampScroll();
        Invalidate();
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        TrackMouseEvent(&track);
        const auto hit = HitTest(point.x, point.y);
        const auto token = HitToToken(hit);
        if (token != hover_token_) {
            hover_token_ = token;
            Invalidate();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_token_.clear();
        Invalidate();
        return 0;
    case WM_MOUSEWHEEL:
        scroll_offset_ -= static_cast<short>(HIWORD(wparam)) / WHEEL_DELTA * ScaleInt(56.0f);
        ClampScroll();
        Invalidate();
        return 0;
    case WM_LBUTTONDOWN: {
        const auto hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        if (IsInteractive(hit)) {
            pressed_token_ = HitToToken(hit);
            SetCapture(hwnd_);
            Invalidate();
        } else {
            pressed_token_.clear();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        const auto released = HitTest(point.x, point.y);
        const auto released_token = HitToToken(released);
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }

        if (!pressed_token_.empty() && pressed_token_ == released_token) {
            switch (released.type) {
            case HitType::Close:
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                break;
            case HitType::ItemCard:
            case HitType::ItemToggle:
                ToggleItem(released.key);
                break;
            case HitType::SectionSelectAll:
                SetSectionSelection(released.cautious, true);
                break;
            case HitType::SectionClearAll:
                SetSectionSelection(released.cautious, false);
                break;
            case HitType::Scan:
                StartScan();
                break;
            case HitType::Clean:
                StartClean();
                break;
            default:
                break;
            }
        }

        pressed_token_.clear();
        Invalidate();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DWMCOMPOSITIONCHANGED:
        ApplyWindowEffects();
        Invalidate();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (busy_) {
            MessageBoxW(hwnd_, L"任务仍在运行，请等待完成后再关闭。", L"DiskCleaner C++", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        SaveConfig();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        SaveConfig();
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    case WM_APP_WORKER_EVENT: {
        std::unique_ptr<WorkerEvent> event(reinterpret_cast<WorkerEvent*>(lparam));
        switch (event->type) {
        case WorkerEventType::Log:
            AppendLog(event->text);
            break;
        case WorkerEventType::Status:
            SetStatus(event->text);
            break;
        case WorkerEventType::ScanItem:
            size_labels_[event->key] = event->text;
            break;
        case WorkerEventType::ScanFinished:
            busy_ = false;
            worker_running_ = false;
            SetStatus(L"扫描完成");
            JoinWorker();
            AppendLog(L"[" + TimestampNow() + L"] 扫描完成");
            break;
        case WorkerEventType::CleanFinished:
            busy_ = false;
            worker_running_ = false;
            SetStatus(L"清理完成");
            for (const auto& key : active_clean_keys_) {
                size_labels_[key] = L"已清理";
            }
            active_clean_keys_.clear();
            JoinWorker();
            AppendLog(L"[" + TimestampNow() + L"] 清理完成，释放空间: " + event->text);
            break;
        case WorkerEventType::CleanError:
            busy_ = false;
            worker_running_ = false;
            SetStatus(L"处理失败");
            active_clean_keys_.clear();
            JoinWorker();
            AppendLog(L"[" + TimestampNow() + L"] 错误: " + event->text);
            break;
        }
        Invalidate();
        return 0;
    }
    default:
        break;
    }

    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void AppWindow::InitializeVisuals() {
    if (!com_initialized_) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(hr)) {
            com_initialized_ = true;
        }
    }

    font_family_ = ChooseUIFontFamily();

    if (!d2d_factory_) {
        D2D1_FACTORY_OPTIONS options{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options,
                          reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    }

    if (!dwrite_factory_) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    }

    if (!wic_factory_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_));
    }
}

void AppWindow::ShutdownVisuals() {
    DiscardDeviceResources();
    log_body_format_.Reset();
    log_title_format_.Reset();
    button_format_.Reset();
    item_meta_format_.Reset();
    item_desc_format_.Reset();
    item_title_format_.Reset();
    action_format_.Reset();
    section_format_.Reset();
    title_format_.Reset();
    wic_factory_.Reset();
    dwrite_factory_.Reset();
    d2d_factory_.Reset();

    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

void AppWindow::LoadConfig() {
    for (const auto& key : DiskCleanerCore::DefaultSelection()) {
        selected_[key] = true;
    }

    std::ifstream file(std::filesystem::path(ConfigPath()), std::ios::binary);
    if (!file) {
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto content = buffer.str();
    const auto array_pos = content.find("\"selected_items\"");
    if (array_pos == std::string::npos) {
        return;
    }

    const auto start = content.find('[', array_pos);
    const auto end = content.find(']', start);
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return;
    }

    for (auto& [key, checked] : selected_) {
        checked = false;
    }

    std::string token;
    bool in_string = false;
    for (std::size_t i = start + 1; i < end; ++i) {
        const char ch = content[i];
        if (ch == '"') {
            if (in_string) {
                selected_[Utf8ToWide(token)] = true;
                token.clear();
            }
            in_string = !in_string;
        } else if (in_string) {
            token.push_back(ch);
        }
    }
}

void AppWindow::SaveConfig() const {
    std::wstring json = L"{\"selected_items\":[";
    bool first = true;
    for (const auto& item : DiskCleanerCore::AllItems()) {
        const auto it = selected_.find(item.key);
        if (it != selected_.end() && it->second) {
            if (!first) {
                json += L",";
            }
            json += L"\"" + item.key + L"\"";
            first = false;
        }
    }
    json += L"]}";

    std::ofstream file(std::filesystem::path(ConfigPath()), std::ios::binary | std::ios::trunc);
    if (!file) {
        return;
    }

    const auto utf8 = WideToUtf8(json);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

std::wstring AppWindow::ConfigPath() const {
    wchar_t module_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    std::wstring path = module_path;
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    }
    path += L"config.json";
    return path;
}

void AppWindow::ApplyWindowEffects() const {
    ApplySystemBackdrop(hwnd_);
}

void AppWindow::Invalidate() {
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::UpdateDpi(UINT dpi) {
    dpi_ = (std::max)(96u, dpi);
    title_format_.Reset();
    section_format_.Reset();
    action_format_.Reset();
    item_title_format_.Reset();
    item_desc_format_.Reset();
    item_meta_format_.Reset();
    button_format_.Reset();
    log_title_format_.Reset();
    log_body_format_.Reset();
}

float AppWindow::Scale(float value) const {
    return value * static_cast<float>(dpi_) / 96.0f;
}

int AppWindow::ScaleInt(float value) const {
    return static_cast<int>(Scale(value) + 0.5f);
}

void AppWindow::DiscardDeviceResources() {
    title_icon_bitmap_.Reset();
    render_target_.Reset();
}

bool AppWindow::CreateTextFormats() {
    if (!dwrite_factory_) {
        return false;
    }

    if (!title_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(16.8f), L"zh-cn", &title_format_))) {
            return false;
        }
        title_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        title_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!section_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(11.4f), L"zh-cn", &section_format_))) {
            return false;
        }
        section_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        section_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!action_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(10.4f), L"zh-cn", &action_format_))) {
            return false;
        }
        action_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        action_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!item_title_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(12.8f), L"zh-cn", &item_title_format_))) {
            return false;
        }
        item_title_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        item_title_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    if (!item_desc_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(10.7f), L"zh-cn", &item_desc_format_))) {
            return false;
        }
        item_desc_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        item_desc_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    if (!item_meta_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(10.8f), L"zh-cn", &item_meta_format_))) {
            return false;
        }
        item_meta_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        item_meta_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!button_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(11.8f), L"zh-cn", &button_format_))) {
            return false;
        }
        button_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        button_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!log_title_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                font_family_.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(11.2f), L"zh-cn", &log_title_format_))) {
            return false;
        }
        log_title_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        log_title_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!log_body_format_) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, Scale(10.3f), L"zh-cn", &log_body_format_))) {
            return false;
        }
        log_body_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        log_body_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    return true;
}

bool AppWindow::EnsureDeviceResources() {
    if (!render_target_) {
        if (!d2d_factory_) {
            return false;
        }

        RECT client{};
        GetClientRect(hwnd_, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT>((std::max)(1L, client.right - client.left)),
            static_cast<UINT>((std::max)(1L, client.bottom - client.top)));

        const HRESULT hr = d2d_factory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            D2D1::HwndRenderTargetProperties(hwnd_, size),
            &render_target_);
        if (FAILED(hr)) {
            return false;
        }

        render_target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        render_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }

    if (!title_icon_bitmap_ && wic_factory_ && render_target_) {
        LoadTitleIconBitmapFromResource(wic_factory_.Get(), render_target_.Get(), &title_icon_bitmap_);
    }

    return CreateTextFormats();
}

RECT AppWindow::GetCloseRect() const {
    RECT client{};
    if (hwnd_ != nullptr) {
        GetClientRect(hwnd_, &client);
    } else {
        client.right = ScaleInt(static_cast<float>(kWindowWidth));
        client.bottom = ScaleInt(static_cast<float>(kWindowHeight));
    }
    return MakeRect(client.right - ScaleInt(38.0f), ScaleInt(12.0f), client.right - ScaleInt(12.0f), ScaleInt(38.0f));
}

RECT AppWindow::GetViewportRect() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT log_rect = GetLogRect();
    return MakeRect(ScaleInt(14.0f), ScaleInt(64.0f), client.right - ScaleInt(14.0f), log_rect.top - ScaleInt(14.0f));
}

RECT AppWindow::GetLogRect() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    return MakeRect(ScaleInt(14.0f), client.bottom - ScaleInt(128.0f), client.right - ScaleInt(14.0f), client.bottom - ScaleInt(58.0f));
}

RECT AppWindow::GetScanButtonRect() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int left = ScaleInt(14.0f);
    const int gap = ScaleInt(10.0f);
    const int width = (client.right - ScaleInt(28.0f) - gap) / 2;
    const int top = client.bottom - ScaleInt(46.0f);
    return MakeRect(left, top, left + width, client.bottom - ScaleInt(14.0f));
}

RECT AppWindow::GetCleanButtonRect() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT scan_rect = GetScanButtonRect();
    const int gap = ScaleInt(10.0f);
    return MakeRect(scan_rect.right + gap, scan_rect.top, client.right - ScaleInt(14.0f), scan_rect.bottom);
}

void AppWindow::Paint() {
    if (!EnsureDeviceResources()) {
        return;
    }

    auto make_brush = [&](COLORREF value, float alpha = 1.0f) {
        ComPtr<ID2D1SolidColorBrush> brush;
        render_target_->CreateSolidColorBrush(ToColor(value, alpha), &brush);
        return brush;
    };

    auto draw_text = [&](IDWriteTextFormat* format, const RECT& rect, const std::wstring& text,
                         COLORREF color, float alpha, DWRITE_TEXT_ALIGNMENT alignment) {
        if (format == nullptr || text.empty()) {
            return;
        }
        format->SetTextAlignment(alignment);
        const auto brush = make_brush(color, alpha);
        render_target_->DrawTextW(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            D2D1::RectF(
                static_cast<float>(rect.left),
                static_cast<float>(rect.top),
                static_cast<float>(rect.right),
                static_cast<float>(rect.bottom)),
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    auto draw_card = [&](const RECT& rect, COLORREF fill, float fill_alpha, COLORREF border, float border_alpha,
                         float radius, bool pressed = false, float shadow_strength = 1.0f) {
        const float eased_shadow = pressed ? shadow_strength * 0.40f : shadow_strength;

        const RECT halo_shadow_rect = MakeRect(
            rect.left - ScaleInt(9.0f),
            rect.top - ScaleInt(6.0f),
            rect.right + ScaleInt(9.0f),
            rect.bottom + ScaleInt(8.0f));
        const RECT veil_shadow_rect = MakeRect(
            rect.left - ScaleInt(6.0f),
            rect.top - ScaleInt(3.0f),
            rect.right + ScaleInt(6.0f),
            rect.bottom + ScaleInt(5.0f));
        const RECT ambient_shadow_rect = MakeRect(
            rect.left - ScaleInt(4.0f),
            rect.top - ScaleInt(1.0f),
            rect.right + ScaleInt(4.0f),
            rect.bottom + ScaleInt(3.0f));
        const RECT soft_shadow_rect = MakeRect(
            rect.left - ScaleInt(2.0f),
            rect.top + ScaleInt(pressed ? 0.5f : 1.0f),
            rect.right + ScaleInt(2.0f),
            rect.bottom + ScaleInt(pressed ? 1.0f : 2.5f));
        const RECT key_shadow_rect = MakeRect(
            rect.left,
            rect.top + ScaleInt(pressed ? 0.5f : 1.5f),
            rect.right,
            rect.bottom + ScaleInt(pressed ? 1.0f : 3.0f));

        const auto halo_shadow_brush = make_brush(RGB(15, 23, 42), 0.012f * eased_shadow);
        const auto veil_shadow_brush = make_brush(RGB(15, 23, 42), 0.018f * eased_shadow);
        const auto ambient_shadow_brush = make_brush(RGB(15, 23, 42), 0.026f * eased_shadow);
        const auto soft_shadow_brush = make_brush(RGB(15, 23, 42), 0.034f * eased_shadow);
        const auto key_shadow_brush = make_brush(RGB(15, 23, 42), 0.042f * eased_shadow);
        render_target_->FillRoundedRectangle(ToRoundedRect(halo_shadow_rect, radius + Scale(7.0f)), halo_shadow_brush.Get());
        render_target_->FillRoundedRectangle(ToRoundedRect(veil_shadow_rect, radius + Scale(5.0f)), veil_shadow_brush.Get());
        render_target_->FillRoundedRectangle(ToRoundedRect(ambient_shadow_rect, radius + Scale(3.0f)), ambient_shadow_brush.Get());
        render_target_->FillRoundedRectangle(ToRoundedRect(soft_shadow_rect, radius + Scale(2.0f)), soft_shadow_brush.Get());
        render_target_->FillRoundedRectangle(ToRoundedRect(key_shadow_rect, radius + Scale(1.0f)), key_shadow_brush.Get());

        const auto fill_brush = make_brush(fill, fill_alpha);
        const auto border_brush = make_brush(border, border_alpha);
        const auto rounded = ToRoundedRect(rect, radius);
        render_target_->FillRoundedRectangle(rounded, fill_brush.Get());
        render_target_->DrawRoundedRectangle(rounded, border_brush.Get(), Scale(1.0f));
    };

    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT close_rect = GetCloseRect();
    const RECT viewport_rect = GetViewportRect();
    const RECT log_rect = GetLogRect();
    const RECT scan_rect = GetScanButtonRect();
    const RECT clean_rect = GetCleanButtonRect();

    hit_regions_.clear();
    hit_regions_.reserve(DiskCleanerCore::AllItems().size() * 2 + 8);

    render_target_->BeginDraw();
    render_target_->Clear(ToColor(RGB(245, 247, 251), 0.18f));

    const auto top_glow = make_brush(RGB(255, 255, 255), 0.10f);
    render_target_->FillEllipse(
        D2D1::Ellipse(
            D2D1::Point2F(static_cast<float>(ScaleInt(82.0f)), static_cast<float>(ScaleInt(18.0f))),
            Scale(96.0f),
            Scale(42.0f)),
        top_glow.Get());

    if (title_icon_bitmap_) {
        const RECT icon_tile_rect = MakeRect(ScaleInt(14.0f), ScaleInt(14.0f), ScaleInt(40.0f), ScaleInt(40.0f));
        const auto tile_fill = make_brush(RGB(255, 255, 255), 0.50f);
        const auto tile_stroke = make_brush(RGB(255, 255, 255), 0.24f);
        render_target_->FillRoundedRectangle(ToRoundedRect(icon_tile_rect, Scale(7.0f)), tile_fill.Get());
        render_target_->DrawRoundedRectangle(ToRoundedRect(icon_tile_rect, Scale(7.0f)), tile_stroke.Get(), Scale(1.0f));
        render_target_->DrawBitmap(
            title_icon_bitmap_.Get(),
            D2D1::RectF(
                static_cast<float>(ScaleInt(16.0f)),
                static_cast<float>(ScaleInt(16.0f)),
                static_cast<float>(ScaleInt(38.0f)),
                static_cast<float>(ScaleInt(38.0f))),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    draw_text(title_format_.Get(),
              MakeRect(ScaleInt(48.0f), ScaleInt(12.0f), client.right - ScaleInt(54.0f), ScaleInt(40.0f)),
              L"磁盘清理",
              RGB(24, 26, 32),
              1.0f,
              DWRITE_TEXT_ALIGNMENT_LEADING);

    const auto admin_brush = make_brush(is_admin_ ? RGB(55, 138, 255) : RGB(150, 154, 164), is_admin_ ? 0.20f : 0.16f);
    const auto admin_stroke = make_brush(is_admin_ ? RGB(126, 188, 255) : RGB(220, 224, 232), is_admin_ ? 0.32f : 0.24f);
    const RECT admin_rect = MakeRect(ScaleInt(48.0f), ScaleInt(38.0f), ScaleInt(112.0f), ScaleInt(58.0f));
    render_target_->FillRoundedRectangle(ToRoundedRect(admin_rect, Scale(9.0f)), admin_brush.Get());
    render_target_->DrawRoundedRectangle(ToRoundedRect(admin_rect, Scale(9.0f)), admin_stroke.Get(), Scale(1.0f));
    draw_text(item_meta_format_.Get(),
              admin_rect,
              is_admin_ ? L"管理员" : L"普通模式",
              is_admin_ ? RGB(24, 102, 228) : RGB(103, 108, 118),
              1.0f,
              DWRITE_TEXT_ALIGNMENT_CENTER);

    const bool close_hover = hover_token_ == HitToToken({ HitType::Close, close_rect, L"", false });
    const bool close_pressed = pressed_token_ == HitToToken({ HitType::Close, close_rect, L"", false });
    const auto close_fill = make_brush(close_hover ? RGB(255, 95, 87) : RGB(255, 255, 255), close_hover ? 0.94f : 0.42f);
    const auto close_stroke = make_brush(close_hover ? RGB(255, 255, 255) : RGB(96, 102, 112), close_hover ? 1.0f : 0.86f);
    const auto close_border = make_brush(RGB(255, 255, 255), close_hover ? 0.16f : 0.26f);
    const auto close_shadow = make_brush(RGB(15, 23, 42), close_pressed ? 0.022f : 0.038f);
    const RECT close_shadow_rect = MakeRect(
        close_rect.left,
        close_rect.top + ScaleInt(close_pressed ? 0.5f : 1.5f),
        close_rect.right,
        close_rect.bottom + ScaleInt(close_pressed ? 0.5f : 2.5f));
    render_target_->FillRoundedRectangle(ToRoundedRect(close_shadow_rect, Scale(8.5f)), close_shadow.Get());
    render_target_->FillRoundedRectangle(ToRoundedRect(close_rect, Scale(8.0f)), close_fill.Get());
    render_target_->DrawRoundedRectangle(ToRoundedRect(close_rect, Scale(8.0f)), close_border.Get(), Scale(1.0f));
    render_target_->DrawLine(
        D2D1::Point2F(static_cast<float>(close_rect.left + ScaleInt(8.0f)), static_cast<float>(close_rect.top + ScaleInt(8.0f))),
        D2D1::Point2F(static_cast<float>(close_rect.right - ScaleInt(8.0f)), static_cast<float>(close_rect.bottom - ScaleInt(8.0f))),
        close_stroke.Get(),
        Scale(1.6f));
    render_target_->DrawLine(
        D2D1::Point2F(static_cast<float>(close_rect.right - ScaleInt(8.0f)), static_cast<float>(close_rect.top + ScaleInt(8.0f))),
        D2D1::Point2F(static_cast<float>(close_rect.left + ScaleInt(8.0f)), static_cast<float>(close_rect.bottom - ScaleInt(8.0f))),
        close_stroke.Get(),
        Scale(1.6f));
    hit_regions_.push_back({ HitType::Close, close_rect, L"", false });

    const auto viewport_clip = D2D1::RectF(
        static_cast<float>(viewport_rect.left),
        static_cast<float>(viewport_rect.top),
        static_cast<float>(viewport_rect.right),
        static_cast<float>(viewport_rect.bottom));
    render_target_->PushAxisAlignedClip(viewport_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const auto& items = DiskCleanerCore::AllItems();
    const int section_header_height = ScaleInt(22.0f);
    const int item_height = ScaleInt(54.0f);
    const int item_gap = ScaleInt(8.0f);
    const int section_gap = ScaleInt(12.0f);
    int cursor_y = viewport_rect.top - scroll_offset_;

    auto draw_section = [&](const wchar_t* title, bool cautious) {
        const RECT section_rect = MakeRect(viewport_rect.left, cursor_y, viewport_rect.right, cursor_y + section_header_height);
        draw_text(section_format_.Get(),
                  MakeRect(section_rect.left + ScaleInt(2.0f), section_rect.top, section_rect.left + ScaleInt(118.0f), section_rect.bottom),
                  title,
                  RGB(34, 37, 44),
                  0.98f,
                  DWRITE_TEXT_ALIGNMENT_LEADING);

        const RECT select_rect = MakeRect(section_rect.right - ScaleInt(82.0f), section_rect.top, section_rect.right - ScaleInt(44.0f), section_rect.bottom);
        const RECT clear_rect = MakeRect(section_rect.right - ScaleInt(38.0f), section_rect.top, section_rect.right, section_rect.bottom);
        draw_text(action_format_.Get(), select_rect, L"全选", RGB(26, 112, 242), 0.92f, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(action_format_.Get(), clear_rect, L"清除", RGB(26, 112, 242), 0.76f, DWRITE_TEXT_ALIGNMENT_CENTER);
        hit_regions_.push_back({ HitType::SectionSelectAll, select_rect, L"", cautious });
        hit_regions_.push_back({ HitType::SectionClearAll, clear_rect, L"", cautious });
        cursor_y += section_header_height + ScaleInt(6.0f);
    };

    auto draw_item = [&](const CleanItemDefinition& item) {
        const RECT card_rect = MakeRect(viewport_rect.left + ScaleInt(2.0f), cursor_y, viewport_rect.right - ScaleInt(2.0f), cursor_y + item_height);
        const RECT toggle_rect = MakeRect(card_rect.right - ScaleInt(48.0f), card_rect.top + ScaleInt(17.0f), card_rect.right - ScaleInt(14.0f), card_rect.top + ScaleInt(37.0f));
        const bool checked = selected_[item.key];
        const auto card_token = HitToToken({ HitType::ItemCard, card_rect, item.key, item.cautious });
        const auto toggle_token = HitToToken({ HitType::ItemToggle, toggle_rect, item.key, item.cautious });
        const bool hovered = hover_token_ == card_token || hover_token_ == toggle_token;
        const bool pressed = pressed_token_ == card_token || pressed_token_ == toggle_token;

        COLORREF fill_color = RGB(255, 255, 255);
        float fill_alpha = checked ? 0.74f : (hovered ? 0.66f : 0.58f);
        COLORREF border_color = checked ? RGB(136, 188, 255) : RGB(255, 255, 255);
        float border_alpha = checked ? 0.48f : (hovered ? 0.34f : 0.22f);
        if (item.cautious && checked) {
            border_color = RGB(255, 182, 120);
            border_alpha = 0.46f;
        }
        draw_card(card_rect, fill_color, fill_alpha, border_color, border_alpha, Scale(14.0f), pressed, hovered || checked ? 1.28f : 1.08f);

        const auto accent = make_brush(
            item.cautious ? (checked ? RGB(255, 148, 71) : RGB(255, 196, 138)) : (checked ? RGB(55, 138, 255) : RGB(191, 225, 255)),
            checked ? 0.92f : 0.54f);
        render_target_->FillRoundedRectangle(
            ToRoundedRect(MakeRect(card_rect.left + ScaleInt(10.0f), card_rect.top + ScaleInt(12.0f),
                                   card_rect.left + ScaleInt(14.0f), card_rect.bottom - ScaleInt(12.0f)),
                          Scale(2.0f)),
            accent.Get());

        draw_text(item_title_format_.Get(),
                  MakeRect(card_rect.left + ScaleInt(22.0f), card_rect.top + ScaleInt(8.0f), card_rect.right - ScaleInt(64.0f), card_rect.top + ScaleInt(28.0f)),
                  item.name,
                  RGB(28, 31, 37),
                  1.0f,
                  DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(item_desc_format_.Get(),
                  MakeRect(card_rect.left + ScaleInt(22.0f), card_rect.top + ScaleInt(28.0f), card_rect.right - ScaleInt(92.0f), card_rect.bottom - ScaleInt(8.0f)),
                  item.description,
                  RGB(106, 111, 121),
                  0.98f,
                  DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(item_meta_format_.Get(),
                  MakeRect(card_rect.right - ScaleInt(126.0f), card_rect.top + ScaleInt(8.0f), card_rect.right - ScaleInt(54.0f), card_rect.top + ScaleInt(27.0f)),
                  size_labels_[item.key],
                  checked ? RGB(32, 92, 188) : RGB(96, 102, 112),
                  0.96f,
                  DWRITE_TEXT_ALIGNMENT_TRAILING);

        const auto toggle_track = make_brush(
            checked ? (item.cautious ? RGB(255, 148, 71) : RGB(0, 122, 255)) : RGB(218, 223, 230),
            checked ? 0.98f : 0.94f);
        const auto toggle_thumb = make_brush(RGB(255, 255, 255), 1.0f);
        render_target_->FillRoundedRectangle(ToRoundedRect(toggle_rect, Scale(10.0f)), toggle_track.Get());
        const int thumb_left = checked ? toggle_rect.right - ScaleInt(18.0f) : toggle_rect.left + ScaleInt(2.0f);
        render_target_->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(
                    static_cast<float>(thumb_left + ScaleInt(8.0f)),
                    static_cast<float>(toggle_rect.top + ScaleInt(10.0f))),
                Scale(8.0f),
                Scale(8.0f)),
            toggle_thumb.Get());

        hit_regions_.push_back({ HitType::ItemCard, card_rect, item.key, item.cautious });
        hit_regions_.push_back({ HitType::ItemToggle, toggle_rect, item.key, item.cautious });
        cursor_y += item_height + item_gap;
    };

    draw_section(L"常规清理", false);
    for (const auto& item : items) {
        if (!item.cautious) {
            draw_item(item);
        }
    }

    cursor_y += section_gap;
    draw_section(L"谨慎清理", true);
    for (const auto& item : items) {
        if (item.cautious) {
            draw_item(item);
        }
    }

    render_target_->PopAxisAlignedClip();

    const int viewport_height = viewport_rect.bottom - viewport_rect.top;
    const int max_scroll = (std::max)(0, content_height_ - viewport_height);
    if (max_scroll > 0) {
        const RECT track_rect = MakeRect(viewport_rect.right + ScaleInt(2.0f), viewport_rect.top + ScaleInt(8.0f), viewport_rect.right + ScaleInt(5.0f), viewport_rect.bottom - ScaleInt(8.0f));
        const auto track_brush = make_brush(RGB(255, 255, 255), 0.16f);
        render_target_->FillRoundedRectangle(ToRoundedRect(track_rect, Scale(2.5f)), track_brush.Get());

        const float ratio = static_cast<float>(viewport_height) / static_cast<float>(content_height_);
        const int thumb_height = (std::max)(ScaleInt(40.0f), static_cast<int>((track_rect.bottom - track_rect.top) * ratio + 0.5f));
        const float scroll_ratio = max_scroll > 0 ? static_cast<float>(scroll_offset_) / static_cast<float>(max_scroll) : 0.0f;
        const int thumb_top = track_rect.top + static_cast<int>((track_rect.bottom - track_rect.top - thumb_height) * scroll_ratio + 0.5f);
        const RECT thumb_rect = MakeRect(track_rect.left - ScaleInt(1.0f), thumb_top, track_rect.right + ScaleInt(1.0f), thumb_top + thumb_height);
        const auto thumb_brush = make_brush(RGB(117, 138, 170), 0.44f);
        render_target_->FillRoundedRectangle(ToRoundedRect(thumb_rect, Scale(3.0f)), thumb_brush.Get());
    }

    draw_card(log_rect, RGB(255, 255, 255), 0.54f, RGB(255, 255, 255), 0.24f, Scale(15.0f), false, 1.02f);
    draw_text(log_title_format_.Get(),
              MakeRect(log_rect.left + ScaleInt(12.0f), log_rect.top + ScaleInt(8.0f), log_rect.left + ScaleInt(96.0f), log_rect.top + ScaleInt(28.0f)),
              L"运行日志",
              RGB(29, 31, 37),
              1.0f,
              DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(item_meta_format_.Get(),
              MakeRect(log_rect.right - ScaleInt(92.0f), log_rect.top + ScaleInt(8.0f), log_rect.right - ScaleInt(12.0f), log_rect.top + ScaleInt(28.0f)),
              status_text_,
              RGB(42, 113, 222),
              0.96f,
              DWRITE_TEXT_ALIGNMENT_TRAILING);

    int line_y = log_rect.top + ScaleInt(30.0f);
    const int line_height = ScaleInt(16.0f);
    const int available_log_height = static_cast<int>(log_rect.bottom) - line_y - ScaleInt(8.0f);
    const int visible_lines = (std::max)(1, available_log_height / line_height);
    const int line_count = static_cast<int>(log_lines_.size());
    const int start_index = (std::max)(0, line_count - visible_lines);
    for (int i = start_index; i < line_count; ++i) {
        draw_text(log_body_format_.Get(),
                  MakeRect(log_rect.left + ScaleInt(12.0f), line_y, log_rect.right - ScaleInt(12.0f), line_y + line_height),
                  log_lines_[static_cast<std::size_t>(i)],
                  RGB(98, 103, 114),
                  0.96f,
                  DWRITE_TEXT_ALIGNMENT_LEADING);
        line_y += line_height;
    }

    const auto scan_region = HitRegion{ HitType::Scan, scan_rect, L"", false };
    const auto clean_region = HitRegion{ HitType::Clean, clean_rect, L"", false };
    const bool scan_hover = hover_token_ == HitToToken(scan_region);
    const bool clean_hover = hover_token_ == HitToToken(clean_region);
    const bool scan_pressed = pressed_token_ == HitToToken(scan_region);
    const bool clean_pressed = pressed_token_ == HitToToken(clean_region);

    draw_card(scan_rect,
              busy_ ? RGB(248, 249, 251) : (scan_hover ? RGB(255, 255, 255) : RGB(251, 252, 254)),
              busy_ ? 0.48f : (scan_hover ? 0.72f : 0.60f),
              RGB(255, 255, 255),
              busy_ ? 0.16f : 0.26f,
              Scale(13.0f),
              scan_pressed,
              busy_ ? 0.74f : 1.18f);

    draw_card(clean_rect,
              busy_ ? RGB(102, 142, 205) : (clean_hover ? RGB(0, 106, 221) : RGB(0, 122, 255)),
              busy_ ? 0.80f : (clean_hover ? 0.98f : 0.94f),
              RGB(255, 255, 255),
              0.12f,
              Scale(13.0f),
              clean_pressed,
              1.32f);

    hit_regions_.push_back(scan_region);
    hit_regions_.push_back(clean_region);

    draw_text(button_format_.Get(),
              scan_rect,
              ButtonText(busy_, L"扫描空间"),
              busy_ ? RGB(150, 155, 164) : RGB(34, 37, 44),
              1.0f,
              DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(button_format_.Get(),
              clean_rect,
              ButtonText(busy_, L"开始清理"),
              RGB(255, 255, 255),
              1.0f,
              DWRITE_TEXT_ALIGNMENT_CENTER);

    const HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

void AppWindow::StartScan() {
    if (busy_ || worker_running_) {
        return;
    }

    JoinWorker();
    busy_ = true;
    worker_running_ = true;
    log_lines_.clear();
    AppendLog(L"[" + TimestampNow() + L"] 开始扫描...");
    SetStatus(L"正在扫描");
    for (auto& [key, text] : size_labels_) {
        text = L"扫描中";
    }
    Invalidate();

    worker_ = std::thread([this]() {
        DiskCleanerCore cleaner;
        try {
            for (const auto& item : DiskCleanerCore::AllItems()) {
                const auto size = cleaner.ScanItem(item.key);
                PostWorkerEvent(WorkerEventType::ScanItem, item.key, cleaner.FormatSize(size));
            }
            PostWorkerEvent(WorkerEventType::ScanFinished);
        } catch (const std::exception& ex) {
            PostWorkerEvent(WorkerEventType::CleanError, L"", Utf8ToWide(ex.what()));
        }
    });
}

void AppWindow::StartClean() {
    if (busy_ || worker_running_) {
        return;
    }

    active_clean_keys_.clear();
    for (const auto& item : DiskCleanerCore::AllItems()) {
        if (selected_[item.key]) {
            active_clean_keys_.push_back(item.key);
        }
    }

    if (active_clean_keys_.empty()) {
        AppendLog(L"[" + TimestampNow() + L"] 未选择任何项目");
        Invalidate();
        return;
    }

    JoinWorker();
    busy_ = true;
    worker_running_ = true;
    SetStatus(L"开始清理");
    Invalidate();

    const auto keys = active_clean_keys_;
    worker_ = std::thread([this, keys]() {
        DiskCleanerCore cleaner(
            [this](const std::wstring& line) {
                PostWorkerEvent(WorkerEventType::Log, L"", L"[" + TimestampNow() + L"] " + line);
            },
            [this](const std::wstring& status) {
                PostWorkerEvent(WorkerEventType::Status, L"", status);
            });

        try {
            PostWorkerEvent(WorkerEventType::Log, L"", L"[" + TimestampNow() + L"] ---- 开始清理 ----");
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const auto* item = FindItemByKey(keys[i]);
                if (item != nullptr) {
                    PostWorkerEvent(WorkerEventType::Log, L"", L"[" + TimestampNow() + L"] 正在清理: " + item->name);
                }
                cleaner.RunCleanAction(keys[i]);
                const int progress = static_cast<int>(((i + 1) * 100) / keys.size());
                PostWorkerEvent(WorkerEventType::Status, L"", L"清理中 " + std::to_wstring(progress) + L"%");
            }
            PostWorkerEvent(WorkerEventType::Log, L"", L"[" + TimestampNow() + L"] --------------------------");
            if (!cleaner.errors().empty()) {
                PostWorkerEvent(WorkerEventType::Log, L"", L"[" + TimestampNow() + L"] 有 " +
                    std::to_wstring(cleaner.errors().size()) + L" 个项目被跳过");
            }
            PostWorkerEvent(WorkerEventType::CleanFinished, L"", cleaner.FormatSize(cleaner.total_cleaned()));
        } catch (const std::exception& ex) {
            PostWorkerEvent(WorkerEventType::CleanError, L"", Utf8ToWide(ex.what()));
        }
    });
}

void AppWindow::JoinWorker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AppWindow::PostWorkerEvent(WorkerEventType type, std::wstring key, std::wstring text) {
    auto* event = new WorkerEvent{ type, std::move(key), std::move(text) };
    PostMessageW(hwnd_, WM_APP_WORKER_EVENT, 0, reinterpret_cast<LPARAM>(event));
}

void AppWindow::AppendLog(const std::wstring& text) {
    log_lines_.push_back(text);
    while (log_lines_.size() > 80) {
        log_lines_.pop_front();
    }
}

void AppWindow::SetStatus(const std::wstring& text) {
    status_text_ = text;
}

void AppWindow::ToggleItem(const std::wstring& key) {
    if (busy_) {
        return;
    }
    selected_[key] = !selected_[key];
    Invalidate();
}

void AppWindow::SetSectionSelection(bool cautious, bool checked) {
    if (busy_) {
        return;
    }
    for (const auto& item : DiskCleanerCore::AllItems()) {
        if (item.cautious == cautious) {
            selected_[item.key] = checked;
        }
    }
    Invalidate();
}

void AppWindow::RecalculateContentHeight() {
    const int section_header_height = ScaleInt(22.0f);
    const int item_height = ScaleInt(54.0f);
    const int item_gap = ScaleInt(8.0f);
    const int section_gap = ScaleInt(12.0f);

    int normal_count = 0;
    int cautious_count = 0;
    for (const auto& item : DiskCleanerCore::AllItems()) {
        if (item.cautious) {
            ++cautious_count;
        } else {
            ++normal_count;
        }
    }

    content_height_ =
        (section_header_height + ScaleInt(6.0f)) +
        normal_count * (item_height + item_gap) +
        section_gap +
        (section_header_height + ScaleInt(6.0f)) +
        cautious_count * (item_height + item_gap);
}

void AppWindow::ClampScroll() {
    if (hwnd_ == nullptr) {
        scroll_offset_ = 0;
        return;
    }

    const RECT viewport = GetViewportRect();
    const int viewport_height = (std::max)(0, static_cast<int>(viewport.bottom) - static_cast<int>(viewport.top));
    const int max_scroll = (std::max)(0, content_height_ - viewport_height);
    scroll_offset_ = (std::clamp)(scroll_offset_, 0, max_scroll);
}

AppWindow::HitRegion AppWindow::HitTest(int x, int y) const {
    const RECT close_rect = GetCloseRect();
    if (IsPointInRect(close_rect, x, y)) {
        return { HitType::Close, close_rect, L"", false };
    }

    const RECT scan_rect = GetScanButtonRect();
    if (IsPointInRect(scan_rect, x, y)) {
        return { HitType::Scan, scan_rect, L"", false };
    }

    const RECT clean_rect = GetCleanButtonRect();
    if (IsPointInRect(clean_rect, x, y)) {
        return { HitType::Clean, clean_rect, L"", false };
    }

    for (auto it = hit_regions_.rbegin(); it != hit_regions_.rend(); ++it) {
        if (IsPointInRect(it->rect, x, y)) {
            return *it;
        }
    }
    return {};
}

bool AppWindow::IsPointInRect(const RECT& rect, int x, int y) const {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

bool AppWindow::IsInteractive(const HitRegion& region) const {
    if (region.type == HitType::None) {
        return false;
    }
    if (region.type == HitType::Close) {
        return true;
    }
    return !busy_;
}

bool AppWindow::IsAdminUser() const {
    BOOL is_member = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID admin_group = nullptr;
    if (AllocateAndInitializeSid(&authority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_member);
        FreeSid(admin_group);
    }
    return is_member == TRUE;
}
