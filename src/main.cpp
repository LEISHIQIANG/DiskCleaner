#include <windows.h>
#include <commctrl.h>

#include "AppWindow.h"

namespace {

void EnableDpiAwareness() {
    const auto user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        const auto set_dpi = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_dpi != nullptr) {
            set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }
    SetProcessDPIAware();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    EnableDpiAwareness();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    AppWindow window(instance);
    if (!window.Create()) {
        return 0;
    }

    ShowWindow(window.hwnd(), show_cmd == SW_HIDE ? SW_SHOWNORMAL : show_cmd);
    UpdateWindow(window.hwnd());

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
