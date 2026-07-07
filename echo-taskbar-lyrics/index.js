const DEFAULT_SETTINGS = {
  enabled: true,
  httpPort: 6523,
  syncIntervalMs: 1000,
};

const AUTH_HEADER = "X-Echo-Token";
const STORAGE_KEY = "settings";
const TOKEN_KEY = "authToken";

let state = null;
let helperPid = 0;
let snapshotDispose = null;
let syncTimer = 0;
let commandTimer = 0;
let latestSnapshot = null;
let settingsDispose = null;
let nativeStarting = false;

const clamp = (value, min, max) =>
  Math.max(min, Math.min(max, Number(value) || 0));

const normalizeSettings = (value) => {
  const source = value && typeof value === "object" ? value : {};
  return {
    ...DEFAULT_SETTINGS,
    ...source,
    enabled: source.enabled ?? DEFAULT_SETTINGS.enabled,
    httpPort: Math.round(
      clamp(source.httpPort ?? DEFAULT_SETTINGS.httpPort, 1024, 65535),
    ),
    syncIntervalMs: Math.round(
      clamp(
        source.syncIntervalMs ?? DEFAULT_SETTINGS.syncIntervalMs,
        300,
        5000,
      ),
    ),
  };
};

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

const getBaseUrl = () => `http://127.0.0.1:${state.settings.httpPort}`;

const requestNative = async (path, options = {}) => {
  const response = await fetch(`${getBaseUrl()}${path}`, {
    ...options,
    headers: {
      [AUTH_HEADER]: state.authToken,
      ...(options.body ? { "Content-Type": "application/json" } : {}),
      ...(options.headers || {}),
    },
  });
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  return response;
};

const waitForNative = async () => {
  let lastError = null;
  for (let i = 0; i < 30; i += 1) {
    try {
      await requestNative("/ping");
      return true;
    } catch (error) {
      lastError = error;
      await new Promise((resolve) => setTimeout(resolve, 200));
    }
  }
  throw lastError || new Error("native helper unavailable");
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

const syncSnapshot = async () => {
  if (!state?.settings.enabled || !state.ready || !latestSnapshot) return;
  const payload = buildPayload(latestSnapshot);
  await requestNative("/lyrics", {
    method: "POST",
    body: JSON.stringify(payload),
  });
};

const pollCommands = async (ctx) => {
  if (!state?.ready) return;
  try {
    const response = await requestNative("/commands");
    const body = await response.json();
    const commands = Array.isArray(body?.commands) ? body.commands : [];
    for (const command of commands) {
      if (typeof command === "string") {
        if (command === "dislikeFm") {
          // 私人 FM 不喜欢优先走播放器 API：会上报垃圾歌曲并切歌。
          if (ctx.player && typeof ctx.player.dislikePersonalFm === "function") {
            try {
              await ctx.player.dislikePersonalFm();
              continue;
            } catch {
              // API 不可用或失败时，回退为普通下一首，至少保证按钮可跳过。
            }
          }
          await ctx.nowPlaying.command("nextTrack").catch(() => undefined);
        } else {
          await ctx.nowPlaying.command(command).catch(() => undefined);
        }
      }
    }
  } catch {
    // The native helper may still be starting or shutting down.
  }
};

const startTimers = (ctx) => {
  if (syncTimer) window.clearInterval(syncTimer);
  if (commandTimer) window.clearInterval(commandTimer);
  syncTimer = window.setInterval(() => {
    void syncSnapshot().catch((error) => {
      console.warn("[echo-taskbar-lyrics] sync failed", error);
    });
  }, state.settings.syncIntervalMs);
  commandTimer = window.setInterval(() => {
    void pollCommands(ctx);
  }, 500);
};

const stopTimers = () => {
  if (syncTimer) window.clearInterval(syncTimer);
  if (commandTimer) window.clearInterval(commandTimer);
  syncTimer = 0;
  commandTimer = 0;
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
        "--http-port",
        String(state.settings.httpPort),
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
      await waitForNative();
    } catch (error) {
      console.warn("[echo-taskbar-lyrics] native helper failed to become ready", error);
      ctx.toast.warning("任务栏歌词启动后未响应，请确认端口未被占用");
      await stopNative(ctx);
      return;
    }
    state.ready = true;
    latestSnapshot = await ctx.nowPlaying.getSnapshot().catch(() => null);
    await syncSnapshot().catch(() => undefined);
    startTimers(ctx);
  } finally {
    nativeStarting = false;
  }
};

