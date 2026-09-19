


















#include "setup_ui.h"

#include "aero_window.h"
#include "audio.h"
#include "entity_log.h"
#include "settings.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

using namespace Gdiplus;

namespace {


    const Color kPanelBg(120, 10, 12, 18);
    const Color kPanelEdge(70, 200, 40, 40);
    const Color kTextMain(238, 238, 242, 248);
    const Color kTextDim(190, 150, 152, 165);
    const Color kTextFaint(150, 118, 120, 132);
    const Color kAccent(255, 200, 26, 30);
    const Color kAccentSoft(120, 200, 26, 30);
    const Color kTrackBg(200, 26, 28, 34);
    const Color kTrackFill(255, 176, 22, 26);
    const Color kKnob(255, 236, 236, 240);
    const Color kKnobActive(255, 255, 92, 92);



    bool Hit(const RECT& r, const POINT& p)
    {
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }



    void SetRectLocal(RECT& r, int x, int y, int w, int h)
    {
        r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    }




    FontFamily* MonoFamily()
    {
        static FontFamily* fam = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            fam = aero::UiFontFamily();
        }
        return fam;
    }


    Font MakeCjkFont(float px, int style = FontStyleRegular)
    {
        return Font(L"Microsoft YaHei", px, style, UnitPixel);
    }


    Font MakeMonoFont(float px, int style = FontStyleRegular)
    {
        FontFamily* fam = MonoFamily();
        if (fam) return Font(fam, px, style, UnitPixel);
        return Font(L"Consolas", px, style, UnitPixel);
    }

    void DrawTextCjk(Graphics& g, const wchar_t* s, const RectF& rc, float px,
                     const Color& c, StringAlignment align = StringAlignmentNear,
                     int style = FontStyleRegular)
    {
        Font f = MakeCjkFont(px, style);
        StringFormat fmt;
        fmt.SetAlignment(align);
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush b(c);
        g.DrawString(s, -1, &f, rc, &fmt, &b);
    }

    void DrawTextMono(Graphics& g, const wchar_t* s, const RectF& rc, float px,
                      const Color& c, StringAlignment align = StringAlignmentNear,
                      int style = FontStyleRegular)
    {
        Font f = MakeMonoFont(px, style);
        StringFormat fmt;
        fmt.SetAlignment(align);
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush b(c);
        g.DrawString(s, -1, &f, rc, &fmt, &b);
    }

    void AddRound(GraphicsPath& p, const RectF& r, REAL radius)
    {
        REAL d = radius * 2.0f;
        if (d > r.Width)  d = r.Width;
        if (d > r.Height) d = r.Height;
        if (d < 0) d = 0;
        p.Reset();
        p.StartFigure();
        p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
        p.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
        p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
        p.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
        p.CloseFigure();
    }

    void FillRound(Graphics& g, const RectF& r, REAL radius, const Color& c)
    {
        GraphicsPath p;
        AddRound(p, r, radius);
        SolidBrush b(c);
        g.FillPath(&b, &p);
    }

    void StrokeRound(Graphics& g, const RectF& r, REAL radius, const Color& c, REAL w)
    {
        GraphicsPath p;
        AddRound(p, r, radius);
        Pen pen(c, w);
        g.DrawPath(&pen, &p);
    }


    void DrawPanel(Graphics& g, const RectF& rc)
    {
        FillRound(g, rc, 10.0f, kPanelBg);
        StrokeRound(g, rc, 10.0f, kPanelEdge, 1.0f);
    }



    bool SavePng(Bitmap& bmp, const wchar_t* path);





    setup_ui::HotkeyFn g_hotkeyFn = nullptr;
    void*              g_hotkeyUser = nullptr;

    namespace setup { void Commit(setup_ui::Verdict v); }


    void HotkeyWhileSetup(void*)
    {
        elog::Write(L"[setup] 设置窗口里按下了安全阀热键，直接退出");
        setup::Commit(setup_ui::VERDICT_ABORT);
    }




    void DrawGrid(Graphics& g, const RectF& rc)
    {
        Pen thin(Color(60, 120, 200, 240), 1.0f);
        Pen bold(Color(150, 120, 200, 240), 1.0f);
        Font f = MakeMonoFont(9.0f);
        SolidBrush label(Color(190, 160, 220, 255));

        for (int x = 0; x < (int)rc.Width; x += 20)
        {
            const REAL px = rc.X + (REAL)x;
            const bool major = (x % 100 == 0);
            g.DrawLine(major ? &bold : &thin, px, rc.Y, px, rc.Y + rc.Height);

            if (major)
            {
                wchar_t s[16];
                swprintf_s(s, L"%d", x);
                RectF t(px + 2.0f, rc.Y + 1.0f, 34.0f, 11.0f);
                g.DrawString(s, -1, &f, t, nullptr, &label);
            }
        }
        for (int y = 0; y < (int)rc.Height; y += 20)
        {
            const REAL py = rc.Y + (REAL)y;
            const bool major = (y % 100 == 0);
            g.DrawLine(major ? &bold : &thin, rc.X, py, rc.X + rc.Width, py);

            if (major)
            {
                wchar_t s[16];
                swprintf_s(s, L"%d", y);
                RectF t(rc.X + 2.0f, py + 1.0f, 34.0f, 11.0f);
                g.DrawString(s, -1, &f, t, nullptr, &label);
            }
        }
    }




    void DrawButton(Graphics& g, const RectF& rc, const RECT& local,
                    const wchar_t* label, bool hot, bool primary)
    {
        RectF b(rc.X + (REAL)local.left, rc.Y + (REAL)local.top,
                (REAL)(local.right - local.left), (REAL)(local.bottom - local.top));

        const Color fill = primary
            ? (hot ? Color(230, 176, 24, 28) : Color(190, 132, 18, 22))
            : (hot ? Color(120, 44, 46, 54) : Color(80, 26, 28, 34));
        const Color edge = primary ? kAccent
            : (hot ? Color(180, 150, 152, 165) : Color(90, 96, 98, 110));

        FillRound(g, b, 7.0f, fill);
        StrokeRound(g, b, 7.0f, edge, 1.4f);

        DrawTextCjk(g, label, b, 15.0f, kTextMain,
            StringAlignmentCenter, FontStyleBold);
    }




    namespace setup {


        const int kW = 520;
        const int kH = 470;
        const int kM = 22;
        const int kLabelH = 18;
        const int kTrackH = 18;
        const int kKnobR = 8;

        const int kRow1Top = 64;
        const int kRow2Top = 124;
        const int kSafeTop = 184;
        const int kSafeRowH = 34;
        const int kRow3Top = 236;
        const int kRow4Top = 296;
        const int kBtnTop = 356;
        const int kBtnH = 42;
        const int kBtnGap = 14;
        const int kBtnW = 150;
        const int kHintTop = 412;

        const int kTrackLeft = kM + 18;
        const int kTrackRight = kW - kM - 18;
        const int kTrackW = kTrackRight - kTrackLeft;



        const int kLoMs = settings::kIntervalMinMs;
        const int kHiMs = settings::kIntervalMaxMs;


        const int kGoldLo = settings::kGoldMin;
        const int kGoldHi = settings::kGoldMax;

        const int kVolStep = 5;






        int MsStep()
        {
            const int span = kHiMs - kLoMs;
            int step = span / 90;
            if (step < 20) step = 20;
            return step;
        }


        HWND  g_hwnd = nullptr;
        bool  g_done = false;
        setup_ui::Verdict g_verdict = setup_ui::VERDICT_ERROR;
        int   g_panicVk = 'Q';

        enum Hot { HOT_NONE = 0, HOT_RESET, HOT_START };
        int   g_hot = HOT_NONE;


        int   g_drag = 0;



        settings::Set g_edit;

        RECT BgmTrack() { RECT r; SetRectLocal(r, kTrackLeft, kRow1Top + kLabelH + 2, kTrackW, kTrackH); return r; }
        RECT SfxTrack() { RECT r; SetRectLocal(r, kTrackLeft, kRow2Top + kLabelH + 2, kTrackW, kTrackH); return r; }
        RECT IntTrack() { RECT r; SetRectLocal(r, kTrackLeft, kRow3Top + kLabelH + 2, kTrackW, kTrackH); return r; }
        RECT GoldTrack() { RECT r; SetRectLocal(r, kTrackLeft, kRow4Top + kLabelH + 2, kTrackW, kTrackH); return r; }

        RECT SafeBox()
        {
            RECT r;
            SetRectLocal(r, kM, kSafeTop + (kSafeRowH - 20) / 2, 20, 20);
            return r;
        }



        RECT SafeHit()
        {
            RECT r;
            SetRectLocal(r, kM - 4, kSafeTop, 430, kSafeRowH);
            return r;
        }

        RECT ResetBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW * 2 - kBtnGap, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        RECT StartBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW, kBtnTop, kBtnW, kBtnH);
            return r;
        }


        int VolFromX(int x)
        {
            const RECT t = BgmTrack();
            double f = (double)(x - t.left) / (double)(t.right - t.left);
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            return (int)std::lround(f * settings::kVolMax);
        }

        int VolToX(int v, const RECT& t)
        {
            const double f = (double)v / (double)settings::kVolMax;
            return t.left + (int)std::lround(f * (t.right - t.left));
        }

        int MsFromX(int x)
        {
            const RECT t = IntTrack();
            double f = (double)(x - t.left) / (double)(t.right - t.left);
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            return kLoMs + (int)std::lround(f * (kHiMs - kLoMs));
        }

        int MsToX(int ms)
        {
            const RECT t = IntTrack();
            const double f = (double)(ms - kLoMs) / (double)(kHiMs - kLoMs);
            return t.left + (int)std::lround(f * (t.right - t.left));
        }



        int GoldFromX(int x)
        {
            const RECT t = GoldTrack();
            double f = (double)(x - t.left) / (double)(t.right - t.left);
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;

            int v = kGoldLo + (int)std::lround(f * (kGoldHi - kGoldLo));
            v = (v / 10) * 10;
            if (v < kGoldLo) v = kGoldLo;
            if (v > kGoldHi) v = kGoldHi;
            return v;
        }

        int GoldToX(int v)
        {
            const RECT t = GoldTrack();
            const double f = (double)(v - kGoldLo) / (double)(kGoldHi - kGoldLo);
            return t.left + (int)std::lround(f * (t.right - t.left));
        }


        void PushLive()
        {
            settings::SetCurrent(g_edit);
            settings::Apply();
        }



        void DrawTrack(Graphics& g, const RectF& rc, const RECT& local,
                       int knobCount, int x1, int x2, int fillL, int fillR,
                       int hotKnob)
        {
            const REAL cy = rc.Y + (REAL)local.top + (REAL)kTrackH * 0.5f;
            const REAL x0 = rc.X + (REAL)local.left;
            const REAL xN = rc.X + (REAL)local.right;


            {
                RectF t(x0, cy - 3.0f, xN - x0, 6.0f);
                FillRound(g, t, 3.0f, kTrackBg);
            }

            if (fillR > fillL)
            {
                RectF f(rc.X + (REAL)fillL, cy - 3.0f, (REAL)(fillR - fillL), 6.0f);
                FillRound(g, f, 3.0f, kTrackFill);
            }

            {
                SolidBrush dim(kTextFaint);
                g.FillRectangle(&dim, x0, cy - 6.0f, 1.0f, 12.0f);
                g.FillRectangle(&dim, xN - 1.0f, cy - 6.0f, 1.0f, 12.0f);
            }

            const int xs[2] = { x1, x2 };
            for (int i = 0; i < knobCount; ++i)
            {
                const REAL kx = rc.X + (REAL)xs[i];
                const Color c = (i == hotKnob) ? kKnobActive : kKnob;

                SolidBrush b(c);
                g.FillEllipse(&b, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);

                Pen edge(Color(200, 30, 20, 24), 1.6f);
                g.DrawEllipse(&edge, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);
            }
        }


        void DrawCheckbox(Graphics& g, const RectF& rc, bool checked, bool hot)
        {
            const RECT b = SafeBox();
            RectF box(rc.X + (REAL)b.left, rc.Y + (REAL)b.top, 20.0f, 20.0f);

            FillRound(g, box, 4.0f, checked ? kAccentSoft : kTrackBg);
            StrokeRound(g, box, 4.0f, hot ? kAccent : Color(140, 90, 92, 104), 1.6f);

            if (checked)
            {
                Pen pen(Color(255, 245, 245, 250), 2.6f);
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                g.DrawLine(&pen,
                    box.X + 5.0f, box.Y + 10.5f,
                    box.X + 8.5f, box.Y + 14.0f);
                g.DrawLine(&pen,
                    box.X + 8.5f, box.Y + 14.0f,
                    box.X + 15.0f, box.Y + 6.0f);
            }
        }








        void PaintContent(Graphics& g, const RectF& rc)
        {
            DrawPanel(g, rc);

            const REAL xL = rc.X + (REAL)kM;
            const REAL xR = rc.X + (REAL)(kW - kM);
            const REAL w = xR - xL;


            {
                SolidBrush bar(kAccent);
                g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 18.0f);
                RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 30.0f);
                DrawTextCjk(g, L"启动设置", t, 20.0f, kTextMain, StringAlignmentNear, FontStyleBold);
            }
            {
                RectF t(xL, rc.Y + 42.0f, w, 16.0f);
                DrawTextCjk(g, L"改动立刻生效，点「开始」后会记住，下次启动自动带出来。",
                    t, 12.0f, kTextFaint);
            }

            wchar_t buf[96];


            {
                RectF t(xL, rc.Y + (REAL)kRow1Top, w, (REAL)kLabelH);
                DrawTextCjk(g, L"背景音乐", t, 14.0f, kTextMain);

                swprintf_s(buf, L"%d%%", g_edit.bgmVol);
                RectF v(xL, rc.Y + (REAL)kRow1Top, w, (REAL)kLabelH);
                DrawTextMono(g, buf, v, 14.0f,
                    g_edit.bgmVol > 100 ? kAccent : kTextDim, StringAlignmentFar);

                const RECT tr = BgmTrack();
                const int kx = VolToX(g_edit.bgmVol, tr);
                DrawTrack(g, rc, tr, 1, kx, kx, tr.left, kx,
                          g_drag == 1 ? 0 : -1);

                RectF lo(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
                DrawTextMono(g, L"0", lo, 11.0f, kTextFaint);
                RectF hi(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
                DrawTextMono(g, L"200%", hi, 11.0f, kTextFaint, StringAlignmentFar);
            }


            {
                RectF t(xL, rc.Y + (REAL)kRow2Top, w, (REAL)kLabelH);
                DrawTextCjk(g, L"音效", t, 14.0f, kTextMain);

                swprintf_s(buf, L"%d%%", g_edit.sfxVol);
                RectF v(xL, rc.Y + (REAL)kRow2Top, w, (REAL)kLabelH);
                DrawTextMono(g, buf, v, 14.0f,
                    g_edit.sfxVol > 100 ? kAccent : kTextDim, StringAlignmentFar);

                const RECT tr = SfxTrack();
                const int kx = VolToX(g_edit.sfxVol, tr);
                DrawTrack(g, rc, tr, 1, kx, kx, tr.left, kx,
                          g_drag == 2 ? 0 : -1);
            }


            {
                DrawCheckbox(g, rc, g_edit.photosensitiveSafe, false);

                const RECT b = SafeBox();
                RectF t(rc.X + (REAL)(b.right + 10), rc.Y + (REAL)kSafeTop, 260.0f, (REAL)kSafeRowH);
                DrawTextCjk(g, L"光敏安全模式（癫痫模式）", t, 14.0f, kTextMain);

                RectF d(rc.X + (REAL)(b.right + 10), rc.Y + (REAL)(kSafeTop + 28), 380.0f, 16.0f);
                DrawTextCjk(g, L"压低整屏亮度跳变与闪烁，光敏人群建议开启", d, 11.0f, kTextFaint);
            }


            {
                RectF t(xL, rc.Y + (REAL)kRow3Top, w, (REAL)kLabelH);
                DrawTextCjk(g, L"每次跳杀间隔（随机区间）", t, 14.0f, kTextMain);

                const double f1 = (double)g_edit.minMs / 1000.0;
                const double f2 = (double)g_edit.maxMs / 1000.0;
                if (g_edit.minMs == g_edit.maxMs)
                    swprintf_s(buf, L"%d-%d ms（固定）", g_edit.minMs, g_edit.maxMs);
                else
                    swprintf_s(buf, L"%d-%d ms（%.2f-%.2f 秒）",
                        g_edit.minMs, g_edit.maxMs, f1, f2);

                RectF v(xL, rc.Y + (REAL)kRow3Top, w, (REAL)kLabelH);
                DrawTextMono(g, buf, v, 14.0f, kTextDim, StringAlignmentFar);

                const RECT tr = IntTrack();
                const int x1 = MsToX(g_edit.minMs);
                const int x2 = MsToX(g_edit.maxMs);
                const int hotKnob = (g_drag == 3) ? 0 : (g_drag == 4 ? 1 : -1);
                DrawTrack(g, rc, tr, 2, x1, x2, x1, x2, hotKnob);

                RectF lo(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
                DrawTextMono(g, L"20ms", lo, 11.0f, kTextFaint);



                {
                    const REAL mx = rc.X + (REAL)((tr.left + tr.right) / 2);
                    const REAL my = rc.Y + (REAL)tr.top + (REAL)kTrackH * 0.5f;
                    SolidBrush tick(kTextFaint);
                    g.FillRectangle(&tick, mx, my - 9.0f, 1.0f, 18.0f);

                    RectF mid(xL + w * 0.5f - 40.0f,
                              rc.Y + (REAL)(tr.bottom + 2), 80.0f, 14.0f);
                    DrawTextMono(g, L"45s", mid, 11.0f, kTextFaint, StringAlignmentCenter);
                }

                RectF hi(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
                DrawTextMono(g, L"90s", hi, 11.0f, kTextFaint, StringAlignmentFar);
            }


            {
                RectF t(xL, rc.Y + (REAL)kRow4Top, w, (REAL)kLabelH);
                DrawTextCjk(g, L"赎金目标金币", t, 14.0f, kTextMain);

                swprintf_s(buf, L"%d", g_edit.goldGoal);
                RectF v(xL, rc.Y + (REAL)kRow4Top, w, (REAL)kLabelH);
                DrawTextMono(g, buf, v, 14.0f,
                    g_edit.goldGoal > 500 ? kAccent : kTextDim, StringAlignmentFar);

                const RECT tr = GoldTrack();
                const int kx = GoldToX(g_edit.goldGoal);
                DrawTrack(g, rc, tr, 1, kx, kx, tr.left, kx,
                    g_drag == 5 ? 0 : -1);

                RectF lo(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
                DrawTextMono(g, L"10", lo, 11.0f, kTextFaint);


                {
                    const REAL mx = rc.X + (REAL)GoldToX(500);
                    const REAL my = rc.Y + (REAL)tr.top + (REAL)kTrackH * 0.5f;
                    SolidBrush tick(kTextFaint);
                    g.FillRectangle(&tick, mx, my - 9.0f, 1.0f, 18.0f);

                    RectF mid(xL + w * 0.5f - 40.0f,
                        rc.Y + (REAL)(tr.bottom + 2), 80.0f, 14.0f);
                    DrawTextMono(g, L"500", mid, 11.0f, kTextFaint, StringAlignmentCenter);
                }

                RectF hi(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
                DrawTextMono(g, L"1000", hi, 11.0f, kTextFaint, StringAlignmentFar);
            }


            DrawButton(g, rc, ResetBtn(), L"恢复默认", g_hot == HOT_RESET, false);
            DrawButton(g, rc, StartBtn(), L"开 始", g_hot == HOT_START, true);


            {
                wchar_t hint[192];
                swprintf_s(hint,
                    L"开始后随时可以按 Ctrl + Alt + Shift + %c 立刻停下并全部还原。",
                    (wchar_t)g_panicVk);
                RectF t(xL, rc.Y + (REAL)kHintTop, w, 16.0f);
                DrawTextCjk(g, hint, t, 12.0f, kTextFaint);
            }
        }

        void Paint(Graphics& g, const RectF& rc, DWORD, void*)
        {
            PaintContent(g, rc);
        }


        void ApplyDrag(POINT p)
        {
            switch (g_drag)
            {
            case 1:
                g_edit.bgmVol = VolFromX(p.x);
                audio::SetBgmLevel(g_edit.bgmVol);
                break;
            case 2:
                g_edit.sfxVol = VolFromX(p.x);
                audio::SetSfxLevel(g_edit.sfxVol);
                break;
            case 3:
            {
                int v = MsFromX(p.x);
                if (v > g_edit.maxMs) v = g_edit.maxMs;
                g_edit.minMs = v;
                break;
            }
            case 4:
            {
                int v = MsFromX(p.x);
                if (v < g_edit.minMs) v = g_edit.minMs;
                g_edit.maxMs = v;
                break;
            }
            case 5:
                g_edit.goldGoal = GoldFromX(p.x);
                break;
            default:
                return;
            }
            aero::Repaint(g_hwnd);
        }

        void RefreshHot(POINT p)
        {
            int h = HOT_NONE;
            if (Hit(ResetBtn(), p)) h = HOT_RESET;
            else if (Hit(StartBtn(), p)) h = HOT_START;

            if (h != g_hot)
            {
                g_hot = h;
                aero::Repaint(g_hwnd);
            }
        }

        void Commit(setup_ui::Verdict v)
        {
            if (g_done) return;
            g_done = true;
            g_verdict = v;
            g_drag = 0;
            g_hot = HOT_NONE;




            settings::Set s = g_edit;
            s.save = (v == setup_ui::VERDICT_START);
            settings::SetCurrent(s);
            settings::Apply();

            if (s.save) settings::Save();

            aero::AnimateClose(g_hwnd);
        }

        void OnMouse(HWND hwnd, UINT msg, POINT p, WPARAM, void*)
        {
            switch (msg)
            {
            case WM_MOUSEMOVE:
                if (g_drag) { ApplyDrag(p); return; }
                RefreshHot(p);
                return;

            case WM_LBUTTONDOWN:
            {
                if (Hit(StartBtn(), p)) { Commit(setup_ui::VERDICT_START); return; }

                if (Hit(ResetBtn(), p))
                {
                    settings::ResetToDefault();
                    g_edit = settings::Current();
                    PushLive();
                    elog::Write(L"[setup] 已恢复默认设置");
                    aero::Repaint(hwnd);
                    return;
                }

                if (Hit(SafeHit(), p))
                {
                    g_edit.photosensitiveSafe = !g_edit.photosensitiveSafe;
                    PushLive();
                    aero::Repaint(hwnd);
                    return;
                }

                const RECT bt = BgmTrack();
                const RECT st = SfxTrack();
                const RECT it = IntTrack();
                const RECT gt = GoldTrack();

                if (p.y >= bt.top - 8 && p.y < bt.bottom + 8 &&
                    p.x >= bt.left - 10 && p.x < bt.right + 10)
                {
                    g_drag = 1;
                    SetCapture(hwnd);
                    ApplyDrag(p);
                    return;
                }
                if (p.y >= st.top - 8 && p.y < st.bottom + 8 &&
                    p.x >= st.left - 10 && p.x < st.right + 10)
                {
                    g_drag = 2;
                    SetCapture(hwnd);
                    ApplyDrag(p);
                    return;
                }
                if (p.y >= it.top - 8 && p.y < it.bottom + 8 &&
                    p.x >= it.left - 10 && p.x < it.right + 10)
                {

                    const int x1 = MsToX(g_edit.minMs);
                    const int x2 = MsToX(g_edit.maxMs);
                    const int d1 = (p.x > x1) ? (p.x - x1) : (x1 - p.x);
                    const int d2 = (p.x > x2) ? (p.x - x2) : (x2 - p.x);
                    g_drag = (d1 <= d2) ? 3 : 4;
                    SetCapture(hwnd);
                    ApplyDrag(p);
                    return;
                }
                if (p.y >= gt.top - 8 && p.y < gt.bottom + 8 &&
                    p.x >= gt.left - 10 && p.x < gt.right + 10)
                {
                    g_drag = 5;
                    SetCapture(hwnd);
                    ApplyDrag(p);
                    return;
                }
                return;
            }

            case WM_LBUTTONUP:
                if (g_drag)
                {
                    g_drag = 0;
                    ReleaseCapture();
                    aero::Repaint(hwnd);
                }
                return;

            default:
                return;
            }
        }





        void OnWheel(HWND hwnd, POINT p, int delta, void*)
        {
            const int step = (delta > 0) ? 1 : -1;

            if (Hit(SafeHit(), p))
            {
                g_edit.photosensitiveSafe = !g_edit.photosensitiveSafe;
                PushLive();
                aero::Repaint(hwnd);
                return;
            }

            const RECT bt = BgmTrack();
            const RECT st = SfxTrack();
            const RECT it = IntTrack();
            const RECT gt = GoldTrack();

            if (p.y >= bt.top - 10 && p.y < bt.bottom + 10)
            {
                g_edit.bgmVol += step * kVolStep;
                if (g_edit.bgmVol < settings::kVolMin) g_edit.bgmVol = settings::kVolMin;
                if (g_edit.bgmVol > settings::kVolMax) g_edit.bgmVol = settings::kVolMax;
                audio::SetBgmLevel(g_edit.bgmVol);
            }
            else if (p.y >= st.top - 10 && p.y < st.bottom + 10)
            {
                g_edit.sfxVol += step * kVolStep;
                if (g_edit.sfxVol < settings::kVolMin) g_edit.sfxVol = settings::kVolMin;
                if (g_edit.sfxVol > settings::kVolMax) g_edit.sfxVol = settings::kVolMax;
                audio::SetSfxLevel(g_edit.sfxVol);
            }
            else if (p.y >= it.top - 10 && p.y < it.bottom + 10)
            {



                const int step = MsStep() * ((delta > 0) ? 1 : -1);
                const int x1 = MsToX(g_edit.minMs);
                const int x2 = MsToX(g_edit.maxMs);
                if (p.x < (x1 + x2) / 2)
                {
                    g_edit.minMs += step;
                    if (g_edit.minMs < kLoMs)       g_edit.minMs = kLoMs;
                    if (g_edit.minMs > g_edit.maxMs) g_edit.minMs = g_edit.maxMs;
                }
                else
                {
                    g_edit.maxMs += step;
                    if (g_edit.maxMs > kHiMs)        g_edit.maxMs = kHiMs;
                    if (g_edit.maxMs < g_edit.minMs) g_edit.maxMs = g_edit.minMs;
                }
            }
            else if (p.y >= gt.top - 10 && p.y < gt.bottom + 10)
            {

                g_edit.goldGoal += step * 25;
                if (g_edit.goldGoal < kGoldLo) g_edit.goldGoal = kGoldLo;
                if (g_edit.goldGoal > kGoldHi) g_edit.goldGoal = kGoldHi;

                g_edit.goldGoal = (g_edit.goldGoal / 10) * 10;
            }
            else
            {
                return;
            }

            aero::Repaint(hwnd);
        }















        bool RenderPreview(const wchar_t* path, const settings::Set& s, bool grid)
        {
            const int CW = kW, CH = kH;

            Bitmap bmp(CW, CH, PixelFormat32bppARGB);
            if (bmp.GetLastStatus() != Ok) return false;

            {
                Graphics g(&bmp);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);


                SolidBrush bg(Color(255, 26, 28, 34));
                g.FillRectangle(&bg, 0, 0, CW, CH);

                const settings::Set saved = g_edit;
                g_edit = s;
                PaintContent(g, RectF(0.0f, 0.0f, (REAL)CW, (REAL)CH));
                g_edit = saved;

                if (grid) DrawGrid(g, RectF(0.0f, 0.0f, (REAL)CW, (REAL)CH));
            }


            return SavePng(bmp, path);
        }

    }










    namespace notice {

        const int kW = 520;
        const int kH = 400;
        const int kM = 26;
        const int kTextW = kW - kM * 2;

        const int kTitleTop = 20;
        const int kLede1Top = 74;
        const int kLede2Top = 104;
        const int kExitTop = 210;
        const int kFootTop = 296;
        const int kBtnTop = 336;
        const int kBtnH = 42;
        const int kBtnW = 176;

        HWND  g_hwnd = nullptr;
        bool  g_done = false;
        bool  g_hotBtn = false;
        bool  g_armed = false;
        bool  g_confirm = false;
        setup_ui::Verdict g_verdict = setup_ui::VERDICT_ABORT;
        int   g_panicVk = 'Q';

        RECT OkBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW, kBtnTop, kBtnW, kBtnH);
            return r;
        }



        void PaintContent(Graphics& g, const RectF& rc)
        {
            DrawPanel(g, rc);

            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)kTextW;


            {
                SolidBrush bar(Color(255, 236, 92, 92));
                g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 20.0f);
                RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 34.0f);
                DrawTextCjk(g, L"这一步之前，先说清楚", t, 21.0f, kTextMain,
                    StringAlignmentNear, FontStyleBold);
            }
            {
                RectF t(xL, rc.Y + (REAL)kLede1Top, w, 20.0f);
                DrawTextCjk(g, L"接下来这个程序会真的动你的桌面，不是模拟：",
                    t, 13.0f, kAccent);
            }
            {
                RectF t(xL, rc.Y + (REAL)kLede2Top, w, 20.0f);
                DrawTextCjk(g, L"桌面图标会被标记成「已加密」、开着的窗口会被收进任务栏、",
                    t, 13.0f, kTextDim);
            }
            {
                RectF t(xL, rc.Y + (REAL)kLede2Top + 20.0f, w, 20.0f);
                DrawTextCjk(g, L"满屏弹窗会挡住你正在做的事。超时没付清的话，",
                    t, 13.0f, kTextDim);
            }
            {
                RectF t(xL, rc.Y + (REAL)kLede2Top + 40.0f, w, 20.0f);
                DrawTextCjk(g, L"桌面快捷方式会被丢进回收站（可以还原）。",
                    t, 13.0f, kTextDim);
            }


            {
                const REAL top = rc.Y + (REAL)kExitTop;
                RectF box(xL, top, w, 68.0f);
                FillRound(g, box, 8.0f, Color(150, 120, 16, 18));
                StrokeRound(g, box, 8.0f, Color(160, 200, 26, 30), 1.4f);

                RectF t1(xL + 14.0f, top + 8.0f, w - 28.0f, 24.0f);
                wchar_t line[160];
                swprintf_s(line, L"想立刻停：按 Ctrl + Alt + Shift + %c",
                    (wchar_t)g_panicVk);
                DrawTextCjk(g, line, t1, 16.0f, kTextMain,
                    StringAlignmentNear, FontStyleBold);

                RectF t2(xL + 14.0f, top + 32.0f, w - 28.0f, 20.0f);
                DrawTextCjk(g, L"这是全局热键，任何时候都有效，按下去立刻停下并全部还原。",
                    t2, 12.0f, kTextDim);
            }


            {
                RectF t(xL, rc.Y + (REAL)kFootTop, w, 18.0f);
                DrawTextCjk(g, L"万一被任务管理器强杀没还原：Ransom_dev.exe --restore 放回快捷方式，",
                    t, 12.0f, kTextFaint);
            }
            {
                RectF t(xL, rc.Y + (REAL)kFootTop + 18.0f, w, 18.0f);
                DrawTextCjk(g, L"--clean-gold 清掉残留的金币文件。",
                    t, 12.0f, kTextFaint);
            }

            DrawButton(g, rc, OkBtn(), L"我知道了，开始", g_hotBtn, true);
        }

        void Paint(Graphics& g, const RectF& rc, DWORD, void*)
        {
            PaintContent(g, rc);
        }

        void OnMouse(HWND hwnd, UINT msg, POINT p, WPARAM, void*)
        {
            if (msg == WM_MOUSEMOVE)
            {
                const bool hot = Hit(OkBtn(), p);
                if (hot != g_hotBtn)
                {
                    g_hotBtn = hot;
                    aero::Repaint(hwnd);
                }
                return;
            }








            if (msg == WM_LBUTTONDOWN)
            {
                g_armed = Hit(OkBtn(), p);
                return;
            }

            if (msg == WM_LBUTTONUP)
            {
                const bool hit = Hit(OkBtn(), p);
                if (g_armed && hit && !g_done)
                {
                    g_confirm = true;
                    g_done = true;





                    aero::AnimateClose(hwnd);
                }
                g_armed = false;
                return;
            }
        }




        void OnHotkey(void*)
        {
            elog::Write(L"[setup] 应急提示窗口里按下了安全阀热键，直接退出");
            if (!g_done)
            {
                g_done = true;
                g_confirm = false;
                g_verdict = setup_ui::VERDICT_ABORT;
                aero::AnimateClose(g_hwnd);
            }
        }

    }


    bool RenderNoticePreview(const wchar_t* path, bool grid)
    {
        using namespace notice;

        Bitmap bmp(kW, kH, PixelFormat32bppARGB);
        if (bmp.GetLastStatus() != Ok) return false;

        {
            Graphics g(&bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

            SolidBrush bg(Color(255, 26, 28, 34));
            g.FillRectangle(&bg, 0, 0, kW, kH);

            PaintContent(g, RectF(0.0f, 0.0f, (REAL)kW, (REAL)kH));
            if (grid) DrawGrid(g, RectF(0.0f, 0.0f, (REAL)kW, (REAL)kH));
        }

        return SavePng(bmp, path);
    }


    bool SavePng(Bitmap& bmp, const wchar_t* path)
    {
        UINT num = 0, size = 0;
        GetImageEncodersSize(&num, &size);
        if (size == 0) return false;

        std::vector<BYTE> buf(size);
        ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
        GetImageEncoders(num, size, info);

        for (UINT i = 0; i < num; ++i)
            if (wcscmp(info[i].MimeType, L"image/png") == 0)
                return bmp.Save(path, &info[i].Clsid, nullptr) == Ok;

        return false;
    }

}

