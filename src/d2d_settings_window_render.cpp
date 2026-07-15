// SPDX-License-Identifier: GPL-3.0
// D2D settings window rendering.
#include "d2d_settings_window.h"
#include "d2d_settings_window_internal.h"
#include "color_utils.h"
#include "settings_draw_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace echo {

using namespace Microsoft::WRL;
using namespace settings_window_detail;

void D2DSettingsWindow::DrawAll() {
    if (!renderTarget_) return;

    renderTarget_->BeginDraw();
    renderTarget_->Clear(theme_.bg);

    // 更新画刷颜色（暗色模式切换后可能变化）
    bgBrush_->SetColor(theme_.bg);
    surfaceBrush_->SetColor(theme_.surface);
    borderBrush_->SetColor(theme_.border);
    textBrush_->SetColor(theme_.text);
    textSecondaryBrush_->SetColor(theme_.textSecondary);
    accentBrush_->SetColor(theme_.accent);
    accentHoverBrush_->SetColor(theme_.accentHover);

    RECT clientRc; GetClientRect(hwnd_, &clientRc);
    const int clientH = clientRc.bottom;
    const int contentBottom = std::max(kTitleBarHeight, clientH - kActionBarHeight);

    // 绘制每个可见控件
    for (const auto& c : controls_) {
        if (!c.visible) continue;
        if (c.id == "applyBtn" || c.id == "cancelBtn") continue;
        // 裁剪：只绘制在可视区域内的控件
        if (c.rect.bottom - scrollOffset_ < kTitleBarHeight ||
            c.rect.top - scrollOffset_ > contentBottom)
            continue;

        switch (c.type) {
        case CtrlType::SectionHeader:   DrawSectionHeader(renderTarget_.Get(), c); break;
        case CtrlType::LabelRow:        DrawLabelRow(renderTarget_.Get(), c);      break;
        case CtrlType::ToggleRow:       DrawToggleRow(renderTarget_.Get(), c);      break;
        case CtrlType::SliderRow:       DrawSliderRow(renderTarget_.Get(), c);      break;
        case CtrlType::ColorRow:        DrawColorRow(renderTarget_.Get(), c);       break;
        case CtrlType::DropdownRow:     DrawDropdownRow(renderTarget_.Get(), c);    break;
        case CtrlType::ButtonRow:       DrawButtonRow(renderTarget_.Get(), c);      break;
        case CtrlType::ThemePresets:    DrawThemePresets(renderTarget_.Get(), c);   break;
        case CtrlType::HintText:        DrawHintText(renderTarget_.Get(), c);       break;
        default: break;
        }
    }

    DrawScrollBar(renderTarget_.Get(), clientH);

    // 绘制标题栏（在控件之上）
    DrawTitleBar(renderTarget_.Get());

    // 固定底部操作栏
    DrawBottomBar(renderTarget_.Get());
    for (const auto& c : controls_) {
        if (c.visible && (c.id == "applyBtn" || c.id == "cancelBtn")) {
            DrawButtonRow(renderTarget_.Get(), c);
        }
    }

    // 下拉弹层单独后绘制，避免被后续控件盖住
    for (const auto& c : controls_) {
        if (c.visible && c.type == CtrlType::DropdownRow && c.dropdownOpen) {
            DrawDropdownPopup(renderTarget_.Get(), c);
        }
    }

    // 绘制颜色选择器弹窗（在最上层）
    colorPicker_.Draw(renderTarget_.Get(), isDarkMode_, theme_,
                      valueFmt_.Get(), hintFmt_.Get(),
                      textSecondaryBrush_.Get(), scrollOffset_);

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderTarget_.Reset();
        RECT rc; GetClientRect(hwnd_, &rc);
        d2dFactory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd_,
                D2D1_SIZE_U{static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom)}),
            &renderTarget_);
        // 重建画刷
        if (renderTarget_) {
            renderTarget_->CreateSolidColorBrush(theme_.bg, &bgBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.surface, &surfaceBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.border, &borderBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.text, &textBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.textSecondary, &textSecondaryBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.accent, &accentBrush_);
            renderTarget_->CreateSolidColorBrush(theme_.accentHover, &accentHoverBrush_);
        }
    }
}

// 绘制辅助函数已移至 settings_draw_utils.h（inline，namespace echo）

