


#include "lockdown.h"

#include "entity_log.h"

#include <cwchar>
#include <string>
#include <vector>

namespace {




    const DWORD kRepressMs = 150;

    struct LockedWindow {
        HWND  hwnd   = nullptr;
        DWORD pid    = 0;
        std::wstring title;





        int   resist = 0;
    };



    const int kGiveUpAfter = 6;


    const int kMaxPerTick = 8;

    std::vector<LockedWindow> g_locked;
    std::vector<std::wstring> g_ownClasses;

    bool  g_active    = false;
    bool  g_enabled   = true;
    bool  g_blockKeys = true;
    DWORD g_lastTick  = 0;
    int   g_repressed = 0;

    DWORD g_selfPid = 0;



    bool IsOwnClass(const wchar_t* cls)
    {
        for (size_t i = 0; i < g_ownClasses.size(); ++i)
            if (_wcsicmp(g_ownClasses[i].c_str(), cls) == 0) return true;
        return false;
    }

    std::wstring WindowTitle(HWND h)
    {
        const int n = GetWindowTextLengthW(h);
        if (n <= 0) return std::wstring();
        std::wstring s((size_t)n + 1, L'\0');
        const int got = GetWindowTextW(h, &s[0], n + 1);
        s.resize(got > 0 ? (size_t)got : 0);
        return s;
    }

    DWORD WindowPid(HWND h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        return pid;
    }









    bool ShouldMinimize(HWND h)
    {
        if (!IsWindowVisible(h)) return false;
        if (GetWindow(h, GW_OWNER) != nullptr) return false;

        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) return false;

        wchar_t cls[128] = {0};
        if (!GetClassNameW(h, cls, 128)) return false;
        if (IsOwnClass(cls)) return false;

        if (WindowPid(h) == g_selfPid) return false;


        WINDOWPLACEMENT wp;
        ZeroMemory(&wp, sizeof(wp));
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(h, &wp)) return false;
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_SHOWMINNOACTIVE)
            return false;



        return !WindowTitle(h).empty();
    }

    BOOL CALLBACK CollectProc(HWND h, LPARAM lp)
    {
        std::vector<HWND>* out = (std::vector<HWND>*)lp;
        if (ShouldMinimize(h)) out->push_back(h);
        return TRUE;
    }


    int MinimizeEverything()
    {
        std::vector<HWND> targets;
        targets.reserve(64);
        EnumWindows(CollectProc, (LPARAM)&targets);

        int done = 0;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            HWND h = targets[i];
            const std::wstring title = WindowTitle(h);
            const DWORD pid = WindowPid(h);



            ShowWindowAsync(h, SW_MINIMIZE);

            LockedWindow lw;
            lw.hwnd  = h;
            lw.pid   = pid;
            lw.title = title;
            g_locked.push_back(lw);
            ++done;

            elog::Write(L"[lockdown] 收起窗口: \"%s\" (pid=%lu)",
                        title.c_str(), (unsigned long)pid);
        }
        return done;
    }



    int RestoreAll()
    {



        HWND prevForeground = GetForegroundWindow();

        int done = 0;
        for (size_t i = 0; i < g_locked.size(); ++i)
        {
            HWND h = g_locked[i].hwnd;
            if (!h || !IsWindow(h)) continue;

            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != g_locked[i].pid) continue;



            if (!IsIconic(h)) continue;












            ShowWindowAsync(h, SW_SHOWNOACTIVATE);
            ++done;
        }




        if (prevForeground && IsWindow(prevForeground) &&
            GetForegroundWindow() != prevForeground)
        {
            SetForegroundWindow(prevForeground);
        }

        return done;
    }


    int RepressRestored()
    {
        int hit = 0, gaveUp = 0;

        for (size_t i = 0; i < g_locked.size(); ++i)
        {
            LockedWindow& lw = g_locked[i];
            HWND h = lw.hwnd;
            if (!h || !IsWindow(h)) continue;
            if (IsIconic(h)) continue;

            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != lw.pid) continue;

            if (++lw.resist > kGiveUpAfter)
            {
                if (lw.resist == kGiveUpAfter + 1)
                {
                    ++gaveUp;
                    elog::Write(L"[lockdown] 放弃 \"%s\"：它被收起后总自己弹回来（试了 %d 次）",
                                lw.title.c_str(), kGiveUpAfter);
                }
                continue;
            }

            if (hit >= kMaxPerTick) continue;



            if (lw.resist == 1)
                elog::Write(L"[lockdown] \"%s\" 被叫回来了，重新收起（盯着它）", lw.title.c_str());

            ShowWindowAsync(h, SW_MINIMIZE);
            ++hit;
        }

        if (hit > 0)
        {
            const int total = g_repressed += hit;
            elog::Write(L"[lockdown] 本轮 有 %d 个窗口被叫回来了，已重新收起（累计 %d 次）",
                        hit, total);
        }
        return gaveUp;
    }

}

namespace lockdown {

void AddOwnClass(const wchar_t* className)
{
    if (!className || !*className) return;
    g_ownClasses.push_back(className);
}

int Start()
{
    g_selfPid = GetCurrentProcessId();

    if (!g_enabled)
    {
        elog::Write(L"[lockdown] 清场已被关闭（--no-lockdown），跳过");
        return 0;
    }


    if (g_active) Restore();

    g_locked.clear();
    g_active   = true;
    g_lastTick = GetTickCount();
    g_repressed = 0;

    const int n = MinimizeEverything();

    elog::Write(L"[lockdown] 清场完成：收走 %d 个窗口（键盘拦截=%d，守望间隔 %lums）",
                n, (int)g_blockKeys, (unsigned long)kRepressMs);
    return n;
}

int Restore()
{
    if (!g_active)
    {

        if (g_locked.empty()) return 0;
    }

    g_active = false;
    const int back = RestoreAll();
    const int total = (int)g_locked.size();
    g_locked.clear();

    elog::Write(L"[lockdown] 散场：还原 %d 个（本次共收走 %d 个，期间被叫回来又压回去 %d 次）",
                back, total, g_repressed);
    return back;
}

bool Active()          { return g_active; }
int  LockedCount()     { return (int)g_locked.size(); }
int  RepressedCount()  { return g_repressed; }

void Tick()
{
    if (!g_active) return;

    const DWORD now = GetTickCount();
    if (now - g_lastTick < kRepressMs) return;
    g_lastTick = now;

    const int gaveUp = RepressRestored();
    if (gaveUp > 0)
        elog::Write(L"[lockdown] 本轮放弃 %d 个「收不住」的窗口（剩下的继续看着）", gaveUp);
}

void SetBlockKeys(bool on)
{
    if (g_blockKeys == on) return;
    g_blockKeys = on;
    elog::Write(L"[lockdown] 键盘拦截（Win / Alt+Tab / Ctrl+Esc）：%s", on ? L"开" : L"关");
}

bool BlockKeys() { return g_blockKeys; }

void SetEnabled(bool on)
{
    if (g_enabled == on) return;
    g_enabled = on;
    if (!on && g_active) Restore();
    elog::Write(L"[lockdown] 清场功能：%s", on ? L"开" : L"关");
}

bool Enabled() { return g_enabled; }

bool ShouldSwallowKey(DWORD vkCode)
{
    if (!g_active || !g_blockKeys) return false;

    switch (vkCode)
    {
    case VK_LWIN:
    case VK_RWIN:
    case VK_TAB:
    case VK_ESCAPE:
        break;
    default:
        return false;
    }



    const bool alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (vkCode == VK_TAB)    return alt;
    if (vkCode == VK_ESCAPE) return ctrl;
    return true;
}

}
