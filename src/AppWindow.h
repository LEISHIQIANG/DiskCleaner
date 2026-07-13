#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "DiskCleanerCore.h"

class AppWindow {
public:
    explicit AppWindow(HINSTANCE instance);
    ~AppWindow();

    bool Create();
    HWND hwnd() const noexcept { return hwnd_; }

    enum class HitType {
        None = 0,
        Close,
        Scan,
        Clean,
        ItemCard,
        ItemToggle,
        SectionSelectAll,
        SectionClearAll
    };

    enum class WorkerEventType {
        Log,
        Status,
        ScanItem,
        ScanFinished,
        CleanFinished,
        CleanError
    };

    struct HitRegion {
        HitType type = HitType::None;
        RECT rect{};
        std::wstring key;
        bool cautious = false;
    };

    struct WorkerEvent {
        WorkerEventType type = WorkerEventType::Log;
        std::wstring key;
        std::wstring text;
    };

private:
    static constexpr UINT WM_APP_WORKER_EVENT = WM_APP + 0x77;
    static constexpr int kWindowWidth = 320;
    static constexpr int kWindowHeight = 612;

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

    void InitializeState();
    void InitializeVisuals();
    void ShutdownVisuals();
    void LoadConfig();
    void SaveConfig() const;
    std::wstring ConfigPath() const;

    void ApplyWindowEffects() const;
    void Invalidate();
    void UpdateDpi(UINT dpi);
    float Scale(float value) const;
    int ScaleInt(float value) const;
    void DiscardDeviceResources();
    bool EnsureDeviceResources();
    bool CreateTextFormats();

    RECT GetCloseRect() const;
    RECT GetViewportRect() const;
    RECT GetLogRect() const;
    RECT GetScanButtonRect() const;
    RECT GetCleanButtonRect() const;

    void Paint();

    void StartScan();
    void StartClean();
    void JoinWorker();
    void PostWorkerEvent(WorkerEventType type, std::wstring key = {}, std::wstring text = {});
    void AppendLog(const std::wstring& text);
    void SetStatus(const std::wstring& text);
    void ToggleItem(const std::wstring& key);
    void SetSectionSelection(bool cautious, bool checked);
    void RecalculateContentHeight();
    void ClampScroll();

    HitRegion HitTest(int x, int y) const;
    bool IsPointInRect(const RECT& rect, int x, int y) const;
    bool IsInteractive(const HitRegion& region) const;
    bool IsAdminUser() const;

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    bool com_initialized_ = false;
    UINT dpi_ = 96;
    std::wstring font_family_;

    std::unordered_map<std::wstring, bool> selected_;
    std::unordered_map<std::wstring, std::wstring> size_labels_;
    std::deque<std::wstring> log_lines_;
    std::vector<HitRegion> hit_regions_;
    std::vector<std::wstring> active_clean_keys_;

    bool busy_ = false;
    bool is_admin_ = false;
    int scroll_offset_ = 0;
    int content_height_ = 0;
    std::wstring hover_token_;
    std::wstring pressed_token_;
    std::wstring status_text_ = L"就绪";

    std::thread worker_;
    std::atomic_bool worker_running_{ false };

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> title_icon_bitmap_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> section_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> action_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> item_title_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> item_desc_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> item_meta_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> button_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> log_title_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> log_body_format_;
};
