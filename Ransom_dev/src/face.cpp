
























#include "face.h"

#include "assets.h"
#include "image_blob.h"
#include "entity_log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

using namespace Gdiplus;

#pragma comment(lib, "gdiplus.lib")

namespace {

    const wchar_t* kFaceClass = L"RansomFaceWnd";
    const wchar_t* kDriverClass = L"RansomFaceDriver";

    const UINT_PTR kTickId = 1;
    const UINT     kTickMs = 33;


    const int kGenW = 56;
    const int kGenH = 64;




    const float kStopSizeFrac = 0.20f;


    const DWORD kAttackSmallMs = 100;


    const REAL kAttackBigFrac = 1.02f;


    const DWORD kIdleBlackMs = 100;


    struct Surface {
        Bitmap* bmp = nullptr;
        int     w = 0;
        int     h = 0;

        bool Ok() const { return bmp != nullptr && w > 0 && h > 0; }
        void Free() { delete bmp; bmp = nullptr; w = h = 0; }
    };

    Surface g_idle;
    Surface g_gape;
    Surface g_cruc;
    Surface g_stop;



    Surface g_warpA;


    const int kPopupCount = 5;
    Surface   g_popup[kPopupCount];
    int       g_popupLoaded = 0;



    const int kLoadFrameCount = 10;
    Surface   g_loadBg;
    Surface   g_loadFrames[kLoadFrameCount];
    int       g_loadLoaded = 0;
    DWORD     g_loadStart = 0;
    DWORD     g_loadFillMs = 0;
    DWORD     g_loadFinishMs = 0;
    DWORD     g_loadMs = 0;
    bool      g_loadFinLogged = false;





    const double kLoadEaseB = 0.80;
    double LoadProgress(double t)
    {
        if (t <= 0.0) return 0.0;
        if (t >= 1.0) return 1.0;
        return t + (kLoadEaseB / 6.283185307179586) * sin(6.283185307179586 * t);
    }


    const int kLoadJitterPx = 4;




    const REAL kLoadBarScale = 0.70f;


    const int kLoadNoiseBlocks = 520;
    const int kLoadNoiseLevels = 6;
    const int kLoadNoiseBase = 96;
    const int kLoadNoiseStep = 16;


    const wchar_t* kLoadFinishText = L"FINISH\uff01";


    const DWORD kLoadFrameGuardMs = 34;

    bool g_usingAssets = false;




    bool g_attackVeil = false;







    bool g_attackSmallToBig = true;

    HWND      g_wnd = nullptr;
    HWND      g_driver = nullptr;
    HINSTANCE g_hInst = nullptr;

    HDC       g_memDC = nullptr;
    HBITMAP   g_bmp = nullptr;
    HGDIOBJ   g_oldBmp = nullptr;
    void* g_bits = nullptr;
    HDC       g_screen = nullptr;
    int       g_w = 0, g_h = 0;

    face::Mode g_mode = face::FACE_HIDDEN;
    DWORD      g_modeEnd = 0;
    DWORD      g_modeStart = 0;
    DWORD      g_frame = 0;

    int        g_idleX = 0, g_idleY = 0, g_idleSize = 0;


    bool  g_idleShowStop = false;



    DWORD g_idleStopHideAt = 0;


    bool  g_idleBlackout = false;


    void VirtualRect(RECT& r)
    {
        r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
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
        if (g_bmp) { DeleteObject(g_bmp); g_bmp = nullptr; }
        if (g_memDC) { DeleteDC(g_memDC);   g_memDC = nullptr; }
        if (g_screen) { ReleaseDC(nullptr, g_screen); g_screen = nullptr; }
        g_bits = nullptr;
        g_w = g_h = 0;
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
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        g_bmp = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
        if (!g_bmp || !g_bits) { FreeBuffers(); return false; }

        g_oldBmp = SelectObject(g_memDC, g_bmp);
        g_w = w; g_h = h;
        return true;
    }

