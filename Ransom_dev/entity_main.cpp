// ============================================================================
//  entity_main.cpp
//
//  Ransom（DOORS）桌面版 —— 进程入口。
//
//  模块：
//    face            实体本体：程序生成的像素化单色扭曲脸 + 停牌
//    motion          移动检测（Ransom 的核心机制：不许动）
//    director        遭遇战调度器
//    desktop_overlay 桌面快捷方式覆盖层（被抓后标记为「已加密」）
//    fx              屏幕特效（红噪 / 闪屏）
//    audio           合成音效
//    entity_log      公共日志
//
//  两条铁律：
//    1. Windows 子系统，wWinMain 是唯一入口。
//    2. 强制退出热键 Ctrl+Alt+Shift+Q **始终有效**，并且被**
//       移动检测永久豁免**——否则玩家按 Ctrl 的那一下就会立刻被抓，
//       安全阀永远按不完。
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>          // SetCurrentProcessExplicitAppUserModelID

// GDI+ 需要 IStream / PROPID，WIN32_LEAN_AND_MEAN 不会带进来
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <cstdlib>
#include <cwchar>
#include <string>

#include "assets.h"
#include "aero_window.h"
#include "ui_layout.h"
#include "desktop_overlay.h"
#include "guardian.h"
#include "lockdown.h"
#include "audio.h"
#include "director.h"
#include "entity_log.h"
#include "face.h"
#include "fx.h"
#include "gold.h"
#include "motion.h"
#include "recycle.h"
#include "settings.h"
#include "setup_ui.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

namespace {

const wchar_t* kEntityName = L"Ransom_dev";

// 安全阀
const int  kHotkeyPanic = 1;
const UINT kPanicMods   = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
const UINT kPanicVK     = 'Q';

// 用于收热键和跨进程消息的隐藏窗口
const wchar_t* kIpcClass = L"RansomDevIpcWnd";

volatile bool g_running = true;

// 跨进程消息：金币 .lnk 被双击时，由新起的进程发过来
UINT g_msgPay = 0;

// ---------------------------------------------------------------------------
//  付款模式
//
//  被双击的金币 .lnk 会以 `--pay <金额> --token <编号>` 拉起本程序的一份新实例。
//  这个实例**必须立刻发消息然后退出**——绝不能跑下去变成第二个实体。
//  所以它在任何模块启动之前就返回了。
// ---------------------------------------------------------------------------
int SendPayment(int amount, int token)
{
    const UINT msg = RegisterWindowMessageW(L"RansomDev_PayGold");
    if (!msg) return 2;

    HWND h = FindWindowW(L"RansomDevIpcWnd", nullptr);
    if (!h || !IsWindow(h)) return 3;      // 实体没在运行，这次点击作废

    // PostMessage 跨进程只传两个整数，不需要任何内存封送
    PostMessageW(h, msg, (WPARAM)amount, (LPARAM)token);
    return 0;
}

LRESULT CALLBACK IpcProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 开场设置 / 应急提示那两屏自带模态循环，主循环里那一段解释走不到，
    // 而 WM_HOTKEY 又是投给**注册它的窗口**（就是这个 IPC 窗口）的。
    // 所以这里先把热键转给正在开着的设置界面；没人接再往下走各自的处理。
    if (msg == WM_HOTKEY && wp == kHotkeyPanic)
    {
        if (setup_ui::DispatchHotkey()) return 0;
    }

    // 守护进程心跳：定期检查守护还在不在，不在就重启一个。
    if (msg == WM_TIMER && wp == 2)
    {
        guardian::Tick();
        return 0;
    }

