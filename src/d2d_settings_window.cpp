// SPDX-License-Identifier: GPL-3.0
// d2d_settings_window.cpp - Direct2D settings window lifecycle and message dispatch
#include "d2d_settings_window.h"
#include "color_utils.h"
#include "logger.h"

#include <algorithm>
#include <dwmapi.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

namespace echo {

using namespace Microsoft::WRL;

bool D2DSettingsWindow::classRegistered_ = false;

// ═══════════════════════════════
// 构造 / 析构
// ═══════════════════════════════

D2DSettingsWindow::D2DSettingsWindow() = default;
D2DSettingsWindow::~D2DSettingsWindow() { Close(); }

// 颜色工具由 color_utils.h 提供（namespace echo 自由函数）。

// ═══════════════════════════════
// 暗色模式检测
// ═══════════════════════════════

void D2DSettingsWindow::DetectDarkMode() {
    isDarkMode_ = false;

    DWORD appsUseLightTheme = 1;
    DWORD dataSize = sizeof(appsUseLightTheme);
    const LSTATUS status = ::RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &appsUseLightTheme,
        &dataSize);
    if (status == ERROR_SUCCESS) {
        isDarkMode_ = (appsUseLightTheme == 0);
    }
}

void D2DSettingsWindow::UpdateThemeColors() {
    if (isDarkMode_) {
        theme_.bg            = HexToColorF("#181A1B");
        theme_.surface       = HexToColorF("#24282B");
        theme_.border        = HexToColorF("#343A40");
        theme_.text          = HexToColorF("#F2F4F7");
        theme_.textSecondary = HexToColorF("#A7AFB8");
        theme_.accent        = HexToColorF("#2F80ED");
        theme_.accentHover   = HexToColorF("#4A94F2");
    } else {
        theme_.bg            = HexToColorF("#F7F8FA");
        theme_.surface       = HexToColorF("#FFFFFF");
        theme_.border        = HexToColorF("#D8DEE6");
        theme_.text          = HexToColorF("#17202A");
        theme_.textSecondary = HexToColorF("#667281");
        theme_.accent        = HexToColorF("#2563EB");
        theme_.accentHover   = HexToColorF("#1D4ED8");
    }
}

// ═══════════════════════════════
// 显示窗口
// ═══════════════════════════════

bool D2DSettingsWindow::Show(HINSTANCE hInstance, HWND parent, const Config& currentConfig) {
    if (hwnd_ && ::IsWindow(hwnd_)) {
        editedConfig_ = currentConfig;
        currentConfig_ = currentConfig;
        BuildControls(currentConfig);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        contentWidth_ = rc.right > 0 ? rc.right : kWinWidth;
        LayoutControls(contentWidth_);
        ::ShowWindow(hwnd_, SW_RESTORE);
        ::SetForegroundWindow(hwnd_);
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    hInstance_   = hInstance;
    parentWnd_   = parent;
    currentConfig_ = currentConfig;
    editedConfig_ = currentConfig;

    DetectDarkMode();
    UpdateThemeColors();

    // 注册窗口类（仅一次）
    if (!classRegistered_) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wc.lpfnWndProc   = &WndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        // 背景画刷：空（自绘）
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); // 兜底背景色（D2D 会覆盖）
        classRegistered_ = (::RegisterClassExW(&wc) != 0);
    }

    // 创建窗口（不使用 WS_EX_LAYERED，避免 D2D 渲染异常）
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,  // 显示在任务栏
        kWindowClass, L"任务栏歌词 - 设置",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, kWinWidth, kWinHeight,  // 先创建在 (0,0)，下面再定位
        parent, nullptr, hInstance, this);

    if (!hwnd_) return false;

    // 居中显示到主显示器（或父窗口所在显示器）
    HMONITOR hMon = MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - kWinWidth) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - kWinHeight) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, kWinWidth, kWinHeight, SWP_NOZORDER);

    // Win11 可用时使用系统圆角；Win10 不支持该属性时会安全返回失败。
    constexpr DWORD kDwmwaWindowCornerPreference = 33;
    constexpr int kDwmwcpRound = 2;
    int cornerPref = kDwmwcpRound;
    DwmSetWindowAttribute(hwnd_,
                          static_cast<DWMWINDOWATTRIBUTE>(kDwmwaWindowCornerPreference),
                          &cornerPref,
                          sizeof(cornerPref));

    // 初始化 D2D
    if (!InitD2D()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // 构建控件
    BuildControls(currentConfig);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    contentWidth_ = rc.right > 0 ? rc.right : kWinWidth;
    LayoutControls(contentWidth_);

    ShowWindow(hwnd_, SW_SHOW);
    SetFocus(hwnd_);
    return true;
}