void D2DSettingsWindow::DrawRowBackground(ID2D1RenderTarget* rt, const Control& c,
                                           float top, bool active) {
    const bool hovered = (hoverCtrl_ == &c) || active || c.pressed;
    D2D1_COLOR_F fill = hovered ? theme_.surface
                                : (isDarkMode_
                                       ? D2D1::ColorF(1.f, 1.f, 1.f, 0.025f)
                                       : D2D1::ColorF(0.f, 0.f, 0.f, 0.018f));
    if (hovered) fill.a = isDarkMode_ ? 0.72f : 0.92f;

    ComPtr<ID2D1SolidColorBrush> bg;
    rt->CreateSolidColorBrush(fill, &bg);
    FillRoundedRect(rt, bg.Get(),
                    static_cast<float>(c.rect.left) - kRowBleedX,
                    top + kRowTopInset,
                    static_cast<float>(c.rect.right - c.rect.left) + kRowBleedX * 2.f,
                    static_cast<float>(rowHeight_) - kRowTopInset * 2.f,
                    kRowRadius);

    if (hovered) {
        D2D1_COLOR_F border = active ? theme_.accent : theme_.border;
        border.a = active ? 0.65f : 0.55f;
        ComPtr<ID2D1SolidColorBrush> br;
        rt->CreateSolidColorBrush(border, &br);
        DrawRoundedRect(rt, br.Get(), 1.f,
                        static_cast<float>(c.rect.left) - kRowBleedX,
                        top + kRowTopInset,
                        static_cast<float>(c.rect.right - c.rect.left) + kRowBleedX * 2.f,
                        static_cast<float>(rowHeight_) - kRowTopInset * 2.f,
                        kRowRadius);
    }
}

void D2DSettingsWindow::DrawBottomBar(ID2D1RenderTarget* rt) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float y = static_cast<float>(std::max(kTitleBarHeight, static_cast<int>(rc.bottom) - kActionBarHeight));
    const float w = static_cast<float>(rc.right);
    const float h = static_cast<float>(kActionBarHeight);

    D2D1_COLOR_F barColor = theme_.bg;
    barColor.a = isDarkMode_ ? 0.96f : 0.98f;
    ComPtr<ID2D1SolidColorBrush> barBg;
    rt->CreateSolidColorBrush(barColor, &barBg);
    rt->FillRectangle(D2D1::RectF(0.f, y, w, y + h), barBg.Get());

    ComPtr<ID2D1SolidColorBrush> line;
    rt->CreateSolidColorBrush(theme_.border, &line);
    rt->DrawLine(D2D1::Point2F(0.f, y + 0.5f), D2D1::Point2F(w, y + 0.5f), line.Get());

    const wchar_t* versionText = L"Echo Taskbar Lyrics v1.6.1";
    DrawTextLine(rt, hintFmt_.Get(), textSecondaryBrush_.Get(),
                 versionText, 24.f, y + 23.f, 240.f);
}

void D2DSettingsWindow::DrawScrollBar(ID2D1RenderTarget* rt, int clientHeight) {
    const int viewportH = std::max(1, clientHeight - kActionBarHeight);
    const int maxScroll = std::max(0, totalContentHeight_ - viewportH);
    if (maxScroll <= 0) return;

    const float trackTop = static_cast<float>(kTitleBarHeight + 8);
    const float trackBottom = static_cast<float>(std::max(kTitleBarHeight + 48, clientHeight - kActionBarHeight - 8));
    const float trackH = std::max(1.f, trackBottom - trackTop);
    const float minThumbH = std::min(32.f, trackH);
    const float thumbH = std::clamp(trackH * static_cast<float>(viewportH) /
                                        static_cast<float>(std::max(totalContentHeight_, viewportH)),
                                    minThumbH, trackH);
    const float ratio = static_cast<float>(scrollOffset_) / static_cast<float>(maxScroll);
    const float thumbY = trackTop + (trackH - thumbH) * ratio;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float x = static_cast<float>(rc.right) - 9.f;
    const float w = 4.f;

    D2D1_COLOR_F trackColor = theme_.border;
    trackColor.a = isDarkMode_ ? 0.28f : 0.38f;
    D2D1_COLOR_F thumbColor = theme_.textSecondary;
    thumbColor.a = isDarkMode_ ? 0.56f : 0.46f;

    ComPtr<ID2D1SolidColorBrush> trackBr;
    ComPtr<ID2D1SolidColorBrush> thumbBr;
    rt->CreateSolidColorBrush(trackColor, &trackBr);
    rt->CreateSolidColorBrush(thumbColor, &thumbBr);
    FillRoundedRect(rt, trackBr.Get(), x, trackTop, w, trackH, 2.f);
    FillRoundedRect(rt, thumbBr.Get(), x, thumbY, w, thumbH, 2.f);
}