    void PremultiplyAll(void* bits, int w, int h)
    {
        BYTE* p = (BYTE*)bits;
        const size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; ++i, p += 4)
        {
            const BYTE a = p[3];
            if (a == 255) continue;
            if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
            p[0] = (BYTE)(p[0] * a / 255);
            p[1] = (BYTE)(p[1] * a / 255);
            p[2] = (BYTE)(p[2] * a / 255);
        }
    }





    Surface LoadPng(const wchar_t* fileName)
    {
        Surface s;

        assets::Blob blob;
        if (!assets::Get(assets::KIND_IMAGE, fileName, blob)) return s;

        Bitmap* copy = image_blob::Decode(blob.Data(), blob.Size());
        if (!copy) return s;

        s.bmp = copy;
        s.w = (int)copy->GetWidth();
        s.h = (int)copy->GetHeight();
        return s;
    }


    void BuildFallbackFace(Bitmap* bmp, bool gaping)
    {
        const int LW = kGenW, LH = kGenH;
        Rect r(0, 0, LW, LH);
        BitmapData d;
        if (bmp->LockBits(&r, ImageLockModeWrite, PixelFormat32bppARGB, &d) != Ok) return;

        BYTE* base = (BYTE*)d.Scan0;

        for (int y = 0; y < LH; ++y)
            for (int x = 0; x < LW; ++x)
            {
                const double nx = (x + 0.5) / LW * 2.0 - 1.0;
                const double ny = (y + 0.5) / LH * 2.0 - 1.0;
                const double ang = atan2(ny, nx);
                const double wob = 0.11 * sin(ang * 3.0 + 1.3) + 0.06 * sin(ang * 5.0 - 0.7);
                const double rad = 1.0 + wob;
                const double rr = sqrt(nx * nx * 1.30 + ny * ny * 0.88);

                BYTE a = 0; int v = 0;
                if (rr < rad)
                {
                    a = 255;
                    const double edge = (rad - rr) / rad;
                    double lum = 0.54 + 0.26 * edge;
                    lum *= 0.88 + 0.12 * (ny * 0.5 + 0.5);
                    v = (int)(255.0 * lum);
                }
                v += (rand() % 9) - 4;
                if (v < 0) v = 0; if (v > 255) v = 255;

                BYTE* p = base + (size_t)y * d.Stride + (size_t)x * 4;
                p[0] = p[1] = p[2] = (BYTE)v; p[3] = a;
            }

        {
            const double ex[2] = { -0.35, 0.35 };
            const double ey = -0.17;
            for (int e = 0; e < 2; ++e)
                for (int y = 0; y < LH; ++y)
                    for (int x = 0; x < LW; ++x)
                    {
                        const double nx = (x + 0.5) / LW * 2.0 - 1.0;
                        const double ny = (y + 0.5) / LH * 2.0 - 1.0;
                        const double dx = (nx - ex[e]) / 0.31, dy = (ny - ey) / 0.25;
                        if (dx * dx + dy * dy < 1.0)
                        {
                            BYTE* p = base + (size_t)y * d.Stride + (size_t)x * 4;
                            if (p[3] > 0) p[0] = p[1] = p[2] = 0;
                        }
                    }
        }

        for (int y = 0; y < LH; ++y)
            for (int x = 0; x < LW; ++x)
            {
                const double nx = (x + 0.5) / LW * 2.0 - 1.0;
                const double ny = (y + 0.5) / LH * 2.0 - 1.0;
                bool in = false;
                if (gaping) { const double dx = nx / 0.46, dy = (ny - 0.50) / 0.34; in = (dx * dx + dy * dy < 1.0); }
                else { const double wob = 0.012 * sin(nx * 9.0); in = (fabs(ny - (0.38 + wob)) < 0.030) && (fabs(nx) < 0.30); }
                if (in)
                {
                    BYTE* p = base + (size_t)y * d.Stride + (size_t)x * 4;
                    if (p[3] > 0) p[0] = p[1] = p[2] = 0;
                }
            }

        bmp->UnlockBits(&d);
    }

    void BuildFallbackStop(Bitmap* bmp)
    {
        const int N = 64;
        Graphics g(bmp);
        g.SetSmoothingMode(SmoothingModeNone);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));

        PointF pts[8];
        const REAL R = 30.0f;
        for (int i = 0; i < 8; ++i)
        {
            const double a = (22.5 + i * 45.0) * 3.14159265358979 / 180.0;
            pts[i].X = (REAL)(32.0 + R * cos(a));
            pts[i].Y = (REAL)(32.0 + R * sin(a));
        }

        SolidBrush red(Color(255, 208, 32, 32));
        g.FillPolygon(&red, pts, 8);
        Pen white(Color(255, 245, 245, 245), 3.0f);
        g.DrawPolygon(&white, pts, 8);

        Font f(L"Arial", 13.0f, FontStyleBold, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush wb(Color(255, 250, 250, 250));
        RectF rc(0, 0, (REAL)N, (REAL)N);
        g.DrawString(L"STOP", -1, &f, rc, &sf, &wb);
    }

    Surface MakeGenerated(bool gaping, bool stop)
    {
        Surface s;
        s.w = stop ? 64 : kGenW;
        s.h = stop ? 64 : kGenH;
        s.bmp = new Bitmap(s.w, s.h, PixelFormat32bppARGB);
        if (!s.bmp) { s.w = s.h = 0; return s; }

        if (stop) BuildFallbackStop(s.bmp);
        else      BuildFallbackFace(s.bmp, gaping);

        return s;
    }




    void RenderWhiteFlashRed(Surface& dst, const Surface& src, DWORD frame)
    {
        if (!src.Ok()) return;

        if (dst.w != src.w || dst.h != src.h)
        {
            dst.Free();
            dst.bmp = new Bitmap(src.w, src.h, PixelFormat32bppARGB);
            if (!dst.bmp) { dst.w = dst.h = 0; return; }
            dst.w = src.w;
            dst.h = src.h;
        }

        Rect r(0, 0, src.w, src.h);
        BitmapData ds, ss;
        if (dst.bmp->LockBits(&r, ImageLockModeWrite, PixelFormat32bppARGB, &ds) != Ok) return;
        if (src.bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &ss) != Ok)
        {
            dst.bmp->UnlockBits(&ds);
            return;
        }

        bool redOn = ((frame / 4) & 1) != 0;
        if ((rand() & 3) == 0) redOn = !redOn;

        const int kWhite = 190;

        for (int y = 0; y < src.h; ++y)
        {
            const BYTE* srow = (BYTE*)ss.Scan0 + (size_t)y * ss.Stride;
            BYTE* drow = (BYTE*)ds.Scan0 + (size_t)y * ds.Stride;

            for (int x = 0; x < src.w; ++x)
            {
                const BYTE* s = srow + (size_t)x * 4;
                BYTE* d = drow + (size_t)x * 4;

                const BYTE b = s[0], g = s[1], rr = s[2], a = s[3];

                if (a == 0)
                {
                    d[0] = d[1] = d[2] = d[3] = 0;
                    continue;
                }

                if (redOn && rr >= kWhite && g >= kWhite && b >= kWhite)
                {
                    const int lum = (rr + g + b) / 3;
                    const int gb = lum / 5;
                    d[2] = (BYTE)lum;
                    d[1] = (BYTE)gb;
                    d[0] = (BYTE)gb;
                    d[3] = a;
                }
                else
                {
                    d[0] = b; d[1] = g; d[2] = rr; d[3] = a;
                }
            }
        }

        src.bmp->UnlockBits(&ss);
        dst.bmp->UnlockBits(&ds);
    }

    void SprinkleRedStatic(BYTE* bits, int w, int h, int density, int alpha)
    {
        BYTE* p = (BYTE*)bits;
        const size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; ++i, p += 4)
        {
            if ((rand() & 0xFF) < density)
            {
                const int r = 120 + rand() % 136;
                p[0] = 0; p[1] = 0; p[2] = (BYTE)r; p[3] = (BYTE)alpha;
            }
        }
    }


    void Render()
    {
        if (!g_wnd) return;

        if (g_mode == face::FACE_HIDDEN) { ShowWindow(g_wnd, SW_HIDE); return; }

        RECT vr;
        VirtualRect(vr);
        const int w = vr.right - vr.left;
        const int h = vr.bottom - vr.top;
        if (w <= 0 || h <= 0) return;
        if (!EnsureBuffers(w, h)) return;

        RECT cur;
        GetWindowRect(g_wnd, &cur);
        if (cur.left != vr.left || cur.top != vr.top ||
            cur.right != vr.right || cur.bottom != vr.bottom)
        {
            SetWindowPos(g_wnd, HWND_TOPMOST, vr.left, vr.top, w, h, SWP_NOACTIVATE);
        }

        memset(g_bits, 0, (size_t)w * h * 4);

        {
            Graphics g(g_memDC);
            g.SetSmoothingMode(SmoothingModeNone);
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);

            switch (g_mode)
            {
            case face::FACE_LOADING:
            {
                SolidBrush veil(Color(255, 80, 0, 0));
                g.FillRectangle(&veil, 0.0f, 0.0f, (REAL)w, (REAL)h);

                {
                    SolidBrush* nb[kLoadNoiseLevels];
                    for (int i = 0; i < kLoadNoiseLevels; ++i)
                    {
                        const int v = kLoadNoiseBase + i * kLoadNoiseStep;
                        nb[i] = new SolidBrush(Color(255, v, v / 8, v / 8));
                    }
                    for (int i = 0; i < kLoadNoiseBlocks; ++i)
                    {
                        const int a1 = rand() % kLoadNoiseLevels;
                        const int a2 = rand() % kLoadNoiseLevels;
                        const int use = (a1 < a2) ? a1 : a2;
                        const REAL nx = (REAL)(rand() % w);
                        const REAL ny = (REAL)(rand() % h);
                        g.FillRectangle(nb[use], nx, ny, 2.0f, 2.0f);
                    }
                    for (int i = 0; i < kLoadNoiseLevels; ++i) delete nb[i];
                }

                if (g_loadLoaded > 0)
                {
                    const DWORD now = GetTickCount();
                    const DWORD el = (g_loadFillMs > 0) ? (now - g_loadStart) : (DWORD)-1;
                    const bool  fin = (g_loadFillMs == 0) ||
                        (el + kLoadFrameGuardMs >= g_loadFillMs);

                    if (fin && !g_loadFinLogged)
                    {
                        g_loadFinLogged = true;
                        elog::Write(L"[face] 加载条走满（%lu ms），切到 FINISH！停 %lu ms",
                            (unsigned long)el, (unsigned long)g_loadFinishMs);
                    }

                    double t = (g_loadFillMs > 0) ? (double)el / (double)g_loadFillMs : 1.0;
                    if (t > 1.0) t = 1.0;

                    const double prog = fin ? 1.0 : LoadProgress(t);




                    const REAL grow = 1.0f + 0.05f * (REAL)prog;




                    const REAL barJx = (REAL)((rand() % (kLoadJitterPx * 2 + 1)) - kLoadJitterPx);
                    const REAL barJy = (REAL)((rand() % (kLoadJitterPx * 2 + 1)) - kLoadJitterPx);
                    const REAL txtJx = (REAL)((rand() % (kLoadJitterPx * 2 + 1)) - kLoadJitterPx);
                    const REAL txtJy = (REAL)((rand() % (kLoadJitterPx * 2 + 1)) - kLoadJitterPx);

                    const int   bw0 = (g_loadBg.Ok() ? g_loadBg.w : 962);
                    const int   bh0 = (g_loadBg.Ok() ? g_loadBg.h : 68);
                    REAL sc = kLoadBarScale;
                    const REAL maxW = (REAL)w * 0.70f;
                    if ((REAL)bw0 * sc > maxW) sc = maxW / (REAL)bw0;
                    sc *= grow;

                    const REAL bw = (REAL)bw0 * sc;
                    const REAL bh = (REAL)bh0 * sc;
                    const REAL bx = ((REAL)w - bw) / 2.0f + barJx;
                    const REAL by = (REAL)h * 0.5f + 40.0f * sc + barJy;

                    g.SetInterpolationMode(InterpolationModeBilinear);

                    if (g_loadBg.Ok())
                        g.DrawImage(g_loadBg.bmp, RectF(bx, by, bw, bh),
                            0.0f, 0.0f, (REAL)g_loadBg.w, (REAL)g_loadBg.h, UnitPixel);

                    int fi = (int)(prog * kLoadFrameCount);
                    if (fi < 0) fi = 0;
                    if (fi >= kLoadFrameCount) fi = kLoadFrameCount - 1;
                    if (g_loadFrames[fi].Ok())
                        g.DrawImage(g_loadFrames[fi].bmp, RectF(bx, by, bw, bh),
                            0.0f, 0.0f,
                            (REAL)g_loadFrames[fi].w, (REAL)g_loadFrames[fi].h,
                            UnitPixel);




                    std::wstring txt;
                    if (fin) txt = kLoadFinishText;
                    else
                    {
                        const int dots = (int)((now / 250) % 4);
                        txt = L"DOWNLOADING";
                        for (int i = 0; i < dots; ++i) txt += L'.';
                    }

                    const REAL fs = 34.0f * sc;
                    Font f(L"Microsoft YaHei", fs, FontStyleBold, UnitPixel);

                    StringFormat sf;
                    sf.SetAlignment(StringAlignmentCenter);
                    sf.SetLineAlignment(StringAlignmentCenter);


                    const RectF baseBox(bx, by - fs - 22.0f * sc, bw, fs + 14.0f * sc);
                    const RectF box(baseBox.X + txtJx, baseBox.Y + txtJy,
                        baseBox.Width, baseBox.Height);




                    bool redOn = ((g_frame / 4) & 1) != 0;
                    if ((rand() & 3) == 0) redOn = !redOn;


                    SolidBrush outline(Color(255, 216, 24, 24));
                    const REAL off = (fs > 40.0f) ? 3.0f : 2.0f;
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            if (dx == 0 && dy == 0) continue;
                            const RectF o(box.X + dx * off, box.Y + dy * off,
                                box.Width, box.Height);
                            g.DrawString(txt.c_str(), -1, &f, o, &sf, &outline);
                        }


                    SolidBrush main(redOn ? Color(255, 235, 24, 24)
                        : Color(255, 245, 245, 245));
                    g.DrawString(txt.c_str(), -1, &f, box, &sf, &main);
                }
                break; 
            }

            case face::FACE_IDLE:
            {

                const DWORD el = (g_modeStart != 0) ? (GetTickCount() - g_modeStart) : 0;
                const bool blackout = g_idleBlackout && (el < kIdleBlackMs);


                if (g_idle.Ok())
                {
                    RectF rc((REAL)(g_idleX - vr.left),
                        (REAL)(g_idleY - vr.top),
                        (REAL)g_idleSize, (REAL)g_idleSize);

                    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                    g.SetPixelOffsetMode(PixelOffsetModeHalf);

                    if (blackout)
                    {

                        ColorMatrix cm = {
                            0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0,
                            0, 0, 0, 1, 0,
                            0, 0, 0, 0, 1
                        };
                        ImageAttributes ia;
                        ia.SetColorMatrix(&cm);
                        g.DrawImage(g_idle.bmp, rc, 0.0f, 0.0f,
                            (REAL)g_idle.w, (REAL)g_idle.h, UnitPixel, &ia);
                    }
                    else
                    {
                        g.DrawImage(g_idle.bmp, rc, 0.0f, 0.0f,
                            (REAL)g_idle.w, (REAL)g_idle.h, UnitPixel);
                    }
                }





                bool stopVisible = g_idleShowStop;
                if (stopVisible && g_idleStopHideAt != 0 &&
                    GetTickCount() >= g_idleStopHideAt)
                    stopVisible = false;

                if (stopVisible && g_stop.Ok())
                {
                    const int size = (int)(h * kStopSizeFrac);
                    if (size > 0)
                    {
                        const int cx = g_idleX + g_idleSize / 2;
                        const int cy = g_idleY + g_idleSize / 2;
                        RectF rc((REAL)(cx - size / 2 - vr.left),
                            (REAL)(cy - size / 2 - vr.top),
                            (REAL)size, (REAL)size);

                        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
                        g.SetPixelOffsetMode(PixelOffsetModeHalf);
                        g.DrawImage(g_stop.bmp, rc, 0.0f, 0.0f,
                            (REAL)g_stop.w, (REAL)g_stop.h, UnitPixel);
                    }
                }
                break;
            }

            case face::FACE_STOP:
            {

                SprinkleRedStatic((BYTE*)g_bits, w, h, 26, 150);
                const int size = (int)(h * kStopSizeFrac);
                RectF rc((REAL)(w - size) / 2.0f, (REAL)(h - size) / 2.0f, (REAL)size, (REAL)size);
                g.SetInterpolationMode(InterpolationModeNearestNeighbor);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                if (g_stop.Ok())
                    g.DrawImage(g_stop.bmp, rc, 0.0f, 0.0f,
                        (REAL)g_stop.w, (REAL)g_stop.h, UnitPixel);
                break;
            }

            case face::FACE_ATTACK:
            {
                if (g_attackVeil)
                {
                    SolidBrush veil(Color(255, 80, 0, 0));
                    g.FillRectangle(&veil, 0.0f, 0.0f, (REAL)w, (REAL)h);
                }

                SprinkleRedStatic((BYTE*)g_bits, w, h, 55, 190);


                const int sx = (rand() % 17) - 8;
                const int sy = (rand() % 17) - 8;







                const DWORD el = (g_modeStart != 0) ? (GetTickCount() - g_modeStart) : 0;
                const REAL frac = (!g_attackSmallToBig || el >= kAttackSmallMs)
                    ? kAttackBigFrac
                    : kStopSizeFrac;

                const REAL fh = (REAL)h * frac;
                const REAL fw = fh * ((REAL)(g_gape.Ok() ? g_gape.w : 1) /
                    (REAL)(g_gape.Ok() ? g_gape.h : 1));
                RectF rc((REAL)w / 2.0f - fw / 2.0f + sx,
                    (REAL)h / 2.0f - fh / 2.0f + sy, fw, fh);

                RenderWhiteFlashRed(g_warpA, g_gape, g_frame);
                if (g_warpA.Ok())
                {
                    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
                    g.SetPixelOffsetMode(PixelOffsetModeHalf);
                    g.DrawImage(g_warpA.bmp, rc, 0.0f, 0.0f,
                        (REAL)g_warpA.w, (REAL)g_warpA.h, UnitPixel);
                }
                break;
            }

            case face::FACE_THANKS:
            {
                const int bw = 460, bh = 190;
                RectF box((REAL)(w - bw) / 2.0f, (REAL)(h - bh) / 2.0f, (REAL)bw, (REAL)bh);

                SolidBrush body(Color(252, 240, 240, 240));
                g.FillRectangle(&body, box);
                Pen edge(Color(255, 90, 90, 90), 1.0f);
                g.DrawRectangle(&edge, box.X, box.Y, box.Width, box.Height);

                Font ft(L"Microsoft YaHei", 20.0f, FontStyleBold, UnitPixel);
                Font fb(L"Microsoft YaHei", 15.0f, FontStyleRegular, UnitPixel);
                SolidBrush dark(Color(255, 30, 30, 30));
                SolidBrush mid(Color(255, 70, 70, 70));

                g.DrawString(L"谢谢。", -1, &ft, PointF(box.X + 32.0f, box.Y + 40.0f), &dark);
                g.DrawString(L"你的物品已解冻。", -1, &fb, PointF(box.X + 34.0f, box.Y + 92.0f), &mid);
                g.DrawString(L"……暂时。", -1, &fb, PointF(box.X + 34.0f, box.Y + 122.0f), &mid);
                break;
            }

            default: break;
            }
        }

        PremultiplyAll(g_bits, w, h);

        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;

        SIZE  size = { w, h };
        POINT src = { 0, 0 };
        UpdateLayeredWindow(g_wnd, g_screen, nullptr, &size,
            g_memDC, &src, 0, &bf, ULW_ALPHA);

        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void Tick()
    {
        ++g_frame;

        if (g_modeEnd != 0 && GetTickCount() >= g_modeEnd)
        {
            if (g_mode == face::FACE_LOADING)
            {
                elog::Write(L"[face] 加载画面结束（走条 %lu + FINISH %lu ms）",
                    (unsigned long)g_loadFillMs,
                    (unsigned long)g_loadFinishMs);
            }
            g_modeEnd = 0;
            g_mode = face::FACE_HIDDEN;
            g_idleShowStop = false;
            g_idleStopHideAt = 0;
        }
        Render();
    }

    LRESULT CALLBACK FaceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST:  return HTTRANSPARENT;
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
        case WM_ERASEBKGND: return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    std::wstring JoinPath(const wchar_t* dir, const wchar_t* file)
    {
        std::wstring p = dir ? dir : L"";
        if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
        p += file;
        return p;
    }

}