    if (msg == g_msgPay && g_msgPay != 0)
    {
        const int amount = (int)wp;
        const int token  = (int)lp;

        elog::Write(L"[ipc] 收到付款请求 %d Gold（token=%d）", amount, token);

        // 只有真的找到并删掉了对应的金币文件才计入——防止重复点击刷金额。
        // fakeMask 回传这枚币是不是假的、以及哪一段被污染过。
        int fakeMask = 0;
        const int got = gold::Consume(token, &fakeMask);
        if (got > 0)
        {
            if (fakeMask == 0)
            {
                director::CreditGold(got);
                audio::PlayCoin();          // 拾取音
            }
            else
            {
                // 假金币**一分钱都不进账**，只按污染的部位吃惩罚。
                // bit0（前缀 "Gold" 被污染）-> 倒计时扣 面额 × 0.1 秒
                // bit1（面额数字被污染）    -> 未付的赎金 += 面额
                // 两个都中就是两种同时生效（mask = 3）。
                if (fakeMask & 1) director::PenalizeTime((DWORD)got * 100);
                if (fakeMask & 2) director::PenalizeGoal(got);

                audio::PlayError();         // 用 UI 错误音，一听就知道吃瘪了
            }
        }

        return 0;
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ------------------------------------------------------------------ 入口 ----
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // ============================================================
    //  任务栏归组：给本进程一个显式的 AppUserModelID
    // ============================================================
    //
    // 不加这一句的话，Windows 的任务栏会自己推断这个进程属于哪个"应用"。
    // 它的启发式规则在本项目这套窗口结构下会算歪：
    //
    //   * 主勒索窗口带 WS_EX_TOOLWINDOW，不进任务栏 —— 它没资格当分组锚点；
    //   * 设置窗口 / 应急提示 / 弹窗子窗口 带 WS_EX_APPWINDOW，会出现在
    //     任务栏里 —— 但它们**创建得很晚**，任务栏会用它们创建那一刻的
    //     状态去推断 AppUserModelID。
    //
    // 结果是：硬核模式下大量生成的光标阻挡弹窗 + 后续随机子窗口被
    // 任务栏当成和主程序无关的"另一个应用"，表现为两个图标。
    //
    // 显式指定一个固定 ID 之后，本进程的所有顶层窗口都会被归到同一组。
    //
    // 这个调用是**进程级**的，而且必须在任何 CreateWindowEx 之前完成 ——
    // 任务栏一旦根据第一个窗口定完分组，之后再设就没用了。
    // 对守护进程也生效（同一个 exe），所以它跑惩罚演出时开的窗口
    // 也会被归到同一组。
    ::SetCurrentProcessExplicitAppUserModelID(L"RansomDev.DesktopEntity");

    // ---- 守护模式：直接进守护循环，不碰主流程 ----
    //
    // 守护进程是同一个 exe 拉起来的自己，只是带了 --guardian <pid> <gen>。
    // 这一段必须在**最前面**：守护进程不该碰高 DPI 设置、命令行解析、
    // 模块启动……它只需要监护主进程，然后在需要时跑一次惩罚演出。
    if (guardian::IsGuardianMode())
    {
        DWORD targetPid = 0;
        int   gen = 0;
        if (!guardian::ParseGuardianArgs(__argc, __wargv, targetPid, gen) ||
            targetPid == 0)
        {
            // 参数坏了（不该发生），直接退，别把守护进程留成孤儿
            return 2;
        }
        return guardian::RunGuardian(hInst, targetPid, gen);
    }

    // ---- 高 DPI ----
    // ---- 高 DPI ----
    {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32)
        {
            PFN_SetProcessDpiAwarenessContext fn =
                (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
            if (!fn || !fn((HANDLE)-4))
            {
                typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
                PFN_SetProcessDPIAware fn2 =
                    (PFN_SetProcessDPIAware)GetProcAddress(u32, "SetProcessDPIAware");
                if (fn2) fn2();
            }
        }
    }

    // ---- 命令行 ----
    //   --phase NAME|N         跳到某阶段并停住
    //   --no-auto              不自动推进
    //   --no-audio             不启动声音
    //   --no-overlay           不启动桌面覆盖层
    //   --overlay-topmost      覆盖层置顶（默认贴桌面层）
    //   --no-block-menu        不拦截被加密图标上的右键菜单（排查问题时用）
    //   --no-lockdown          勒索时不最小化别的程序（排查问题时用）
    //   --no-guardian          不启动双进程看守（调试时用；有调试器时也会自动跳过）
    //   --no-setup             不弹启动设置和应急提示，直接用 ini 里的值开演
    //   --tolerance N          鼠标容差像素（默认 10）
    //   --face-demo MODE       只显示某张脸：idle/stop/attack/thanks/loading
    //   --face-dump DIR        把程序生成的素材导出成 PNG 后退出（验证外观用）
    //   --clean-gold           只扫描并删除遗留的金币快捷方式，然后退出
    //   --restore              从回收站按清单还原被没收的快捷方式，然后退出
    //   --audio-dump PATH      把全部音效离线渲染成 WAV 后退出（验证波形用）
    //   --theme-dump PATH      把处理后的主题曲导成 WAV 后退出（试听用）
    //   --ui-preview PATH      把勒索主窗口排版渲染成 PNG 后退出（调排版用）
    //   --ui-grid              配合 --ui-preview，预览图叠坐标网格
    //   --setup-ui PATH        把启动设置窗口渲染成 PNG 后退出（调排版用）
    //   --notice-ui PATH       把应急提示窗口渲染成 PNG 后退出
    //   --setup-grid           配合上面两个，预览图叠坐标网格
    //   --fx-demo NAME PATH    设置 fx 图层状态并把它单独导成 PNG 后退出。
    //                          NAME：glow（四角红光）/ black（黑幕+雪花）/ stop（亮红幕）
    //   --image-dir DIR        覆盖图片素材目录（默认自动找 assets\image）
    //   --audio-dir DIR        覆盖音频素材目录（默认自动找 assets\audio）
    //   --diag PATH            写诊断日志
    bool           noAuto      = false;
    bool           noAudio     = false;
    bool           noOverlay   = false;
    bool           overlayTop  = false;
    bool           noBlockMenu = false;
    bool           noLockdown = false;
    bool           noGuardian = false;    
    bool           noSetup    = false;
    const wchar_t* startPhase  = nullptr;
    const wchar_t* faceDemo    = nullptr;
    const wchar_t* faceDump    = nullptr;
    const wchar_t* diagPath    = nullptr;
    int            tolerance   = 20;
    int            payAmount   = 0;
    int            payToken    = 0;
    bool           payMode     = false;
    bool           cleanGold   = false;
    bool           doRestore   = false;
    const wchar_t* audioDump   = nullptr;
    const wchar_t* themeDump   = nullptr;
    const wchar_t* uiPreview   = nullptr;
    bool           uiPreviewGrid = false;
    bool           uiPreviewPayup = false;
    const wchar_t* setupUi     = nullptr;
    const wchar_t* noticeUi    = nullptr;
    bool           setupUiGrid = false;
    const wchar_t* fxDemo      = nullptr;
    const wchar_t* fxDump      = nullptr;
    const wchar_t* audioDir    = nullptr;
    const wchar_t* imageDir    = nullptr;

    for (int i = 1; i < __argc; ++i)
    {
        const wchar_t* a = __wargv[i];
        const bool hasNext = (i + 1 < __argc);

        if (_wcsicmp(a, L"--no-auto") == 0)           noAuto     = true;
        else if (_wcsicmp(a, L"--no-audio") == 0)     noAudio    = true;
        else if (_wcsicmp(a, L"--no-overlay") == 0)   noOverlay  = true;
        else if (_wcsicmp(a, L"--overlay-topmost") == 0) overlayTop = true;
        else if (_wcsicmp(a, L"--no-block-menu") == 0)   noBlockMenu = true;        
        else if (_wcsicmp(a, L"--no-lockdown") == 0)     noLockdown = true;
        else if (_wcsicmp(a, L"--no-guardian") == 0)     noGuardian = true;
        else if (_wcsicmp(a, L"--no-setup") == 0)        noSetup    = true;
        else if (_wcsicmp(a, L"--phase")      == 0 && hasNext) startPhase = __wargv[++i];
        else if (_wcsicmp(a, L"--face-demo")  == 0 && hasNext) faceDemo   = __wargv[++i];
        else if (_wcsicmp(a, L"--face-dump")  == 0 && hasNext) faceDump   = __wargv[++i];
        else if (_wcsicmp(a, L"--diag")       == 0 && hasNext) diagPath   = __wargv[++i];
        else if (_wcsicmp(a, L"--tolerance")  == 0 && hasNext) tolerance  = _wtoi(__wargv[++i]);
        else if (_wcsicmp(a, L"--pay")        == 0 && hasNext) { payAmount = _wtoi(__wargv[++i]); payMode = true; }
        else if (_wcsicmp(a, L"--token")      == 0 && hasNext) payToken   = _wtoi(__wargv[++i]);
        else if (_wcsicmp(a, L"--clean-gold") == 0)            cleanGold  = true;
        else if (_wcsicmp(a, L"--restore")    == 0)            doRestore  = true;
        else if (_wcsicmp(a, L"--audio-dump") == 0 && hasNext) audioDump  = __wargv[++i];
        else if (_wcsicmp(a, L"--theme-dump") == 0 && hasNext) themeDump  = __wargv[++i];
        else if (_wcsicmp(a, L"--ui-preview") == 0 && hasNext) uiPreview = __wargv[++i];
        else if (_wcsicmp(a, L"--ui-grid") == 0)               uiPreviewGrid = true;
        else if (_wcsicmp(a, L"--payup") == 0)                 uiPreviewPayup = true;
        else if (_wcsicmp(a, L"--setup-ui")   == 0 && hasNext) setupUi = __wargv[++i];
        else if (_wcsicmp(a, L"--notice-ui")  == 0 && hasNext) noticeUi = __wargv[++i];
        else if (_wcsicmp(a, L"--setup-grid") == 0)            setupUiGrid = true;
        else if (_wcsicmp(a, L"--fx-demo")    == 0 && hasNext) fxDemo = __wargv[++i];
        else if (_wcsicmp(a, L"--fx-dump")    == 0 && hasNext) fxDump = __wargv[++i];
        else if (_wcsicmp(a, L"--audio-dir")  == 0 && hasNext) { audioDir = __wargv[++i]; assets::UseDiskDir(assets::KIND_AUDIO, audioDir); }
        else if (_wcsicmp(a, L"--image-dir")  == 0 && hasNext) { imageDir = __wargv[++i]; assets::UseDiskDir(assets::KIND_IMAGE, imageDir); }
    }

    // ---- 付款模式：发完消息立刻退出，不启动任何模块 ----
    if (payMode)
    {
        const int rc = SendPayment(payAmount, payToken);
        return rc;
    }

    elog::Open(diagPath);
    elog::Write(L"===== Ransom_dev 启动 =====");

    // ---- 设置：尽早读回来 ----
    //
    // 必须排在下面前面那几个"看一眼就退出"的开发用模式之前
    // （--clean-gold / --audio-dump / --theme-dump / --restore，见下面几段）。
    // 它们是会读设置的：尤其是 --theme-dump，要按硬核开关决定导哪一轨主题曲。
    // 读晚了那些路径里拿到的全是默认值，日志和导出的东西都会误导人
    // （实测过一次：硬核开着导出来的还是原版那一轨）。
    //
    // 真正把设置**推给子系统**（音量 + 光敏安全）仍要等 GDI+ 起来之后
    // （fx::SetPhotosensitiveSafe 会碰覆盖层），所以文件后面还有一次 Apply。
    settings::Load();

    // ---- 手动清理模式：扫掉所有遗留的金币快捷方式然后退出 ----
    if (cleanGold)
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        gold::Start(hInst);
        const int n = gold::CleanupStale();
        gold::Stop();
        if (SUCCEEDED(hr)) CoUninitialize();
        elog::Write(L"清理模式：删除 %d 个遗留金币", n);
        elog::Close();
        return 0;
    }

