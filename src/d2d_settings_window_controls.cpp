// SPDX-License-Identifier: GPL-3.0
// D2D settings window control construction and layout.
#include "d2d_settings_window.h"
#include "d2d_settings_window_internal.h"
#include "color_utils.h"
#include "constants.h"

#include <algorithm>

namespace echo {

using namespace settings_window_detail;

void D2DSettingsWindow::BuildControls(const Config& cfg) {
    if (hwnd_ && colorPicker_.IsActive()) {
        colorPicker_.Deactivate(hwnd_);
    }
    controls_.clear();
    hoverCtrl_ = nullptr;
    captureCtrl_ = nullptr;
    activeColorCtrl_ = nullptr;
    const auto& a = cfg.Appearance();
    const auto& adv = cfg.Advanced();

    auto addSection = [&](const std::string& title) {
        Control c;
        c.type = CtrlType::SectionHeader;
        c.label = title;
        controls_.push_back(c);
    };

    auto addLabelRow = [&](const std::string& id, const std::string& label,
                           const std::string& value, bool readOnly = false, int maxLen = 32) {
        Control c;
        c.type = CtrlType::LabelRow;
        c.id = id; c.label = label;
        c.textValue = value; c.readOnly = readOnly; c.textMaxLen = maxLen;
        controls_.push_back(c);
    };

    auto addToggle = [&](const std::string& id, const std::string& label, bool value) {
        Control c;
        c.type = CtrlType::ToggleRow;
        c.id = id; c.label = label; c.toggleValue = value;
        controls_.push_back(c);
    };

    auto addSlider = [&](const std::string& id, const std::string& label,
                         float minV, float maxV, float val, const std::string& suffix) {
        Control c;
        c.type = CtrlType::SliderRow;
        c.id = id; c.label = label;
        c.sliderMin = minV; c.sliderMax = maxV; c.sliderValue = val;
        c.sliderSuffix = suffix;
        controls_.push_back(c);
    };

    auto addColor = [&](const std::string& id, const std::string& label, const std::string& hex) {
        Control c;
        c.type = CtrlType::ColorRow;
        c.id = id; c.label = label;
        c.colorValue = HexToColorF(hex);
        c.textValue = hex;
        controls_.push_back(c);
    };

    auto addDropdown = [&](const std::string& id, const std::string& label,
                           const std::vector<std::string>& items, int selected) {
        Control c;
        c.type = CtrlType::DropdownRow;
        c.id = id; c.label = label;
        c.dropdownItems = items; c.dropdownSelected = selected;
        controls_.push_back(c);
    };

    auto addButton = [&](const std::string& id, const std::string& label,
                         const std::string& text, bool primary = false, bool danger = false) {
        Control c;
        c.type = CtrlType::ButtonRow;
        c.id = id; c.label = label;
        c.buttonText = text; c.isPrimary = primary; c.isDanger = danger;
        controls_.push_back(c);
    };

    auto addHint = [&](const std::string& text) {
        Control c;
        c.type = CtrlType::HintText;
        c.label = text;
        controls_.push_back(c);
    };

    auto addSpacer = [&]() {
        Control c;
        c.type = CtrlType::Spacer;
        controls_.push_back(c);
    };

    // ===== 外观 =====
    addSection("外观");

    // 显示模式
    addDropdown("displayMode", "显示模式",
                {"卡拉OK（默认）", "卡片样式"},
                a.displayMode == "card" ? 1 : 0);
    addSlider("lyricWindowWidth", "窗口宽度",
              static_cast<float>(constants::MIN_CONFIGURABLE_LYRIC_WINDOW_WIDTH_BASE_DP),
              static_cast<float>(constants::MAX_CONFIGURABLE_LYRIC_WINDOW_WIDTH_BASE_DP),
              static_cast<float>(a.lyricWindowWidth), " px");

    // 卡拉OK 区域的控件（始终构建，通过 visible 控制）
    addSlider("fontSize", "字号", 10.f, 28.f, static_cast<float>(a.fontSize), "");
    addLabelRow("fontFamily", "字体", a.fontFamily, /*readOnly*/true);
    addColor("normalColor", "普通歌词颜色", a.normalColor);
    addColor("highlightColor", "高亮歌词颜色", a.highlightColor);
    addSlider("opacity", "不透明度", 20.f, 100.f, a.normalOpacity * 100.f, "%");
    addToggle("karaoke", "卡拉OK 效果", a.enableKaraoke);
    addToggle("translation", "显示翻译", a.enableTranslation);

    // 跑马灯
    addSection("长歌词滚动（跑马灯）");
    addToggle("marquee", "启用跑马灯", a.enableMarquee);
    addDropdown("marqueeMode", "滚动模式",
                {"往返滚动（推荐）", "循环跑马灯", "关闭（截断显示）"},
                a.marqueeMode == "loop" ? 1 : (a.marqueeMode == "off" ? 2 : 0));
    addSlider("marqueeDelay", "开始延迟", 0.f, 5000.f, static_cast<float>(a.marqueeDelayMs), " ms");
    addSlider("marqueePause", "端点暂停", 0.f, 3000.f, static_cast<float>(a.marqueePauseMs), " ms");
    addSlider("marqueeSpeed", "滚动速度", 10.f, 200.f, a.marqueeSpeedPxPerSec, " px/s");

    // 预设主题
    {
        Control c;
        c.type = CtrlType::ThemePresets;
        c.id = "themePresets";
        c.themePresets = {
            {HexToColorF("#4CC2FF"), HexToColorF("#FFFFFF"), "默认"},
            {HexToColorF("#EC4141"), HexToColorF("#FFFFFF"), "网易云红"},
            {HexToColorF("#31C27C"), HexToColorF("#FFFFFF"), "QQ音乐绿"},
            {HexToColorF("#FF9800"), HexToColorF("#FFFFFF"), "暖橙"},
            {HexToColorF("#E040FB"), HexToColorF("#E0E0E0"), "紫罗兰"},
            {HexToColorF("#00E676"), HexToColorF("#B0BEC5"), "薄荷"},
        };
        // 检查当前是否匹配某个预设
        for (int i = 0; i < static_cast<int>(c.themePresets.size()); ++i) {
            if (ColorFToHex(c.themePresets[i].hlColor) == a.highlightColor &&
                ColorFToHex(c.themePresets[i].nlColor) == a.normalColor) {
                c.themeSelected = i;
                break;
            }
        }
        controls_.push_back(c);
    }

    // 卡片模式
    addSection("卡片样式设置");
    addSlider("cardFontSizeCurrent", "当前行字号", 10.f, 20.f,
              static_cast<float>(a.cardFontSizeCurrent), "");
    addSlider("cardFontSizeNext", "下一行字号", 8.f, 18.f,
              static_cast<float>(a.cardFontSizeNext), "");
    addLabelRow("cardFontFamily", "字体", a.cardFontFamily.empty() ? "(与主模式相同)" : a.cardFontFamily, /*readOnly*/true);
    addColor("cardCurrentColor", "当前行颜色", a.cardCurrentColor);
    addColor("cardNextColor", "第二行颜色", a.cardNextColor);
    addToggle("cardTranslation", "显示翻译", a.cardShowTranslation);
    addDropdown("cardCoverPosition", "封面位置",
                {"左侧", "右侧"},
                a.cardCoverPosition == "right" ? 1 : 0);

    // ===== 位置 =====
    addSection("位置");
    addHint("拖动歌词窗口可直接调整位置");
    addButton("resetPos", "重置位置", "重置", false, /*danger*/true);

    // ===== 高级 =====
    addSection("高级");
    addSlider("refreshRate", "刷新率", 15.f, 120.f, static_cast<float>(adv.refreshRateHz), " FPS");
    addToggle("debugLog", "调试日志", adv.debugLog);

    // ===== 通用 =====
    addSection("通用");
    addToggle("autoStart", "开机自动启动", cfg.IsAutoStart());

    // ===== 操作按钮 =====
    addSpacer();
    {
        Control c;
        c.type = CtrlType::ButtonRow;
        c.id = "applyBtn";
        c.label = ""; // 右对齐按钮不需要标签
        c.buttonText = "应用并保存";
        c.isPrimary = true;
        controls_.push_back(c);
    }
    {
        Control c;
        c.type = CtrlType::ButtonRow;
        c.id = "cancelBtn";
        c.label = "";
        c.buttonText = "取消";
        controls_.push_back(c);
    }

    // 初始可见性：根据当前 displayMode 过滤不相关控件
    UpdateControlVisibility();
}

void D2DSettingsWindow::UpdateControlVisibility() {
    for (auto& c : controls_) {
        c.visible = true;
        c.dropdownOpen = false;
    }

    // 根据当前 displayMode 动态显示/隐藏对应模式的专属控件组。
    // 控件分三类：
    //   1) ID 列表匹配（karaokeOnly / cardOnly）：遍历控件按 ID 精确匹配
    //   2) SectionHeader 匹配（"长歌词滚动" / "卡片样式设置"）：通过标签关键词检测
    //   3) 其余控件（位置/高级/通用/按钮）：保持默认 visible=true
    //
    // 调用时机：BuildControls 末尾（初始化）、displayMode dropdown 切换时（OnMouseDown）。
    bool isCard = false;
    for (const auto& c : controls_) {
        if (c.id == "displayMode") {
            isCard = (c.dropdownSelected == 1);
            break;
        }
    }

    // ID 白名单：不在列表中的控件不受影响（保持默认 visible=true）
    static const std::vector<std::string> karaokeOnly = {
        "fontSize", "fontFamily", "normalColor", "highlightColor",
        "opacity", "karaoke", "translation", "themePresets"
    };
    static const std::vector<std::string> cardOnly = {
        "cardFontSizeCurrent", "cardFontSizeNext", "cardFontFamily",
        "cardCurrentColor", "cardNextColor", "cardTranslation", "cardCoverPosition"
    };

    // 跟踪「长歌词滚动（跑马灯）」和「卡片样式设置」两个 section 的范围。
    // 这两个 SectionHeader 本身也需要按模式显示/隐藏，但其 ID 为空，无法用 ID 列表匹配，
    // 故通过标签关键词 + 状态机方式处理：每次遇到 SectionHeader 时更新标志。
    bool inKaraokeSection = false;
    bool inCardSection = false;

    for (auto& c : controls_) {
        // 更新 section 状态标志（每个 SectionHeader 都会重置标志为精确值）
        if (c.type == CtrlType::SectionHeader) {
            inKaraokeSection = (c.label.find("跑马灯") != std::string::npos);
            inCardSection = (c.label.find("卡片样式") != std::string::npos);
        }

        if (c.type == CtrlType::SectionHeader) {
            if (inKaraokeSection) {
                c.visible = true;          // 跑马灯配置在卡拉OK/卡片模式共用
                continue;
            }
            if (inCardSection) {
                c.visible = isCard;        // 卡片模式可见，卡拉OK模式隐藏
                continue;
            }
            // 外观 / 位置 / 高级 / 通用 — 双模式均可见
            continue;
        }

        // ID 匹配：仅修改命中控件的 visible，其余控件原样保留
        for (const auto& id : karaokeOnly) {
            if (c.id == id) { c.visible = !isCard; break; }
        }
        for (const auto& id : cardOnly) {
            if (c.id == id) { c.visible = isCard; break; }
        }
    }
}

void D2DSettingsWindow::LayoutControls(int contentWidth) {
    const int leftPad = kContentPadX;
    const int rightPad = kContentPadX;
    const int controlRight = contentWidth - rightPad;

    RECT clientRc{0, 0, kWinWidth, kWinHeight};
    if (hwnd_) GetClientRect(hwnd_, &clientRc);
    const int actionY = clientRc.bottom - kActionBarHeight + 14;

    int y = kTitleBarHeight + 14; // 标题栏下方开始

    for (auto& c : controls_) {
        if (!c.visible) continue;  // 不可见控件不占布局空间，避免页面出现空白间隙

        switch (c.type) {
        case CtrlType::SectionHeader:
            c.rect = {leftPad, y, controlRight, y + 26};
            y += 26 + sectionPadding_;
            break;

        case CtrlType::LabelRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::ToggleRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::SliderRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::ColorRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::DropdownRow:
            c.rect = {leftPad, y, controlRight, y + rowHeight_};
            y += rowHeight_;
            break;

        case CtrlType::ButtonRow:
            if (c.id == "applyBtn" || c.id == "cancelBtn") {
                if (c.id == "applyBtn") {
                    c.rect = {controlRight - kActionPrimaryWidth, actionY,
                              controlRight, actionY + kActionButtonHeight};
                } else {
                    c.rect = {controlRight - kActionPrimaryWidth - kActionButtonGap - kActionSecondaryWidth, actionY,
                              controlRight - kActionPrimaryWidth - kActionButtonGap, actionY + kActionButtonHeight};
                }
            } else {
                c.rect = {leftPad, y, controlRight, y + rowHeight_};
                y += rowHeight_;
            }
            break;

        case CtrlType::ThemePresets:
            c.rect = {leftPad, y, controlRight, y + 46};
            y += 46;
            break;

        case CtrlType::HintText:
            c.rect = {leftPad, y, controlRight, y + 22};
            y += 24;
            break;

        case CtrlType::Spacer:
            y += 10;
            c.rect = {0, y, 0, y};
            break;
        }
    }

    totalContentHeight_ = y + 18;
    if (hwnd_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int viewportH = std::max(1, static_cast<int>(rc.bottom) - kActionBarHeight);
        const int maxScroll = std::max(0, totalContentHeight_ - viewportH);
        scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
    }
}

// ═══════════════════════════════
// 命中测试
// ═══════════════════════════════

D2DSettingsWindow::Control* D2DSettingsWindow::HitTest(int x, int y) {
    for (auto& c : controls_) {
        if (!c.visible) continue;
        if ((c.id == "applyBtn" || c.id == "cancelBtn") &&
            y >= c.rect.top && y < c.rect.bottom &&
            x >= c.rect.left && x < c.rect.right) {
            return &c;
        }
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (y >= rc.bottom - kActionBarHeight) return nullptr;

    int testY = y + scrollOffset_; // 转换为内容坐标
    for (auto& c : controls_) {
        if (!c.visible) continue;
        if (c.id == "applyBtn" || c.id == "cancelBtn") continue;
        if (testY >= c.rect.top && testY < c.rect.bottom &&
            x >= c.rect.left && x < c.rect.right) {
            return &c;
        }
    }
    return nullptr;
}

} // namespace echo
