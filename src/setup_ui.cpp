// ============================================================================
//  setup_ui.cpp
//
//  开场设置窗口 + 应急提示窗口。都建在 aero_window 上（逐像素透明的分层窗），
//  内容全部自绘 —— 这样滑条的观感能和勒索窗口那套玻璃风格对上，
//  也不需要在分层窗口里塞子控件（分层窗口本来就不支持子控件）。
//
//  ---- 为什么是阻塞式 ----
//
//  这两个窗口出现在**一切演出模块启动之前**：director、桌面覆盖层、双进程
//  看守、声音都还没起来。所以直接在主线程上跑一个模态消息循环最省事，
//  也不会出现「设置还开着，脸已经飘出来了」这种错位。
//
//  ---- 坐标约定 ----
//
//  aero 的 paint 回调把内容区矩形给过来，但鼠标回调给的是**换算过的
//  内容区坐标**（见 aero_window.h）。两者同一套原点，所以下面所有控件
//  矩形都是「相对内容区左上角」的裸坐标，调用时再加 rc.X / rc.Y。
// ============================================================================
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

    // ---------------------------------------------------------------- 配色 ----
    const Color kPanelBg(120, 10, 12, 18);        // 内容底板
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

    // ------------------------------------------------------------ 小工具 ----

    bool Hit(const RECT& r, const POINT& p)
    {
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }

    // 注意**不叫** Rect：windows.h 里已经有一个同名的 GDI 函数，
    // 在这里再定义一个会把它遮住，以后谁想用 GDI 的 Rect 就会莫名其妙报错。
    void SetRectLocal(RECT& r, int x, int y, int w, int h)
    {
        r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    }

    // 字体族。
    // 中文字**不能**用素材里的 Roboto Mono（没有汉字字形，会出豆腐块），
    // 但数字和拉丁字母用它更贴近整套 UI 的调子。所以分两套用。
    FontFamily* MonoFamily()
    {
        static FontFamily* fam = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            fam = aero::UiFontFamily();     // 没注册成功就是 nullptr
        }
        return fam;
    }

    // 中文标签用的字体族。雅黑在各版本 Windows 上都有。
    Font MakeCjkFont(float px, int style = FontStyleRegular)
    {
        return Font(L"Microsoft YaHei", px, style, UnitPixel);
    }

    // 数值 / 拉丁字母。拿不到内嵌字体就退回 Consolas（等宽，观感接近）。
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

    // 在内容区上垫一层底板，让控件别直接浮在桌面透出来的背景上。
    void DrawPanel(Graphics& g, const RectF& rc)
    {
        FillRound(g, rc, 10.0f, kPanelBg);
        StrokeRound(g, rc, 10.0f, kPanelEdge, 1.0f);
    }

    // 把位图存成 PNG。定义在文件末尾（两个窗口的导出都要用），
    // 这里先声明。
    bool SavePng(Bitmap& bmp, const wchar_t* path);

    // ---- 安全阀热键的转接 ----
    // 开场两屏拿不到 WM_HOTKEY（那条消息是投给 IPC 窗口的），
    // 所以由 entity_main 里的 IPC 窗口收到之后转过来。这两个全局是那
    // 一条转接路径的落点。
    setup_ui::HotkeyFn g_hotkeyFn = nullptr;
    void*              g_hotkeyUser = nullptr;

    namespace setup { void Commit(setup_ui::Verdict v); }   // 下面要用

    // 设置窗口开着的时候按下安全阀热键 = 中止，不演出。
    void HotkeyWhileSetup(void*)
    {
        elog::Write(L"[setup] 设置窗口里按下了安全阀热键，直接退出");
        setup::Commit(setup_ui::VERDICT_ABORT);
    }

    // ---- 调排版用的坐标网格 ----    // 和 ui_layout::Preview 的 --ui-grid 一个路子：每 20px 一条细线，
    // 每 100px 一条亮线 + 标注。内容全是手算的绝对坐标，没有网格
    // 就只能靠截图数像素（这正是本项目 --ui-preview 存在的原因）。
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

    // ---- 一个按钮 ----
    // 放在匿名 namespace 的顶层（不是哪个子 namespace 里）：设置窗口和
    // 应急提示窗口都要用它，塞进 setup 里的话 notice 那边就找不到了。
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

    // ==========================================================================
    //  设置窗口
    // ==========================================================================
    namespace setup {

        // ---- 纯数据布局（相对内容区左上角）----
        const int kW = 520;
        const int kH = 530;             // 470 -> 530：给"硬核模式"那一行腾地方
        const int kM = 22;              // 左右页边距
        const int kLabelH = 18;         // 标签行高
        const int kTrackH = 18;         // 条带高度（视觉上的「槽」）
        const int kKnobR = 8;

        const int kRow1Top = 64;        // 背景音乐
        const int kRow2Top = 124;       // 音效
        const int kSafeTop = 184;       // 光敏安全（复选框，= kSafeRowH 高）
        const int kSafeRowH = 34;
        const int kRow3Top = 236;       // 随机区间
        const int kRow4Top = 296;       // 赎金目标金币
        const int kHardTop = 356;       // 硬核模式（复选框，行高同 kSafeRowH）
        const int kBtnTop = 416;        // 356 -> 416
        const int kBtnH = 42;
        const int kBtnGap = 14;
        const int kBtnW = 150;
        const int kHintTop = 472;       // 412 -> 472

        const int kTrackLeft = kM + 18;                 // 条带左右端
        const int kTrackRight = kW - kM - 18;
        const int kTrackW = kTrackRight - kTrackLeft;   // 440

        // 区间条的数值范围（毫秒）。和 settings 的边界保持一致，
        // 改一边就得改另一边——所以这里直接从 settings 取。
        const int kLoMs = settings::kIntervalMinMs;
        const int kHiMs = settings::kIntervalMaxMs;

        // 金币目标的数值范围。同样从 settings 取。
        const int kGoldLo = settings::kGoldMin;
        const int kGoldHi = settings::kGoldMax;

        const int kVolStep = 5;         // 滚轮一格 5%

        // 滚轮一格多少毫秒。
        //
        // 不能写死：区间上限现在是 90 秒，固定 20ms 一格的话从 20ms 调到
        // 90000ms 要滚 4500 格。所以按「整条滑条大约 90 格」反推步长——
        // 4 秒的区间是 20ms 一格（细调），90 秒的区间是 1 秒一格（快速粗调）。
        int MsStep()
        {
            const int span = kHiMs - kLoMs;
            int step = span / 90;
            if (step < 20) step = 20;
            return step;
        }

        // ---- 状态 ----
        HWND  g_hwnd = nullptr;
        bool  g_done = false;
        setup_ui::Verdict g_verdict = setup_ui::VERDICT_ERROR;
        int   g_panicVk = 'Q';

        enum Hot { HOT_NONE = 0, HOT_RESET, HOT_START };
        int   g_hot = HOT_NONE;

        // drag：0 没有，1 背景音乐，2 音效，3 区间下限，4 区间上限
        int   g_drag = 0;

        // 编辑中的一份副本。拖滑条只改它 + 实时推给子系统，
        // 点「开始」才写回 settings 并落盘。
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

        // 复选框的**可点区域**：框 + 后面那串文字，但不横跨整行——
        // 整行可点的话，用户想在右边空白处拖动窗口就会误触。
        RECT SafeHit()
        {
            RECT r;
            SetRectLocal(r, kM - 4, kSafeTop, 430, kSafeRowH);
            return r;
        }

        RECT HardBox()
        {
            RECT r;
            SetRectLocal(r, kM, kHardTop + (kSafeRowH - 20) / 2, 20, 20);
            return r;
        }

        // 和光敏那条一样：框 + 文字可点，但不横跨整行。
        RECT HardHit()
        {
            RECT r;
            SetRectLocal(r, kM - 4, kHardTop, 430, kSafeRowH);
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

        // ---- 数值 <-> 像素 ----
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

        // 金币目标：10-1000。吸到 10 的整数倍 —— 金币面额全是 10 的倍数，
        // 目标也跟着取整，滑条上的读数看起来才整齐。
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

        // ---- 把编辑中的值推给子系统（实时生效）----
        void PushLive()
        {
            settings::SetCurrent(g_edit);
            settings::Apply();
        }

        // ---- 画一条滑条 ----
        // hotKnob：第几颗珠子要高亮（0 起）；-1 = 都不亮。
        void DrawTrack(Graphics& g, const RectF& rc, const RECT& local,
                       int knobCount, int x1, int x2, int fillL, int fillR,
                       int hotKnob)
        {
            const REAL cy = rc.Y + (REAL)local.top + (REAL)kTrackH * 0.5f;
            const REAL x0 = rc.X + (REAL)local.left;
            const REAL xN = rc.X + (REAL)local.right;

            // 槽
            {
                RectF t(x0, cy - 3.0f, xN - x0, 6.0f);
                FillRound(g, t, 3.0f, kTrackBg);
            }
            // 已选段（两条音量条就是 0 -> 当前值；区间条是 下限 -> 上限）
            if (fillR > fillL)
            {
                RectF f(rc.X + (REAL)fillL, cy - 3.0f, (REAL)(fillR - fillL), 6.0f);
                FillRound(g, f, 3.0f, kTrackFill);
            }
            // 端点刻度
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

        // ---- 一个复选框 ----
        // box：这个复选框方框的本地区域（由调用方给，SafeBox() / HardBox()）。
        // 以前这里写死用 SafeBox()，加了第二个复选框之后两个会画在同一个位置。
        void DrawCheckbox(Graphics& g, const RectF& rc, const RECT& b,
                          bool checked, bool hot)
        {
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

        // ---- 一个按钮 ----（定义在匿名 namespace 顶层，见文件上半部分）

        // ---- 主绘制 ----
        //
        // 真正的绘制逻辑在 PaintContent 里，只认「内容区矩形」这一个几何输入。
        // 活窗口的 aero 回调和 `--setup-ui` 离线导出都走它，
        // 所以导出来的 PNG 就是窗口里长的那张，不存在两套排版跑偏的可能。
        void PaintContent(Graphics& g, const RectF& rc)
        {
            DrawPanel(g, rc);

            const REAL xL = rc.X + (REAL)kM;
            const REAL xR = rc.X + (REAL)(kW - kM);
            const REAL w = xR - xL;

            // ---- 标题 ----
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

            // ---- 背景音乐 ----
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

            // ---- 音效 ----
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

            // ---- 光敏安全（癫痫模式）----
            {
                DrawCheckbox(g, rc, SafeBox(), g_edit.photosensitiveSafe, false);

                const RECT b = SafeBox();
                RectF t(rc.X + (REAL)(b.right + 10), rc.Y + (REAL)kSafeTop, 260.0f, (REAL)kSafeRowH);
                DrawTextCjk(g, L"光敏安全模式（癫痫模式）", t, 14.0f, kTextMain);

                RectF d(rc.X + (REAL)(b.right + 10), rc.Y + (REAL)(kSafeTop + 28), 380.0f, 16.0f);
                DrawTextCjk(g, L"压低整屏亮度跳变与闪烁，光敏人群建议开启", d, 11.0f, kTextFaint);
            }

            // ---- 遭遇战间隔（随机区间）----
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

                // 中间刻度。区间跨度到 90 秒之后，光有两端读数很难估出
                // 「我这一拖大概落在多少秒」，加一个中点就够用了。
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

            // ---- 赎金目标金币 ----
            //
            // 硬核开着的时候这一行是**锁死的**：读数强行显示 5000，
            // 珠子停在最右端，整体转成灰字。settings::GoldGoal() 那边
            // 也是直接覆盖成 kHardcoreGoldGoal，不改写用户存的自定义值，
            // 所以关掉硬核之后他自己调的那个数会原样回来。
            const bool goldLocked = g_edit.hardcore;
            const int  shownGoal = goldLocked ? settings::kHardcoreGoldGoal : g_edit.goldGoal;
            {
                RectF t(xL, rc.Y + (REAL)kRow4Top, w, (REAL)kLabelH);
                DrawTextCjk(g, L"赎金目标金币", t, 14.0f,
                    goldLocked ? kTextFaint : kTextMain);

                swprintf_s(buf, goldLocked ? L"%d（硬核锁定）" : L"%d", shownGoal);
                RectF v(xL, rc.Y + (REAL)kRow4Top, w, (REAL)kLabelH);
                DrawTextMono(g, buf, v, 14.0f,
                    goldLocked ? kTextFaint : (g_edit.goldGoal > 500 ? kAccent : kTextDim),
                    StringAlignmentFar);

                const RECT tr = GoldTrack();
                // 锁定态把珠子顶到最右端，一眼就能看出"这条不是你在控"
                const int kx = goldLocked ? tr.right : GoldToX(shownGoal);
                DrawTrack(g, rc, tr, 1, kx, kx, tr.left, kx,
                    goldLocked ? -1 : (g_drag == 5 ? 0 : -1));

                RectF lo(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
                DrawTextMono(g, L"10", lo, 11.0f, kTextFaint);

                // 中点刻度标 500（原作默认值）—— 偏了之后好一眼找到原位
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

            // ---- 硬核模式 ----
            {
                DrawCheckbox(g, rc, HardBox(), g_edit.hardcore, false);

                const RECT hb = HardBox();
                RectF t(rc.X + (REAL)(hb.right + 10), rc.Y + (REAL)kHardTop, 300.0f, (REAL)kSafeRowH);
                DrawTextCjk(g, L"硬核模式", t, 14.0f,
                    g_edit.hardcore ? kAccent : kTextMain, StringAlignmentNear, FontStyleBold);

                RectF d(rc.X + (REAL)(hb.right + 10), rc.Y + (REAL)(kHardTop + 28), 440.0f, 16.0f);
                DrawTextCjk(g,
                    g_edit.hardcore
                    ? L"已开启：3 分钟 / 赎金 5000 / 假金币 / 弹窗更多 / 随机锁桌面项 / 金币撒到磁盘目录"
                    : L"3 分钟倒计时、赎金 5000、假金币、弹窗更多、随机锁桌面项、金币撒到磁盘目录",
                    d, 11.0f, kTextFaint);
            }

            // ---- 按钮 ----
            DrawButton(g, rc, ResetBtn(), L"恢复默认", g_hot == HOT_RESET, false);
            DrawButton(g, rc, StartBtn(), L"开 始", g_hot == HOT_START, true);

            // ---- 底部提示 ----
            // 硬核模式下安全阀**也是按一次就停**（曾经打算做成"连按两次"，
            // 后来放弃了），所以这句话在两种模式下都成立，不用分支。
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

        // ---- 拖拽 ----
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
            case 3:     // 下限：不许越过上限
            {
                int v = MsFromX(p.x);
                if (v > g_edit.maxMs) v = g_edit.maxMs;
                g_edit.minMs = v;
                break;
            }
            case 4:     // 上限：不许越过下限
            {
                int v = MsFromX(p.x);
                if (v < g_edit.minMs) v = g_edit.minMs;
                g_edit.maxMs = v;
                break;
            }
            case 5:     // 赎金目标金币（硬核下锁死 5000，拖不动）
                if (g_edit.hardcore) break;
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

            // 把值交给 settings 并收场。
            // 顺序：改全局 -> （只有「开始」才）落盘 -> 关窗。
            // 中途关掉窗口（中止）不落盘 —— 用户没确认的东西不该被记住。
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

                if (Hit(HardHit(), p))
                {
                    g_edit.hardcore = !g_edit.hardcore;
                    PushLive();

                    // 打开开关时整扇窗抖一下，给个"这东西很重"的反馈。
                    // AnimateJolt 走的是真实窗口位移（SetWindowPos），
                    // 和 SetWobble 那种绘制层微抖不是一回事；有动画在跑时
                    // 它会排队，不会把别的动画掐掉。
                    aero::AnimateJolt(hwnd, 9.0f, 380);

                    elog::Write(L"[setup] 硬核模式 %s（赎金 %d，倒计时 %d 秒）",
                        g_edit.hardcore ? L"开" : L"关",
                        g_edit.hardcore ? settings::kHardcoreGoldGoal : g_edit.goldGoal,
                        g_edit.hardcore ? settings::kHardcoreRansomMs / 1000 : 90);
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
                    // 两颗珠子重合时按距离选一颗，跟手的那颗才会动。
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
                    if (g_edit.hardcore) return;   // 硬核：赎金锁死 5000
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

        // 滚轮微调：指针压在哪个控件上就调哪个。
        // 改 1% 也要拖半天的话，这两条滑条就太难用了。
        // 滚轮微调：指针压在哪个控件上就调哪个。
        // 只靠拖动的话，想把 100% 改成 105% 得拖半天。
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

            // 硬核开关**不接受滚轮**：这么重的开关被滚轮误触翻掉太容易了，
            // 想开就老老实实点一下。
            if (Hit(HardHit(), p)) return;

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
                // 指针更靠近下限就调下限，否则调上限。两颗珠子重合时
                // 走到 else 分支，也就是往「拉开区间」的方向走——
                // 比卡在固定值上更符合直觉。
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
                if (g_edit.hardcore) return;   // 硬核：赎金锁死 5000
                // 一格 25：10-1000 跨度太大，一格 10 太慢、一格 100 太粗。
                g_edit.goldGoal += step * 25;
                if (g_edit.goldGoal < kGoldLo) g_edit.goldGoal = kGoldLo;
                if (g_edit.goldGoal > kGoldHi) g_edit.goldGoal = kGoldHi;
                // 吸到 10 的整倍数，和拖拽保持一致
                g_edit.goldGoal = (g_edit.goldGoal / 10) * 10;
            }
            else
            {
                return;
            }

            aero::Repaint(hwnd);
        }

        // 安全阀热键在这个窗口里也要管用：用户还没开始演出就改主意的话，
        // 按 Ctrl+Alt+Shift+Q 应该当场退出，而不是非要点那个 X。
        //
        // 真正的处理函数是上面的 HotkeyWhileSetup（匿名 namespace 顶层）：
        // RegisterHotKey 绑的是隐藏的 IPC 窗口，那条消息**不会**投到本窗口，
        // 只能由那边转一手（见 setup_ui.h 的 SetHotkeyHandler）。

        // ---- 离线导出（`--setup-ui`）----
        // 把这一屏按给定的一整套值渲染成 PNG 再退出。不建窗口、不碰设备。
        //
        // 为什么要它：这一屏有大半是**手算的绝对坐标**（哪一行在 y=124、
        // 按钮从 x=184 起），而它是全屏上唯一一个没有桌面干扰的窗口吗？
        // 不是——盖在桌面上截屏会掺进壁纸和别的窗口，量不准。
        // 和 --ui-preview / --fx-demo 一个路子，宁可多一个开关。
        bool RenderPreview(const wchar_t* path, const settings::Set& s, bool grid)
        {
            const int CW = kW, CH = kH;

            Bitmap bmp(CW, CH, PixelFormat32bppARGB);
            if (bmp.GetLastStatus() != Ok) return false;

            {
                Graphics g(&bmp);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

                // 铺一层和桌面无关的深底色，方便判断半透明够不够看
                SolidBrush bg(Color(255, 26, 28, 34));
                g.FillRectangle(&bg, 0, 0, CW, CH);

                const settings::Set saved = g_edit;
                g_edit = s;
                PaintContent(g, RectF(0.0f, 0.0f, (REAL)CW, (REAL)CH));
                g_edit = saved;

                if (grid) DrawGrid(g, RectF(0.0f, 0.0f, (REAL)CW, (REAL)CH));
            }

            // PNG 编码器查找 + 存盘在下面共用的 SavePng 里
            return SavePng(bmp, path);
        }

    } // namespace setup

    // ==========================================================================
    //  应急提示窗口
    //
    //  设置确认之后、演出开始之前必弹一次。目的很单纯：**在动用户的东西之前，
    //  让他知道会动什么、以及怎么喊停**。
    //
    //  这一屏刻意不做任何花哨效果：它出现在演出之前，本身不该吓人，
    //  否则「应急说明」就变成了第一场 jumpscare。
    // ==========================================================================
    namespace notice {

        const int kW = 520;
        const int kH = 400;
        const int kM = 26;
        const int kTextW = kW - kM * 2;      // 468

        const int kTitleTop = 20;
        const int kLede1Top = 74;
        const int kLede2Top = 104;           // 三行 13px 正文，往后错开
        const int kExitTop = 210;            // 高亮框：退出快捷键
        const int kFootTop = 296;
        const int kBtnTop = 336;
        const int kBtnH = 42;
        const int kBtnW = 176;

        HWND  g_hwnd = nullptr;
        bool  g_done = false;
        bool  g_hotBtn = false;
        bool  g_armed = false;              // 在本按钮上按下过左键
        bool  g_confirm = false;            // 点了「我知道了」（区别于关窗口）
        setup_ui::Verdict g_verdict = setup_ui::VERDICT_ABORT;
        int   g_panicVk = 'Q';

        RECT OkBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        // 和设置窗口一样：绘制逻辑单独一个 PaintContent，活窗口和
        // `--notice-ui` 离线导出共用同一份。
        void PaintContent(Graphics& g, const RectF& rc)
        {
            DrawPanel(g, rc);

            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)kTextW;

            // ---- 标题 ----
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

            // ---- 更要紧的：怎么退 ----
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

            // ---- 收尾说明 ----
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

            // ---- 按下 + 抬起才算一次点击 ----
            //
            // **必须**成对判定，不能只看按下（原本就是只看按下，实测踩到）：
            // 这一屏是在用户刚点完设置窗口的「开始」之后弹出来的，那一瞬间
            // 左键可能还被按着。窗口在按下的状态下出现时，系统会把抬起事件
            // 补给新窗口 —— 于是「一弹出来就自己确认了」，日志里表现为
            // 应急提示刚弹出 32ms 就成了「结果 0」。
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

                    // 这里以前**只**置了标志，没请求关闭动画 ——
                    // 于是窗口被模态循环后面那句 aero::Destroy() 直接销毁，
                    // 玩家点「我知道了」看到的是一下子消失。补上这一句，
                    // 让窗口走和其它路径一致的 180ms 淡出。
                    aero::AnimateClose(hwnd);
                }
                g_armed = false;
                return;
            }
        }

        // 在这一屏上按安全阀热键 = 当场退出。
        // 能走到这里说明热键**已经注册成功**了，否则 entity_main
        // 根本不会弹这一屏（见那边的说明）。
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

    } // namespace notice

    // 应急提示的离线导出。和 setup::RenderPreview 同一套做法。
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

    // 两个窗口的 PNG 导出共用这一段（找编码器 + 存盘）。
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

} // namespace

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
        g_edit = settings::Current();     // 从当前设置起手（含 ini 读回来的值）

        aero::Options opt;
        opt.title = L"Ransom_dev — 启动设置";
        opt.width = aero::OptionsWidthForContent(kW);
        opt.height = aero::OptionsHeightForContent(kH);
        opt.buttons = true;               // 有关闭按钮：关掉 = 不演了
        opt.topmost = true;
        opt.resizable = false;
        opt.animate = true;
        opt.tickMs = 80;                  // 内容基本静止，不必按 33ms 重绘
        opt.onContentMouse = OnMouse;
        opt.onContentMouseUser = nullptr;
        opt.onContentWheel = OnWheel;
        opt.onContentWheelUser = nullptr;
        SetHotkeyHandler(HotkeyWhileSetup, nullptr);
        opt.onUserClose = [](HWND, void*) {
            // 点右上角 X / Alt+F4：中止，不演出。
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

        // ---- 模态循环 ----
        // 退出条件是「窗口真的没了」，**不是** g_done。
        //
        // Commit() 在**开始**播放关闭动画的那一刻就把 g_done 置位了；
        // 如果循环以 g_done 为条件，下面那句 aero::Destroy() 会立刻
        // 销毁窗口，180ms 的淡出动画一帧都播不出来 —— 这正是
        // 「关闭动画失效」的根因。窗口由 aero 在关闭动画播完后自己
        // 销毁，让循环跑到那一刻为止就对了。
        MSG msg;
        while (aero::IsAlive(h) && GetMessageW(&msg, h, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 兜底：只有异常路径才会走到这里（正常路径窗口已被 aero 销毁）。
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
        g_verdict = VERDICT_ABORT;      // 默认「不演」——只有明确点了按钮才继续
        g_panicVk = panicVk;

        aero::Options opt;
        opt.title = L"Ransom_dev — 开始前请读这里";
        opt.width = aero::OptionsWidthForContent(kW);
        opt.height = aero::OptionsHeightForContent(kH);
        opt.buttons = true;             // 关掉 = 不演了
        opt.topmost = true;
        opt.resizable = false;
        opt.animate = true;
        opt.tickMs = 120;
        opt.onContentMouse = OnMouse;
        opt.onContentMouseUser = nullptr;
        SetHotkeyHandler(OnHotkey, nullptr);
        opt.onUserClose = [](HWND, void*) {
            // 关掉这一屏 = 用户决定不玩了。整个程序干净退出。
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

        // 同上：跑到窗口消失为止，让关闭动画播完。
        // 这里有两处会把 g_done 置位（点按钮 / 点 X / 按热键），
        // 都以 g_done 为循环条件的话，关闭动画全都会被跳过去。
        MSG msg;
        while (aero::IsAlive(h) && GetMessageW(&msg, h, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_confirm) g_verdict = VERDICT_START;   // 明确点了「我知道了」才继续

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

} // namespace setup_ui
