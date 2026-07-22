// SPDX-License-Identifier: GPL-3.0
// lyrics_parser.h - 歌词解析与同步
//
// 职责:
//   - 持有最新的 LyricsData / PlayerState
//   - 主循环每帧调用 GetCurrentRenderState()
//   - 二分查找当前时间对应的歌词行与字符进度
//
#pragma once

#include "lyrics_data.h"

#include <mutex>
#include <string>
#include <vector>

namespace echo {

class LyricsParser {
public:
    LyricsParser() = default;

    // 从 WebSocket 接收歌词时调用
    void UpdateLyrics(const LyricsData& data);

    // 从 WebSocket 接收播放状态时调用
    void UpdatePlayerState(const PlayerState& state);

    // P0-3: 轻量心跳同步。仅当播放状态变化或与本地推算偏差超阈值时
    // 才重置本地时基，避免传输链路的固定延迟每秒被重新注入
    void SyncPlaybackHeartbeat(bool isPlaying, double currentTimeSec,
                               double playbackRate, double durationSec);

    // 主循环每帧调用,返回当前应渲染的内容
    RenderState GetCurrentRenderState() const;

    bool HasLyrics() const;
    bool IsPlaying() const;

    // ---- 辅助:LRC 解析(备用) ----
    struct LrcLine {
        double      timeSec{0.0};
        std::string text;
    };
    static std::vector<LrcLine> ParseLRC(const std::string& lrcContent);

#ifdef ECHO_UNIT_TEST
public:
#else
private:
#endif
    // 二分查找 currentTimeSec 对应的歌词行索引;未匹配返回 -1
    int FindLineIndex(double currentTimeSec) const;

    // 互斥:歌词数据 / 播放状态均可能被 WebSocket 线程更新
    mutable std::mutex   mutex_;
    LyricsData            lyrics_;
    PlayerState           state_;

    // 本地时钟死推算：记录收到 playerState 时的本地时间
    // 用于在 GetCurrentRenderState() 中推算播放进度，使逐字高亮平滑推进
    mutable double lastUpdateWallTime_{0.0};

    // P0-2: 播放中但无歌词数据的起始时刻（0 = 未计时）。
    // 超过宽限期仍无歌词则按纯音乐处理（显示频谱），宽限期用于区分"歌词加载中"
    mutable double noLyricsSinceWallTime_{0.0};
};

} // namespace echo
