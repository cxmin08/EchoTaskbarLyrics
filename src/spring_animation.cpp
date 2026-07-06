// SPDX-License-Identifier: GPL-3.0
// spring_animation.cpp - 弹簧物理动画（缓动函数、弹簧步进、行切换淡入淡出）
#include "renderer.h"
#include "constants.h"

#include <algorithm>
#include <cmath>

namespace echo {

float TaskbarRenderer::EaseOutCubic(float t) {
    // ease-out cubic: f(t) = 1 - (1-t)^3
    float c = std::clamp(t, 0.0f, 1.0f);
    float inv = 1.0f - c;
    return 1.0f - inv * inv * inv;
}

float TaskbarRenderer::EaseInOutQuad(float t) {
    // ease-in-out quad: t<0.5 → 2*t^2, 否则 1-(2-2t)^2/2
    float c = std::clamp(t, 0.0f, 1.0f);
    if (c < 0.5f) {
        return 2.0f * c * c;
    }
    float inv = 1.0f - c;
    return 1.0f - 2.0f * inv * inv;
}

float TaskbarRenderer::EaseOutBack(float t) {
    // ease-out back：末端轻微"冲出"然后回弹
    // f(t) = 1 + c3*(t-1)^3 + c2*(t-1)^2, c1=1.70158, c2=c1+1, c3=c1+1
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float c = std::clamp(t, 0.0f, 1.0f);
    float v = c - 1.0f;
    return 1.0f + c3 * v * v * v + c1 * v * v;
}

bool TaskbarRenderer::UpdateLyricFade(const std::wstring& newText)
{
    // 首帧：新行文本入库，不做 fade
    static std::wstring lastText;
    if (lastText.empty()) {
        lastText = newText;
        return false;
    }

    // 歌词行发生变化且新旧均非空 → 启动 fade
    const bool lineChanged = (newText != lastText);
    if (lineChanged && !newText.empty() && !lastText.empty()) {
        lyricFadeOldText_ = lastText;
        springProgress_ = 0.0;
        springVelocity_ = 0.0;
        springLastTime_ = 0.0;

        LARGE_INTEGER li, freq;
        ::QueryPerformanceCounter(&li);
        ::QueryPerformanceFrequency(&freq);
        lyricFadeStartTime_ = static_cast<double>(li.QuadPart) / static_cast<double>(freq.QuadPart);
        lyricFadeActive_ = true;
    }
    lastText = newText;

    if (!lyricFadeActive_) {
        return false;
    }

    // 检查动画是否到期
    LARGE_INTEGER li2, freq2;
    ::QueryPerformanceCounter(&li2);
    ::QueryPerformanceFrequency(&freq2);
    const double now = static_cast<double>(li2.QuadPart) / static_cast<double>(freq2.QuadPart);
    const double elapsed = now - lyricFadeStartTime_;
    const double dur = static_cast<double>(constants::LYRIC_FADE_DURATION_MS) / 1000.0;

    if (elapsed >= dur) {
        lyricFadeActive_ = false;
        lyricFadeOldText_.clear();
        lyricFadeOldTrans_.clear();
        return false;
    }

    return true;
}

// ═════ P3-②: 卡拉OK进度缓存 ═════
// 解析器已经使用本地高精度时钟逐帧插值 progress。渲染层只保持“同一句不后退”，
// 避免额外弹簧造成高亮慢半拍或看起来卡顿。
bool TaskbarRenderer::UpdateProgressSpring(double target, double now)
{
    target = std::clamp(target, 0.0, 1.0);

    // 首次调用初始化
    if (springLastTime_ == 0.0) {
        springProgress_ = target;
        springVelocity_ = 0.0;
        springLastTime_ = now;
        return false;
    }

    springLastTime_ = now;

    if (target < springProgress_) {
        springVelocity_ = 0.0;
        return false;
    }

    const bool changed = std::abs(springProgress_ - target) >= 0.0005;
    springProgress_ = target;
    springVelocity_ = 0.0;
    if (!changed) {
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════
// 频谱渲染实现
// ═══════════════════════════════════════════


} // namespace echo