void D2DSettingsWindow::DrawSectionHeader(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float cy = top + 13.f;

    // 左侧竖线装饰
    ComPtr<ID2D1SolidColorBrush> accentBr;
    rt->CreateSolidColorBrush(theme_.accent, &accentBr);
    FillRoundedRect(rt, accentBr.Get(),
                    static_cast<float>(c.rect.left), cy - 8.f, 3.f, 16.f, 1.5f);

    // 标题文字
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, sectionFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left) + 12.f, cy - 9.f, 160.f);

    ComPtr<ID2D1SolidColorBrush> lineBr;
    D2D1_COLOR_F lineColor = theme_.border;
    lineColor.a = isDarkMode_ ? 0.55f : 0.75f;
    rt->CreateSolidColorBrush(lineColor, &lineBr);
    rt->DrawLine(D2D1::Point2F(static_cast<float>(c.rect.left) + 112.f, cy),
                 D2D1::Point2F(static_cast<float>(c.rect.right), cy),
                 lineBr.Get(), 1.f);
}

void D2DSettingsWindow::DrawLabelRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;
    DrawRowBackground(rt, c, top, c.editing);

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 180.f);

    // 输入框区域
    float inputLeft = static_cast<float>(c.rect.right) - kFieldWidth;
    float inputTop = top + kFieldTopInset;
    float inputW = kFieldWidth - kFieldRightInset;
    float inputH = kFieldHeight;

    // 背景
    ComPtr<ID2D1SolidColorBrush> inputBg;
    rt->CreateSolidColorBrush(c.editing ? theme_.bg : theme_.surface, &inputBg);
    FillRoundedRect(rt, inputBg.Get(), inputLeft, inputTop, inputW, inputH, 5.f);

    // 边框（焦点时高亮）
    ComPtr<ID2D1SolidColorBrush> borderBr;
    rt->CreateSolidColorBrush(c.editing ? theme_.accent : theme_.border, &borderBr);
    DrawRoundedRect(rt, borderBr.Get(), 1.f, inputLeft, inputTop, inputW, inputH, 5.f);

    // 文字
    std::wstring wideVal = Utf8ToWide(c.textValue);
    ComPtr<ID2D1SolidColorBrush> txtBr;
    rt->CreateSolidColorBrush(c.readOnly ? theme_.textSecondary : theme_.text, &txtBr);
    DrawTextLine(rt, valueFmt_.Get(), txtBr.Get(),
                 wideVal.c_str(), inputLeft + 8.f, mid - 7.f, inputW - 16.f);

    // 光标
    if (c.editing && c.showCaret) {
        ComPtr<ID2D1SolidColorBrush> caretBr;
        rt->CreateSolidColorBrush(theme_.text, &caretBr);
        // 计算光标 X 位置
        float caretX = inputLeft + 8.f;
        if (c.caretPos > 0 && !wideVal.empty()) {
            DWRITE_TEXT_METRICS metrics{};
            ComPtr<IDWriteTextLayout> layout;
            dwriteFactory_->CreateTextLayout(
                wideVal.c_str(), static_cast<UINT>(c.caretPos),
                valueFmt_.Get(), 200.f, 30.f, &layout);
            if (layout) layout->GetMetrics(&metrics);
            caretX += metrics.widthIncludingTrailingWhitespace;
        }
        rt->DrawLine(D2D1::Point2F(caretX, inputTop + 4.f),
                     D2D1::Point2F(caretX, inputTop + inputH - 4.f),
                     caretBr.Get(), 1.f);
    }

    // 字体选择按钮（fontFamily / cardFontFamily 控件）
    if (c.id == "fontFamily" || c.id == "cardFontFamily") {
        float btnX = inputLeft - 58.f;
        float btnW = 48.f;
        float btnH = kFieldHeight;
        ComPtr<ID2D1SolidColorBrush> btnBg;
        rt->CreateSolidColorBrush(hoverCtrl_ == &c ? theme_.bg : theme_.surface, &btnBg);
        FillRoundedRect(rt, btnBg.Get(), btnX, top + kFieldTopInset, btnW, btnH, 5.f);
        ComPtr<ID2D1SolidColorBrush> btnBorder;
        rt->CreateSolidColorBrush(hoverCtrl_ == &c ? theme_.accent : theme_.border, &btnBorder);
        DrawRoundedRect(rt, btnBorder.Get(), 1.f, btnX, top + kFieldTopInset, btnW, btnH, 5.f);
        const wchar_t* pickText = L"选择";
        DrawTextLine(rt, hintFmt_.Get(), textBrush_.Get(),
                     pickText, btnX + 9.f, mid - 6.f, 36.f);
    }
}

