


#include "motion.h"

#include "entity_log.h"

#include <cmath>
#include <cstdlib>

namespace {

const wchar_t* kDriverClass = L"RansomMotionDriver";
const UINT_PTR kTickId = 1;
const UINT     kTickMs = 16;

HHOOK     g_hook    = nullptr;
HWND      g_driver  = nullptr;

bool      g_armed   = false;
bool      g_byMouse = false;
bool      g_byKb    = false;
int       g_drift   = 0;
UINT      g_lastKey = 0;


int       g_tol     = 20;

POINT     g_anchor  = { 0, 0 };


UINT      g_panicVK    = 'Q';
bool      g_panicCtrl  = true;
bool      g_panicAlt   = true;
bool      g_panicShift = true;



bool IsModifierKey(DWORD vk)
{
    switch (vk)
    {
    case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU:    case VK_LMENU:    case VK_RMENU:
        return true;
    default:
        return false;
    }
}


bool PanicModifiersHeld()
{
    if (g_panicCtrl  && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
    if (g_panicAlt   && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
    if (g_panicShift && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
    return true;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode != HC_ACTION)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    if (!down)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;

    if (!g_armed)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);


    if (IsModifierKey(k->vkCode))
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    if (k->vkCode == g_panicVK && PanicModifiersHeld())
        return CallNextHookEx(nullptr, nCode, wParam, lParam);


    g_byKb    = true;
    g_lastKey = k->vkCode;

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void PollMouse()
{
    if (!g_armed || g_byMouse) return;

    POINT p;
    if (!GetCursorPos(&p)) return;

    const int dx = p.x - g_anchor.x;
    const int dy = p.y - g_anchor.y;
    const int d  = (int)sqrt((double)(dx * dx + dy * dy));

    if (d > g_drift) g_drift = d;

    if (d > g_tol)
        g_byMouse = true;
}

LRESULT CALLBACK DriverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == kTickId) { PollMouse(); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}

namespace motion {

bool Start(HINSTANCE hInst, UINT panicVK,
           bool needCtrl, bool needAlt, bool needShift)
{
    g_panicVK    = panicVK;
    g_panicCtrl  = needCtrl;
    g_panicAlt   = needAlt;
    g_panicShift = needShift;

    if (!g_driver)
    {
        WNDCLASSEXW dc = { sizeof(WNDCLASSEXW) };
        dc.lpfnWndProc   = DriverProc;
        dc.hInstance     = hInst;
        dc.hCursor       = nullptr;
        dc.hbrBackground = nullptr;
        dc.lpszClassName = kDriverClass;
        RegisterClassExW(&dc);

        g_driver = CreateWindowExW(0, kDriverClass, L"", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, nullptr, hInst, nullptr);
        if (!g_driver)
        {
            elog::Write(L"[motion] 驱动窗口创建失败, err=%lu", GetLastError());
            return false;
        }
        SetTimer(g_driver, kTickId, kTickMs, nullptr);
    }

    if (!g_hook)
    {
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
        if (!g_hook)
        {
            elog::Write(L"[motion] 键盘钩子安装失败, err=%lu", GetLastError());
            return false;
        }
    }

    elog::Write(L"[motion] 已就绪（容差 %dpx，安全阀 vk=0x%02X 已豁免）", g_tol, panicVK);
    return true;
}

void Stop()
{
    Disarm();

    if (g_hook)
    {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_driver)
    {
        KillTimer(g_driver, kTickId);
        DestroyWindow(g_driver);
        g_driver = nullptr;
    }
    elog::Write(L"[motion] 已停止");
}

void Arm()
{
    GetCursorPos(&g_anchor);
    g_byMouse = false;
    g_byKb    = false;
    g_drift   = 0;
    g_lastKey = 0;
    g_armed   = true;

    elog::Write(L"[motion] 开始监视（锚点 %ld,%ld，容差 %dpx）",
                g_anchor.x, g_anchor.y, g_tol);
}

void Disarm()
{
    if (g_armed)
    {
        g_armed = false;
        elog::Write(L"[motion] 停止监视（最大偏移 %dpx，鼠标动=%d 键盘动=%d）",
                    g_drift, (int)g_byMouse, (int)g_byKb);
    }
}

bool Armed()          { return g_armed; }
bool Moved()          { return g_byMouse || g_byKb; }
bool MovedByMouse()   { return g_byMouse; }
bool MovedByKeyboard(){ return g_byKb; }
int  MouseDrift()     { return g_drift; }
UINT LastKey()        { return g_lastKey; }

void SetTolerance(int px)
{
    if (px < 1)   px = 1;
    if (px > 200) px = 200;
    g_tol = px;
}

int Tolerance() { return g_tol; }

}