const stopNative = async (ctx) => {
  stopTimers();
  state.ready = false;
  await requestNative("/shutdown", {
    method: "POST",
    body: JSON.stringify({ command: "shutdown" }),
  }).catch(() => undefined);
  if (helperPid) {
    await ctx.process.terminate(helperPid).catch(() => undefined);
  }
  helperPid = 0;
};

const registerSettings = (ctx) => {
  const Settings = ctx.vue.defineComponent({
    name: "EchoTaskbarLyricsSettings",
    setup() {
      const { defineAsyncComponent, h } = ctx.vue;
      const Button = defineAsyncComponent(ctx.ui.components.Button);
      const Switch = defineAsyncComponent(ctx.ui.components.Switch);

      const save = async (patch) => {
        const wasEnabled = Boolean(state.settings.enabled);
        const previousPort = state.settings.httpPort;
        const previousSyncIntervalMs = state.settings.syncIntervalMs;
        state.settings = normalizeSettings({ ...state.settings, ...patch });
        await ctx.storage.set(STORAGE_KEY, state.settings);
        const isEnabled = Boolean(state.settings.enabled);
        if (wasEnabled !== isEnabled) {
          if (isEnabled) await startNative(ctx);
          else await stopNative(ctx);
        } else if (isEnabled && previousPort !== state.settings.httpPort) {
          await stopNative(ctx);
          await startNative(ctx);
        } else if (
          isEnabled &&
          previousSyncIntervalMs !== state.settings.syncIntervalMs &&
          state.ready
        ) {
          startTimers(ctx);
        }
      };

      return () =>
        h("div", { style: "display: grid; gap: 12px;" }, [
          h(
            "label",
            {
              style:
                "display: flex; align-items: center; justify-content: space-between; gap: 12px;",
            },
            [
              h("span", "启用"),
              h(Switch, {
                modelValue: state.settings.enabled,
                "onUpdate:modelValue": (value) => save({ enabled: Boolean(value) }),
              }),
            ],
          ),
          h("label", { style: "display: grid; gap: 6px;" }, [
            h("span", "本地端口"),
            h("input", {
              type: "number",
              min: "1024",
              max: "65535",
              value: String(state.settings.httpPort),
              style:
                "width: 100%; min-width: 0; border: 1px solid color-mix(in srgb, var(--color-text-main, #f8fafc) 14%, transparent); border-radius: 8px; background: color-mix(in srgb, var(--surface-card-base, #111827) 86%, transparent); color: var(--color-text-main, #f8fafc); padding: 8px 10px;",
              onChange: (event) =>
                save({
                  httpPort:
                    Number(event?.target?.value) || DEFAULT_SETTINGS.httpPort,
                }),
            }),
          ]),
          h(
            Button,
            {
              size: "xs",
              variant: "outline",
              onClick: async () => {
                await stopNative(ctx);
                await startNative(ctx);
              },
            },
            { default: () => "重启任务栏歌词" },
          ),
        ]);
    },
  });

  settingsDispose = ctx.ui.settings.define({
    title: "任务栏歌词",
    description: "管理 EchoMusic 到 Windows 任务栏歌词原生程序的桥接。",
    component: Settings,
  });
};

export async function activate(ctx) {
  state = ctx.vue.reactive({
    settings: normalizeSettings(await ctx.storage.get(STORAGE_KEY)),
    authToken: await getToken(ctx),
    ready: false,
  });

  registerSettings(ctx);

  snapshotDispose = ctx.nowPlaying.onSnapshot((snapshot) => {
    latestSnapshot = snapshot;
    void syncSnapshot().catch((error) => {
      console.warn("[echo-taskbar-lyrics] snapshot sync failed", error);
    });
  });

  ctx.commands.register("restart", async () => {
    await stopNative(ctx);
    await startNative(ctx);
  }, {
    title: "重启任务栏歌词",
  });

  if (state.settings.enabled) {
    await startNative(ctx);
  }
}

export async function deactivate(ctx) {
  snapshotDispose?.();
  snapshotDispose = null;
  settingsDispose?.();
  settingsDispose = null;
  await stopNative(ctx);
  latestSnapshot = null;
  state = null;
}