void D2DSettingsWindow::DrawToggleRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;
    DrawRowBackground(rt, c, top, c.toggleValue);

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 220.f);

    // Toggle 开关（右侧）
    float tw = 42.f, th = 22.f, tr = 11.f;
    float tx = static_cast<float>(c.rect.right) - tw - 18.f;
    float ty = mid - th / 2.f;

    D2D1_COLOR_F trackColor = c.toggleValue ? theme_.accent : theme_.border;
    trackColor.a = c.toggleValue ? (isDarkMode_ ? 0.38f : 0.20f) : (isDarkMode_ ? 0.55f : 0.70f);
    ComPtr<ID2D1SolidColorBrush> trackBr;
    rt->CreateSolidColorBrush(trackColor, &trackBr);
    FillRoundedRect(rt, trackBr.Get(), tx, ty, tw, th, tr);
    D2D1_COLOR_F trackBorderColor = c.toggleValue ? theme_.accent : theme_.border;
    trackBorderColor.a = c.toggleValue ? 0.72f : 0.85f;
    ComPtr<ID2D1SolidColorBrush> trackBorder;
    rt->CreateSolidColorBrush(trackBorderColor, &trackBorder);
    DrawRoundedRect(rt, trackBorder.Get(), 1.f, tx, ty, tw, th, tr);

    // 圆形滑块
    float knobR = 8.f;
    float knobPad = (th - knobR * 2.f) * 0.5f;
    float knobLeft = c.toggleValue ? tx + tw - knobR * 2.f - knobPad : tx + knobPad;
    float knobX = knobLeft + knobR;
    float knobY = ty + th * 0.5f;
    ComPtr<ID2D1SolidColorBrush> knobBr;
    rt->CreateSolidColorBrush(c.toggleValue ? theme_.accent : theme_.surface, &knobBr);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, knobY), knobR, knobR), knobBr.Get());
    ComPtr<ID2D1SolidColorBrush> knobBorder;
    rt->CreateSolidColorBrush(c.toggleValue ? theme_.accentHover : theme_.border, &knobBorder);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, knobY), knobR, knobR), knobBorder.Get(), 1.f);
}

void D2DSettingsWindow::DrawSliderRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;
    DrawRowBackground(rt, c, top, captureCtrl_ == &c);

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 180.f);

    // 滑块轨道
    float sl = static_cast<float>(c.rect.right) - kSliderLeftOffset;
    float sw = sliderWidth_;
    float st = mid - 2.f;
    float sh = 4.f;

    ComPtr<ID2D1SolidColorBrush> trackBr;
    rt->CreateSolidColorBrush(theme_.border, &trackBr);
    FillRoundedRect(rt, trackBr.Get(), sl, st, sw, sh, 2.f);

    // 已填充部分
    float fillRatio = (c.sliderValue - c.sliderMin) / (c.sliderMax - c.sliderMin);
    float fillW = sw * fillRatio;
    ComPtr<ID2D1SolidColorBrush> fillBr;
    rt->CreateSolidColorBrush(theme_.accent, &fillBr);
    FillRoundedRect(rt, fillBr.Get(), sl, st, fillW, sh, 2.f);

    // 滑块手柄
    float handleX = sl + fillW;
    float handleR = 7.f;
    ComPtr<ID2D1SolidColorBrush> handleBr;
    rt->CreateSolidColorBrush(theme_.accent, &handleBr);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(handleX, mid), handleR, handleR), handleBr.Get());
    ComPtr<ID2D1SolidColorBrush> handleBorder;
    rt->CreateSolidColorBrush(theme_.bg, &handleBorder);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(handleX, mid), handleR, handleR), handleBorder.Get(), 2.f);

    // 数值显示
    char valBuf[32];
    if (c.sliderSuffix == "%")
        std::snprintf(valBuf, sizeof(valBuf), "%.0f%s", c.sliderValue, c.sliderSuffix.c_str());
    else if (c.sliderSuffix.find("ms") != std::string::npos)
        std::snprintf(valBuf, sizeof(valBuf), "%.0f %s", c.sliderValue, c.sliderSuffix.c_str());
    else if (c.sliderSuffix.find("px/s") != std::string::npos)
        std::snprintf(valBuf, sizeof(valBuf), "%.0f %s", c.sliderValue, c.sliderSuffix.c_str());
    else if (c.sliderSuffix.find("px") != std::string::npos)
        std::snprintf(valBuf, sizeof(valBuf), "%.0f %s", c.sliderValue, c.sliderSuffix.c_str());
    else if (c.sliderSuffix.find("FPS") != std::string::npos)
        std::snprintf(valBuf, sizeof(valBuf), "%.0f %s", c.sliderValue, c.sliderSuffix.c_str());
    else
        std::snprintf(valBuf, sizeof(valBuf), "%.0f", c.sliderValue);

    std::wstring wval(valBuf, valBuf + strlen(valBuf));
    DrawTextLine(rt, valueFmt_.Get(), textSecondaryBrush_.Get(),
                 wval.c_str(), sl + sw + 12.f, mid - 7.f, kSliderValueWidth);
}

