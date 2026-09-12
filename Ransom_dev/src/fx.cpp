// ============================================================================
//  fx.cpp
// ============================================================================
#include "fx.h"

#include "entity_log.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <magnification.h>
#include <gdiplus.h>

#pragma comment(lib, "magnification.lib")

namespace {

const wchar_t* kFxClass     = L"RansomFxOverlay";
const wchar_t* kDriverClass = L"RansomFxDriver";

const UINT_PTR kTickId = 1;
const UINT     kTickMs = 33;      // ~30fps，闪屏和雪花够用

// ---------------------------------------------------------------- 状态 ----
HWND      g_overlay = nullptr;
HWND      g_driver  = nullptr;
HINSTANCE g_hInst   = nullptr;

HDC       g_memDC   = nullptr;
HBITMAP   g_bmp     = nullptr;
HGDIOBJ   g_oldBmp  = nullptr;
void*     g_bits    = nullptr;
HDC       g_screen  = nullptr;
int       g_w = 0, g_h = 0;

// 闪屏
int       g_flashTimes   = 0;
bool      g_flashPhaseOn = false;
DWORD     g_flashNext    = 0;
DWORD     g_flashOnMs    = 90;
DWORD     g_flashOffMs   = 70;
COLORREF  g_flashColor   = RGB(255, 255, 255);
BYTE      g_flashAlpha   = 210;

// 噪声
int       g_noise = 0;

// 全屏纯色底（不透明）
bool      g_solid       = false;
COLORREF  g_solidColor  = RGB(200, 0, 0);

// 四角红光（「已加密」阶段的红幕）
bool                g_glow        = false;
COLORREF            g_glowColor   = RGB(210, 16, 16);
int                 g_glowStrength = 170;
DWORD               g_glowFrame = 0; // 呼吸相位

// 反色
bool      g_invert     = false;
bool      g_magReady   = false;
DWORD     g_invertOffAt = 0;

// ---------------------------------------------------------------- 尺寸 ----
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

// ---------------------------------------------------------------- 四角红光 ----
// 「已加密」阶段的红幕：**集中在四个角**的红色大噪点像素。
//
// 和普通的「发光」有两处根本不同：
//
//   1. 权重是两条轴衰减的**乘积**（不是取 max）。
//      取 max 的话整条边都会亮——那是四边泛光。
//      乘积要求两轴同时靠近边缘，所以只有四个角有值，边的中段自然为零。
//
//   2. 不是逐像素渐变，而是 kGlowBlock × kGlowBlock 的**大块**，
//      每块**随机**决定亮不亮、有多亮。所以是一粒粒的噪点，不是一层光晕。
//      每帧重掷一次随机数，看起来就是四角在闪噪点。
//
// 开销：只遍历四个角区，而且是按块写（不是逐像素），比原来那条
// 全屏逐像素的通道便宜得多——所以 90 秒的持续演出也扛得住。
const int   kGlowBlock     = 4;      // 噪点块边长（px）
// 角区占屏的比例。原来两边都是 0.30，四个角的光斑显得太「收」，
// 中间留出 40% 的纯黑带，看着像四个孤立的小方块。
// 放大到 0.42 之后角区在屏幕中段附近才收住，四个角连成一片包围感。
const float kGlowBandFracX = 0.42f;  // 左右向角区宽度占屏宽的比例
const float kGlowBandFracY = 0.42f;  // 上下向角区高度占屏高的比例
// ---- 彩色噪点 ----
// 红幕上极少量地混进绿/蓝/紫/黄的亮点。
// 数量刻意压得很低（十几到二十几块），目的是给「一条纯红通道坏掉」的
// 画面加一点 CRT 色散的脏感；撒多了就变成彩噪花屏，反而糊。
const int kAccentMin = 14;
const int kAccentMax = 26;

// 彩色点**只落在贴着角的那一小块**里（不是整个 42% 的角区）。
// 用 0.16 而不是 kGlowBandFrac：红光本来就随权重往里衰减，
// 彩色点再摊到整个角区上就散成满屏彩点了，和「集中到四个角」正相反。
const float kAccentCornerFrac = 0.16f;

struct Accent { BYTE r, g, b; };
const Accent kAccents[4] = {
    {  60, 255,  70 },   // 绿
    {  70, 120, 255 },   // 蓝
    { 190,  80, 255 },   // 紫
    { 255, 235,  60 },   // 黄
};

// 四个角上撒几颗彩色亮点（未预乘的直通 alpha）。
//
// 两个「和红光保持一致」的地方：
//   1. **块大小一样**：都是 kGlowBlock × kGlowBlock（4x4），
//      不是 1x1/2x2 的细点——细点在颗粒状的红光里像坏点，不像同一种噪点。
//   2. **对齐同一张网格**：块坐标按 kGlowBlock 取整，
//      和 RenderCornerGlow 用的是同一套「从角往里数」的 bx/by，
//      右/下两边同样要翻过来（w - kGlowBlock - bx），否则衰减方向会反。
// 中间那片（玩家要找金币的地方）一个点都不撒。
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

