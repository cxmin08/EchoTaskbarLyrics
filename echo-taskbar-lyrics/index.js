// SPDX-License-Identifier: GPL-3.0

const NATIVE_WS_PORT = 6523;
const TOKEN_KEY = "authToken";

let state = null;
let helperPid = 0;
let snapshotDispose = null;
let syncTimer = 0;
let lastNativeSync = null;
let latestSnapshot = null;
let nativeStarting = false;

const makeToken = () => {
  if (globalThis.crypto?.randomUUID) return `EchoTL-${crypto.randomUUID()}`;
  return `EchoTL-${Date.now().toString(36)}-${Math.random()
    .toString(36)
    .slice(2)}`;
};

const getToken = async (ctx) => {
  const saved = await ctx.storage.get(TOKEN_KEY);
  if (typeof saved === "string" && saved.length >= 16) return saved;
  const token = makeToken();
  await ctx.storage.set(TOKEN_KEY, token);
  return token;
};

const getWsUrl = () =>
  `ws://127.0.0.1:${NATIVE_WS_PORT}/?token=${encodeURIComponent(
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
  const updatedAt = Number(playback.updatedAt || Date.now());
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
    .filter((line) => line.text);

  return lines.map((line, index) => {
    const nextStart =
      index + 1 < lines.length
        ? Math.max(lines[index + 1].startTime, line.startTime + 300)
        : line.startTime + 2800;
    return {
      text: line.text,
      translated: line.translated,
      startTime: line.startTime,
      characters: normalizeCharacters(
        line.raw,
        line.text,
        line.startTime,
        nextStart,
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

  return {
    source: "EchoMusic",
    isPlaying: Boolean(playback.isPlaying),
    // 私人 FM 状态来自 EchoMusic snapshot，原生端据此把“上一首”切换为“不喜欢”。
    isPersonalFM: Boolean(playback.isPersonalFM),
    currentTime: getEstimatedPlaybackMs(playback) / 1000,
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
  const signature = JSON.stringify({ ...payload, currentTime: 0 });
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

const startSyncTimer = () => {
  if (syncTimer) window.clearInterval(syncTimer);
  syncTimer = window.setInterval(() => {
    // The native window can lose its topmost Z-order after Shell interactions.
    // A periodic lyrics heartbeat lets the helper restore it promptly.
    void syncSnapshot({ force: true }).catch((error) => {
      console.warn("[echo-taskbar-lyrics] heartbeat sync failed", error);
    });
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
    let timer = 0;
    const finish = (ok, error) => {
      if (settled) return;
      settled = true;
      window.clearTimeout(timer);
      if (ok) resolve(socket);
      else reject(error || new Error("native WebSocket unavailable"));
    };
    timer = window.setTimeout(
      () => finish(false, new Error("native WebSocket timeout")),
      1000,
    );

    socket.onopen = () => {
      state.ws = socket;
      finish(true);
    };
    socket.onmessage = (event) => handleNativeMessage(ctx, event.data);
    socket.onclose = () => {
      if (state?.ws === socket) {
        state.ready = false;
        state.ws = null;
      }
      finish(false, new Error("native WebSocket closed"));
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

    const result = await ctx.process.launch({
      executable: "EchoTaskbarLyrics.exe",
      args: [
        "--echo-plugin",
        "--auth-token",
        state.authToken,
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
    } catch (error) {
      console.warn("[echo-taskbar-lyrics] native helper failed to accept WebSocket", error);
      ctx.toast.warning("任务栏歌词启动后未响应，请确认端口未被占用");
      await stopNative(ctx);
      return;
    }
    state.ready = true;
    latestSnapshot = await getNowPlayingSnapshot(ctx);
    await syncSnapshot({ force: true }).catch(() => undefined);
    startSyncTimer();
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
    ready: false,
    ws: null,
  };

  snapshotDispose = ctx.nowPlaying.onSnapshot((snapshot) => {
    latestSnapshot = snapshot;
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
  state = null;
}
