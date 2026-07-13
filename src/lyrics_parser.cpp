// SPDX-License-Identifier: GPL-3.0
// lyrics_parser.cpp - 歌词解析与同步实现
#include "lyrics_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <regex>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace echo {

namespace {

/// 高精度本地时钟（秒），基于 QueryPerformanceCounter
double GetWallTimeSeconds() {
    static const LARGE_INTEGER freq = []{
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter;
    ::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
}

std::string TrimAscii(std::string s) {
    auto isSpace = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

bool ContainsAny(const std::string& text, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (text.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool IsDecorativePlaceholder(std::string text) {
    text = TrimAscii(text);
    if (text.empty()) return true;
    for (const char* token : {"[", "]", "(", ")", "{", "}", "<", ">",
                              " ", "\t", ".", ",", "-", "_", "~",
                              "*", "·", "•", "。", "，", "、", "！",
                              "!", "?", "？", "…", "♪", "♫", "♬", "♩"}) {
        size_t pos = 0;
        const size_t len = std::char_traits<char>::length(token);
        while (len > 0 && (pos = text.find(token, pos)) != std::string::npos) {
            text.erase(pos, len);
        }
    }
    return text.empty();
}

bool IsPureMusicPlaceholder(const std::string& text) {
    const std::string trimmed = TrimAscii(text);
    if (ContainsAny(trimmed, {
            "纯音乐", "純音樂", "此歌曲为没有填词的纯音乐", "该歌曲为没有填词的纯音乐",
            "此歌曲为纯音乐", "该歌曲为纯音乐"})) {
        return true;
    }

    const std::string lower = ToLowerAscii(trimmed);
    return ContainsAny(lower, {"instrumental", "music only"});
}

bool IsNonLyricPlaceholder(const std::string& text) {
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty() || IsDecorativePlaceholder(trimmed) ||
        IsPureMusicPlaceholder(trimmed)) {
        return true;
    }

    if (ContainsAny(trimmed, {
            "没有填词", "暂无歌词", "暂时没有歌词", "没有歌词",
            "无歌词", "未找到歌词", "歌词加载中"})) {
        return true;
    }

    const std::string lower = ToLowerAscii(trimmed);
    return ContainsAny(lower, {
        "no lyrics", "no lyric", "lyrics unavailable", "lyric unavailable",
        "lyrics not available", "lyric not available"});
}

std::string SongIdentity(const PlayerState& state) {
    return state.songName.empty() ? state.songTitle : state.songName;
}

} // namespace

void LyricsParser::UpdateLyrics(const LyricsData& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    lyrics_ = data;
}

void LyricsParser::UpdatePlayerState(const PlayerState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string oldSong = SongIdentity(state_);
    const std::string newSong = SongIdentity(state);
    if (!oldSong.empty() && !newSong.empty() && oldSong != newSong) {
        lyrics_ = LyricsData{};
    }
    state_ = state;
    // 记录本地高精度时钟，用于 GetCurrentRenderState() 中推算时间
    lastUpdateWallTime_ = GetWallTimeSeconds();
}

bool LyricsParser::HasLyrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lyrics_.valid && !lyrics_.lines.empty();
}

bool LyricsParser::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.isPlaying;
}

int LyricsParser::FindLineIndex(double currentTimeSec) const {
    // 优先使用 characters 数组的 startTime（字符级精确同步）
    // 若 characters 为空则回退到 line 级别的 startTime（来自 LRC 行标签）
    const int64_t tMs = static_cast<int64_t>(currentTimeSec * 1000.0);
    const int n = static_cast<int>(lyrics_.lines.size());
    int lo = 0, hi = n - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const auto& line = lyrics_.lines[mid];

        // 获取该行的起始时间：优先 characters，回退到 line.startTime
        int64_t startMs = 0;
        if (!line.characters.empty()) {
            startMs = line.characters.front().startTime;
        } else {
            startMs = line.startTime;  // LRC 行级时间戳
        }

        if (startMs <= tMs) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

RenderState LyricsParser::GetCurrentRenderState() const {
    RenderState out;
    std::lock_guard<std::mutex> lock(mutex_);

    out.isPlaying    = state_.isPlaying;
    out.isPersonalFM = state_.isPersonalFM;
    out.coverArtUrl  = state_.coverArtUrl;
    out.songName     = state_.songName;

    // ── 本地时钟推算：仅在播放状态变化、切歌或跳转时需要上游重新同步 ──
    // EchoMusic 官方插件文档建议根据快照时间与倍速在本地推算，避免持续传输歌词。
    double effectiveTime = state_.currentTime;
    if (state_.isPlaying && lastUpdateWallTime_ > 0.0) {
        const double elapsed = GetWallTimeSeconds() - lastUpdateWallTime_;
        if (elapsed > 0.0) {
            effectiveTime = state_.currentTime + elapsed * std::max(0.1, state_.playbackRate);
        }
    }
    if (state_.duration > 0.0) {
        effectiveTime = std::clamp(effectiveTime, 0.0, state_.duration);
    } else {
        effectiveTime = std::max(0.0, effectiveTime);
    }
    out.currentTime = effectiveTime;

    if (!lyrics_.valid || lyrics_.lines.empty()) {
        out.hasLyrics = false;
        return out;
    }
    out.hasLyrics = true;

    // 检测纯音乐/无歌词占位：只有歌词数据明确标记当前整首歌为纯音乐时才律动。
    // 普通歌曲歌词尚未到达、暂无歌词或加载中，不能走频谱路径。
    {
        bool hasNonEmptyLine = false;
        bool allNonLyricPlaceholder = true;
        bool hasPureMusicMarker = false;
        for (const auto& line : lyrics_.lines) {
            if (line.text.empty()) continue;
            hasNonEmptyLine = true;
            if (IsPureMusicPlaceholder(line.text)) {
                hasPureMusicMarker = true;
            }
            if (!IsNonLyricPlaceholder(line.text)) {
                allNonLyricPlaceholder = false;
                break;
            }
        }
        if (!hasNonEmptyLine || allNonLyricPlaceholder) {
            out.hasLyrics = false;
            out.isInstrumental = hasPureMusicMarker;
            return out;
        }
    }

    const int idx = FindLineIndex(effectiveTime);
    if (idx < 0) {
        // 进度在第一行之前 → 兜底：返回第一行（独立模式 currentTime=0 时触发）
        if (!lyrics_.lines.empty()) {
            const auto& line = lyrics_.lines[0];
            out.currentLineIndex = 0;
            out.currentLine      = line.text;
            out.currentTranslated= line.translated;
        }
        return out;
    }

    const auto& line = lyrics_.lines[idx];
    out.currentLineIndex = idx;
    out.currentLine      = line.text;
    out.currentTranslated= line.translated;

    // 卡片模式：输出下一行歌词预览
    if (idx + 1 < static_cast<int>(lyrics_.lines.size())) {
        out.nextLine = lyrics_.lines[idx + 1].text;
    }

    // 在该行内计算字符级进度
    if (!line.characters.empty()) {
        const int64_t tMs = static_cast<int64_t>(effectiveTime * 1000.0);
        const auto& chars = line.characters;
        // 找到 tMs 落入哪个字符
        int charIdx = -1;
        for (size_t i = 0; i < chars.size(); ++i) {
            if (tMs >= chars[i].startTime && tMs <= chars[i].endTime) {
                charIdx = static_cast<int>(i);
                break;
            }
            if (tMs > chars[i].endTime &&
                (i + 1 >= chars.size() || tMs < chars[i + 1].startTime)) {
                charIdx = static_cast<int>(i);
                break;
            }
        }
        if (charIdx < 0) {
            if (tMs < chars.front().startTime) charIdx = -1;
            else charIdx = static_cast<int>(chars.size()) - 1;
        }

        if (charIdx < 0) {
            out.progress = 0.0;
        } else if (static_cast<size_t>(charIdx) >= chars.size() - 1) {
            // 最后一个字符: 与其他字符一样的平滑插值
            const auto& cur = chars[chars.size() - 1];
            const int64_t dur = std::max<int64_t>(1, cur.endTime - cur.startTime);
            const double inside = std::clamp(
                static_cast<double>(tMs - cur.startTime) / static_cast<double>(dur),
                0.0, 1.0);
            out.progress = (static_cast<double>(chars.size() - 1) + inside) /
                           static_cast<double>(chars.size());
        } else {
            // 进度 = (已唱完整字符数 + 当前字符内进度) / 总字符数
            const auto& cur = chars[charIdx];
            const int64_t dur = std::max<int64_t>(1, cur.endTime - cur.startTime);
            const double inside = std::clamp(
                static_cast<double>(tMs - cur.startTime) / static_cast<double>(dur),
                0.0, 1.0);
            out.progress = (static_cast<double>(charIdx) + inside) /
                           static_cast<double>(chars.size());
        }
    } else {
        // 没有逐字时间轴 -> 使用整行
        if (!line.text.empty()) {
            out.progress = 0.0; // 简化: 整行一次性显示
        }
    }

    return out;
}

// ---- LRC 解析(备用方案) ----
std::vector<LyricsParser::LrcLine> LyricsParser::ParseLRC(const std::string& lrcContent) {
    std::vector<LrcLine> result;
    static const std::regex pattern(R"(\[(\d{2}):(\d{2})\.(\d{2,3})\](.*))");
    std::istringstream stream(lrcContent);
    std::string line;
    std::smatch match;
    while (std::getline(stream, line)) {
        if (std::regex_match(line, match, pattern)) {
            int    minutes = std::stoi(match[1].str());
            int    seconds = std::stoi(match[2].str());
            double frac    = 0.0;
            const std::string msStr = match[3].str();
            if (msStr.length() == 3) {
                frac = std::stoi(msStr) / 1000.0;
            } else {
                frac = std::stoi(msStr) / 100.0;
            }
            result.push_back({minutes * 60 + seconds + frac, match[4].str()});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const LrcLine& a, const LrcLine& b) { return a.timeSec < b.timeSec; });
    return result;
}

} // namespace echo
