




















#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>


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

using namespace Gdiplus;

namespace {

const wchar_t* kEntityName = L"Ransom_dev";


const int  kHotkeyPanic = 1;
const UINT kPanicMods   = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
const UINT kPanicVK     = 'Q';


const wchar_t* kIpcClass = L"RansomDevIpcWnd";

volatile bool g_running = true;


UINT g_msgPay = 0;








int SendPayment(int amount, int token)
{
    const UINT msg = RegisterWindowMessageW(L"RansomDev_PayGold");
    if (!msg) return 2;

    HWND h = FindWindowW(L"RansomDevIpcWnd", nullptr);
    if (!h || !IsWindow(h)) return 3;


    PostMessageW(h, msg, (WPARAM)amount, (LPARAM)token);
    return 0;
}

LRESULT CALLBACK IpcProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{



    if (msg == WM_HOTKEY && wp == kHotkeyPanic)
    {
        if (setup_ui::DispatchHotkey()) return 0;
    }


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


        const int got = gold::Consume(token);
        if (got > 0)
        {
            director::CreditGold(got);
            audio::PlayCoin();
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

}


int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{





    if (guardian::IsGuardianMode())
    {
        DWORD targetPid = 0;
        int   gen = 0;
        if (!guardian::ParseGuardianArgs(__argc, __wargv, targetPid, gen) ||
            targetPid == 0)
        {

            return 2;
        }
        return guardian::RunGuardian(hInst, targetPid, gen);
    }



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


    if (payMode)
    {
        const int rc = SendPayment(payAmount, payToken);
        return rc;
    }

    elog::Open(diagPath);
    elog::Write(L"===== Ransom_dev 启动 =====");


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


    if (audioDump)
    {
        if (!audio::Start()) { elog::Write(L"音频设备不可用，无法导出"); elog::Close(); return 1; }
        const bool ok = audio::DumpMix(audioDump, 10);
        audio::Stop();
        elog::Write(L"音效导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }



    if (themeDump)
    {
        audio::Start();
        const bool ok = audio::DumpTheme(themeDump);
        audio::Stop();
        elog::Write(L"主题曲导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }


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








    settings::Load();




    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    if (GdiplusStartup(&gdipToken, &gsi, nullptr) != Gdiplus::Ok)
    {
        elog::Write(L"GDI+ 初始化失败");
        elog::Close();
        return 1;
    }

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);




    if (uiPreview)
    {
        ui_layout::Load(L"main_window.ini", L"payup.ini");
        if (uiPreviewPayup) ui_layout::SetActive(1);


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
            demo.goldGoal = 750;
            ok = setup_ui::DumpSettingsPreview(setupUi, demo, setupUiGrid) && ok;
        }
        if (noticeUi)
        {
            ok = setup_ui::DumpNoticePreview(noticeUi, setupUiGrid) && ok;
        }

        elog::Write(L"开场界面预览结束，成功=%d", (int)ok);
        elog::Close();
        GdiplusShutdown(gdipToken);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return ok ? 0 : 1;
    }


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







    aero::SetAppIconFromSelf();
    {
        assets::Blob fnt;
        if (assets::Get(assets::KIND_ROOT, L"RobotoMono-VariableFont_wght.ttf", fnt))
            aero::SetTitleFontFromMemory(fnt.Data(), fnt.Size(), L"Roboto Mono");
        else
            elog::Write(L"字体素材不在包里，标题栏回退 Microsoft YaHei");
    }
















    {
        const bool wantSetup = !noSetup && !faceDemo && !faceDump &&
                               !fxDump && !audioDump && !themeDump &&
                               !setupUi && !noticeUi;

        if (!noAudio) audio::Start();

        if (wantSetup)
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



            if (panicHotkeyOk)
            {
                const setup_ui::Verdict v2 =
                    setup_ui::ShowSafetyNotice(hInst, kPanicVK);

                if (v2 == setup_ui::VERDICT_ABORT)
                {
                    elog::Write(L"[main] 用户在应急提示里退出了，不演出");
                    audio::Stop();
                    UnregisterHotKey(hIpc, kHotkeyPanic);
                    DestroyWindow(hIpc);
                    if (SUCCEEDED(hrCom)) CoUninitialize();
                    GdiplusShutdown(gdipToken);
                    elog::Close();
                    return 0;
                }
            }
            else
            {
                elog::Write(L"[main] 安全阀热键不可用，跳过应急提示（免得承诺一个假快捷键）");
            }



            settings::Apply();
            elog::Write(L"[main] 设置确认，开始演出");
        }
        else
        {
            settings::Apply();
        }
    }


    face::Start(hInst);


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
    gold::CleanupStale();
    if (!noAudio) audio::Start();

    const bool overlayOn = !noOverlay && overlay::Start(hInst, overlayTop);
    if (overlayOn)
    {
        overlay::SetVisible(false);

        if (noBlockMenu) overlay::SetBlockContextMenu(false);
    }

    if (noLockdown) lockdown::SetEnabled(false);




    guardian::ClearPunishMark();












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




        SetTimer(hIpc, 2, 5000, nullptr);
    }




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




        guardian::Disarm();
        guardian::Stop();

        elog::Close();
        GdiplusShutdown(gdipToken);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return ok ? 0 : 1;
    }


    if (faceDemo)
    {
        elog::Write(L"面部演示模式: %s", faceDemo);
        if      (_wcsicmp(faceDemo, L"idle")   == 0) face::SpawnIdle(0);
        else if (_wcsicmp(faceDemo, L"stop")   == 0) face::ShowStopSign(0);
        else if (_wcsicmp(faceDemo, L"attack") == 0) face::ShowAttack(0);
        else if (_wcsicmp(faceDemo, L"thanks") == 0) face::ShowThanks(0);


        else if (_wcsicmp(faceDemo, L"loading") == 0) face::ShowLoading(4000, 800);
    }
    else
    {

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


    elog::Write(L"===== 开始收尾 =====");









    guardian::Disarm();
    guardian::Stop();

    if (faceDemo) {}
    else { director::Stop(); }
    motion::Stop();
    face::Stop();
    fx::Stop();
    gold::Stop();
    ui_layout::Shutdown();
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
