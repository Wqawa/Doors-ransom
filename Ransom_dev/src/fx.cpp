


#include "fx.h"

#include "entity_log.h"

#include <cstdlib>
#include <cstring>
#include <atomic>
#include <vector>

#include <magnification.h>
#include <gdiplus.h>

#pragma comment(lib, "magnification.lib")

namespace {

const wchar_t* kFxClass     = L"RansomFxOverlay";
const wchar_t* kDriverClass = L"RansomFxDriver";

const UINT_PTR kTickId = 1;
const UINT     kTickMs = 33;


HWND      g_overlay = nullptr;
HWND      g_driver  = nullptr;
HINSTANCE g_hInst   = nullptr;

HDC       g_memDC   = nullptr;
HBITMAP   g_bmp     = nullptr;
HGDIOBJ   g_oldBmp  = nullptr;
void*     g_bits    = nullptr;
HDC       g_screen  = nullptr;
int       g_w = 0, g_h = 0;


int       g_flashTimes   = 0;
bool      g_flashPhaseOn = false;
DWORD     g_flashNext    = 0;
DWORD     g_flashOnMs    = 90;
DWORD     g_flashOffMs   = 70;
COLORREF  g_flashColor   = RGB(255, 255, 255);
BYTE      g_flashAlpha   = 210;


int       g_noise = 0;


bool      g_solid       = false;
COLORREF  g_solidColor  = RGB(200, 0, 0);


bool                g_glow        = false;
COLORREF            g_glowColor   = RGB(210, 16, 16);
int                 g_glowStrength = 170;
DWORD               g_glowFrame = 0;


bool      g_invert     = false;
bool      g_magReady   = false;
DWORD     g_invertOffAt = 0;




std::atomic<bool> g_safe{ false };






const int    kSafeNoise = 24;
const double kSafeSolid = 0.55;
const double kSafeGlow  = 0.45;


COLORREF DimColor(COLORREF c, double f)
{
    const int r = (int)(GetRValue(c) * f + 0.5);
    const int g = (int)(GetGValue(c) * f + 0.5);
    const int b = (int)(GetBValue(c) * f + 0.5);
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}


void VirtualRect(RECT& r)
{
    r.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (r.right <= r.left || r.bottom <= r.top)
    {
        r.left = 0; r.top = 0;
        r.right = GetSystemMetrics(SM_CXSCREEN);
        r.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

void FreeBuffers()
{
    if (g_memDC && g_oldBmp) { SelectObject(g_memDC, g_oldBmp); g_oldBmp = nullptr; }
    if (g_bmp)   { DeleteObject(g_bmp); g_bmp = nullptr; }
    if (g_memDC) { DeleteDC(g_memDC);   g_memDC = nullptr; }
    if (g_screen){ ReleaseDC(nullptr, g_screen); g_screen = nullptr; }
    g_bits = nullptr;
    g_w = g_h = 0;
}
















const int   kGlowBlock     = 4;



const float kGlowBandFracX = 0.42f;
const float kGlowBandFracY = 0.42f;




const int kAccentMin = 14;
const int kAccentMax = 26;




const float kAccentCornerFrac = 0.16f;

struct Accent { BYTE r, g, b; };
const Accent kAccents[4] = {
    {  60, 255,  70 },
    {  70, 120, 255 },
    { 190,  80, 255 },
    { 255, 235,  60 },
};










void RenderAccentPixels(BYTE* bits, int w, int h)
{
    const int spanX = (int)(w * kAccentCornerFrac);
    const int spanY = (int)(h * kAccentCornerFrac);
    const int nx = spanX / kGlowBlock;
    const int ny = spanY / kGlowBlock;
    if (nx <= 0 || ny <= 0) return;

    const int n = kAccentMin + (rand() % (kAccentMax - kAccentMin + 1));

    for (int i = 0; i < n; ++i)
    {
        const int ci     = rand() & 3;
        const bool right = (ci & 1) != 0;
        const bool bot   = (ci & 2) != 0;


        const int bx = (rand() % nx) * kGlowBlock;
        const int by = (rand() % ny) * kGlowBlock;

        const int x0 = right ? (w - kGlowBlock - bx) : bx;
        const int y0 = bot   ? (h - kGlowBlock - by) : by;
        if (x0 < 0 || y0 < 0) continue;

        const int a = 150 + (rand() % 106);
        const Accent& c = kAccents[rand() & 3];

        int x1 = x0 + kGlowBlock, y1 = y0 + kGlowBlock;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;

        for (int py = y0; py < y1; ++py)
        {
            BYTE* row = bits + (size_t)py * w * 4;
            for (int px = x0; px < x1; ++px)
            {
                BYTE* q = row + (size_t)px * 4;
                if (a <= q[3]) continue;
                q[0] = c.b; q[1] = c.g; q[2] = c.r; q[3] = (BYTE)a;
            }
        }
    }
}

void RenderCornerGlow(BYTE* bits, int w, int h, int strength, COLORREF color)
{
    const int bandX = (int)(w * kGlowBandFracX);
    const int bandY = (int)(h * kGlowBandFracY);
    if (bandX < kGlowBlock || bandY < kGlowBlock) return;

    const BYTE cb = GetBValue(color);
    const BYTE cg = GetGValue(color);
    const BYTE cr = GetRValue(color);


    for (int ci = 0; ci < 4; ++ci)
    {
        const bool right  = (ci & 1) != 0;
        const bool bottom = (ci & 2) != 0;

        for (int by = 0; by < bandY; by += kGlowBlock)
        for (int bx = 0; bx < bandX; bx += kGlowBlock)
        {



            const double fx = 1.0 - ((double)bx + kGlowBlock * 0.5) / (double)bandX;
            const double fy = 1.0 - ((double)by + kGlowBlock * 0.5) / (double)bandY;
            if (fx <= 0.0 || fy <= 0.0) continue;


            const double weight = fx * fx * fy * fy;
            if (weight <= 0.01) continue;

            const int rnd = rand();


            if ((rnd & 0xFF) > (int)(weight * 255.0)) continue;


            int a = (int)((double)strength * weight *
                          (0.45 + 0.55 * (double)((rnd >> 8) & 0xFF) / 255.0));
            if (a <= 0)   continue;
            if (a > 255)  a = 255;




            const int x0 = right  ? (w - kGlowBlock - bx) : bx;
            const int y0 = bottom ? (h - kGlowBlock - by) : by;
            if (x0 < 0 || y0 < 0) continue;

            int x1 = x0 + kGlowBlock, y1 = y0 + kGlowBlock;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;

            for (int y = y0; y < y1; ++y)
            {
                BYTE* row = bits + (size_t)y * w * 4;
                for (int x = x0; x < x1; ++x)
                {
                    BYTE* q = row + (size_t)x * 4;
                    if (a <= q[3]) continue;
                    q[0] = cb; q[1] = cg; q[2] = cr; q[3] = (BYTE)a;
                }
            }
        }
    }
}

bool EnsureBuffers(int w, int h)
{
    if (g_memDC && g_bits && w == g_w && h == g_h) return true;

    FreeBuffers();

    g_screen = GetDC(nullptr);
    if (!g_screen) return false;

    g_memDC = CreateCompatibleDC(g_screen);
    if (!g_memDC) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_bmp = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    if (!g_bmp || !g_bits) { FreeBuffers(); return false; }

    g_oldBmp = SelectObject(g_memDC, g_bmp);
    g_w = w;
    g_h = h;
    return true;
}


bool ApplyInvertMatrix(bool on)
{
    if (!g_magReady) return false;

    MAGCOLOREFFECT eff;

    if (on)
    {

        const float m[5][5] = {
            { -1.0f,  0.0f,  0.0f, 0.0f, 0.0f },
            {  0.0f, -1.0f,  0.0f, 0.0f, 0.0f },
            {  0.0f,  0.0f, -1.0f, 0.0f, 0.0f },
            {  0.0f,  0.0f,  0.0f, 1.0f, 0.0f },
            {  0.0f,  0.0f,  0.0f, 0.0f, 1.0f },
        };
        memcpy(eff.transform, m, sizeof(m));
    }
    else
    {

        const float m[5][5] = {
            { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },
        };
        memcpy(eff.transform, m, sizeof(m));
    }

    return MagSetFullscreenColorEffect(&eff) != FALSE;
}


void ShowOverlay(bool show)
{
    if (!g_overlay) return;
    ShowWindow(g_overlay, show ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Render()
{
    const bool flashActive = (g_flashTimes > 0) && g_flashPhaseOn;
    const bool noiseActive = (g_noise > 0);
    const bool solidActive = g_solid;
    const bool glowActive  = g_glow;


    if (!flashActive && !noiseActive && !solidActive && !glowActive)
    {
        ShowOverlay(false);
        return;
    }

    RECT vr;
    VirtualRect(vr);
    const int w = vr.right - vr.left;
    const int h = vr.bottom - vr.top;
    if (w <= 0 || h <= 0) return;

    if (!EnsureBuffers(w, h)) return;


    RECT cur;
    GetWindowRect(g_overlay, &cur);
    if (cur.left != vr.left || cur.top != vr.top ||
        cur.right != vr.right || cur.bottom != vr.bottom)
    {
        SetWindowPos(g_overlay, HWND_TOPMOST,
                     vr.left, vr.top, w, h,
                     SWP_NOACTIVATE);
    }

    BYTE* p   = (BYTE*)g_bits;
    const size_t n = (size_t)w * h;


    if (g_solid)
    {

        const BYTE r2 = GetRValue(g_solidColor);
        const BYTE g2 = GetGValue(g_solidColor);
        const BYTE b2 = GetBValue(g_solidColor);
        for (size_t i = 0; i < n; ++i, p += 4)
        {
            p[0] = b2; p[1] = g2; p[2] = r2; p[3] = 255;
        }
        p = (BYTE*)g_bits;
    }
    else if (flashActive)
    {
        const BYTE a  = g_flashAlpha;
        const BYTE r  = GetRValue(g_flashColor);
        const BYTE g2 = GetGValue(g_flashColor);
        const BYTE b  = GetBValue(g_flashColor);
        for (size_t i = 0; i < n; ++i, p += 4)
        {
            p[0] = b; p[1] = g2; p[2] = r; p[3] = a;
        }
        p = (BYTE*)g_bits;
    }
    else
    {
        memset(g_bits, 0, n * 4);
    }


    if (noiseActive)
    {
        const int density = g_noise;






        const BYTE sr = solidActive ? GetRValue(g_solidColor) : 0;
        const BYTE sg = solidActive ? GetGValue(g_solidColor) : 0;
        const BYTE sb = solidActive ? GetBValue(g_solidColor) : 0;

        for (size_t i = 0; i < n; ++i, p += 4)
        {
            if ((rand() & 0xFF) < density)
            {
                const BYTE v = (BYTE)(rand() & 0xFF);
                if (solidActive)
                {
                    const int bv = sb + v, gv = sg + v, rv = sr + v;
                    p[0] = (BYTE)(bv > 255 ? 255 : bv);
                    p[1] = (BYTE)(gv > 255 ? 255 : gv);
                    p[2] = (BYTE)(rv > 255 ? 255 : rv);
                    p[3] = 255;
                }
                else
                {
                    p[0] = v; p[1] = v; p[2] = v; p[3] = v;
                }
            }
        }
    }





    if (glowActive)
    {
        ++g_glowFrame;


        const int pulse = 224 + (int)(31 * ((g_glowFrame / 4) & 1));
        RenderCornerGlow((BYTE*)g_bits, w, h,
                         g_glowStrength * pulse / 255, g_glowColor);


        RenderAccentPixels((BYTE*)g_bits, w, h);
    }


    p = (BYTE*)g_bits;
    for (size_t i = 0; i < n; ++i, p += 4)
    {
        const BYTE a = p[3];
        if (a == 255) continue;
        if (a == 0)   { p[0] = p[1] = p[2] = 0; continue; }
        p[0] = (BYTE)(p[0] * a / 255);
        p[1] = (BYTE)(p[1] * a / 255);
        p[2] = (BYTE)(p[2] * a / 255);
    }

    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    SIZE  size = { w, h };
    POINT src  = { 0, 0 };
    UpdateLayeredWindow(g_overlay, g_screen, nullptr, &size,
                        g_memDC, &src, 0, &bf, ULW_ALPHA);

    ShowOverlay(true);
}

void Tick()
{
    const DWORD now = GetTickCount();


    if (g_invertOffAt != 0 && now >= g_invertOffAt)
    {
        g_invertOffAt = 0;
        fx::SetInvert(false);
    }


    if (g_flashTimes > 0 && now >= g_flashNext)
    {
        if (g_flashPhaseOn)
        {
            g_flashPhaseOn = false;
            g_flashNext = now + g_flashOffMs;
        }
        else
        {
            g_flashPhaseOn = true;
            --g_flashTimes;
            g_flashNext = now + g_flashOnMs;
        }
    }

    Render();
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DriverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == kTickId) { Tick(); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}

namespace fx {

bool Start(HINSTANCE hInst)
{
    if (g_driver) return true;
    g_hInst = hInst;

    WNDCLASSEXW oc = { sizeof(WNDCLASSEXW) };
    oc.lpfnWndProc   = OverlayProc;
    oc.hInstance     = hInst;
    oc.hCursor       = nullptr;
    oc.hbrBackground = nullptr;
    oc.lpszClassName = kFxClass;
    RegisterClassExW(&oc);

    WNDCLASSEXW dc = { sizeof(WNDCLASSEXW) };
    dc.lpfnWndProc   = DriverProc;
    dc.hInstance     = hInst;
    dc.hCursor       = nullptr;
    dc.hbrBackground = nullptr;
    dc.lpszClassName = kDriverClass;
    RegisterClassExW(&dc);

    RECT vr;
    VirtualRect(vr);

    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kFxClass, L"", WS_POPUP,
        vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top,
        nullptr, nullptr, hInst, nullptr);
    if (!g_overlay)
    {
        elog::Write(L"[fx] 覆盖窗口创建失败, err=%lu", GetLastError());
        return false;
    }

    g_driver = CreateWindowExW(0, kDriverClass, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_driver)
    {
        elog::Write(L"[fx] 驱动窗口创建失败, err=%lu", GetLastError());
        DestroyWindow(g_overlay);
        g_overlay = nullptr;
        return false;
    }

    SetTimer(g_driver, kTickId, kTickMs, nullptr);


    g_magReady = (MagInitialize() != FALSE);
    elog::Write(L"[fx] 就绪，反色支持 %d", (int)g_magReady);

    return true;
}

void Stop()
{

    if (g_invert) SetInvert(false);

    if (g_driver)
    {
        KillTimer(g_driver, kTickId);
        DestroyWindow(g_driver);
        g_driver = nullptr;
    }
    if (g_overlay)
    {
        DestroyWindow(g_overlay);
        g_overlay = nullptr;
    }

    if (g_magReady)
    {
        MagUninitialize();
        g_magReady = false;
    }

    FreeBuffers();
    g_flashTimes = 0;
    g_noise = 0;
    g_glow  = false;
    elog::Write(L"[fx] 已停止");
}

void Flash(COLORREF color, int times, DWORD onMs, DWORD offMs)
{
    if (!g_driver) return;



    BYTE alpha = 210;
    if (g_safe.load())
    {
        times = (times + 1) / 2;
        onMs  = (onMs > 40) ? 40 : onMs;
        offMs = (offMs < 80) ? 80 : offMs;
        color = DimColor(color, 0.55);
        alpha = 120;
    }

    g_flashColor   = color;
    g_flashTimes   = (times < 1) ? 1 : times;
    g_flashOnMs    = (onMs < 16) ? 16 : onMs;
    g_flashOffMs   = (offMs < 16) ? 16 : offMs;
    g_flashAlpha   = alpha;
    g_flashPhaseOn = true;
    g_flashNext    = GetTickCount() + g_flashOnMs;

    elog::Write(L"[fx] 闪屏 x%d color=%06lX on=%lums off=%lums%s",
                g_flashTimes, (unsigned long)color, g_flashOnMs, g_flashOffMs,
                g_safe.load() ? L"（光敏安全模式已削弱）" : L"");
}

void StopFlash()
{
    g_flashTimes   = 0;
    g_flashPhaseOn = false;
}

bool Flashing() { return g_flashTimes > 0; }

void SetPhotosensitiveSafe(bool on)
{
    const bool prev = g_safe.exchange(on);
    if (prev == on) return;


    static COLORREF savedSolidColor = 0;
    static int      savedGlowStrength = 0;
    static int      savedNoise = 0;
    static bool     savedValid = false;

    if (on)
    {

        savedSolidColor = g_solidColor;
        savedGlowStrength = g_glowStrength;
        savedNoise = g_noise;
        savedValid = true;

        if (g_noise > kSafeNoise) { g_noise = kSafeNoise; }
        if (g_solid) { g_solidColor = DimColor(g_solidColor, kSafeSolid); }
        if (g_glowStrength > 0) { g_glowStrength = (int)(g_glowStrength * kSafeGlow + 0.5); }
    }
    else
    {

        if (savedValid)
        {
            g_solidColor = savedSolidColor;
            g_glowStrength = savedGlowStrength;
            g_noise = savedNoise;
            savedValid = false;
        }
    }

    elog::Write(L"[fx] 光敏安全模式 %s", on ? L"开启" : L"关闭");
}

bool PhotosensitiveSafe() { return g_safe.load(); }

void SetNoise(int level)
{
    if (level < 0)   level = 0;
    if (level > 255) level = 255;
    if (g_safe.load() && level > kSafeNoise) level = kSafeNoise;

    if (level == g_noise) return;

    g_noise = level;
    elog::Write(L"[fx] 雪花强度=%d", level);
}

int Noise() { return g_noise; }

bool SetInvert(bool on)
{
    if (!g_magReady)
    {
        elog::Write(L"[fx] 反色不可用（Magnification API 未就绪）");
        return false;
    }
    if (on == g_invert) return true;

    if (!ApplyInvertMatrix(on))
    {
        elog::Write(L"[fx] MagSetFullscreenColorEffect 失败, err=%lu", GetLastError());
        return false;
    }

    g_invert = on;
    elog::Write(L"[fx] 反色 %s", on ? L"开启" : L"关闭");
    return true;
}

bool Invert() { return g_invert; }

void InvertPulse(DWORD ms)
{


    if (g_safe.load())
    {
        elog::Write(L"[fx] 反色脉冲被光敏安全模式拦下（%lums）", (unsigned long)ms);
        return;
    }

    if (ms < 16) ms = 16;
    if (!SetInvert(true)) return;
    g_invertOffAt = GetTickCount() + ms;
    elog::Write(L"[fx] 反色脉冲 %lums", ms);
}

void SetSolid(bool on, COLORREF color)
{


    if (on && g_safe.load()) color = DimColor(color, kSafeSolid);

    if (g_solid == on && g_solidColor == color) return;
    g_solid      = on;
    g_solidColor = color;
    elog::Write(L"[fx] 全屏纯色底 %s color=%06lX",
                on ? L"开启" : L"关闭", (unsigned long)color);
}

bool Solid() { return g_solid; }

void SetEdgeGlow(bool on, COLORREF color, int strength)
{
    if (strength < 0)   strength = 0;
    if (strength > 255) strength = 255;
    if (on && g_safe.load()) strength = (int)(strength * kSafeGlow + 0.5);

    if (g_glow == on && g_glowColor == color && g_glowStrength == strength) return;

    g_glow         = on;
    g_glowColor    = color;
    g_glowStrength = strength;
    if (on) g_glowFrame = 0;
    elog::Write(L"[fx] 边缘红光 %s color=%06lX 强度=%d",
                on ? L"开启" : L"关闭", (unsigned long)color, strength);
}

bool EdgeGlow() { return g_glow; }








bool DumpLayer(const wchar_t* path)
{
    if (!g_overlay)
    {
        elog::Write(L"[fx] 图层导出失败：覆盖层窗口还没建");
        return false;
    }


    Render();

    if (!g_bits || g_w <= 0 || g_h <= 0)
    {
        elog::Write(L"[fx] 图层导出失败：后备缓冲没就绪（w=%d h=%d bits=%d）",
                    g_w, g_h, g_bits ? 1 : 0);
        return false;
    }

    Gdiplus::Bitmap out(g_w, g_h, PixelFormat32bppARGB);
    if (out.GetLastStatus() != Gdiplus::Ok)
    {
        elog::Write(L"[fx] 图层导出失败：建位图 err=%d", (int)out.GetLastStatus());
        return false;
    }

    Gdiplus::Rect r(0, 0, g_w, g_h);
    Gdiplus::BitmapData bd;
    const Gdiplus::Status ls = out.LockBits(&r, Gdiplus::ImageLockModeRead,
                                            PixelFormat32bppARGB, &bd);
    if (ls != Gdiplus::Ok)
    {
        elog::Write(L"[fx] 图层导出失败：LockBits err=%d", (int)ls);
        return false;
    }
    const BYTE* src = (const BYTE*)g_bits;
    for (int y = 0; y < g_h; ++y)
    {
        const BYTE* s = src + (size_t)y * g_w * 4;
        BYTE* d = (BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
        for (int x = 0; x < g_w; ++x, s += 4, d += 4)
        {
            const BYTE a = s[3];
            d[3] = a;


            if (a == 0)          { d[0] = d[1] = d[2] = 0; }
            else if (a == 255)   { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            else
            {
                const int b0 = s[0] * 255 / a, g0 = s[1] * 255 / a, r0 = s[2] * 255 / a;
                d[0] = (BYTE)(b0 > 255 ? 255 : b0);
                d[1] = (BYTE)(g0 > 255 ? 255 : g0);
                d[2] = (BYTE)(r0 > 255 ? 255 : r0);
            }
        }
    }
    out.UnlockBits(&bd);

    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, info);
    CLSID clsid; bool found = false;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { clsid = info[i].Clsid; found = true; break; }
    if (!found) { elog::Write(L"[fx] 图层导出失败：没有 PNG 编码器"); return false; }

    const Gdiplus::Status ss = out.Save(path, &clsid, nullptr);
    if (ss != Gdiplus::Ok)
        elog::Write(L"[fx] 图层导出失败：Save err=%d path=%s", (int)ss, path);
    return ss == Gdiplus::Ok;
}

void ClearAll()
{
    StopFlash();
    SetNoise(0);
    g_solid = false;
    g_glow  = false;
    g_invertOffAt = 0;
    SetInvert(false);
    ShowOverlay(false);
}

}
