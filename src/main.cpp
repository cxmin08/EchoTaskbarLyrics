// SPDX-License-Identifier: GPL-3.0
// main.cpp - EchoMusic 任务栏歌词插件入口
//
#include "config.h"
#include "api_enabler.h"
#include "config_dialog.h"
#include "constants.h"
#include "fullscreen_detector.h"
#include "http_server.h"
#include "logger.h"

#include "d2d_settings_window.h"
#include "lyrics_data.h"
#include "lyrics_parser.h"
#include "native_messaging.h"
#include "renderer.h"
#include "renderer_utils.h"
#include "spectrum_capture.h"
#include "taskbar_window.h"
#include "tray_icon.h"
#include "websocket_client.h"


#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

// 日志快捷方式（使用统一日志系统）
using echo::Log;

// 全局应用上下文（成员按析构顺序排列：后声明的先析构）
// 依赖关系：taskbarWindow → renderer → config
struct AppContext {
    HINSTANCE                hInstance{nullptr};
    HWND                     hwnd{nullptr};
    bool                     running{true};

    // RAII 管理的组件（按依赖顺序声明：先声明后析构）
    std::unique_ptr<echo::NativeMessagingHost> nativeHost;
    std::unique_ptr<echo::D2DSettingsWindow>   d2dSettingsWindow;
    std::unique_ptr<echo::HttpServer>           httpServer;
    std::unique_ptr<echo::WebSocketClient>      wsClient;
    std::unique_ptr<echo::LyricsParser>         parser;
    std::unique_ptr<echo::SpectrumCapture>      spectrumCapture;
    std::unique_ptr<echo::TaskbarRenderer>      renderer;        // 依赖 taskbarWindow 的 HWND
    std::unique_ptr<echo::TaskbarWindow>         taskbarWindow;   // 依赖 renderer
    std::unique_ptr<echo::TrayIcon>             tray;
    std::unique_ptr<echo::Config>               config;          // 最后声明，最先析构
};

using namespace echo::constants;
using echo::renderer_utils::WideToUtf8;

struct RuntimeOptions {
    bool echoPluginMode{false};
    int httpPortOverride{0};
    std::string authToken;
};

RuntimeOptions ParseRuntimeOptions() {
    RuntimeOptions options;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return options;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i] ? argv[i] : L"";
        if (arg == L"--echo-plugin") {
            options.echoPluginMode = true;
        } else if (arg == L"--http-port" && i + 1 < argc) {
            options.httpPortOverride = std::max(0, _wtoi(argv[++i]));
        } else if (arg == L"--auth-token" && i + 1 < argc) {
            options.authToken = WideToUtf8(argv[++i]);
        }
    }

    ::LocalFree(argv);
    return options;
}

// 工具: 把 UTF-8 字符串限制到 Windows Tooltip 长度
std::wstring ToTooltipWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    if (len > WINDOWS_TOOLTIP_MAX_LEN) len = WINDOWS_TOOLTIP_MAX_LEN;
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

// 应用渲染器配置（直接传递 AppearanceConfig，无需逐字段拷贝）
void ApplyRendererSettings(AppContext& app) {
    if (!app.renderer || !app.config) return;
    Log("[CONFIG] ApplyRenderer: hl=%s nl=%s font=%s size=%d opacity=%.2f\n",
        app.config->Appearance().highlightColor.c_str(), app.config->Appearance().normalColor.c_str(),
        app.config->Appearance().fontFamily.c_str(), app.config->Appearance().fontSize,
        app.config->Appearance().normalOpacity);
    app.renderer->ApplySettings(app.config->Appearance());
}