void D2DSettingsWindow::DrawColorRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;
    DrawRowBackground(rt, c, top);

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 180.f);

    // 颜色方块
    float cbSz = 28.f;
    float cbX = static_cast<float>(c.rect.right) - kFieldRightInset - cbSz - 6.f - kColorHexWidth;
    float cbY = top + (static_cast<float>(rowHeight_) - cbSz) / 2.f;
    ComPtr<ID2D1SolidColorBrush> colorBr;
    rt->CreateSolidColorBrush(c.colorValue, &colorBr);
    FillRoundedRect(rt, colorBr.Get(), cbX, cbY, cbSz, cbSz, 5.f);
    ComPtr<ID2D1SolidColorBrush> colorBorder;
    rt->CreateSolidColorBrush(theme_.border, &colorBorder);
    DrawRoundedRect(rt, colorBorder.Get(), 1.5f, cbX, cbY, cbSz, cbSz, 5.f);

    // Hex 输入
    float hexX = cbX + cbSz + 6.f;
    float hexW = kColorHexWidth;
    float hexH = kFieldHeight;
    float hexY = top + kFieldTopInset;
    ComPtr<ID2D1SolidColorBrush> hexBg;
    rt->CreateSolidColorBrush(theme_.surface, &hexBg);
    FillRoundedRect(rt, hexBg.Get(), hexX, hexY, hexW, hexH, 5.f);
    ComPtr<ID2D1SolidColorBrush> hexBorder;
    rt->CreateSolidColorBrush(theme_.border, &hexBorder);
    DrawRoundedRect(rt, hexBorder.Get(), 1.f, hexX, hexY, hexW, hexH, 5.f);

    std::wstring wideVal = Utf8ToWide(c.textValue);
    DrawTextLine(rt, valueFmt_.Get(), textBrush_.Get(),
                 wideVal.c_str(), hexX + 6.f, mid - 7.f, hexW - 12.f);
}

void D2DSettingsWindow::DrawDropdownRow(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    float mid = top + static_cast<float>(rowHeight_) / 2.f;
    DrawRowBackground(rt, c, top, c.dropdownOpen);

    // 标签
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 180.f);

    // 下拉框背景
    float ddW = kFieldWidth, ddH = kFieldHeight;
    float ddX = static_cast<float>(c.rect.right) - ddW - kFieldRightInset;
    float ddY = top + kFieldTopInset;
    ComPtr<ID2D1SolidColorBrush> ddBg;
    rt->CreateSolidColorBrush(theme_.surface, &ddBg);
    FillRoundedRect(rt, ddBg.Get(), ddX, ddY, ddW, ddH, 5.f);
    ComPtr<ID2D1SolidColorBrush> ddBorder;
    rt->CreateSolidColorBrush(hoverCtrl_ == &c ? theme_.accent : theme_.border, &ddBorder);
    DrawRoundedRect(rt, ddBorder.Get(), 1.f, ddX, ddY, ddW, ddH, 5.f);

    // 选中项文字
    if (c.dropdownSelected >= 0 && c.dropdownSelected < static_cast<int>(c.dropdownItems.size())) {
        std::wstring sel = Utf8ToWide(c.dropdownItems[c.dropdownSelected]);
        DrawTextLine(rt, valueFmt_.Get(), textBrush_.Get(),
                     sel.c_str(), ddX + 10.f, mid - 7.f, ddW - 34.f);
    }

    // 下拉箭头
    const wchar_t* arrow = c.dropdownOpen ? L"\u25B4" : L"\u25BE";
    DrawTextLine(rt, labelFmt_.Get(), textSecondaryBrush_.Get(),
                 arrow, ddX + ddW - 20.f, mid - 8.f, 14.f);
}