bool D2DSettingsWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void D2DSettingsWindow::Close() {
    if (hwnd_) {
        if (colorPicker_.IsActive()) {
            colorPicker_.Deactivate(hwnd_);
            activeColorCtrl_ = nullptr;
        }
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ShutdownD2D();
}

// ═══════════════════════════════
// D2D 初始化
// ═══════════════════════════════

bool D2DSettingsWindow::InitD2D() {
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        d2dFactory_.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    if (FAILED(hr)) return false;

    // 创建文本格式
    dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"zh-Hans", &titleFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-Hans", &labelFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &valueFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-Hans", &sectionFmt_);
    sectionFmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); // 不换行

    dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-Hans", &hintFmt_);

    dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-Hans", &btnFmt_);
    if (btnFmt_) {
        btnFmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        btnFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        btnFmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    // 创建渲染目标（使用固定窗口尺寸，确保非零）
    hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_,
            D2D1_SIZE_U{static_cast<UINT>(kWinWidth), static_cast<UINT>(kWinHeight)}),
        &renderTarget_);
    if (FAILED(hr)) return false;

    // 创建预设画刷
    renderTarget_->CreateSolidColorBrush(theme_.bg, &bgBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.surface, &surfaceBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.border, &borderBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.text, &textBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.textSecondary, &textSecondaryBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.accent, &accentBrush_);
    renderTarget_->CreateSolidColorBrush(theme_.accentHover, &accentHoverBrush_);

    return true;
}

void D2DSettingsWindow::ShutdownD2D() {
    bgBrush_.Reset();
    surfaceBrush_.Reset();
    borderBrush_.Reset();
    textBrush_.Reset();
    textSecondaryBrush_.Reset();
    accentBrush_.Reset();
    accentHoverBrush_.Reset();

    titleFmt_.Reset();
    labelFmt_.Reset();
    valueFmt_.Reset();
    sectionFmt_.Reset();
    hintFmt_.Reset();
    btnFmt_.Reset();

    renderTarget_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
    controls_.clear();
}

LRESULT CALLBACK D2DSettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    D2DSettingsWindow* self = reinterpret_cast<D2DSettingsWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 1;
    }

    case WM_CREATE:
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (self) self->DrawAll();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        ::PostMessageW(hwnd, D2DSettingsWindow::kMsgCancel, 0, 0);
        return 0;

    case WM_SIZE:
        if (self && self->renderTarget_) {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                D2D1_SIZE_U size = D2D1::SizeU(width, height);
                self->renderTarget_->Resize(&size);
                self->contentWidth_ = static_cast<int>(width);
                self->LayoutControls(self->contentWidth_);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; // 防止闪烁

    case WM_LBUTTONDOWN: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

            // 标题栏关闭按钮
            if (PtInRect(&self->closeBtnRect_, {x, y})) {
                ::PostMessageW(hwnd, D2DSettingsWindow::kMsgCancel, 0, 0);
                return 0;
            }
            // 标题栏最小化按钮
            if (PtInRect(&self->minBtnRect_, {x, y})) {
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            }
            // 标题栏拖动区域
            if (PtInRect(&self->titleBarRect_, {x, y})) {
                ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                               MAKELPARAM(x, y));
                return 0;
            }

            bool hasOpenDropdown = false;
            for (const auto& c : self->controls_) {
                if (c.visible && c.type == CtrlType::DropdownRow && c.dropdownOpen) {
                    hasOpenDropdown = true;
                    break;
                }
            }

            Control* hit = self->HitTest(x, y);
            if (hit || self->colorPicker_.IsActive() || hasOpenDropdown) {
                self->OnMouseDown(x, y); // 点击了控件或颜色选择器弹窗激活时，正常处理
            } else {
                // 点击空白区域，启动系统拖动
                ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                               MAKELPARAM(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            self->OnMouseUp(x, y);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (self) {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            self->OnMouseMove(x, y);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self) {
            self->trackingMouse_ = false;
            self->hoverCtrl_ = nullptr;
            self->hoverClose_ = false;
            self->hoverMin_ = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }
    case WM_CHAR:
        if (self) self->OnChar(static_cast<wchar_t>(wParam));
        return 0;
    case WM_KEYDOWN:
        if (self) self->OnKeyDown(static_cast<UINT>(wParam));
        return 0;
    case WM_KILLFOCUS:
        if (self) self->OnLoseFocus();
        return 0;

    case D2DSettingsWindow::kMsgApplySave:
        if (self) self->ApplyAndSave();
        return 0;

    case D2DSettingsWindow::kMsgCancel:
        if (self) self->Cancel();
        return 0;

    case WM_TIMER: {
        // 光标闪烁
        if (self && wParam == 1) {
            LARGE_INTEGER freq, now;
            ::QueryPerformanceFrequency(&freq);
            ::QueryPerformanceCounter(&now);
            double nowSec = static_cast<double>(now.QuadPart) / freq.QuadPart;
            for (auto& c : self->controls_) {
                if (c.editing && (nowSec - c.caretBlinkTime) > 0.53) {
                    c.showCaret = !c.showCaret;
                    c.caretBlinkTime = nowSec;
                    InvalidateRect(self->hwnd_, nullptr, FALSE);
                    break;
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        // 设置对话框关闭，不退出主程序（不能调用 PostQuitMessage）
        return 0;

    case WM_NCDESTROY:
        if (self) {
            self->hwnd_ = nullptr;
            self->ShutdownD2D();
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace echo