// 菜单命令处理
void OnTrayCommand(AppContext& app, UINT menuId) {
    using namespace echo;
    switch (menuId) {
    case ID_MENU_AUTOSTART: {
        const bool newState = !app.config->IsAutoStart();
        const bool regOk = app.config->SetAutoStart(newState);
        app.config->Save();
        if (app.tray) {
            app.tray->SetMenuCheckedAutoStart(newState);
        }
        // 用 MessageBox 直接弹模态对话框反馈结果（气泡通知在 Win10/11 常被禁用）
        const std::wstring title = L"开机自启";
        std::wstring msg;
        if (regOk) {
            if (newState) {
                msg = L"已启用开机自启。\n\n"
                      L"程序会尝试以下三种方式（按顺序，自动跳过失败的）：\n"
                      L"1) 注册表 Run 键（可能被杀毒软件拦截）\n"
                      L"2) 任务计划程序（推荐）\n"
                      L"3) 启动文件夹快捷方式\n\n"
                      L"查看实际生效方式：\n"
                      L"注册表: reg query HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\n"
                      L"任务计划: schtasks /Query /TN EchoTaskbarLyrics_AutoStart\n"
                      L"启动文件夹: %APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
            } else {
                msg = L"已禁用开机自启，所有方式均已清理。";
            }
        } else {
            msg = L"开机自启设置失败！\n\n"
                  L"三种方式都未生效：\n"
                  L"1. 注册表 Run 键（最可能被拦截）\n"
                  L"2. 任务计划程序 schtasks\n"
                  L"3. 启动文件夹 PowerShell 快捷方式\n\n"
                  L"请查看 debug.log 获取详细错误。";
        }
        ::MessageBoxW(app.hwnd, msg.c_str(), title.c_str(),
            regOk ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
        break;
    }
    case ID_MENU_RECONNECT: {
        if (app.wsClient) app.wsClient->RequestReconnect();
        break;
    }
    case ID_MENU_SETTINGS: {
        // D2D 原生自绘设置界面
        if (!app.d2dSettingsWindow) {
            app.d2dSettingsWindow = std::make_unique<echo::D2DSettingsWindow>();
            app.d2dSettingsWindow->OnConfigChanged([&](const echo::Config& cfg) {
                *app.config = cfg;
                app.config->Save();
                echo::SetLogEnabled(app.config->Advanced().debugLog);
                ApplyRendererSettings(app);
                if (app.renderer) {
                    app.renderer->SetDebugLog(app.config->Advanced().debugLog);
                }
                if (app.taskbarWindow) {
                    app.taskbarWindow->SetDisplayMode(cfg.Appearance().displayMode);
                    app.taskbarWindow->SetLyricWindowWidth(cfg.Appearance().lyricWindowWidth);
                    app.taskbarWindow->SetDragOffset(
                        cfg.Position().offsetX, cfg.Position().offsetY);
                    app.taskbarWindow->SetPositionLocked(cfg.Position().lockPosition);
                    app.taskbarWindow->SetFullyLocked(cfg.Position().lockFully);
                    app.taskbarWindow->InvalidatePositionCache();
                    app.taskbarWindow->Reposition();
                }
                if (app.tray) {
                    app.tray->SetMenuCheckedAutoStart(cfg.IsAutoStart());
                    app.tray->SetMenuCheckedLockPos(cfg.Position().lockPosition);
                    app.tray->SetMenuCheckedLockFull(cfg.Position().lockFully);
                }
                Log("[SETTINGS] D2D config applied and saved\n");
            });
        }

        if (app.d2dSettingsWindow->Show(app.hInstance, app.hwnd, *app.config)) {
            break;
        }
        app.d2dSettingsWindow.reset();
        app.d2dSettingsWindow = nullptr;

        // ═══════ 回退：Win32 原生对话框 ═══════
        if (echo::ConfigDialog::Show(app.hInstance, app.hwnd, *app.config,
                                       /*boundMode*/ false,
                                       [&app]() {
                                           // 同 ID_MENU_EXIT：ConfigDialog 的模态循环
                                           // 嵌套在 TrackPopupMenuEx 内，WM_QUIT 会被吞掉
                                           app.running = false;
                                       })) {
            ApplyRendererSettings(app);
            if (app.taskbarWindow) {
                app.taskbarWindow->SetDragOffset(
                    app.config->Position().offsetX, app.config->Position().offsetY);
                app.taskbarWindow->Reposition();
            }
        }
        break;
    }
    case ID_MENU_UNBIND: {
        // 保留兼容：退出程序
        int ret = ::MessageBoxW(app.hwnd,
            L"确定要退出吗？",
            L"退出", MB_YESNO | MB_ICONQUESTION);
        if (ret == IDYES) {
            // 同 ID_MENU_EXIT：当前仍在 TrackPopupMenuEx 模态循环内，
            // PostQuitMessage 发出的 WM_QUIT 会被吞掉，仅设 running=false
            app.running = false;
        }
        break;
    }
    case ID_MENU_LOCK_POS: {
        const bool newState = !app.config->Position().lockPosition;
        app.config->MutablePosition().lockPosition = newState;
        // 完全锁定隐含位置锁定，取消位置锁定时不影响完全锁定
        if (newState) {
            app.config->MutablePosition().lockFully = false;
        }
        app.config->Save();
        if (app.taskbarWindow) {
            app.taskbarWindow->SetPositionLocked(newState);
            app.taskbarWindow->SetFullyLocked(false);
        }
        if (app.tray) {
            app.tray->SetMenuCheckedLockPos(newState);
            app.tray->SetMenuCheckedLockFull(false);
        }
        break;
    }
    case ID_MENU_LOCK_FULL: {
        const bool newState = !app.config->Position().lockFully;
        app.config->MutablePosition().lockFully = newState;
        // 完全锁定隐含位置锁定
        if (newState) {
            app.config->MutablePosition().lockPosition = true;
        }
        app.config->Save();
        if (app.taskbarWindow) {
            app.taskbarWindow->SetFullyLocked(newState);
            app.taskbarWindow->SetPositionLocked(newState);
        }
        if (app.tray) {
            app.tray->SetMenuCheckedLockFull(newState);
            app.tray->SetMenuCheckedLockPos(newState);
        }
        break;
    }
    case ID_MENU_EXIT: {
        // 不能在此调用 PostQuitMessage(0)：当前处在 TrackPopupMenuEx 的
        // 模态消息循环中，WM_QUIT 会被其内部 GetMessage 吞掉，导致回到
        // 主消息循环后 GetMessage 永久阻塞，程序卡死。
        // 仅设置 running=false，由主循环的 while 条件 (app.running && GetMessage)
        // 短路跳出即可。
        app.running = false;
        break;
    }
    default:
        break;
    }
}

// 隐式消息窗口过程(用于接收托盘消息 + WM_FRAME_TICK)
LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppContext* app = reinterpret_cast<AppContext*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CLOSE:
        app->running = false;
        ::PostQuitMessage(0);
        return 0;

    case WM_TRAY_CALLBACK: { // 托盘回调
        if (app->tray) app->tray->OnTrayMessage(hwnd, wParam, lParam);
        return 0;
    }

    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        OnTrayCommand(*app, id);
        return 0;
    }

    case echo::TaskbarWindow::WM_FRAME_TICK:
    case WM_TIMER: {
        try {
            // ═══════ 帧渲染流程 ═══════
            // 1. 检测任务栏尺寸变化（含 APPBAR 自动隐藏鼠标进出检测）
            if (app->taskbarWindow) app->taskbarWindow->CheckResize();

            // 2. APPBAR 自动隐藏：窗口已隐藏时跳过渲染（避免 UpdateLayeredWindow 隐式显示导致闪烁）
            if (app->taskbarWindow && app->taskbarWindow->IsAutoHideHidden()) return 0;

            // 2.5. 全屏检测防抖：由 FullscreenDetector 独立处理
            if (app->taskbarWindow && app->config && app->config->Advanced().enableFullscreenHide) {
                bool shouldHide = false;
                if (app->taskbarWindow->Fullscreen().Update(
                        app->config->Advanced().enableFullscreenHide,
                        app->config->Advanced().debugLog,
                        shouldHide)) {
                    app->taskbarWindow->SetFullscreenHidden(shouldHide);
                    if (!shouldHide) {
                        // 退出全屏恢复时重定位，确保使用用户拖动的 dragOffset 而非清零
                        app->taskbarWindow->InvalidatePositionCache();
                        app->taskbarWindow->Reposition();
                    }
                }

                // Shell 交互即时恢复：消费 forceDebounceReset 标志
                if (app->taskbarWindow->Fullscreen().ConsumeForceDebounceReset()) {
                    if (app->config->Advanced().debugLog)
                        Log("[FullscreenDetect] debounce reset by external trigger (shell interaction)\n");
                }
            }

            // 2.6. 全屏隐藏时跳过渲染（与 APPBAR 同理）
            if (app->taskbarWindow && app->taskbarWindow->IsFullscreenHidden()) return 0;

            // 3. 从歌词解析器获取当前应渲染的状态
            if (app->parser && app->renderer) {
                auto state = app->parser->GetCurrentRenderState();

                // 3.1 集成频谱数据。只有当前整首歌被歌词数据判定为纯音乐时才显示律动。
                // WASAPI 设备切换/休眠恢复时采集线程可能退出或卡住，纯音乐状态下节流重启。
                if (app->spectrumCapture) {
                    const bool shouldUseSpectrum = state.isPlaying && state.isInstrumental;
                    if (shouldUseSpectrum &&
                        (!app->spectrumCapture->IsRunning() ||
                         app->spectrumCapture->IsStale(5000))) {
                        static ULONGLONG lastSpectrumRestartTick = 0;
                        const ULONGLONG nowTick = ::GetTickCount64();
                        if (lastSpectrumRestartTick == 0 || nowTick - lastSpectrumRestartTick >= 3000) {
                            lastSpectrumRestartTick = nowTick;
                            if (app->spectrumCapture->IsRunning()) {
                                app->spectrumCapture->Stop();
                            }
                            app->spectrumCapture->Start();
                        }
                    }
                    if (shouldUseSpectrum && app->spectrumCapture->IsRunning()) {
                        state.spectrumBands = app->spectrumCapture->GetSpectrum(SPECTRUM_NUM_BANDS);
                    }
                }

                // 3. 附加 UI 状态（悬停/拖动，用于判断是否显示控制按钮）
                if (app->taskbarWindow) {
                    state.isHovering = app->taskbarWindow->IsHovering();
                    state.isDragging = app->taskbarWindow->IsDragging();
                }

                // 4. 同步任务栏方向（运行时位置变化适配）+ 执行渲染
                app->renderer->SetVerticalTaskbar(app->taskbarWindow->IsVerticalTaskbar());
                app->renderer->Render(state);

                // 5. 更新托盘提示文本（实时显示当前歌词）
                if (app->tray && state.hasLyrics) {
                    auto tip = ToTooltipWide(state.currentLine);
                    if (!tip.empty()) {
                        app->tray->SetTooltip(tip);
                    }
                }
            }
        } catch (const std::exception& e) {
            Log("[CRASH] WM_TIMER exception: %s\n", e.what());
            // 恢复策略：尝试重置渲染器状态
            if (app->renderer) {
                try {
                    app->renderer->Shutdown();
                    if (app->taskbarWindow)
                        app->renderer->Initialize(app->taskbarWindow->GetHandle());
                    Log("[RECOVER] Renderer reset succeeded\n");
                } catch (...) {
                    Log("[FATAL] Renderer recovery failed, shutting down\n");
                    app->running = false;
                    ::PostQuitMessage(0);
                }
            }
        } catch (...) {
            Log("[CRASH] WM_TIMER unknown exception, shutting down\n");
            app->running = false;
            ::PostQuitMessage(0);
        }
        return 0;
    }

    case WM_RENDER_UPDATE: {
        try {
            // APPBAR 自动隐藏时跳过悬停重绘
            if (app->taskbarWindow && app->taskbarWindow->IsAutoHideHidden()) return 0;

            // 全屏隐藏时跳过悬停重绘
            if (app->taskbarWindow && app->taskbarWindow->IsFullscreenHidden()) return 0;

            if (app->parser && app->renderer && app->taskbarWindow) {
                auto state = app->parser->GetCurrentRenderState();
                state.isHovering = app->taskbarWindow->IsHovering();
                state.isDragging = app->taskbarWindow->IsDragging();
                app->renderer->Render(state);
            }
        } catch (...) {}
        return 0;
    }

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 注册一个不显示的 message-only 类
bool RegisterMessageClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &MsgWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"EchoTaskbarLyricsMsg";
    return ::RegisterClassExW(&wc) != 0;
}

} // namespace

