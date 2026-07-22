// SPDX-License-Identifier: GPL-3.0

const TOKEN_KEY = "authToken";
const DYNAMIC_PORT_MIN = 49152;
const DYNAMIC_PORT_MAX = 65535;
const NATIVE_START_ATTEMPTS = 3;

let state = null;
let helperPid = 0;
let snapshotDispose = null;
let syncTimer = 0;
let lastNativeSync = null;
let latestSnapshot = null;
let latestSnapshotAt = 0;
let nativeStarting = false;

const makeToken = () => {
  if (globalThis.crypto?.randomUUID) return `EchoTL-${crypto.randomUUID()}`;
  return `EchoTL-${Date.now().toString(36)}-${Math.random()
    .toString(36)
    .slice(2)}`;
};

const makeRandomPort = () => {
  const range = DYNAMIC_PORT_MAX - DYNAMIC_PORT_MIN + 1;
  if (globalThis.crypto?.getRandomValues) {
    const value = new Uint32Array(1);
    crypto.getRandomValues(value);
    return DYNAMIC_PORT_MIN + (value[0] % range);
  }
  return DYNAMIC_PORT_MIN + Math.floor(Math.random() * range);
};

const getToken = async (ctx) => {
  const saved = await ctx.storage.get(TOKEN_KEY);
  if (typeof saved === "string" && saved.length >= 16) return saved;
  const token = makeToken();
  await ctx.storage.set(TOKEN_KEY, token);
  return token;
};

const getWsUrl = () =>
  `ws://127.0.0.1:${state.bridgePort}/?token=${encodeURIComponent(
    state.authToken,
  )}`;

const isNativeOpen = () => state?.ws?.readyState === WebSocket.OPEN;

const sendNative = (message) => {
  if (!isNativeOpen()) return false;
  try {
    state.ws.send(JSON.stringify(message));
    return true;
  } catch {
    return false;
  }
};

const getEstimatedPlaybackMs = (playback) => {
  if (!playback) return 0;
  const baseMs = Math.max(0, Number(playback.currentTime || 0) * 1000);
  if (!playback.isPlaying) return baseMs;
  // P0-4: 快照可能不带 updatedAt 字段——退回到本插件收到快照的时刻，
  // 避免把已过期的 currentTime 当作"现在"发出去
  const updatedAt = Number(playback.updatedAt || latestSnapshotAt || Date.now());
  const rate = Math.max(0.1, Number(playback.playbackRate || 1));
  const elapsedMs = Math.max(0, Date.now() - updatedAt) * rate;
  const durationMs = Math.max(0, Number(playback.duration || 0) * 1000);
  const seekMs = baseMs + elapsedMs;
  return durationMs > 0 ? Math.min(seekMs, durationMs) : seekMs;
};

const toMilliseconds = (value) => {
  const number = Number(value);
  if (!Number.isFinite(number)) return 0;
  return Math.round(number);
};

const secondsToMilliseconds = (value) => {
  const number = Number(value);
  if (!Number.isFinite(number)) return 0;
  return Math.round(number * 1000);
};

const unknownTimeToMilliseconds = (value) => {
  const number = Number(value);
  if (!Number.isFinite(number)) return 0;
  return number > 100000 ? Math.round(number) : Math.round(number * 1000);
};

const pickText = (...values) => {
  for (const value of values) {
    if (typeof value === "string" && value.trim()) return value;
  }
  return "";
};

const getLineText = (line) =>
  pickText(line?.text, line?.content, line?.lyric, line?.lineLyric);

const getLineTranslation = (line) =>
  pickText(
    line?.translated,
    line?.translation,
    line?.translate,
    line?.secondary,
    line?.secondaryText,
  );

const getLineStartMs = (line) => {
  if (!line || typeof line !== "object") return 0;
  const charStart = line.characters?.[0]?.startTime;
  if (Number.isFinite(Number(charStart))) return toMilliseconds(charStart);
  if (line.startTime != null) return toMilliseconds(line.startTime);
  if (line.startMs != null) return Math.round(Number(line.startMs) || 0);
  if (line.time != null) return secondsToMilliseconds(line.time);
  if (line.timestamp != null) return unknownTimeToMilliseconds(line.timestamp);
  return 0;
};