namespace face {

    bool Start(HINSTANCE hInst)
    {
        if (g_driver) return true;
        g_hInst = hInst;

        WNDCLASSEXW fc = { sizeof(WNDCLASSEXW) };
        fc.lpfnWndProc = FaceProc;
        fc.hInstance = hInst;
        fc.hCursor = nullptr;
        fc.hbrBackground = nullptr;
        fc.lpszClassName = kFaceClass;
        RegisterClassExW(&fc);

        WNDCLASSEXW dc = { sizeof(WNDCLASSEXW) };
        dc.lpfnWndProc = DriverProc;
        dc.hInstance = hInst;
        dc.hCursor = nullptr;
        dc.hbrBackground = nullptr;
        dc.lpszClassName = kDriverClass;
        RegisterClassExW(&dc);

        RECT vr;
        VirtualRect(vr);

        g_wnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            kFaceClass, L"", WS_POPUP,
            vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top,
            nullptr, nullptr, hInst, nullptr);
        if (!g_wnd) { elog::Write(L"[face] 窗口创建失败, err=%lu", GetLastError()); return false; }

        g_driver = CreateWindowExW(0, kDriverClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, hInst, nullptr);
        if (!g_driver)
        {
            elog::Write(L"[face] 驱动窗口创建失败, err=%lu", GetLastError());
            DestroyWindow(g_wnd); g_wnd = nullptr;
            return false;
        }