// 全局异常过滤器
static LONG WINAPI GlobalExceptionHandler(EXCEPTION_POINTERS* ep) {
    Log("[CRASH] Unhandled exception code=0x%08lX at address=%p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR /*cmdLine*/, int /*nShow*/) {
    // ═══════ 第 1 阶段：系统初始化 ═══════
    // 目的：为应用提供运行时基础（COM、异常处理、日志、单实例保护）
    echo::InitLogger();
    const RuntimeOptions runtimeOptions = ParseRuntimeOptions();
    if (!runtimeOptions.authToken.empty()) {
        echo::Config::SetAuthTokenOverride(runtimeOptions.authToken);
    }
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC/Direct2D 需要 COM
    ::SetUnhandledExceptionFilter(GlobalExceptionHandler);

    // 单实例保护：避免多个进程竞争任务栏窗口导致闪烁/消息丢失
    ::CreateMutexW(nullptr, FALSE, L"EchoTaskbarLyrics_Mutex");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    Log("[STARTUP] WinMain entered (echoPluginMode=%d)\n", runtimeOptions.echoPluginMode ? 1 : 0);

    // Winsock 初始化（ixwebsocket 依赖）
    WSADATA wsaData;
    int wsRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    Log("[STARTUP] WSAStartup ret=%d\n", wsRet);

    // DPI 感知：Per-Monitor V2，支持多显示器不同缩放
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ═══════ 第 2 阶段：应用初始化 ═══════
    // 目的：加载配置、创建消息窗口和托盘图标

    if (!RegisterMessageClass(hInstance)) {
        std::fprintf(stderr, "[Error] RegisterClassExW failed\n");
        return 1;
    }

    HWND hMsgWnd = ::CreateWindowExW(
        0, L"EchoTaskbarLyricsMsg", L"",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!hMsgWnd) {
        std::fprintf(stderr, "[Error] Create message window failed\n");
        return 1;
    }

    AppContext app;
    app.hwnd   = hMsgWnd;
    app.hInstance = hInstance;
    ::SetWindowLongPtrW(hMsgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    app.config = std::make_unique<echo::Config>();
    auto& config = *app.config;
    config.Load();
    echo::SetLogEnabled(config.Advanced().debugLog);
    Log("[STARTUP] Config loaded\n");

    // EchoMusic 插件模式下由插件桥接歌词数据，不触碰 EchoMusic 配置。
    if (!runtimeOptions.echoPluginMode) {
        // 独立兼容模式下尝试开启旧 API 配置；EchoMusic 插件模式会跳过该逻辑。
        // WriteApiMode 内部已做幂等检查（apiMode='on' 时直接跳过）。
        const std::string cfgPath = echo::ApiEnabler::GetConfigPath();
        if (!cfgPath.empty()) {
            bool apiOk = echo::ApiEnabler::WriteApiMode(cfgPath);
            Log("[STARTUP] ApiEnabler::WriteApiMode %s (path=%s)\n",
                apiOk ? "OK" : "FAILED", cfgPath.c_str());
        } else {
            Log("[STARTUP] ApiEnabler::GetConfigPath returned empty, skipping\n");
        }
    }

    if (!runtimeOptions.echoPluginMode) {
        config.SetAutoStart(config.IsAutoStart());
        Log("[STARTUP] AutoStart=%s\n", config.IsAutoStart() ? "ON" : "OFF");
    }

    // ═══════ 第 3 阶段：业务模块初始化 ═══════
    // 目的：创建核心业务逻辑模块（任务栏窗口、渲染引擎、WebSocket、HTTP服务器）
    // 创建系统托盘
    app.tray = std::make_unique<echo::TrayIcon>();
    auto& tray = *app.tray;
    tray.Initialize(hInstance, hMsgWnd);
    tray.SetMenuCheckedAutoStart(config.IsAutoStart());

    // 7) 查找任务栏
    HWND hTaskbar = echo::TaskbarWindow::FindTaskbarHandle();
    Log("[STARTUP] FindTaskbar hTaskbar=%p\n", hTaskbar);
    if (!hTaskbar) {
        ::MessageBoxW(nullptr,
                      L"未找到 Windows 任务栏，请确认系统正常运行。",
                      L"Echo Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }

    // 8) 创建嵌入任务栏的歌词窗口
    app.taskbarWindow = std::make_unique<echo::TaskbarWindow>();
    auto& taskbarWindow = *app.taskbarWindow;
    if (!taskbarWindow.Create(hInstance, hTaskbar)) {
        ::MessageBoxW(nullptr,
                      L"创建任务栏歌词窗口失败。",
                      L"Echo Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }

    // 应用配置中的显示模式
    taskbarWindow.SetDisplayMode(config.Appearance().displayMode);
    taskbarWindow.SetLyricWindowWidth(config.Appearance().lyricWindowWidth);

    // 应用配置中的位置偏移
    taskbarWindow.SetDragOffset(config.Position().offsetX, config.Position().offsetY);
    taskbarWindow.Reposition();

    // 应用配置中的锁定状态
    taskbarWindow.SetPositionLocked(config.Position().lockPosition);
    taskbarWindow.SetFullyLocked(config.Position().lockFully);
    tray.SetMenuCheckedLockPos(config.Position().lockPosition);
    tray.SetMenuCheckedLockFull(config.Position().lockFully);

    // 初始隐藏窗口，收到歌词数据后再显示
    ::ShowWindow(taskbarWindow.GetHandle(), SW_HIDE);

    // 9) 初始化渲染器
    app.renderer = std::make_unique<echo::TaskbarRenderer>();
    auto& renderer = *app.renderer;
    ApplyRendererSettings(app);
    if (!renderer.Initialize(taskbarWindow.GetHandle())) {
        ::MessageBoxW(nullptr,
                      L"Direct2D 初始化失败。",
                      L"Echo Taskbar Lyrics",
                      MB_OK | MB_ICONERROR);
        tray.Shutdown();
        return 1;
    }

    // 同步任务栏方向到渲染器（纵向屏幕/垂直任务栏适配）
    renderer.SetVerticalTaskbar(taskbarWindow.IsVerticalTaskbar());
    renderer.SetDebugLog(config.Advanced().debugLog);

    // 10) 启动 WebSocket 客户端 + 歌词解析
    app.parser = std::make_unique<echo::LyricsParser>();
    auto& parser = *app.parser;

    // 10.1) 启动频谱捕获（WASAPI loopback，始终运行，开销极低）
    app.spectrumCapture = std::make_unique<echo::SpectrumCapture>();
    app.spectrumCapture->Start();

    app.wsClient = std::make_unique<echo::WebSocketClient>();
    auto& wsClient = *app.wsClient;
    wsClient.SetDebugLog(config.Advanced().debugLog);

    wsClient.OnLyrics([&](const echo::LyricsData& data) {
        if (app.config->Advanced().debugLog) Log("[WS] OnLyrics: valid=%d lines=%zu\n", data.valid, data.lines.size());
        parser.UpdateLyrics(data);
        if (app.taskbarWindow && data.valid) {
            HWND h = taskbarWindow.GetHandle();
            ::ShowWindow(h, SW_SHOWNA);
            ::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    });
    wsClient.OnPlayerState([&](const echo::PlayerState& st) {
        if (app.config->Advanced().debugLog) Log("[WS] OnPlayerState: playing=%d time=%.2f song='%s' cover='%s'\n",
            st.isPlaying, st.currentTime, st.songTitle.c_str(),
            st.coverArtUrl.empty() ? "(empty)" : st.coverArtUrl.substr(0, 60).c_str());
        parser.UpdatePlayerState(st);
    });
    bool firstConnected = true;
    wsClient.OnConnectionStatus([&](bool connected) {
        if (app.tray) {
            app.tray->SetTooltip(connected
                ? L"Echo Taskbar Lyrics (已连接)"
                : L"Echo Taskbar Lyrics (等待连接...)");
            if (connected && firstConnected) {
                firstConnected = false;
                app.tray->ShowBalloon(
                    L"Echo Taskbar Lyrics",
                    L"已连接到 EchoMusic API，歌词将在播放时自动显示");
            }
        }
    });

    // 注册按钮点击回调
    taskbarWindow.OnButtonClicked([&](echo::HoverButton btn) {
        if (runtimeOptions.echoPluginMode) {
            if (!app.httpServer) return;
            switch (btn) {
            case echo::HoverButton::Prev:
                // 私人 FM 中左侧按钮对应“不喜欢”，普通播放仍是上一首。
                app.httpServer->EnqueueControlCommand(
                    (app.parser && app.parser->GetCurrentRenderState().isPersonalFM)
                        ? "dislikeFm"
                        : "previousTrack");
                break;
            case echo::HoverButton::PlayPause:
                app.httpServer->EnqueueControlCommand("togglePlayback");
                break;
            case echo::HoverButton::Next:
                app.httpServer->EnqueueControlCommand("nextTrack");
                break;
            default:
                break;
            }
            return;
        }

        if (!app.wsClient) return;
        switch (btn) {
        case echo::HoverButton::Prev:
            app.wsClient->SendControl("prev");
            break;
        case echo::HoverButton::PlayPause:
            app.wsClient->SendControl("toggle");
            break;
        case echo::HoverButton::Next:
            app.wsClient->SendControl("next");
            break;
        default:
            break;
        }
    });

    // 注册悬停状态变化回调：保存拖动偏移并立即触发重绘。
    taskbarWindow.OnHoverChanged([&]() {
        if (app.taskbarWindow) {
            config.MutablePosition().offsetX = app.taskbarWindow->GetDragOffsetX();
            config.MutablePosition().offsetY = app.taskbarWindow->GetDragOffsetY();
            config.Save();
        }
        ::PostMessageW(hMsgWnd, WM_RENDER_UPDATE, 0, 0);
    });


    if (!runtimeOptions.echoPluginMode) {
        char url[64];
        std::snprintf(url, sizeof(url), "ws://127.0.0.1:%d",
                      config.Advanced().websocketPort);
        wsClient.Connect(url);
    }

    // 10.5) 启动 HTTP 服务器（用于 EchoMusic 插件桥接：ping / shutdown / lyrics）
    app.httpServer = std::make_unique<echo::HttpServer>();
    auto& httpServer = *app.httpServer;
    httpServer.OnCommand([&](const std::string& command) {
        Log("[HTTP] Command received: %s\n", command.c_str());
        if (command == "shutdown") {
            Log("[HTTP] Shutdown via HTTP, exiting...\n");
            app.running = false;
            ::PostQuitMessage(0);
        }
    });
    // HTTP /lyrics 端点：接收外部歌词+封面数据
    httpServer.OnLyrics([&](const std::string& jsonBody) {
        try {
            nlohmann::json j = nlohmann::json::parse(jsonBody);

            // 提取并更新播放器状态（封面 URL、歌曲名等）
            echo::PlayerState st;
            if (j.contains("isPlaying") && j["isPlaying"].is_boolean()) {
                st.isPlaying = j["isPlaying"].get<bool>();
            }
            // EchoMusic 插件桥接字段；兼容几个可能的命名，避免上游字段名调整导致失效。
            if (j.contains("isPersonalFM") && j["isPersonalFM"].is_boolean()) {
                st.isPersonalFM = j["isPersonalFM"].get<bool>();
            } else if (j.contains("isPrivateFM") && j["isPrivateFM"].is_boolean()) {
                st.isPersonalFM = j["isPrivateFM"].get<bool>();
            } else if (j.contains("personalFM") && j["personalFM"].is_boolean()) {
                st.isPersonalFM = j["personalFM"].get<bool>();
            }
            if (j.contains("currentTime") && j["currentTime"].is_number()) {
                st.currentTime = j["currentTime"].get<double>();
            }

            // 提取封面 URL（支持多种字段名）
            for (const auto& key : {"coverArtUrl", "pic", "cover", "albumArt", "image", "poster"}) {
                if (j.contains(key) && j[key].is_string()) {
                    st.coverArtUrl = j[key].get<std::string>();
                    break;
                }
            }
            // 从 currentSong 嵌套对象提取封面
            if (st.coverArtUrl.empty() && j.contains("currentSong") && j["currentSong"].is_object()) {
                const auto& cs = j["currentSong"];
                for (const auto& key : {"pic", "cover", "albumArt", "image", "poster"}) {
                    if (cs.contains(key) && cs[key].is_string()) {
                        st.coverArtUrl = cs[key].get<std::string>();
                        break;
                    }
                }
            }

            // 提取歌曲名称
            if (j.contains("songName") && j["songName"].is_string()) {
                st.songName = j["songName"].get<std::string>();
            } else if (j.contains("currentSong") && j["currentSong"].is_object() &&
                       j["currentSong"].contains("name")) {
                st.songName = j["currentSong"]["name"].get<std::string>();
            }

            parser.UpdatePlayerState(st);

            // 提取歌词数据（如果存在）
            if (j.contains("lyricsData") || j.contains("data")) {
                echo::LyricsData data;
                auto& ld = j.contains("lyricsData") ? j["lyricsData"] : j["data"];
                bool hasLyricsPayload = false;

                if (ld.is_array()) {
                    hasLyricsPayload = true;
                    for (const auto& lineJson : ld) {
                        if (data.lines.size() >= echo::constants::MAX_LYRIC_LINES) break;
                        echo::LyricLine line;
                        line.text       = lineJson.value("text", "");
                        line.translated = lineJson.value("translated", "");
                        line.startTime  = lineJson.value("startTime", static_cast<int64_t>(0));

                        if (lineJson.contains("characters") && lineJson["characters"].is_array()) {
                            for (const auto& c : lineJson["characters"]) {
                                if (line.characters.size() >= echo::constants::MAX_CHARS_PER_LINE) break;
                                echo::CharacterTiming ct;
                                ct.ch        = c.value("char", "");
                                ct.startTime = c.value("startTime", static_cast<int64_t>(0));
                                ct.endTime   = c.value("endTime",   static_cast<int64_t>(0));
                                if (!ct.ch.empty()) {
                                    line.characters.push_back(std::move(ct));
                                }
                            }
                        }
                        data.lines.push_back(std::move(line));
                    }
                } else if (ld.is_string()) {
                    hasLyricsPayload = true;
                    // KRC 字符串格式：使用公共解析方法
                    data = echo::WebSocketClient::ParseKrcString(ld.get<std::string>());
                }

                data.valid = !data.lines.empty();
                if (hasLyricsPayload) {
                    parser.UpdateLyrics(data);
                }
                if (hasLyricsPayload && (data.valid || st.isPlaying) && app.taskbarWindow) {
                    HWND h = taskbarWindow.GetHandle();
                    ::ShowWindow(h, SW_SHOWNA);
                    ::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                }
            }

            Log("[HTTP] Lyrics processed: cover='%s' song='%s'\n",
                st.coverArtUrl.c_str(), st.songName.c_str());
        } catch (const std::exception& e) {
            Log("[HTTP] Failed to parse lyrics JSON: %s\n", e.what());
        } catch (...) {
            Log("[HTTP] Failed to parse lyrics JSON: unknown error\n");
        }
    });
    // HTTP 端口从 config 读取（默认 6523）；异常时退回到 constants.h 中的默认值。
    const int httpPort = runtimeOptions.httpPortOverride > 0
                             ? runtimeOptions.httpPortOverride
                             : ((config.Advanced().httpServerPort > 0)
                                    ? config.Advanced().httpServerPort
                                    : static_cast<int>(echo::constants::HTTP_SERVER_PORT));
    if (httpServer.Start(httpPort)) {
        Log("[STARTUP] HTTP server started on port %d\n", httpPort);
    } else {
        Log("[STARTUP] HTTP server failed to start on port %d (non-fatal)\n", httpPort);
    }

    if (!runtimeOptions.echoPluginMode) {
        // 11) 启动 Native Host stdin 读取（旧托管模式下的生命周期管理）
        // 在后台线程中读取 stdin JSON Lines，收到 shutdown 时通知主线程退出
        app.nativeHost = std::make_unique<echo::NativeMessagingHost>();
        auto& nativeHost = *app.nativeHost;
        nativeHost.SetMessageHandler([&app](const echo::NativeHostMessage& msg) {
            Log("[NATIVE-HOST] Received message: type=%s\n", msg.type.c_str());
            // 业务消息处理：可根据 payload 中的 action 执行对应操作
            if (msg.payload.contains("action")) {
                std::string action = msg.payload["action"].get<std::string>();
                Log("[NATIVE-HOST] Action: %s\n", action.c_str());

                if (action == "enableApiMode") {
                    // 旧托管消息触发：写入 config.json 使主进程启动 WebSocket
                    const std::string cfgPath = echo::ApiEnabler::GetConfigPath();
                    const bool ok = cfgPath.empty() ? false : echo::ApiEnabler::WriteApiMode(cfgPath);
                    Log("[NATIVE-HOST] enableApiMode: %s (path=%s)\n",
                        ok ? "success" : "failed", cfgPath.c_str());

                    nlohmann::json response;
                    response["action"] = "enableApiMode";
                    response["result"] = ok ? "ok" : "fail";
                    response["path"] = cfgPath;
                    app.nativeHost->SendPayloadEvent(response);
                }
                // 未来可扩展: set-config, get-status 等
            }
        });
        // 在独立线程中运行 stdin 循环（阻塞式 getline）
        std::thread stdinThread([&nativeHost, &app]() {
            Log("[NATIVE-HOST] Stdin reader thread started (managed=%d)\n",
                !nativeHost.IsShutdown());
            bool result = nativeHost.Run();
            if (!result) {
                // 收到 shutdown 指令或读取错误 → 通知主线程退出
                Log("[NATIVE-HOST] Run() returned false, requesting shutdown\n");
                app.running = false;
                ::PostQuitMessage(0);
            } else {
                // stdin EOF（独立运行模式，无托管者）→ 继续运行
                Log("[NATIVE-HOST] Run() returned true (standalone mode, continuing)\n");
            }
        });
        stdinThread.detach();
        Log("[STARTUP] Native Host stdin reader started\n");
    }

    // 启动帧定时器
    const int intervalMs = std::max(MIN_FRAME_INTERVAL_MS, 1000 / std::max(1, config.Advanced().refreshRateHz));
    ::SetTimer(hMsgWnd, /*id*/1, static_cast<UINT>(intervalMs), nullptr);

    // ═══════ 第 4 阶段：业务逻辑循环 ═══════
    // 目的：处理消息和事件，直到应用关闭
    MSG msg{};
    while (app.running && ::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    Log("[SHUTDOWN] Message loop ended\n");

    // ═══════ 第 5 阶段：清理和退出 ═══════
    // 目的：释放所有资源，进行优雅关闭
    // 注意：顺序与初始化相反（后创建的先销毁）

    // 退出前保存最终位置偏移（避免拖动后未触发 hover 变化导致位置丢失）
    if (app.taskbarWindow) {
        config.MutablePosition().offsetX = app.taskbarWindow->GetDragOffsetX();
        config.MutablePosition().offsetY = app.taskbarWindow->GetDragOffsetY();
    }
    config.Save();

    ::KillTimer(hMsgWnd, 1);
    if (app.spectrumCapture) {
        app.spectrumCapture->Stop();
    }
    httpServer.Stop();
    wsClient.Disconnect();
    renderer.Shutdown();
    taskbarWindow.Destroy();
    tray.Shutdown();
    ::DestroyWindow(hMsgWnd);

    Log("[SHUTDOWN] Complete\n");
    return 0;
}