void D2DSettingsWindow::DrawDropdownPopup(ID2D1RenderTarget* rt, const Control& c) {
    if (c.dropdownItems.empty()) return;

    float ddW = kFieldWidth, ddH = kFieldHeight;
    float ddX = static_cast<float>(c.rect.right) - ddW - kFieldRightInset;
    float ddY = static_cast<float>(c.rect.top) - scrollOffset_ + kFieldTopInset;
    float itemH = kDropdownItemHeight;
    float popupH = itemH * static_cast<float>(c.dropdownItems.size());

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float visibleTop = static_cast<float>(kTitleBarHeight + 4);
    const float visibleBottom = static_cast<float>(rc.bottom - kActionBarHeight - 4);
    float popupY = ddY + ddH + kDropdownPopupGap;
    if (popupY + popupH > visibleBottom && ddY - popupH - kDropdownPopupGap >= visibleTop) {
        popupY = ddY - popupH - kDropdownPopupGap;
    }

    ComPtr<ID2D1SolidColorBrush> popupBg, popupBorder, selectedBg;
    rt->CreateSolidColorBrush(theme_.surface, &popupBg);
    rt->CreateSolidColorBrush(theme_.border, &popupBorder);
    D2D1_COLOR_F selected = theme_.accent;
    selected.a = isDarkMode_ ? 0.28f : 0.16f;
    rt->CreateSolidColorBrush(selected, &selectedBg);

    FillRoundedRect(rt, popupBg.Get(), ddX, popupY, ddW, popupH, 5.f);
    DrawRoundedRect(rt, popupBorder.Get(), 1.f, ddX, popupY, ddW, popupH, 5.f);

    for (int i = 0; i < static_cast<int>(c.dropdownItems.size()); ++i) {
        const float itemY = popupY + itemH * static_cast<float>(i);
        if (i == c.dropdownSelected) {
            FillRoundedRect(rt, selectedBg.Get(), ddX + 2.f, itemY + 2.f,
                            ddW - 4.f, itemH - 4.f, 4.f);
        }
        std::wstring item = Utf8ToWide(c.dropdownItems[i]);
        DrawTextLine(rt, valueFmt_.Get(), textBrush_.Get(),
                     item.c_str(), ddX + 10.f, itemY + 8.f, ddW - 20.f);
    }
}