    // ---- 音效导出模式：离线渲染成 WAV 然后退出 ----
    if (audioDump)
    {
        if (!audio::Start()) { elog::Write(L"音频设备不可用，无法导出"); elog::Close(); return 1; }
        const bool ok = audio::DumpMix(audioDump, 10);
        audio::Stop();
        elog::Write(L"音效导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }

    // ---- 主题曲导出：把处理后的主题曲导成 WAV 后退出（试听用）----
    // 不需要音频设备，Start 失败（没有声卡）也照样能导。
    if (themeDump)
    {
        audio::Start();
        const bool ok = audio::DumpTheme(themeDump);
        audio::Stop();
        elog::Write(L"主题曲导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }

    // ---- 还原模式：按清单把被没收的快捷方式从回收站放回原位 ----
    if (doRestore)
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        recycle::Start(hInst);
        const int n = recycle::Restore();
        if (SUCCEEDED(hr)) CoUninitialize();
        elog::Write(L"还原模式：放回 %d 个", n);
        elog::Close();
        return 0;
    }

    // ---- 设置已经在文件上方读过了 ----
    //
    // 那里的注释解释了为什么它必须排在 --audio-dump / --theme-dump 这些
    // "看一眼就退出"的模式之前（它们会读设置）。
    // 把设置**推给子系统**（音量 + 光敏安全）仍然在下面主流程里做
    // （搜 settings::Apply），要等 GDI+ 和各个模块起来之后才行，
    // 因为 fx::SetPhotosensitiveSafe 会碰覆盖层。

    // ---- GDI+ ----
    // 进程级只需初始化一次。face / fx / overlay 都靠它，
    // 漏了这一步所有 GDI+ 调用都会卡死（不是报错，是挂住）。
    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    if (GdiplusStartup(&gdipToken, &gsi, nullptr) != Gdiplus::Ok)
    {
        elog::Write(L"GDI+ 初始化失败");
        elog::Close();
        return 1;
    }

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 主窗口排版预览：渲染成 PNG 后退出（调排版用，不必跑整场演出）----
    // 加 --ui-grid 会叠一层 20px 网格 + 每 100px 的坐标标注，方便读坐标。
    // 放在这里是因为它需要 GDI+ 已经初始化，但不需要任何窗口/模块。
    if (uiPreview)
    {
        ui_layout::Load(L"main_window.ini", L"payup.ini");
        if (uiPreviewPayup) ui_layout::SetActive(1);

        // 造一组示例数值，让 {left} / {time} 这些占位符有东西可显示
        ui_layout::Status st;
        st.gold     = 375;
        st.goal     = 500;
        st.remainMs = 83'000;

        const bool ok = ui_layout::Preview(uiPreview, st, uiPreviewGrid);
        ui_layout::Shutdown();
        elog::Write(L"排版预览结束，成功=%d", (int)ok);
        elog::Close();
        GdiplusShutdown(gdipToken);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return ok ? 0 : 1;
    }

    // ---- 开场两屏的排版预览：渲染成 PNG 后退出 ----
    // 和 --ui-preview 同一个用途：这两屏的坐标全是手算的，盖在桌面上
    // 截屏会被壁纸和别的窗口污染，只有单独导出一张才能按像素核对。
    // --setup-ui 会拿一组"极端值"当样例，好把「超出 100% 变红」「区间两端
    // 分开」这些状态也一并画出来。
    if (setupUi || noticeUi)
    {
        bool ok = true;

        if (setupUi)
        {
            settings::Set demo;
            demo.bgmVol = 145;
            demo.sfxVol = 100;
            demo.minMs = 800;
            demo.maxMs = 3600;
            demo.photosensitiveSafe = true;

            // 硬核开着的那一套：赎金滑块的可选区间整段上移（1000-9999）、
            // 关窗惩罚滑条到 30 秒上限、那四条假金币滑条解锁。
            // 想看普通态的排版就把 hardcore 改成 false。
            demo.hardcore = true;
            demo.goldGoal = 5000;             // 硬核段的中点附近
            demo.childCloseMs = 30000;        // 顶到硬核上限
            demo.fakePercent = 35;
            demo.fakePrefixPct = 40;
            demo.fakeSuffixPct = 40;
            demo.fakeBothPct = 20;
            ok = setup_ui::DumpSettingsPreview(setupUi, demo, setupUiGrid) && ok;
        }
        if (noticeUi)
        {
            // 同样按模式分流：硬核开着就导硬核那一屏（和实际演出会弹的一致）
            ok = (settings::Hardcore()
                    ? setup_ui::DumpHardcoreNoticePreview(noticeUi, setupUiGrid)
                    : setup_ui::DumpNoticePreview(noticeUi, setupUiGrid)) && ok;
        }

        elog::Write(L"开场界面预览结束，成功=%d", (int)ok);
        elog::Close();
        GdiplusShutdown(gdipToken);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return ok ? 0 : 1;
    }

    // ---- 隐藏的 IPC / 热键窗口 ----
    WNDCLASSEXW ic = { sizeof(WNDCLASSEXW) };
    ic.lpfnWndProc   = IpcProc;
    ic.hInstance     = hInst;
    ic.hCursor       = nullptr;
    ic.hbrBackground = nullptr;
    ic.lpszClassName = kIpcClass;
    RegisterClassExW(&ic);

    HWND hIpc = CreateWindowExW(0, kIpcClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hIpc)
    {
        elog::Write(L"IPC 窗口创建失败, err=%lu", GetLastError());
        if (SUCCEEDED(hrCom)) CoUninitialize();
        elog::Close();
        return 1;
    }

    g_msgPay = RegisterWindowMessageW(L"RansomDev_PayGold");

    // ---- 安全阀热键 ----
    bool panicHotkeyOk = false;
    if (!RegisterHotKey(hIpc, kHotkeyPanic, kPanicMods, kPanicVK))
    {
        elog::Write(L"!! 安全阀热键注册失败, err=%lu", GetLastError());
        MessageBoxW(nullptr,
                    L"强制退出热键 Ctrl+Alt+Shift+Q 注册失败（可能被其它程序占用）。\n\n"
                    L"请关掉占用者后重开。",
                    kEntityName, MB_ICONWARNING | MB_OK);
    }
    else
    {
        panicHotkeyOk = true;
        elog::Write(L"安全阀热键已注册: Ctrl+Alt+Shift+Q（已从移动判定中豁免）");
    }

    // ---- UI 外观资源：图标 + 字体 ----
    // 两样都**内嵌在 exe 里**（图标是 Ransom_dev.rc 的图标资源，
    // 字体是 assets_gen.rc 里的 RODATA）。
    // 字体注册失败就保持默认的 Microsoft YaHei，不会崩。
    // 必须在第一个 aero 窗口创建**之前**设好——窗口类只注册一次，
    // 类的 hIcon 就是在那一刻定下来的。
    aero::SetAppIconFromSelf();
    {
        assets::Blob fnt;
        if (assets::Get(assets::KIND_ROOT, L"RobotoMono-VariableFont_wght.ttf", fnt))
            aero::SetTitleFontFromMemory(fnt.Data(), fnt.Size(), L"Roboto Mono");
        else
            elog::Write(L"字体素材不在包里，标题栏回退 Microsoft YaHei");
    }

    // ---- 启动设置 + 应急提示 ----
    //
    //  顺序（都在主线程上阻塞跑，这期间演出还没开始）：
    //    1. settings::Load()  把 ini 读回来（音频 / 光敏 / 随机间隔的取值来源）
    //    2. 起音频            设置界面拖滑条要能当场听见
    //    3. 设置窗口          改的是内存里那份，点「开始」才落盘
    //    4. 应急提示          讲清楚会动什么、以及怎么喊停
    //    5. 之后才轮到 director / 覆盖层 / 守护进程
    //
    //  audio 提前起还有一个副作用要注意：这里是唯一一次 Start()。
    //  后面那段 `if (!noAudio) audio::Start()` 已经是幂等的（见 audio.cpp），
    //  不会把素材重载一遍。
    //
    //  --no-setup / --face-demo / 各种 dump 模式都跳过这两个窗口：
    //  它们要么是给自动化用的，要么是开发时看单帧的，不该被弹窗挡住。
    {
        const bool wantSetup = !noSetup && !faceDemo && !faceDump &&
                               !fxDump && !audioDump && !themeDump &&
                               !setupUi && !noticeUi;

        if (!noAudio) audio::Start();      // 失败不致命，设置界面静默无声音

        if (wantSetup)
        {
            // ---- 设置 <-> 二级警告 的小循环 ----
            //
            // 二级警告屏上有个「← 返回上一级」：点了就回到设置界面重来。
            // 所以这里必须是个循环，不能只弹一次 —— 否则返回按钮就成了摆设。
            // 退出循环只有两条路：设置里点「开始」并过了警告屏，任何一步中止。
            for (;;)
            {
                const setup_ui::Verdict v =
                    setup_ui::ShowSettings(hInst, kPanicVK);

                if (v == setup_ui::VERDICT_ABORT)
                {
                    elog::Write(L"[main] 用户在设置窗口里取消了，不演出");
                    audio::Stop();
                    UnregisterHotKey(hIpc, kHotkeyPanic);
                    DestroyWindow(hIpc);
                    if (SUCCEEDED(hrCom)) CoUninitialize();
                    GdiplusShutdown(gdipToken);
                    elog::Close();
                    return 0;
                }

                // 热键没注册成功的话，警告屏里那句「按这个键立刻停」
                // 就是假的。宁可少弹一屏，也不给一个空头承诺。
                if (!panicHotkeyOk)
                {
                    elog::Write(L"[main] 安全阀热键不可用，跳过警告屏（免得承诺一个假快捷键）");
                    break;
                }

                // 这一屏按模式分流：硬核走**专用警告**（内容、配色、按钮文案都不一样），
                // 普通模式走原来那一屏。两屏共用同一套窗口实现，只是 g_mode 不同。
                // 注意模式要在**每次**弹之前重新读 —— 用户可能刚在设置里把它翻掉。
                const bool hc = settings::Hardcore();

                const setup_ui::Verdict v2 = hc
                    ? setup_ui::ShowHardcoreNotice(hInst, kPanicVK)
                    : setup_ui::ShowSafetyNotice(hInst, kPanicVK);

                if (v2 == setup_ui::VERDICT_ABORT)
                {
                    elog::Write(L"[main] 用户在%s里退出了，不演出",
                        hc ? L"硬核模式警告" : L"应急提示");
                    audio::Stop();
                    UnregisterHotKey(hIpc, kHotkeyPanic);
                    DestroyWindow(hIpc);
                    if (SUCCEEDED(hrCom)) CoUninitialize();
                    GdiplusShutdown(gdipToken);
                    elog::Close();
                    return 0;
                }

                if (v2 == setup_ui::VERDICT_BACK)
                {
                    elog::Write(L"[main] 用户点了「返回上一级」，回到设置界面");
                    continue;                 // 重新弹设置窗口
                }

                break;                        // 确认了，继续往下走
            }

            // 设置窗口里改过的值在这里正式生效（光敏安全 + 音量）。
            // 单推一次就够：窗口自己每改一次也推过，这里是对显式落盘之后的兜底。
            settings::Apply();
            elog::Write(L"[main] 设置确认，开始演出");
        }
        else
        {
            settings::Apply();
        }
    }

    // ---- 各模块启动 ----
    face::Start(hInst);

    // ---- 只导出素材然后退出（开发时用肉眼检查脸长什么样）----
    if (faceDump)
    {
        const bool ok = face::DumpAssets(faceDump);
        face::Stop();
        UnregisterHotKey(hIpc, kHotkeyPanic);
        DestroyWindow(hIpc);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        GdiplusShutdown(gdipToken);
        elog::Write(L"素材导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }

    motion::SetTolerance(tolerance);
    motion::Start(hInst, kPanicVK, true, true, true);
    fx::Start(hInst);
    gold::Start(hInst);
    gold::CleanupStale();          // 清掉上次被强杀留下的残渣
    if (!noAudio) audio::Start();   // 素材已内嵌；解码失败不致命

    const bool overlayOn = !noOverlay && overlay::Start(hInst, overlayTop);
    if (overlayOn)
    {
        overlay::SetVisible(false);      // 只有被抓时才亮出来
        // 被加密的图标上不给右键菜单（默认开，见 desktop_overlay.h）
        if (noBlockMenu) overlay::SetBlockContextMenu(false);
    }
    // 勒索时的桌面清场（默认开，见 lockdown.h）
    if (noLockdown) lockdown::SetEnabled(false);

    // ---- 双进程看守 ----
    // 每次正常启动都清一下「上一轮惩罚已经跑过」的标记，免得上一轮
    // 遗留的标记把这一轮的惩罚吞掉。
    guardian::ClearPunishMark();

    // 调试器下**不启动**守护进程。
    //
    // 理由：VS 的"停止调试"（Shift+F5）、"停止"工具栏按钮，走的是
    // TerminateProcess，**绕过一切清理代码** —— 包括 Disarm。守护这边
    // 看到的就变成"进程消失但 Disarm 没 signal"，等同于被强杀，
    // 于是立即跑惩罚演出（jumpscare + 桌面快捷方式进回收站）。
    // 用调试器开发时几乎每次退出都会误触发。
    //
    // IsDebuggerPresent() 只对"进程正在被调试"返回真 —— 普通用户用
    // 任务管理器杀进程走不到这条分支（他们没挂调试器），所以这条
    // 跳过逻辑**不会削弱对真实强杀的防护**。
    if (noGuardian)
    {
        elog::Write(L"[main] --no-guardian：跳过守护进程启动");
    }
    else if (IsDebuggerPresent())
    {
        elog::Write(L"[main] 检测到调试器，跳过守护进程启动"
            L"（避免 Shift+F5 等调试退出被误判成强杀）");
    }
    else
    {
        guardian::Start(hInst);

        // 主消息循环里定期检查守护进程还在不在，用 Ipc 窗口的定时器挂上。
        // id 用 2（1 是给将来的用途预留的），5000ms 一次足够 —— guardian::Tick
        // 里自己限流到 1000ms，这里只是提供一个心跳源。
        SetTimer(hIpc, 2, 5000, nullptr);
    }

    // ---- fx 图层导出（开发时看效果用）----
    // fx 是全屏置顶的分层窗口，截屏会被底下的桌面内容污染，
    // 只有把这一层单独导成 PNG 才能按像素数清红光范围和彩色噪点。
    if (fxDump)
    {
        if (fxDemo && _wcsicmp(fxDemo, L"glow") == 0)
        {
            fx::SetEdgeGlow(true, RGB(210, 16, 16), 170);
        }
        else if (fxDemo && _wcsicmp(fxDemo, L"black") == 0)
        {
            fx::SetSolid(true, RGB(0, 0, 0));
            fx::SetNoise(80);
        }
        else if (fxDemo && _wcsicmp(fxDemo, L"stop") == 0)
        {
            fx::SetSolid(true, RGB(170, 0, 0));
        }

        // 第一次 Render 是窗口创建后立刻做的，等一小会儿让图层就位
        Sleep(120);
        const bool ok = fx::DumpLayer(fxDump);
        elog::Write(L"fx 图层导出结束（%s），成功=%d", fxDemo ? fxDemo : L"?", (int)ok);

        fx::ClearAll();
        fx::Stop();
        face::Stop();
        motion::Stop();
        gold::Stop();
        ui_layout::Shutdown();
        audio::Stop();
        if (overlayOn) overlay::Stop();

        // 这条路径走的是"提前返回"，**不经过主消息循环末尾那段收尾代码**，
        // 所以必须在这里自己 Disarm —— 否则守护进程会以为主进程是被强杀的，
        // 立刻跑惩罚演出。
        guardian::Disarm();
        guardian::Stop();

        elog::Close();
        GdiplusShutdown(gdipToken);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return ok ? 0 : 1;
    }

    // ---- 只显示某张脸（开发时看效果用）----
    if (faceDemo)
    {
        elog::Write(L"面部演示模式: %s", faceDemo);
        if      (_wcsicmp(faceDemo, L"idle")   == 0) face::SpawnIdle(0);
        else if (_wcsicmp(faceDemo, L"stop")   == 0) face::ShowStopSign(0);
        else if (_wcsicmp(faceDemo, L"attack") == 0) face::ShowAttack(0);
        else if (_wcsicmp(faceDemo, L"thanks") == 0) face::ShowThanks(0);
        // 加载画面演示：故意把走条拉到 4 秒，方便截图看进度曲线、
        // 抖动幅度和条宽（正常演出里这一段只有 1.3 秒，很难抓帧）。
        else if (_wcsicmp(faceDemo, L"loading") == 0) face::ShowLoading(4000, 800);
    }
    else
    {
        // ---- 遭遇战调度器 ----
        if (director::Start(hInst))
        {
            if (noAuto) director::SetAutoAdvance(false);

            if (startPhase)
            {
                const director::Phase p = director::PhaseFromName(startPhase);
                if (p == director::PHASE_COUNT) elog::Write(L"未知阶段名: %s", startPhase);
                else                            director::JumpTo(p);
            }
        }
    }

    elog::Write(L"进入消息循环");

    // ---- 消息循环 ----
    MSG msg;
    while (g_running && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_HOTKEY)
        {
            if (msg.wParam == kHotkeyPanic)
            {
                elog::Write(L"安全阀触发，退出");
                g_running = false;
                break;
            }
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- 收尾 ----
    elog::Write(L"===== 开始收尾 =====");

    // 双进程看守：先 Disarm（告诉守护这次是正常退出），再关句柄。
    //
    // 顺序很重要：先 SetEvent 再 CloseHandle，最后进程才真正退出。
    // 这样守护那边看到的是「Disarm signaled」，而不是「进程消失但
    // Disarm 没 signal」的强杀状态。
    //
    // 放在收尾最前面：后面那些模块收尾可能需要几秒，早一点告诉守护
    // 它就可以早一点退出，不用一直悬着。
    guardian::Disarm();
    guardian::Stop();

    if (faceDemo) {}
    else { director::Stop(); }
    motion::Stop();
    face::Stop();
    fx::Stop();
    gold::Stop();                  // 删掉全部生成的金币文件
    ui_layout::Shutdown();         // 释放排版用的图片缓存（要在 GdiplusShutdown 之前）
    audio::Stop();
    if (overlayOn) overlay::Stop();

    UnregisterHotKey(hIpc, kHotkeyPanic);
    DestroyWindow(hIpc);

    if (SUCCEEDED(hrCom)) CoUninitialize();
    GdiplusShutdown(gdipToken);

    elog::Write(L"===== 已退出 =====");
    elog::Close();
    return 0;
}