        // 第 0 块就是最贴角的那一块，和红光的 bx/by 同一个含义
        const int bx = (rand() % nx) * kGlowBlock;
        const int by = (rand() % ny) * kGlowBlock;

        const int x0 = right ? (w - kGlowBlock - bx) : bx;
        const int y0 = bot   ? (h - kGlowBlock - by) : by;
        if (x0 < 0 || y0 < 0) continue;

        const int a = 150 + (rand() % 106);       // 150..255
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
                if (a <= q[3]) continue;          // 已经有更实的内容就别盖
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

    // 四个角：ci 的第 0 位 = 在右边，第 1 位 = 在下边
    for (int ci = 0; ci < 4; ++ci)
    {
        const bool right  = (ci & 1) != 0;
        const bool bottom = (ci & 2) != 0;

        for (int by = 0; by < bandY; by += kGlowBlock)
        for (int bx = 0; bx < bandX; bx += kGlowBlock)
        {
            // bx / by 是**从角向里数**的偏移（0 = 最贴角的那一块），
            // 所以左右、上下可以共用同一套衰减公式。
            // 用块中心算距离，免得最贴边那一块权重突变。
            const double fx = 1.0 - ((double)bx + kGlowBlock * 0.5) / (double)bandX;
            const double fy = 1.0 - ((double)by + kGlowBlock * 0.5) / (double)bandY;
            if (fx <= 0.0 || fy <= 0.0) continue;

            // 二次衰减再相乘：角上最亮，沿任一条边走出去都迅速变暗。
            const double weight = fx * fx * fy * fy;
            if (weight <= 0.01) continue;

            const int rnd = rand();

            // 按权重决定这一块亮不亮 —— 越靠角越密
            if ((rnd & 0xFF) > (int)(weight * 255.0)) continue;

            // 亮度也随机（45%~100%），每块深浅不一才像噪点
            int a = (int)((double)strength * weight *
                          (0.45 + 0.55 * (double)((rnd >> 8) & 0xFF) / 255.0));
            if (a <= 0)   continue;
            if (a > 255)  a = 255;

            // 坐标必须**从角往里**摆：右边/下边要翻过来（w - bx - block）。
            // 如果一律写 ox + bx，衰减方向就反了——亮的地方会跑到屏幕中段、
            // 真正该亮的角上反而是黑的。这个错误实测抓到过（左角亮、右角全黑）。
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
                    if (a <= q[3]) continue;      // 已经有更实的内容就别盖
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

// ---------------------------------------------------------------- 反色 ----
bool ApplyInvertMatrix(bool on)
{
    if (!g_magReady) return false;

    MAGCOLOREFFECT eff;

    if (on)
    {
        // 对角线上 R/G/B 取负 = 全屏反色
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
        // 单位矩阵 = 恢复正常
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

// ---------------------------------------------------------------- 渲染 ----
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

    // 注意：纯色底也要算进「有内容」，否则只有它的时候窗口会被直接隐藏。
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

    // 窗口位置跟着虚拟屏走（多屏/改分辨率时）
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

    // ---- 1. 底色：纯色底 > 闪屏 > 透明 ----
    if (g_solid)
    {
        // 完全不透明：桌面完全看不见
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

    // ---- 2. 雪花：按概率把像素换成灰度噪点 ----
    if (noiseActive)
    {
        const int density = g_noise;

        // 有纯色底的时候，噪点必须**不透明**。
        // 原来这里让灰度值同时充当 alpha（p[3]=v），半透明的噪点会把底下的
        // 桌面/窗口透出来——实测黑幕上有 31% 的像素能直接看到背后的窗口，
        // 「黑屏全覆盖」就等于白铺了。所以有纯色底时改成「底色 + 灰度」、
        // alpha 拉满；没有纯色底时保持原来的半透明雪花（那时本来就该透）。
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

    // ---- 3. 四角红光 ----
    // 只遍历四个角区、而且按块写，中间那一大片完全不碰——
    // 90 秒的持续演出里这点开销可以忽略。
    // 写进去的是**直通 alpha**（未预乘），第 4 步会统一预乘。
    if (glowActive)
    {
        ++g_glowFrame;

        // 呼吸：整体强度在 88% ~ 100% 之间缓慢起伏
        const int pulse = 224 + (int)(31 * ((g_glowFrame / 4) & 1));
        RenderCornerGlow((BYTE*)g_bits, w, h,
                         g_glowStrength * pulse / 255, g_glowColor);

        // 极少量彩色亮点，压在红光之上
        RenderAccentPixels((BYTE*)g_bits, w, h);
    }

    // ---- 4. UpdateLayeredWindow 要预乘 alpha ----
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

    // ---- 反色脉冲到点自动还原 ----
    if (g_invertOffAt != 0 && now >= g_invertOffAt)
    {
        g_invertOffAt = 0;
        fx::SetInvert(false);      // Tick 在匿名 namespace 里，要写全名
    }

    // ---- 闪屏相位变化 ----
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
        return HTTRANSPARENT;     // 鼠标穿透
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

} // namespace

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

    // 反色能力探测：只初始化，不改矩阵
    g_magReady = (MagInitialize() != FALSE);
    elog::Write(L"[fx] 就绪，反色支持 %d", (int)g_magReady);

    return true;
}

void Stop()
{
    // 先把反色还原，再拆窗口——顺序不能反，否则可能留下反色的屏幕
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

    g_flashColor   = color;
    g_flashTimes   = (times < 1) ? 1 : times;
    g_flashOnMs    = (onMs  < 16) ? 16 : onMs;
    g_flashOffMs   = (offMs < 16) ? 16 : offMs;
    g_flashPhaseOn = true;
    g_flashNext    = GetTickCount() + g_flashOnMs;

    elog::Write(L"[fx] 闪屏 x%d color=%06lX on=%lums off=%lums",
                g_flashTimes, (unsigned long)color, g_flashOnMs, g_flashOffMs);
}

void StopFlash()
{
    g_flashTimes   = 0;
    g_flashPhaseOn = false;
}

bool Flashing() { return g_flashTimes > 0; }

void SetNoise(int level)
{
    if (level < 0)   level = 0;
    if (level > 255) level = 255;
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
    if (ms < 16) ms = 16;
    if (!SetInvert(true)) return;          // 不支持就什么都不做
    g_invertOffAt = GetTickCount() + ms;
    elog::Write(L"[fx] 反色脉冲 %lums", ms);
}

void SetSolid(bool on, COLORREF color)
{
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

    if (g_glow == on && g_glowColor == color && g_glowStrength == strength) return;

    g_glow         = on;
    g_glowColor    = color;
    g_glowStrength = strength;
    if (on) g_glowFrame = 0;          // 从相位 0 起跑，每次开场图案一致
    elog::Write(L"[fx] 边缘红光 %s color=%06lX 强度=%d",
                on ? L"开启" : L"关闭", (unsigned long)color, strength);
}

bool EdgeGlow() { return g_glow; }

// 把当前 fx 图层的位图（含 alpha）存成 PNG。调外观用。
//
// 为什么非要这个：fx 和 face 都是**全屏置顶的分层窗口**，光靠截屏根本没法
// 把它单独拎出来看——底下的桌面、浏览器窗口自带的红/蓝/黄像素会把
// 「四角红光」和「彩色噪点」的统计完全淹掉（实测：彩色噪点计数被
// 背景窗口的 5 万个蓝色像素盖住，根本读不出数）。
// 存成 PNG 之后就能直接按像素统计这一层自己的内容。
bool DumpLayer(const wchar_t* path)
{
    if (!g_overlay)
    {
        elog::Write(L"[fx] 图层导出失败：覆盖层窗口还没建");
        return false;
    }

    // 后备缓冲是在 Render() 里按需分配的，所以必须先渲染再检查尺寸
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
            // 管线里存的是**预乘过**的像素，直接存出来颜色会偏暗，
            // 这里反预乘回去，方便直接读颜色。
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

} // namespace fx