void D2DSettingsWindow::DrawButtonRow(ID2D1RenderTarget* rt, const Control& c) {
    const bool isActionButton = (c.id == "applyBtn" || c.id == "cancelBtn");
    float top = static_cast<float>(c.rect.top) - (isActionButton ? 0 : scrollOffset_);

    if (isActionButton) {
        // 底部操作按钮
        float bw = static_cast<float>(c.rect.right - c.rect.left);
        float bh = static_cast<float>(c.rect.bottom - c.rect.top);
        float bx = static_cast<float>(c.rect.left);
        float by = top;

        bool hovered = hoverCtrl_ == &c;
        ComPtr<ID2D1SolidColorBrush> btnBg, btnBorder, btnText;

        if (c.isPrimary) {
            rt->CreateSolidColorBrush(hovered ? theme_.accentHover : theme_.accent, &btnBg);
            rt->CreateSolidColorBrush(hovered ? theme_.accentHover : theme_.accent, &btnBorder);
            rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &btnText);
        } else if (c.isDanger) {
            rt->CreateSolidColorBrush(theme_.surface, &btnBg);
            rt->CreateSolidColorBrush(hovered ? HexToColorF("#ff4d4f") : theme_.border, &btnBorder);
            rt->CreateSolidColorBrush(hovered ? HexToColorF("#ff4d4f") : theme_.text, &btnText);
        } else {
            rt->CreateSolidColorBrush(hovered ? theme_.surface : theme_.bg, &btnBg);
            rt->CreateSolidColorBrush(hovered ? theme_.accent : theme_.border, &btnBorder);
            rt->CreateSolidColorBrush(hovered ? theme_.accent : theme_.text, &btnText);
        }

        FillRoundedRect(rt, btnBg.Get(), bx, by, bw, bh, 7.f);
        DrawRoundedRect(rt, btnBorder.Get(), 1.f, bx, by, bw, bh, 7.f);

        std::wstring wtxt = Utf8ToWide(c.buttonText);
        rt->DrawTextW(wtxt.c_str(), static_cast<UINT32>(wtxt.size()), btnFmt_.Get(),
                      D2D1::RectF(bx, by, bx + bw, by + bh),
                      btnText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
        // 行内按钮（如重置位置）
        float mid = top + static_cast<float>(rowHeight_) / 2.f;
        DrawRowBackground(rt, c, top);
        std::wstring wide = Utf8ToWide(c.label);
        DrawTextLine(rt, labelFmt_.Get(), textBrush_.Get(),
                     wide.c_str(), static_cast<float>(c.rect.left), mid - 8.f, 180.f);

        float bw = 64.f, bh = 26.f;
        float bx = static_cast<float>(c.rect.right) - bw - 18.f;
        float by = top + 7.f;
        bool hovered = hoverCtrl_ == &c;
        ComPtr<ID2D1SolidColorBrush> btnBg, btnBorder, btnTxt;
        rt->CreateSolidColorBrush(hovered ? theme_.surface : theme_.bg, &btnBg);
        rt->CreateSolidColorBrush(hovered ? (c.isDanger ? HexToColorF("#ff4d4f") : theme_.accent) : theme_.border, &btnBorder);
        rt->CreateSolidColorBrush(hovered ? (c.isDanger ? HexToColorF("#ff4d4f") : theme_.accent) : theme_.text, &btnTxt);
        FillRoundedRect(rt, btnBg.Get(), bx, by, bw, bh, 6.f);
        DrawRoundedRect(rt, btnBorder.Get(), 1.f, bx, by, bw, bh, 6.f);
        std::wstring wtxt = Utf8ToWide(c.buttonText);
        rt->DrawTextW(wtxt.c_str(), static_cast<UINT32>(wtxt.size()), btnFmt_.Get(),
                      D2D1::RectF(bx, by, bx + bw, by + bh),
                      btnTxt.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void D2DSettingsWindow::DrawThemePresets(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    DrawRowBackground(rt, c, top);
    float startX = static_cast<float>(c.rect.left) + kThemePresetStartOffsetX;
    float y = top + kThemePresetStartOffsetY;
    float sz = kThemePresetSize;
    float gap = kThemePresetGap;

    for (int i = 0; i < static_cast<int>(c.themePresets.size()); ++i) {
        float x = startX + i * (sz + gap);
        const auto& preset = c.themePresets[i];
        bool selected = (c.themeSelected == i);

        // 渐变背景色块
        ComPtr<ID2D1LinearGradientBrush> gradBr;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lgbp = {};
        lgbp.startPoint = D2D1::Point2F(x, y);
        lgbp.endPoint   = D2D1::Point2F(x + sz, y + sz);
        D2D1_GRADIENT_STOP stops[2] = {
            {0.f, preset.hlColor},
            {1.f, preset.nlColor},
        };
        ComPtr<ID2D1GradientStopCollection> gradStops;
        rt->CreateGradientStopCollection(stops, 2, &gradStops);
        rt->CreateLinearGradientBrush(lgbp, gradStops.Get(), &gradBr);

        // 圆形色块
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + sz/2, y + sz/2), sz/2, sz/2), gradBr.Get());

        // 选中边框
        if (selected) {
            ComPtr<ID2D1SolidColorBrush> selBr;
            rt->CreateSolidColorBrush(theme_.text, &selBr);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + sz/2, y + sz/2), sz/2 + 2.f, sz/2 + 2.f), selBr.Get(), 2.f);
        } else {
            ComPtr<ID2D1SolidColorBrush> defBr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0,0,0,0), &defBr);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + sz/2, y + sz/2), sz/2, sz/2), defBr.Get(), 2.f);
        }
    }
}