        g_idle = LoadPng(L"A-90_IDLE.png");
        g_gape = LoadPng(L"A-90_JUMPSCARE.png");
        g_cruc = LoadPng(L"A90Crucifixion.png");
        g_stop = LoadPng(L"Blocka90.png");

        const int loaded = (int)g_idle.Ok() + (int)g_gape.Ok() + (int)g_cruc.Ok() + (int)g_stop.Ok();
        g_usingAssets = (loaded > 0);

        elog::Write(L"[face] 脸素材 —— 载入 %d / 4 张（来源：%s）",
                    loaded, assets::Where(assets::KIND_IMAGE, L"A-90_IDLE.png").c_str());

        g_popupLoaded = 0;
        for (int i = 0; i < kPopupCount; ++i)
        {
            wchar_t name[32];
            swprintf_s(name, L"RansomPopup%d.png", i + 1);
            g_popup[i] = LoadPng(name);
            if (g_popup[i].Ok()) ++g_popupLoaded;
        }
        elog::Write(L"[face] RansomPopup 子窗口图 —— 载入 %d / %d 张",
            g_popupLoaded, kPopupCount);

        g_loadBg = LoadPng(L"loadingBG.png");
        g_loadLoaded = 0;
        for (int i = 0; i < kLoadFrameCount; ++i)
        {
            wchar_t name[32];
            swprintf_s(name, L"loading_%d.png", i + 1);
            g_loadFrames[i] = LoadPng(name);
            if (g_loadFrames[i].Ok()) ++g_loadLoaded;
        }
        elog::Write(L"[face] 加载画面素材 —— 底条=%d，填充帧 %d / %d",
            g_loadBg.Ok() ? 1 : 0, g_loadLoaded, kLoadFrameCount);

