// SPDX-License-Identifier: GPL-3.0
// D2D settings window input handling and config actions.
#include "d2d_settings_window.h"
#include "d2d_settings_window_internal.h"
#include "color_utils.h"
#include "logger.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <windowsx.h>

namespace echo {

using namespace settings_window_detail;

void D2DSettingsWindow::OnMouseDown(int x, int y) {
    // 颜色选择器弹窗优先处理
    if (colorPicker_.IsActive()) {
        D2D1_COLOR_F newColor;
        std::string newHex;
        auto result = colorPicker_.HandleMouseDown(x, y, &newColor, &newHex);

        if (result == ColorPickerPopup::ActionResult::Confirmed) {
            if (activeColorCtrl_) {
                activeColorCtrl_->colorValue = newColor;
                activeColorCtrl_->textValue = newHex;
            }
            colorPicker_.Deactivate(hwnd_);
            activeColorCtrl_ = nullptr;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (result == ColorPickerPopup::ActionResult::Cancelled) {
            colorPicker_.Deactivate(hwnd_);
            activeColorCtrl_ = nullptr;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        // Handled: 色板/亮度条点击 → 重绘
        if (result != ColorPickerPopup::ActionResult::None) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    for (auto& c : controls_) {
        if (!c.visible || c.type != CtrlType::DropdownRow || !c.dropdownOpen) continue;

        const float ddW = kFieldWidth;
        const float ddH = kFieldHeight;
        const float itemH = kDropdownItemHeight;
        const float ddX = static_cast<float>(c.rect.right) - ddW - kFieldRightInset;
        const float ddY = static_cast<float>(c.rect.top) - scrollOffset_ + kFieldTopInset;
        const float popupH = itemH * static_cast<float>(c.dropdownItems.size());
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const float visibleTop = static_cast<float>(kTitleBarHeight + 4);
        const float visibleBottom = static_cast<float>(rc.bottom - kActionBarHeight - 4);
        float popupY = ddY + ddH + kDropdownPopupGap;
        if (popupY + popupH > visibleBottom && ddY - popupH - kDropdownPopupGap >= visibleTop) {
            popupY = ddY - popupH - kDropdownPopupGap;
        }

        const bool inButton =
            x >= ddX && x < ddX + ddW && y >= ddY && y < ddY + ddH;
        const bool inPopup =
            x >= ddX && x < ddX + ddW && y >= popupY && y < popupY + popupH;

        if (inPopup) {
            const int idx = static_cast<int>((static_cast<float>(y) - popupY) / itemH);
            if (idx >= 0 && idx < static_cast<int>(c.dropdownItems.size())) {
                c.dropdownSelected = idx;
            }
            c.dropdownOpen = false;
            if (c.id == "displayMode") {
                UpdateControlVisibility();
                LayoutControls(contentWidth_);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        c.dropdownOpen = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (inButton) return;
    }

    Control* hit = HitTest(x, y);
    if (!hit) return;

    captureCtrl_ = hit;
    hit->pressed = true;

    switch (hit->type) {
    case CtrlType::ToggleRow:
        hit->toggleValue = !hit->toggleValue;
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;

    case CtrlType::LabelRow:
        if (hit->id == "fontFamily" || hit->id == "cardFontFamily") {
            // 点击了"选择"按钮区域
            LOGFONT lf{};
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfHeight = -12;
            wcsncpy_s(lf.lfFaceName, LF_FACESIZE,
                      Utf8ToWide(hit->textValue).c_str(), _TRUNCATE);
            CHOOSEFONTW cf{};
            cf.lStructSize = sizeof(cf);
            cf.hwndOwner = hwnd_;
            cf.lpLogFont = &lf;
            cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
            if (::ChooseFontW(&cf)) {
                hit->textValue = WideToLocalUtf8(std::wstring(lf.lfFaceName));
                if (hit->id == "fontFamily")
                    editedConfig_.MutableAppearance().fontFamily = hit->textValue;
                else
                    editedConfig_.MutableAppearance().cardFontFamily = hit->textValue;
            }
        } else if (!hit->readOnly) {
            // 开始编辑文本
            for (auto& c : controls_) c.editing = false;
            hit->editing = true;
            hit->caretPos = static_cast<int>(hit->textValue.size());
            hit->showCaret = true;
            hit->caretBlinkTime = 0;
            SetCapture(hwnd_);
        }
        break;

    case CtrlType::ColorRow: {
        // 激活 D2D 自定义颜色选择器弹窗
        activeColorCtrl_ = hit;
        colorPicker_.Activate(hwnd_, hit->colorValue, kTitleBarHeight);
        break;
    }

    case CtrlType::DropdownRow:
        for (auto& c : controls_) {
            if (&c != hit && c.type == CtrlType::DropdownRow) {
                c.dropdownOpen = false;
            }
        }
        hit->dropdownOpen = !hit->dropdownOpen;
        break;

    case CtrlType::ButtonRow:
        if (hit->id == "applyBtn") ::PostMessageW(hwnd_, kMsgApplySave, 0, 0);
        else if (hit->id == "cancelBtn") ::PostMessageW(hwnd_, kMsgCancel, 0, 0);
        else if (hit->id == "resetPos") {
            editedConfig_.MutablePosition().offsetX = 0;
            editedConfig_.MutablePosition().offsetY = 0;
        }
        break;

    case CtrlType::ThemePresets: {
        // 计算点击了哪个预设
        float relX = static_cast<float>(x) - (hit->rect.left + kThemePresetStartOffsetX);
        float relY = static_cast<float>(y) + scrollOffset_ - (hit->rect.top + kThemePresetStartOffsetY);
        float sz = kThemePresetSize, gap = kThemePresetGap;
        int idx = static_cast<int>((relX) / (sz + gap));
        const float itemLeft = idx * (sz + gap);
        const bool inSwatch = idx >= 0 &&
                              relX >= itemLeft && relX <= itemLeft + sz &&
                              relY >= 0.f && relY <= sz;
        if (inSwatch && idx < static_cast<int>(hit->themePresets.size())) {
            hit->themeSelected = idx;
            // 应用预设到对应的颜色控件
            const auto& preset = hit->themePresets[idx];
            for (auto& c : controls_) {
                if (c.id == "highlightColor") {
                    c.colorValue = preset.hlColor;
                    c.textValue = ColorFToHex(preset.hlColor);
                }
                if (c.id == "normalColor") {
                    c.colorValue = preset.nlColor;
                    c.textValue = ColorFToHex(preset.nlColor);
                }
            }
        }
        break;
    }

    default:
        break;
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DSettingsWindow::OnMouseUp(int x, int y) {
    if (captureCtrl_) {
        captureCtrl_->pressed = false;
        captureCtrl_ = nullptr;
    }
    ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DSettingsWindow::OnMouseMove(int x, int y) {
    if (!trackingMouse_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        trackingMouse_ = ::TrackMouseEvent(&tme) != FALSE;
    }

    // 颜色选择器弹窗内拖动
    if (colorPicker_.IsActive() && (GetKeyState(VK_LBUTTON) & 0x8000)) {
        colorPicker_.HandleMouseMove(x, y, true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 标题栏按钮悬停检测
    bool newHoverClose = PtInRect(&closeBtnRect_, {x, y});
    bool newHoverMin   = PtInRect(&minBtnRect_, {x, y});
    if (newHoverClose != hoverClose_ || newHoverMin != hoverMin_) {
        hoverClose_ = newHoverClose;
        hoverMin_ = newHoverMin;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return; // 标题栏按钮悬停变化时优先处理
    }

    Control* hit = HitTest(x, y);

    // 滑块拖动
    if (captureCtrl_ && captureCtrl_->type == CtrlType::SliderRow) {
        float relX = static_cast<float>(x) -
                     (static_cast<float>(captureCtrl_->rect.right) - kSliderLeftOffset);
        float ratio = relX / sliderWidth_;
        ratio = std::clamp(ratio, 0.f, 1.f);
        captureCtrl_->sliderValue = captureCtrl_->sliderMin +
                                   ratio * (captureCtrl_->sliderMax - captureCtrl_->sliderMin);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (hit != hoverCtrl_) {
        hoverCtrl_ = hit;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void D2DSettingsWindow::OnMouseWheel(int delta) {
    for (auto& c : controls_) {
        if (c.dropdownOpen) c.dropdownOpen = false;
    }
    const int clientH = [this]() {
        RECT rc; GetClientRect(hwnd_, &rc); return rc.bottom; }();
    const int viewportH = std::max(1, clientH - kActionBarHeight);
    int maxScroll = std::max(0, totalContentHeight_ - viewportH);
    scrollOffset_ -= delta / WHEEL_DELTA * 48;
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void D2DSettingsWindow::OnChar(wchar_t ch) {
    // 找到正在编辑的控件
    for (auto& c : controls_) {
        if (c.editing) {
            if (ch == VK_BACK) {
                if (c.caretPos > 0) {
                    c.textValue.erase(c.caretPos - 1, 1);
                    --c.caretPos;
                }
            } else if (ch >= 32 && static_cast<int>(c.textValue.size()) < c.textMaxLen) {
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) {
                    c.textValue.insert(c.caretPos, mb, n);
                    ++c.caretPos;
                }
            }
            c.showCaret = true;
            c.caretBlinkTime = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
    }
}

void D2DSettingsWindow::OnKeyDown(UINT key) {
    if (key == VK_ESCAPE && colorPicker_.IsActive()) {
        colorPicker_.Deactivate(hwnd_);
        activeColorCtrl_ = nullptr;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (key == VK_ESCAPE) {
        for (auto& c : controls_) {
            if (c.dropdownOpen) {
                c.dropdownOpen = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
    }

    bool handledEditing = false;
    for (auto& c : controls_) {
        if (c.editing) {
            switch (key) {
            case VK_LEFT:
                if (c.caretPos > 0) --c.caretPos;
                break;
            case VK_RIGHT:
                if (c.caretPos < static_cast<int>(c.textValue.size())) ++c.caretPos;
                break;
            case VK_HOME: c.caretPos = 0; break;
            case VK_END:  c.caretPos = static_cast<int>(c.textValue.size()); break;
            case VK_RETURN:
                c.editing = false;
                ReleaseCapture();
                break;
            case VK_ESCAPE:
                c.editing = false;
                ReleaseCapture();
                break;
            default:
                return;
            }
            c.showCaret = true;
            c.caretBlinkTime = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            handledEditing = true;
            break;
        }
    }

    if (!handledEditing && key == VK_ESCAPE) {
        ::PostMessageW(hwnd_, kMsgCancel, 0, 0);
    }
}

void D2DSettingsWindow::OnLoseFocus() {
    if (colorPicker_.IsActive()) {
        colorPicker_.Deactivate(hwnd_);
        activeColorCtrl_ = nullptr;
    }

    for (auto& c : controls_) {
        c.editing = false;
        c.dropdownOpen = false;
    }
    hoverCtrl_ = nullptr;
    hoverClose_ = false;
    hoverMin_ = false;
    ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ═══════════════════════════════
// 应用配置 / 取消
// ═══════════════════════════════

void D2DSettingsWindow::ApplyAndSave() {
    // 从控件收集值到 editedConfig_
    for (const auto& c : controls_) {
        auto& ap = editedConfig_.MutableAppearance();
        auto& adv = editedConfig_.MutableAdvanced();

        if (c.id == "displayMode") {
            ap.displayMode = (c.dropdownSelected == 1) ? "card" : "karaoke";
        } else if (c.id == "lyricWindowWidth") {
            ap.lyricWindowWidth = static_cast<int>(c.sliderValue);
        } else if (c.id == "fontSize") {
            ap.fontSize = static_cast<int>(c.sliderValue);
        } else if (c.id == "fontFamily") {
            ap.fontFamily = c.textValue;
        } else if (c.id == "normalColor") {
            ap.normalColor = ColorFToHex(c.colorValue);
        } else if (c.id == "highlightColor") {
            ap.highlightColor = ColorFToHex(c.colorValue);
        } else if (c.id == "opacity") {
            ap.normalOpacity = c.sliderValue / 100.f;
        } else if (c.id == "karaoke") {
            ap.enableKaraoke = c.toggleValue;
        } else if (c.id == "translation") {
            ap.enableTranslation = c.toggleValue;
        } else if (c.id == "marquee") {
            ap.enableMarquee = c.toggleValue;
        } else if (c.id == "marqueeMode") {
            ap.marqueeMode = (c.dropdownSelected == 1) ? "loop" :
                             (c.dropdownSelected == 2) ? "off" : "bounce";
        } else if (c.id == "marqueeDelay") {
            ap.marqueeDelayMs = static_cast<int>(c.sliderValue);
        } else if (c.id == "marqueePause") {
            ap.marqueePauseMs = static_cast<int>(c.sliderValue);
        } else if (c.id == "marqueeSpeed") {
            ap.marqueeSpeedPxPerSec = c.sliderValue;
        } else if (c.id == "cardFontSizeCurrent") {
            ap.cardFontSizeCurrent = static_cast<int>(c.sliderValue);
        } else if (c.id == "cardFontSizeNext") {
            ap.cardFontSizeNext = static_cast<int>(c.sliderValue);
        } else if (c.id == "cardFontFamily") {
            ap.cardFontFamily = (c.textValue == "(与主模式相同)") ? "" : c.textValue;
        } else if (c.id == "cardCurrentColor") {
            ap.cardCurrentColor = ColorFToHex(c.colorValue);
        } else if (c.id == "cardNextColor") {
            ap.cardNextColor = ColorFToHex(c.colorValue);
        } else if (c.id == "cardTranslation") {
            ap.cardShowTranslation = c.toggleValue;
        } else if (c.id == "cardCoverPosition") {
            ap.cardCoverPosition = (c.dropdownSelected == 1) ? "right" : "left";
        } else if (c.id == "refreshRate") {
            adv.refreshRateHz = static_cast<int>(c.sliderValue);
        } else if (c.id == "debugLog") {
            adv.debugLog = c.toggleValue;
        } else if (c.id == "autoStart") {
            editedConfig_.SetAutoStart(c.toggleValue);
        }
    }

    const bool saved = editedConfig_.Save();

    // 回调通知主程序
    if (onConfigChanged_) onConfigChanged_(editedConfig_);

    Log("[D2D-SETTINGS] Config applied and saved=%d\n", saved ? 1 : 0);
    Close();
}

void D2DSettingsWindow::Cancel() {
    Log("[D2D-SETTINGS] Settings cancelled\n");
    Close();
}

} // namespace echo