namespace setup_ui {

    Verdict ShowSettings(HINSTANCE hInst, int panicVk)
    {
        using namespace setup;

        g_hwnd = nullptr;
        g_done = false;
        g_verdict = VERDICT_ERROR;
        g_drag = 0;
        g_hot = HOT_NONE;
        g_panicVk = panicVk;
        g_edit = settings::Current();

        aero::Options opt;
        opt.title = L"Ransom_dev — 启动设置";
        opt.width = aero::OptionsWidthForContent(kW);
        opt.height = aero::OptionsHeightForContent(kH);
        opt.buttons = true;
        opt.topmost = true;
        opt.resizable = false;
        opt.animate = true;
        opt.tickMs = 80;
        opt.onContentMouse = OnMouse;
        opt.onContentMouseUser = nullptr;
        opt.onContentWheel = OnWheel;
        opt.onContentWheelUser = nullptr;
        SetHotkeyHandler(HotkeyWhileSetup, nullptr);
        opt.onUserClose = [](HWND, void*) {

            Commit(VERDICT_ABORT);
        };
        opt.onUserCloseUser = nullptr;

        HWND h = aero::Create(hInst, opt, Paint, nullptr);
        if (!h)
        {
            elog::Write(L"[setup] 设置窗口创建失败");
            return VERDICT_ERROR;
        }
        g_hwnd = h;
        SetWindowTextW(h, L"Ransom_dev");

        elog::Write(L"[setup] 设置窗口已打开（背景音乐 %d%% / 音效 %d%% / 光敏安全 %s / 潜伏 %d-%dms）",
            g_edit.bgmVol, g_edit.sfxVol,
            g_edit.photosensitiveSafe ? L"开" : L"关",
            g_edit.minMs, g_edit.maxMs);









        MSG msg;
        while (aero::IsAlive(h) && GetMessageW(&msg, h, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }


        if (aero::IsAlive(h)) aero::Destroy(h);
        g_hwnd = nullptr;
        ClearHotkeyHandler();

        elog::Write(L"[setup] 设置窗口关闭，结果 %d", (int)g_verdict);
        return g_verdict;
    }

    Verdict ShowSafetyNotice(HINSTANCE hInst, int panicVk)
    {
        using namespace notice;

        g_hwnd = nullptr;
        g_done = false;
        g_hotBtn = false;
        g_confirm = false;
        g_verdict = VERDICT_ABORT;
        g_panicVk = panicVk;

        aero::Options opt;
        opt.title = L"Ransom_dev — 开始前请读这里";
        opt.width = aero::OptionsWidthForContent(kW);
        opt.height = aero::OptionsHeightForContent(kH);
        opt.buttons = true;
        opt.topmost = true;
        opt.resizable = false;
        opt.animate = true;
        opt.tickMs = 120;
        opt.onContentMouse = OnMouse;
        opt.onContentMouseUser = nullptr;
        SetHotkeyHandler(OnHotkey, nullptr);
        opt.onUserClose = [](HWND, void*) {

            if (!g_done)
            {
                g_done = true;
                g_verdict = VERDICT_ABORT;
                aero::AnimateClose(g_hwnd);
            }
        };
        opt.onUserCloseUser = nullptr;

        HWND h = aero::Create(hInst, opt, Paint, nullptr);
        if (!h)
        {
            elog::Write(L"[setup] 应急提示窗口创建失败");
            return VERDICT_ERROR;
        }
        g_hwnd = h;
        SetWindowTextW(h, L"Ransom_dev");

        elog::Write(L"[setup] 应急提示已弹出（安全阀 Ctrl+Alt+Shift+%c）", (wchar_t)panicVk);




        MSG msg;
        while (aero::IsAlive(h) && GetMessageW(&msg, h, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_confirm) g_verdict = VERDICT_START;

        if (aero::IsAlive(h)) aero::Destroy(h);
        g_hwnd = nullptr;
        ClearHotkeyHandler();

        elog::Write(L"[setup] 应急提示关闭，结果 %d", (int)g_verdict);
        return g_verdict;
    }

    bool DumpSettingsPreview(const wchar_t* path, const settings::Set& s, bool grid)
    {
        return setup::RenderPreview(path, s, grid);
    }

    bool DumpNoticePreview(const wchar_t* path, bool grid)
    {
        return RenderNoticePreview(path, grid);
    }

    void SetHotkeyHandler(HotkeyFn fn, void* user)
    {
        g_hotkeyFn = fn;
        g_hotkeyUser = user;
    }

    void ClearHotkeyHandler()
    {
        g_hotkeyFn = nullptr;
        g_hotkeyUser = nullptr;
    }

    bool DispatchHotkey()
    {
        if (!g_hotkeyFn) return false;
        g_hotkeyFn(g_hotkeyUser);
        return true;
    }

}