void D2DSettingsWindow::DrawHintText(ID2D1RenderTarget* rt, const Control& c) {
    float top = static_cast<float>(c.rect.top) - scrollOffset_;
    std::wstring wide = Utf8ToWide(c.label);
    DrawTextLine(rt, hintFmt_.Get(), textSecondaryBrush_.Get(),
                 wide.c_str(), static_cast<float>(c.rect.left), top, 380.f);
}

void D2DSettingsWindow::DrawTitleBar(ID2D1RenderTarget* rt) {
    RECT rc; GetClientRect(hwnd_, &rc);
    float W = static_cast<float>(rc.right);
    float H = static_cast<float>(kTitleBarHeight);

    // 标题栏背景（半透明毛玻璃效果）
    ComPtr<ID2D1SolidColorBrush> titleBg;
    D2D1_COLOR_F bgColor = theme_.bg;
    bgColor.a = isDarkMode_ ? 0.98f : 0.96f;
    rt->CreateSolidColorBrush(bgColor, &titleBg);
    rt->FillRectangle(D2D1::RectF(0, 0, W, H), titleBg.Get());

    // 底部分隔线
    ComPtr<ID2D1SolidColorBrush> lineBr;
    rt->CreateSolidColorBrush(theme_.border, &lineBr);
    rt->DrawLine(D2D1::Point2F(0, H - 0.5f), D2D1::Point2F(W, H - 0.5f), lineBr.Get());

    // 标题文字
    std::wstring wtitle = Utf8ToWide("任务栏歌词 - 设置");
    DrawTextLine(rt, titleFmt_.Get(), textBrush_.Get(),
                 wtitle.c_str(), 18.f, (H - 21.f) / 2.f, W - 110.f);

    // 关闭按钮 × （右侧）
    float btnSize = 28.f;
    float btnY = (H - btnSize) / 2.f;
    float closeX = W - btnSize - 4.f;
    closeBtnRect_ = {static_cast<int>(closeX), static_cast<int>(btnY),
                     static_cast<int>(closeX + btnSize), static_cast<int>(btnY + btnSize)};

    // 最小化按钮 — （关闭按钮左侧）
    float minX = closeX - btnSize - 2.f;
    minBtnRect_ = {static_cast<int>(minX), static_cast<int>(btnY),
                   static_cast<int>(minX + btnSize), static_cast<int>(btnY + btnSize)};

    // 绘制最小化按钮
    {
        ComPtr<ID2D1SolidColorBrush> minBg;
        D2D1_COLOR_F c = hoverMin_ ? theme_.surface : bgColor;
        c.a = hoverMin_ ? 0.8f : 0.f;
        rt->CreateSolidColorBrush(c, &minBg);
        if (hoverMin_) {
            FillRoundedRect(rt, minBg.Get(), minX, btnY, btnSize, btnSize, 4.f);
        }
        // 横线 ─
        ComPtr<ID2D1SolidColorBrush> minIcon;
        rt->CreateSolidColorBrush(hoverMin_ ? theme_.text : theme_.textSecondary, &minIcon);
        float ly = btnY + btnSize / 2.f;
        rt->DrawLine(D2D1::Point2F(minX + 7.f, ly), D2D1::Point2F(minX + btnSize - 7.f, ly),
                     minIcon.Get(), 1.5f);
    }

    // 绘制关闭按钮
    {
        ComPtr<ID2D1SolidColorBrush> closeBg;
        D2D1_COLOR_F c = hoverClose_
            ? (isDarkMode_ ? D2D1::ColorF(0.8f, 0.2f, 0.2f, 0.9f) : D2D1::ColorF(1.f, 0.3f, 0.3f, 0.9f))
            : D2D1::ColorF(0, 0, 0, 0);
        rt->CreateSolidColorBrush(c, &closeBg);
        if (hoverClose_) {
            FillRoundedRect(rt, closeBg.Get(), closeX, btnY, btnSize, btnSize, 4.f);
        }
        // × 符号
        ComPtr<ID2D1SolidColorBrush> xIcon;
        rt->CreateSolidColorBrush(
            hoverClose_ ? D2D1::ColorF(1, 1, 1, 1) : theme_.textSecondary, &xIcon);
        float cx = closeX + btnSize / 2.f;
        float cy = btnY + btnSize / 2.f;
        float d = 5.f;
        rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), xIcon.Get(), 1.6f);
        rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), xIcon.Get(), 1.6f);
    }

    // 更新标题栏区域（用于拖动判定）
    titleBarRect_ = {0, 0, rc.right, kTitleBarHeight};
}

} // namespace echo