const makeCharacters = (text, startTime, endTime) => {
  const chars = Array.from(text || "");
  if (!chars.length) return [];
  const duration = Math.max(300, endTime - startTime);
  const unit = duration / chars.length;
  return chars.map((char, index) => ({
    char,
    startTime: Math.round(startTime + unit * index),
    endTime: Math.round(startTime + unit * (index + 1)),
  }));
};

// P0-1: 无逐字时间轴时按发音单元估算演唱时长（中文约 300ms/字，
// 拉丁字母约 3 个字母一个音节），避免把行后间奏也算进高亮时长
const KARAOKE_CHAR_MS = 300;

const estimateVocalMs = (text) => {
  let units = 0;
  let latinRun = 0;
  for (const ch of Array.from(text || "")) {
    if (/[A-Za-z0-9']/.test(ch)) {
      latinRun += 1;
    } else {
      if (latinRun > 0) {
        units += Math.ceil(latinRun / 3);
        latinRun = 0;
      }
      if (!/\s/.test(ch)) units += 1;
    }
  }
  if (latinRun > 0) units += Math.ceil(latinRun / 3);
  return Math.max(KARAOKE_CHAR_MS, units * KARAOKE_CHAR_MS);
};

// 行自带的演唱结束时间（若上游提供）。短于 300ms 的 duration 视为
// 单位可疑（可能是秒）而丢弃，落回估算路径。
const getLineEndMs = (line, startTime) => {
  const absolute = line?.endTime ?? line?.endMs;
  if (absolute != null) {
    const ms = toMilliseconds(absolute);
    if (ms > startTime) return ms;
  }
  const duration = Number(line?.duration ?? line?.durationMs);
  if (Number.isFinite(duration) && duration >= 300) {
    return startTime + Math.round(duration);
  }
  return 0;
};

const normalizeCharacters = (line, text, startTime, endTime) => {
  if (Array.isArray(line?.characters) && line.characters.length > 0) {
    const chars = line.characters
      .map((char) => ({
        char: pickText(char?.char, char?.ch, char?.text),
        startTime: toMilliseconds(char?.startTime ?? char?.startMs ?? 0),
        endTime: toMilliseconds(char?.endTime ?? char?.endMs ?? 0),
      }))
      .filter((char) => char.char);
    return chars.map((char, index) => {
      const nextStart = chars[index + 1]?.startTime;
      const fallbackEnd =
        Number.isFinite(nextStart) && nextStart > char.startTime
          ? nextStart
          : endTime;
      return {
        ...char,
        endTime:
          char.endTime > char.startTime
            ? char.endTime
            : Math.max(char.startTime + 80, fallbackEnd),
      };
    });
  }
  return makeCharacters(text, startTime, endTime);
};

const normalizeLyrics = (snapshot) => {
  const rawLines = Array.isArray(snapshot?.lyric?.lines)
    ? snapshot.lyric.lines
    : [];
  const lines = rawLines
    .map((line) => ({
      raw: line,
      text: getLineText(line),
      translated: getLineTranslation(line),
      startTime: getLineStartMs(line),
    }))
    .filter((line) => line.text)
    // 原生端 FindLineIndex 用二分查找，依赖行按时间升序（C-23 防御）
    .sort((a, b) => a.startTime - b.startTime);

  return lines.map((line, index) => {
    const nextStart =
      index + 1 < lines.length
        ? Math.max(lines[index + 1].startTime, line.startTime + 300)
        : line.startTime + 2800;
    // P0-1: 伪造逐字时间轴不能铺满整个行间隔——间奏会让高亮慢于人声。
    // 优先用行自带的演唱结束时间，否则按发音单元估算，均以下一行开始为上限。
    const declaredEnd = getLineEndMs(line.raw, line.startTime);
    const estimatedEnd = line.startTime + estimateVocalMs(line.text);
    const vocalEnd = Math.min(
      nextStart,
      declaredEnd > line.startTime ? declaredEnd : estimatedEnd,
    );
    return {
      text: line.text,
      translated: line.translated,
      startTime: line.startTime,
      characters: normalizeCharacters(
        line.raw,
        line.text,
        line.startTime,
        vocalEnd,
      ),
    };
  });
};

const buildPayload = (snapshot) => {
  const playback = snapshot?.playback || {};
  const track = playback.track || playback.currentTrack || {};
  const title = pickText(playback.title, track.title, track.name);
  const artist = pickText(
    playback.artist,
    track.artist,
    Array.isArray(track.artists) ? track.artists.join(" / ") : "",
  );
  // 官方契约：歌词时钟 = 播放进度 + lyric.timeOffset（用户可调偏移，毫秒）
  const lyricSeekMs =
    getEstimatedPlaybackMs(playback) + Number(snapshot?.lyric?.timeOffset || 0);

  return {
    source: "EchoMusic",
    isPlaying: Boolean(playback.isPlaying),
    // 私人 FM 状态来自 EchoMusic snapshot，原生端据此把“上一首”切换为“不喜欢”。
    isPersonalFM: Boolean(playback.isPersonalFM),
    currentTime: lyricSeekMs / 1000,
    duration: Number(playback.duration || track.duration || 0),
    playbackRate: Math.max(0.1, Number(playback.playbackRate || 1)),
    songName: title,
    songTitle: artist ? `${title} - ${artist}` : title,
    coverArtUrl: pickText(
      playback.cover,
      playback.coverUrl,
      playback.pic,
      track.cover,
      track.coverUrl,
      track.pic,
      track.albumArt,
    ),
    lyricsData: normalizeLyrics(snapshot),
  };
};

const syncSnapshot = async ({ force = false } = {}) => {
  if (!state?.ready || !latestSnapshot) return;

  const payload = buildPayload(latestSnapshot);
  const now = Date.now();
  // P0-3: 签名只取元数据，不再对整包歌词做 JSON.stringify。
  // lyricsBytes 用于捕捉行数不变但内容变化的情况（如翻译异步到达）。
  const signature = JSON.stringify({
    isPlaying: payload.isPlaying,
    isPersonalFM: payload.isPersonalFM,
    duration: payload.duration,
    playbackRate: payload.playbackRate,
    songName: payload.songName,
    songTitle: payload.songTitle,
    coverArtUrl: payload.coverArtUrl,
    lineCount: payload.lyricsData.length,
    lyricsBytes: payload.lyricsData.reduce(
      (sum, line) =>
        sum + line.text.length + (line.translated ? line.translated.length : 0),
      0,
    ),
  });
  const rawPredictedTime = lastNativeSync
    ? lastNativeSync.currentTime +
      (lastNativeSync.isPlaying
        ? ((now - lastNativeSync.sentAt) / 1000) * lastNativeSync.playbackRate
        : 0)
    : 0;
  const predictedTime =
    lastNativeSync?.duration > 0
      ? Math.min(rawPredictedTime, lastNativeSync.duration)
      : rawPredictedTime;
  const hasMeaningfulChange =
    !lastNativeSync ||
    signature !== lastNativeSync.signature ||
    Math.abs(payload.currentTime - predictedTime) > 0.75;

  if (!force && !hasMeaningfulChange) return;

  const sent = sendNative({ type: "lyrics", data: payload });
  if (!sent) throw new Error("native WebSocket unavailable");
  lastNativeSync = {
    signature,
    currentTime: payload.currentTime,
    isPlaying: payload.isPlaying,
    playbackRate: payload.playbackRate,
    duration: payload.duration,
    sentAt: now,
  };
};

const runNowPlayingCommand = async (ctx, command) => {
  try {
    const result = ctx.nowPlaying?.command?.(command);
    if (result && typeof result.then === "function") {
      await result;
    }
  } catch {
    // Ignore command failures from EchoMusic while keeping the bridge alive.
  }
};

const getNowPlayingSnapshot = async (ctx) => {
  try {
    return await ctx.nowPlaying?.getSnapshot?.();
  } catch {
    return null;
  }
};

const terminateHelper = async (ctx, pid) => {
  if (!pid) return;
  try {
    await ctx.process?.terminate?.(pid);
  } catch {
    // EchoMusic may already have cleaned up plugin-owned helper processes.
  }
};

const runNativeCommand = async (ctx, command) => {
  if (typeof command !== "string") return;
  if (command === "dislikeFm") {
    // 私人 FM 不喜欢优先走播放器 API：会上报垃圾歌曲并切歌。
    if (ctx.player && typeof ctx.player.dislikePersonalFm === "function") {
      try {
        await ctx.player.dislikePersonalFm();
        return;
      } catch {
        // API 不可用或失败时，回退为普通下一首，至少保证按钮可跳过。
      }
    }
    await runNowPlayingCommand(ctx, "nextTrack");
    return;
  }

  await runNowPlayingCommand(ctx, command);
};

const handleNativeMessage = (ctx, raw) => {
  try {
    const message = JSON.parse(raw);
    if (message?.type === "pong") return;
    if (message?.type !== "command") return;

    const action = message.payload?.action;
    if (typeof action === "string") {
      void runNativeCommand(ctx, action);
    }
  } catch {
    // Ignore malformed bridge messages.
  }
};

// P0-3: 心跳只发轻量播放状态——歌词仅在内容变化时全量发送，
// 避免每秒搬运整包歌词并把序列化/解析延迟每秒重新注入原生端时基
const sendHeartbeat = () => {
  if (!state?.ready || !latestSnapshot) return false;
  const playback = latestSnapshot.playback || {};
  const track = playback.track || playback.currentTrack || {};
  // 与 buildPayload 一致：歌词时钟叠加 lyric.timeOffset（官方契约）
  const lyricSeekMs =
    getEstimatedPlaybackMs(playback) +
    Number(latestSnapshot?.lyric?.timeOffset || 0);
  return sendNative({
    type: "heartbeat",
    data: {
      isPlaying: Boolean(playback.isPlaying),
      currentTime: lyricSeekMs / 1000,
      playbackRate: Math.max(0.1, Number(playback.playbackRate || 1)),
      duration: Number(playback.duration || track.duration || 0),
    },
  });
};

const startSyncTimer = () => {
  if (syncTimer) window.clearInterval(syncTimer);
  syncTimer = window.setInterval(() => {
    // 心跳兼职置顶保活：原生端收到后仅在检测到丢失 TOPMOST 时恢复
    if (!sendHeartbeat()) {
      console.warn("[echo-taskbar-lyrics] heartbeat send failed");
    }
  }, 1000);
};

const stopSyncTimer = () => {
  if (syncTimer) window.clearInterval(syncTimer);
  syncTimer = 0;
};

const closeNativeSocket = () => {
  if (!state?.ws) return;
  state.ws.onopen = null;
  state.ws.onmessage = null;
  state.ws.onclose = null;
  state.ws.onerror = null;
  try {
    state.ws.close();
  } catch {
    // Ignore close errors during shutdown.
  }
  state.ws = null;
};

const openNativeSocket = (ctx) =>
  new Promise((resolve, reject) => {
    const socket = new WebSocket(getWsUrl());
    let settled = false;
    let opened = false;
    let timer = 0;
    const finish = (ok, error) => {
      if (settled) return;
      settled = true;
      window.clearTimeout(timer);
      if (ok) resolve(socket);
      else {
        try {
          socket.close();
        } catch {
          // Ignore close errors while abandoning an unavailable endpoint.
        }
        reject(error || new Error("native WebSocket unavailable"));
      }
    };
    timer = window.setTimeout(
      () => {
        const error = new Error("native WebSocket timeout");
        if (opened) error.code = "PORT_OCCUPIED";
        finish(false, error);
      },
      1000,
    );

    socket.onopen = () => {
      opened = true;
      try {
        socket.send(JSON.stringify({ type: "ping" }));
      } catch (error) {
        finish(false, error);
      }
    };
    socket.onmessage = (event) => {
      if (!settled) {
        try {
          if (JSON.parse(event.data)?.type !== "pong") return;
          state.ws = socket;
          finish(true);
        } catch {
          return;
        }
      } else {
        handleNativeMessage(ctx, event.data);
      }
    };
    socket.onclose = () => {
      if (state?.ws === socket) {
        state.ready = false;
        state.ws = null;
      }
      const error = new Error("native WebSocket closed");
      if (opened) error.code = "PORT_OCCUPIED";
      finish(false, error);
    };
    socket.onerror = () => {
      finish(false, new Error("native WebSocket error"));
    };
  });

const connectNative = async (ctx) => {
  let lastError = null;
  for (let i = 0; i < 30; i += 1) {
    try {
      closeNativeSocket();
      await openNativeSocket(ctx);
      return true;
    } catch (error) {
      lastError = error;
      if (error?.code === "PORT_OCCUPIED") throw error;
      await new Promise((resolve) => setTimeout(resolve, 200));
    }
  }
  throw lastError || new Error("native WebSocket unavailable");
};

const startNative = async (ctx) => {
  if (!state || nativeStarting || state.ready) return;

  nativeStarting = true;
  try {
    if (ctx.electron?.platform && ctx.electron.platform !== "win32") {
      ctx.toast.warning("任务栏歌词仅支持 Windows");
      return;
    }

    if (helperPid) {
      await stopNative(ctx);
    }

    const attemptedPorts = new Set();
    for (let attempt = 0; attempt < NATIVE_START_ATTEMPTS; attempt += 1) {
      do {
        state.bridgePort = makeRandomPort();
      } while (attemptedPorts.has(state.bridgePort));
      attemptedPorts.add(state.bridgePort);

      const result = await ctx.process.launch({
        executable: "EchoTaskbarLyrics.exe",
        args: [
          "--echo-plugin",
          "--auth-token",
          state.authToken,
          "--bridge-port",
          String(state.bridgePort),
        ],
        env: {
          ECHO_TASKBAR_LYRICS_TOKEN: state.authToken,
        },
      });

      if (!result.ok) {
        if (!result.canceled) ctx.toast.warning(result.error || "启动任务栏歌词失败");
        return;
      }

      helperPid = result.pid || 0;
      try {
        await connectNative(ctx);
        state.ready = true;
        latestSnapshot = await getNowPlayingSnapshot(ctx);
        latestSnapshotAt = Date.now();
        await syncSnapshot({ force: true }).catch(() => undefined);
        startSyncTimer();
        return;
      } catch (error) {
        console.warn(
          `[echo-taskbar-lyrics] native helper failed on port ${state.bridgePort}`,
          error,
        );
        await stopNative(ctx);
      }
    }
    ctx.toast.warning("任务栏歌词启动后未响应，无法分配可用的本地通信端口");
  } finally {
    nativeStarting = false;
  }
};

const stopNative = async (ctx) => {
  stopSyncTimer();
  if (state) state.ready = false;
  lastNativeSync = null;
  sendNative({ type: "shutdown", command: "shutdown" });
  closeNativeSocket();
  await terminateHelper(ctx, helperPid);
  helperPid = 0;
};

export async function activate(ctx) {
  state = {
    authToken: await getToken(ctx),
    bridgePort: 0,
    ready: false,
    ws: null,
  };

  snapshotDispose = ctx.nowPlaying.onSnapshot((snapshot) => {
    latestSnapshot = snapshot;
    latestSnapshotAt = Date.now();
    void syncSnapshot().catch((error) => {
      console.warn("[echo-taskbar-lyrics] snapshot sync failed", error);
    });
  });

  await startNative(ctx);
}

export async function deactivate(ctx) {
  snapshotDispose?.();
  snapshotDispose = null;
  await stopNative(ctx);
  latestSnapshot = null;
  latestSnapshotAt = 0;
  state = null;
}