        if (!g_idle.Ok()) { g_idle = MakeGenerated(false, false); elog::Write(L"[face]   A-90_IDLE 缺失，用程序生成回退"); }
        if (!g_gape.Ok()) { g_gape = MakeGenerated(true, false); elog::Write(L"[face]   A-90_JUMPSCARE 缺失，用程序生成回退"); }
        if (!g_cruc.Ok()) { g_cruc = MakeGenerated(true, false); elog::Write(L"[face]   A90Crucifixion 缺失，用程序生成回退"); }
        if (!g_stop.Ok()) { g_stop = MakeGenerated(false, true);  elog::Write(L"[face]   Blocka90 缺失，用程序生成回退"); }

        g_warpA.bmp = new Bitmap(g_gape.w, g_gape.h, PixelFormat32bppARGB);
        g_warpA.w = g_gape.w;
        g_warpA.h = g_gape.h;

        SetTimer(g_driver, kTickId, kTickMs, nullptr);

        elog::Write(L"[face] 已就绪（%s）", g_usingAssets ? L"使用原版素材" : L"全部程序生成");
        return true;
    }

    void Stop()
    {
        if (g_driver) { KillTimer(g_driver, kTickId); DestroyWindow(g_driver); g_driver = nullptr; }
        if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }

        g_idle.Free(); g_gape.Free(); g_cruc.Free(); g_stop.Free();
        g_warpA.Free();

        for (int i = 0; i < kPopupCount; ++i) g_popup[i].Free();
        g_popupLoaded = 0;

        g_loadBg.Free();
        for (int i = 0; i < kLoadFrameCount; ++i) g_loadFrames[i].Free();
        g_loadLoaded = 0;

        FreeBuffers();
        g_mode = FACE_HIDDEN;
        g_idleShowStop = false;
        g_idleStopHideAt = 0;
        g_idleBlackout = false;
        g_attackVeil = false;
        elog::Write(L"[face] 已停止");
    }

    void SpawnAnywhere(DWORD lifeMs)
    {
        if (!g_driver) return;

        RECT vr;
        VirtualRect(vr);
        const int w = vr.right - vr.left;
        const int h = vr.bottom - vr.top;

        g_idleSize = (int)(h * 0.16);
        if (g_idleSize < 90) g_idleSize = 90;

        const int spanX = (w > g_idleSize) ? (w - g_idleSize) : 1;
        const int spanY = (h > g_idleSize) ? (h - g_idleSize) : 1;
        g_idleX = vr.left + rand() % spanX;
        g_idleY = vr.top + rand() % spanY;

        g_idleShowStop = false;
        g_idleStopHideAt = 0;
        g_idleBlackout = true;

        const DWORD now = GetTickCount();
        g_mode = FACE_IDLE;
        g_modeEnd = lifeMs ? (now + lifeMs) : 0;
        g_modeStart = now;
        g_frame = 0;

        elog::Write(L"[face] 脸浮现于 (%d,%d) 尺寸 %d（前 %lums 纯黑剪影）",
            g_idleX, g_idleY, g_idleSize, (unsigned long)kIdleBlackMs);
        Render();
    }

    void MoveToCenter(DWORD lifeMs)
    {
        if (!g_driver) return;

        RECT vr;
        VirtualRect(vr);
        const int w = vr.right - vr.left;
        const int h = vr.bottom - vr.top;

        g_idleX = vr.left + (w - g_idleSize) / 2;
        g_idleY = vr.top + (h - g_idleSize) / 2;

        g_idleShowStop = false;
        g_idleStopHideAt = 0;
        g_idleBlackout = false;

        const DWORD now = GetTickCount();
        g_mode = FACE_IDLE;
        g_modeEnd = lifeMs ? (now + lifeMs) : 0;
        g_modeStart = now;
        g_frame = 0;

        elog::Write(L"[face] 瞬移到中央 (%d,%d)", g_idleX, g_idleY);
        Render();
    }





    void MoveToCenterWithStop(DWORD headLifeMs, DWORD stopLifeMs)
    {
        if (!g_driver) return;

        RECT vr;
        VirtualRect(vr);
        const int w = vr.right - vr.left;
        const int h = vr.bottom - vr.top;

        g_idleX = vr.left + (w - g_idleSize) / 2;
        g_idleY = vr.top + (h - g_idleSize) / 2;

        g_idleShowStop = true;
        g_idleStopHideAt = (stopLifeMs > 0) ? (GetTickCount() + stopLifeMs) : 0;
        g_idleBlackout = false;

        const DWORD now = GetTickCount();
        g_mode = FACE_IDLE;
        g_modeEnd = headLifeMs ? (now + headLifeMs) : 0;
        g_modeStart = now;
        g_frame = 0;

        elog::Write(L"[face] 瞬移到中央 + 停牌叠加 (%d,%d)（头 %lums，停牌 %lums）",
            g_idleX, g_idleY,
            (unsigned long)headLifeMs, (unsigned long)stopLifeMs);
        Render();
    }

    void ShowHeadAgain(DWORD lifeMs)
    {


        MoveToCenter(lifeMs);
        elog::Write(L"[face] 停牌撤掉，头继续露出 %lums", (unsigned long)lifeMs);
    }

    void ShowStopSign(DWORD lifeMs)
    {
        if (!g_driver) return;

        g_idleShowStop = false;
        g_idleStopHideAt = 0;
        g_idleBlackout = false;

        const DWORD now = GetTickCount();
        g_mode = FACE_STOP;
        g_modeEnd = lifeMs ? (now + lifeMs) : 0;
        g_modeStart = now;
        g_frame = 0;
        elog::Write(L"[face] 停牌闪现（头已隐藏）");
        Render();
    }

    void ShowAttackStill(DWORD lifeMs, bool smallToBig)
    {
        if (!g_driver) return;
        g_attackVeil = true;
        g_attackSmallToBig = smallToBig;
        g_idleShowStop = false;
        g_idleStopHideAt = 0;

        const DWORD now = GetTickCount();
        g_mode = FACE_ATTACK;
        g_modeEnd = lifeMs ? (now + lifeMs) : 0;
        g_modeStart = now;
        g_frame = 0;
        elog::Write(L"[face] jumpscare（%s + 深红底）",
            smallToBig ? L"先小后大" : L"直接全屏");
        Render();
    }

    void ShowLoading(DWORD fillMs, DWORD finishMs)
    {
        if (!g_driver) return;
        g_loadStart = GetTickCount();
        g_loadFillMs = fillMs;
        g_loadFinishMs = finishMs;
        g_loadMs = fillMs + finishMs;
        g_loadFinLogged = false;

        g_idleShowStop = false;
        g_idleStopHideAt = 0;
        g_idleBlackout = false;

        g_mode = FACE_LOADING;
        g_modeEnd = g_loadMs ? (g_loadStart + g_loadMs) : 0;
        g_modeStart = g_loadStart;
        g_frame = 0;
        elog::Write(L"[face] 加载画面（走条 %lu ms + FINISH %lu ms，素材底条=%d 帧=%d）",
            (unsigned long)fillMs, (unsigned long)finishMs,
            g_loadBg.Ok() ? 1 : 0, g_loadLoaded);
        Render();
    }



    void ShowAttackShaking(DWORD lifeMs)
    {
        if (!g_driver) return;
        g_attackVeil = false;
        g_attackSmallToBig = false;
        g_idleShowStop = false;
        g_idleStopHideAt = 0;

        const DWORD now = GetTickCount();
        g_mode = FACE_ATTACK;
        g_modeEnd = lifeMs ? (now + lifeMs) : 0;
        g_modeStart = now;
        g_frame = 0;
        elog::Write(L"[face] jumpscare（先小后大 + 抖动）");
        Render();
    }

    void SpawnIdle(DWORD lifeMs) { SpawnAnywhere(lifeMs); }
    void ShowAttack(DWORD lifeMs) { ShowAttackShaking(lifeMs); }

    void ShowThanks(DWORD lifeMs)
    {
        if (!g_driver) return;
        g_idleShowStop = false;
        g_idleStopHideAt = 0;
        g_idleBlackout = false;

        const DWORD now = GetTickCount();
        g_mode = FACE_THANKS;
        g_modeEnd = lifeMs ? (now + lifeMs) : 0;
        g_modeStart = now;
        g_frame = 0;
        elog::Write(L"[face] 致谢画面");
        Render();
    }

    void Hide()
    {
        if (g_mode == FACE_LOADING)
        {
            elog::Write(L"[face] 加载画面结束（走条 %lu + FINISH %lu ms）",
                (unsigned long)g_loadFillMs,
                (unsigned long)g_loadFinishMs);
        }
        g_mode = FACE_HIDDEN;
        g_modeEnd = 0;
        g_idleShowStop = false;
        g_idleStopHideAt = 0;
        g_idleBlackout = false;
        if (g_wnd) ShowWindow(g_wnd, SW_HIDE);
    }

    Mode Current() { return g_mode; }
    bool Visible() { return g_mode != FACE_HIDDEN; }
    bool LoadingDone() { return g_mode != FACE_LOADING; }

    RECT IdleRect()
    {
        RECT r = { g_idleX, g_idleY, g_idleX + g_idleSize, g_idleY + g_idleSize };
        return r;
    }


    namespace {

        bool SavePng(Bitmap* bmp, const wchar_t* path)
        {
            UINT num = 0, size = 0;
            GetImageEncodersSize(&num, &size);
            if (size == 0) return false;

            std::vector<BYTE> buf(size);
            ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
            GetImageEncoders(num, size, info);

            CLSID clsid; bool found = false;
            for (UINT i = 0; i < num; ++i)
                if (wcscmp(info[i].MimeType, L"image/png") == 0) { clsid = info[i].Clsid; found = true; break; }
            if (!found) return false;

            return bmp->Save(path, &clsid, nullptr) == Ok;
        }

        bool DumpOne(const Surface& src, int scale, const wchar_t* dir, const wchar_t* name)
        {
            if (!src.Ok()) return false;

            const int W = src.w * scale, H = src.h * scale;
            Bitmap out(W, H, PixelFormat32bppARGB);
            {
                Graphics g(&out);
                g.SetInterpolationMode(InterpolationModeNearestNeighbor);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.Clear(Color(255, 38, 38, 44));
                g.DrawImage(src.bmp, Rect(0, 0, W, H), 0, 0, src.w, src.h, UnitPixel);
            }

            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\%s", dir, name);
            return SavePng(&out, path);
        }

    }

    bool DumpAssets(const wchar_t* dir)
    {
        if (!dir || !*dir || !g_idle.Ok()) return false;
        CreateDirectoryW(dir, nullptr);

        bool ok = true;
        ok &= DumpOne(g_idle, 3, dir, L"face_idle.png");
        ok &= DumpOne(g_gape, 3, dir, L"face_gape.png");
        ok &= DumpOne(g_cruc, 2, dir, L"face_crucified.png");
        ok &= DumpOne(g_stop, 3, dir, L"stop_sign.png");

        elog::Write(L"[face] 素材已导出到 %s（成功=%d，来源=%s）",
            dir, (int)ok, g_usingAssets ? L"原版素材" : L"程序生成");
        return ok;
    }

    void BlitFace(HDC hdc, const RECT& rc, bool gape, DWORD          )
    {
        if (!hdc) return;

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        const Surface& src = gape ? g_gape : g_idle;
        if (!src.Ok()) return;

        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        const double srcAspect = (double)src.w / (double)src.h;
        const double dstAspect = (double)w / (double)h;

        REAL dw, dh;
        if (dstAspect > srcAspect) { dh = (REAL)h; dw = (REAL)(h * srcAspect); }
        else { dw = (REAL)w; dh = (REAL)(w / srcAspect); }

        const REAL dx = rc.left + ((REAL)w - dw) / 2.0f;
        const REAL dy = rc.top + ((REAL)h - dh) / 2.0f;

        g.DrawImage(src.bmp, RectF(dx, dy, dw, dh), 0.0f, 0.0f,
            (REAL)src.w, (REAL)src.h, UnitPixel);
    }




    void BlitFaceAlpha(HDC hdc, const RECT& rc, bool gape, float alpha)
    {
        if (!hdc) return;

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        if (alpha < 0.02f) return;

        const Surface& src = gape ? g_gape : g_idle;
        if (!src.Ok()) return;

        Graphics g(hdc);


        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);



        const RectF dst((REAL)rc.left, (REAL)rc.top, (REAL)w, (REAL)h);

        if (alpha >= 0.99f)
        {
            g.DrawImage(src.bmp, dst, 0.0f, 0.0f,
                (REAL)src.w, (REAL)src.h, UnitPixel);
        }
        else
        {

            ColorMatrix cm = {
                1, 0, 0, 0, 0,
                0, 1, 0, 0, 0,
                0, 0, 1, 0, 0,
                0, 0, 0, alpha, 0,
                0, 0, 0, 0, 1
            };
            ImageAttributes ia;
            ia.SetColorMatrix(&cm);
            g.DrawImage(src.bmp, dst, 0.0f, 0.0f,
                (REAL)src.w, (REAL)src.h, UnitPixel, &ia);
        }
    }

    void BlitCrucified(HDC hdc, const RECT& rc, DWORD          )
    {
        if (!hdc || !g_cruc.Ok()) return;

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(g_cruc.bmp, Rect(rc.left, rc.top, w, h),
            0, 0, g_cruc.w, g_cruc.h, UnitPixel);
    }

    bool UsingAssets() { return g_usingAssets; }

    int PopupImageCount() { return g_popupLoaded; }

    bool BlitPopupImage(HDC hdc, const RECT& rc, int index)
    {
        if (!hdc) return false;
        if (index < 0 || index >= kPopupCount) return false;

        const Surface& src = g_popup[index];
        if (!src.Ok()) return false;

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return false;

        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(src.bmp, Rect(rc.left, rc.top, w, h),
            0, 0, src.w, src.h, UnitPixel);
        return true;
    }

    void BlitStopSign(HDC hdc, const RECT& rc, float angleDeg, float alpha)
    {
        if (!hdc || !g_stop.Ok()) return;

        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        if (alpha < 0.02f) return;

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        if (angleDeg != 0.0f)
        {
            const REAL cx = rc.left + (REAL)w / 2.0f;
            const REAL cy = rc.top + (REAL)h / 2.0f;
            g.TranslateTransform(cx, cy);
            g.RotateTransform(angleDeg);
            g.TranslateTransform(-cx, -cy);
        }

        if (alpha >= 0.99f)
        {

            g.DrawImage(g_stop.bmp, Rect(rc.left, rc.top, w, h),
                0, 0, g_stop.w, g_stop.h, UnitPixel);
        }
        else
        {

            ColorMatrix cm = {
                1, 0, 0, 0, 0,
                0, 1, 0, 0, 0,
                0, 0, 1, 0, 0,
                0, 0, 0, alpha, 0,
                0, 0, 0, 0, 1
            };
            ImageAttributes ia;
            ia.SetColorMatrix(&cm);
            g.DrawImage(g_stop.bmp, Rect(rc.left, rc.top, w, h),
                0, 0, g_stop.w, g_stop.h, UnitPixel, &ia);
        }
    }

}