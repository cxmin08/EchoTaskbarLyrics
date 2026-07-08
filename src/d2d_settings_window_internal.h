// SPDX-License-Identifier: GPL-3.0
// Internal helpers shared by D2D settings window implementation files.
#pragma once

#include <string>
#include <windows.h>

namespace echo::settings_window_detail {

inline constexpr int kContentPadX = 24;
inline constexpr int kActionButtonGap = 10;
inline constexpr int kActionPrimaryWidth = 112;
inline constexpr int kActionSecondaryWidth = 76;
inline constexpr int kActionButtonHeight = 34;

inline constexpr float kRowBleedX = 8.f;
inline constexpr float kRowTopInset = 3.f;
inline constexpr float kRowRadius = 7.f;
inline constexpr float kFieldWidth = 206.f;
inline constexpr float kFieldHeight = 26.f;
inline constexpr float kFieldTopInset = 7.f;
inline constexpr float kFieldRightInset = 16.f;
inline constexpr float kSliderLeftOffset = 266.f;
inline constexpr float kSliderValueWidth = 58.f;
inline constexpr float kColorHexWidth = 88.f;
inline constexpr float kDropdownItemHeight = 28.f;
inline constexpr float kDropdownPopupGap = 4.f;
inline constexpr float kThemePresetStartOffsetX = 10.f;
inline constexpr float kThemePresetStartOffsetY = 10.f;
inline constexpr float kThemePresetSize = 24.f;
inline constexpr float kThemePresetGap = 8.f;

inline std::string WideToLocalUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

} // namespace echo::settings_window_detail
