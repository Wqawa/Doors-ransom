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
    // 提示类文字的专用色。
    // 原来是直接拿 kTextFaint（150 的不透明度）去画那些小字说明，
    // 压在深色底板上实测太暗、得凑近看 —— 换成接近白的亮色。
    // 刻度标注（"0"/"200%"/"20ms"…）仍然用 kTextFaint：
    // 那些是去强调用的参照物，不是给人读的句子。
    const Color kTextHint(235, 228, 232, 240);
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
        //
        // 控制项变多之后一屏放不下了，所以版面切成三块：
        //
        //     [ 头部 ]  标题 + 副标题                 固定，不滚动
        //     [ 视口 ]  全部滑条 / 复选框             内容比视口高 -> 右侧滚动条
        //     [ 底部 ]  两个按钮 + 安全阀提示           固定，永远看得见
        //
        // 「开始」按钮被滚没了是最糟的体验，所以底部那块不参与滚动。
        const int kW = 520;
        const int kH = 600;             // 窗口内容高度（可见部分）
        const int kM = 22;              // 左右页边距
        const int kLabelH = 18;         // 标签行高
        const int kTrackH = 18;         // 条带高度（视觉上的「槽」）
        const int kKnobR = 8;

        const int kHeaderH = 62;        // 头部（固定）
        const int kFooterH = 96;        // 底部（固定）
        const int kViewTop = kHeaderH;                  // 视口顶边（窗口坐标）
        const int kViewH = kH - kHeaderH - kFooterH;    // 视口高度
        const int kScrollW = 14;                        // 滚动条宽度
        const int kContentW = kW - kScrollW;            // 内容绘制的宽度（左侧留出滚动条）

        const int kRowH = 56;           // 每个滑条行的高度
        const int kCheckH = 34;         // 复选框行的高度

        // 各行在**内容坐标系**里的 y。内容坐标 = 从视口顶部往下算（会滚）。
        //
        // 顺序（从上到下）：光敏安全 → 背景音乐 → 音效 → 硬核模式 →
        //                    遭遇战间隔 → 赎金目标 → 关窗惩罚 → 假金币比例 → 假币形态配比
        //
        //   * 光敏安全提到最上面：它是"看不看得下去"的前置开关。
        //   * 硬核模式紧跟在音效下面：它是个总闸，下面的假金币那两条
        //     都由它解锁，所以排在它们**上面**比排在最底下顺手。
        const int kRowSafe = 0;         // 光敏安全（复选框）
        const int kRowBgm = 40;         // 背景音乐
        const int kRowSfx = 96;         // 音效
        const int kRowHard = 152;       // 硬核模式（复选框，下面还有一行说明）
        const int kRowIdle = 194;       // 遭遇战间隔（双滑块）
        const int kRowGold = 250;       // 赎金目标（范围随模式变）
        const int kRowClose = 306;      // 关窗惩罚时长（范围随模式变）
        const int kRowFakePct = 362;    // 假金币比例（仅硬核）
        const int kRowFakeMix = 418;    // 假币形态配比（**双滑块**，仅硬核）
        const int kContentH = 474;      // 内容总高度（> kViewH，所以要滚）

        const int kTrackLeft = kM + 18;                 // 条带左右端
        const int kTrackRight = kContentW - kM - 18;
        const int kTrackW = kTrackRight - kTrackLeft;

        // 底部那两个按钮的位置（**窗口坐标**，固定不动）
        const int kBtnH = 42;
        const int kBtnGap = 14;
        const int kBtnW = 150;
        const int kBtnTop = kH - 74;
        const int kHintTop = kH - 26;

        // 区间条的数值范围（毫秒）。和 settings 的边界保持一致，
        // 改一边就得改另一边——所以这里直接从 settings 取。
        const int kLoMs = settings::kIntervalMinMs;
        const int kHiMs = settings::kIntervalMaxMs;

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

        // drag：0 = 没在拖。其余是"正在拖哪个控件"。
        enum Drag {
            DRAG_NONE = 0,
            DRAG_BGM, DRAG_SFX, DRAG_IDLE_LO, DRAG_IDLE_HI,
            DRAG_GOLD, DRAG_CLOSE,
            DRAG_FAKE_PCT, DRAG_FAKE_MIX,
            DRAG_SCROLL,
        };
        int   g_drag = DRAG_NONE;
        int   g_dragMixIdx = -1;        // 三滑块那一条正在拖第几颗（0/1/2）
        int   g_scrollGrab = 0;         // 抓滚动条时，光标相对滑块顶部的偏移

        // 滚动位置（内容坐标里的偏移量）
        int   g_scroll = 0;

        // 编辑中的一份副本。拖滑条只改它 + 实时推给子系统，
        // 点「开始」才写回 settings 并落盘。
        settings::Set g_edit;

        // ---- 滚动 ----
        int MaxScroll()
        {
            const int m = kContentH - kViewH;
            return (m > 0) ? m : 0;
        }
        int ClampScroll(int v)
        {
            if (v < 0) return 0;
            const int m = MaxScroll();
            return (v > m) ? m : v;
        }

        // 内容坐标 <-> 窗口坐标的偏移。绘制时内容 y + 它 = 窗口 y；
        // 命中判定时窗口 y - 它 = 内容 y。
        int ViewOffset() { return kViewTop - g_scroll; }

        bool InView(POINT p)
        {
            return p.y >= kViewTop && p.y < kViewTop + kViewH;
        }

        POINT ToContentPt(POINT p)
        {
            POINT q;
            q.x = p.x;
            q.y = p.y - ViewOffset();
            return q;
        }

        // ---- 控件矩形 ----
        //
        // 全部返回**窗口坐标**（已经含了滚动偏移）。这样绘制和命中判定
        // 用的是同一份矩形，不可能各算各的。
        RECT RowTrack(int rowY)
        {
            RECT r;
            SetRectLocal(r, kTrackLeft, ViewOffset() + rowY + kLabelH + 2, kTrackW, kTrackH);
            return r;
        }

        RECT BgmTrack() { return RowTrack(kRowBgm); }
        RECT SfxTrack() { return RowTrack(kRowSfx); }
        RECT IntTrack() { return RowTrack(kRowIdle); }
        RECT GoldTrack() { return RowTrack(kRowGold); }
        RECT CloseTrack() { return RowTrack(kRowClose); }
        RECT FakePctTrack() { return RowTrack(kRowFakePct); }
        RECT FakeMixTrack() { return RowTrack(kRowFakeMix); }

        RECT CheckBoxAt(int rowY)
        {
            RECT r;
            SetRectLocal(r, kM, ViewOffset() + rowY + (kCheckH - 20) / 2, 20, 20);
            return r;
        }

        // 复选框的**可点区域**：框 + 后面那串文字，但不横跨整行——
        // 整行可点的话，用户想在右边空白处拖动窗口就会误触。
        RECT CheckHitAt(int rowY)
        {
            RECT r;
            SetRectLocal(r, kM - 4, ViewOffset() + rowY, 430, kCheckH);
            return r;
        }

        RECT SafeBox() { return CheckBoxAt(kRowSafe); }
        RECT SafeHit() { return CheckHitAt(kRowSafe); }
        RECT HardBox() { return CheckBoxAt(kRowHard); }
        RECT HardHit() { return CheckHitAt(kRowHard); }

        // 滑条命中：条带上下各放宽 8px、左右各放宽 10px。
        // 行距 56、条带 18，放宽后是 34 高，不会和邻行打架。
        bool TrackHit(const RECT& t, POINT p)
        {
            return p.y >= t.top - 8 && p.y < t.bottom + 8 &&
                   p.x >= t.left - 10 && p.x < t.right + 10;
        }

        // 底部按钮（固定，不滚动）
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

        // 滚动条（轨道 + 滑块）
        RECT ScrollTrackRect()
        {
            RECT r;
            SetRectLocal(r, kW - kScrollW - 4, kViewTop + 4, kScrollW, kViewH - 8);
            return r;
        }

        RECT ScrollThumbRect()
        {
            const RECT t = ScrollTrackRect();
            const int th = t.bottom - t.top;

            int h = (kContentH > 0) ? (int)((double)th * (double)kViewH / (double)kContentH) : th;
            if (h < 30) h = 30;
            if (h > th) h = th;

            const int m = MaxScroll();
            const int span = th - h;
            const int y = t.top + ((m > 0 && span > 0)
                ? (int)((double)span * (double)g_scroll / (double)m) : 0);

            RECT r;
            SetRectLocal(r, t.left, y, t.right - t.left, h);
            return r;
        }

        // ---- 数值 <-> 像素 ----
        // 通用版本：把 x 映射到 [lo, hi]，再把值映射回 x。
        int ValueFromX(int x, const RECT& t, int lo, int hi)
        {
            double f = (double)(x - t.left) / (double)(t.right - t.left);
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            return lo + (int)std::lround(f * (hi - lo));
        }

        int XFromValue(int v, const RECT& t, int lo, int hi)
        {
            double f = (hi > lo) ? (double)(v - lo) / (double)(hi - lo) : 0.0;
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            return t.left + (int)std::lround(f * (t.right - t.left));
        }

        int VolFromX(int x) { return ValueFromX(x, BgmTrack(), 0, settings::kVolMax); }

        int VolToX(int v, const RECT& t)
        {
            return XFromValue(v, t, 0, settings::kVolMax);
        }

        int MsFromX(int x) { return ValueFromX(x, IntTrack(), kLoMs, kHiMs); }
        int MsToX(int ms) { return XFromValue(ms, IntTrack(), kLoMs, kHiMs); }

        // ---- 赎金目标：**范围按模式变**，但共用同一条滑条 ----
        //   普通 10 ~ 1000（原作 500）
        //   硬核 1000 ~ 9999
        // 硬核那一段对 500 这种小值是"够不着"的，所以切开关时会顺手把值抬上来
        // （见 ApplyModeDefaults）。
        int GoldLo() { return g_edit.hardcore ? settings::kGoldHardMin : settings::kGoldMin; }
        int GoldHi() { return g_edit.hardcore ? settings::kGoldHardMax : settings::kGoldMax; }

        // 吸到 10 的整数倍 —— 金币面额全是 10 的倍数，目标也跟着取整，
        // 滑条读数看起来才整齐。
        int GoldFromX(int x)
        {
            int v = ValueFromX(x, GoldTrack(), GoldLo(), GoldHi());
            v = (v / 10) * 10;
            if (v < GoldLo()) v = GoldLo();
            if (v > GoldHi()) v = GoldHi();
            return v;
        }

        int GoldToX(int v)
        {
            return XFromValue(v, GoldTrack(), GoldLo(), GoldHi());
        }

        // ---- 关窗惩罚时长：同样按模式变上限，0~18 秒 / 0~30 秒 ----
        int CloseHi()
        {
            return g_edit.hardcore ? settings::kCloseHardMax : settings::kCloseNormalMax;
        }

        // 吸到 100ms —— 一格 0.1 秒，读数看着舒服，也不至于拖不动。
        int CloseFromX(int x)
        {
            int v = ValueFromX(x, CloseTrack(), 0, CloseHi());
            v = (v / 100) * 100;
            if (v < 0) v = 0;
            if (v > CloseHi()) v = CloseHi();
            return v;
        }

        int CloseToX(int ms)
        {
            return XFromValue(ms, CloseTrack(), 0, CloseHi());
        }

        // ---- 假金币的百分比（0-100，只有硬核能拖）----
        bool FakeEnabled() { return g_edit.hardcore; }

        int PctFromX(int x, const RECT& t)
        {
            return ValueFromX(x, t, settings::kFakePctMin, settings::kFakePctMax);
        }

        int PctToX(int v, const RECT& t)
        {
            return XFromValue(v, t, settings::kFakePctMin, settings::kFakePctMax);
        }

        int ClampI(int v, int lo, int hi)
        {
            if (v < lo) return lo;
            if (v > hi) return hi;
            return v;
        }

        // ---- 假币三种形态：**一条滑条两颗珠子** ----
        //
        // 存储仍然是三个权重（前缀 / 后缀 / 两个都改），生成时按权重比例分配
        // （见 gold.cpp）。界面上把它们画成一条 0-100 轨道上的**两个分界点**：
        //
        //     [0 .. b1)   前缀形态（Gold → G01d）
        //     [b1 .. b2)  后缀形态（50 → 5o）
        //     [b2 .. 100] 两个都改（G01d_5o）—— **剩下多少全归它**
        //
        // 关键：第三段是"吃掉剩下的"，没有第三颗珠子。原来三颗珠子可以一起
        // 滑到 0，三种比例全变 0 —— 那会让假币"选不出形态"，是个不可控的坑。
        // 现在两颗珠子怎么滑都至少有一段是非零的：b1=b2=0 时全部落进"都改"。
        //
        // 于是三个权重**恒等于 100**，gold.cpp 那边的归一化照旧能用。
        const int kMixMin = 0;
        const int kMixMax = 100;

        // 两个权重 -> 两个分界点（b1 = 前缀，b2 = 前缀+后缀）
        void FakeMixBounds(int& b1, int& b2)
        {
            b1 = ClampI(g_edit.fakePrefixPct, kMixMin, kMixMax);
            b2 = b1 + ClampI(g_edit.fakeSuffixPct, kMixMin, kMixMax);
            if (b2 > kMixMax) b2 = kMixMax;
        }

        int MixToX(int v)
        {
            return XFromValue(v, FakeMixTrack(), kMixMin, kMixMax);
        }

        int MixFromX(int x)
        {
            return ValueFromX(x, FakeMixTrack(), kMixMin, kMixMax);
        }

        // 第 idx 颗珠子（0=前缀边界 1=后缀边界）的 x
        int MixKnobX(int idx)
        {
            int b1, b2;
            FakeMixBounds(b1, b2);
            return MixToX(idx == 0 ? b1 : b2);
        }

        // 离光标最近的是哪一颗珠子（拖的时候按它算）
        int MixNearestKnob(int x)
        {
            int best = 0, bestD = -1;
            for (int i = 0; i < 2; ++i)
            {
                const int d = MixKnobX(i) - x;
                const int ad = (d < 0) ? -d : d;
                if (bestD < 0 || ad < bestD) { bestD = ad; best = i; }
            }
            return best;
        }

        // 拖动第 idx 个分界点。两颗珠子互相不能越过（b1 <= b2）。
        void SetFakeMixBound(int idx, int v)
        {
            int b1, b2;
            FakeMixBounds(b1, b2);

            v = ClampI(v, kMixMin, kMixMax);

            if (idx == 0)
            {
                b1 = v;
                if (b1 > b2) b1 = b2;
            }
            else
            {
                b2 = v;
                if (b2 < b1) b2 = b1;
            }

            // 反算回三个权重：第三段吃掉剩余，所以三者恒和为 100
            g_edit.fakePrefixPct = b1;
            g_edit.fakeSuffixPct = b2 - b1;
            g_edit.fakeBothPct = kMixMax - b2;
        }

        // 切换硬核开关时，把"还是另一套默认值"的项换成这一套的默认值。
        // 用户手动调过的不动 —— 这样两种模式各自的默认观感都能保住：
        //   · 普通：赎金 500、关窗扣 10 秒
        //   · 硬核：赎金 5000、关窗扣 30 秒
        // 另外硬核那一段赎金区间是从 1000 起，所以低于 1000 的一律抬到默认值。
        void ApplyModeDefaults(settings::Set& s, bool toHardcore)
        {
            if (toHardcore)
            {
                if (s.goldGoal < settings::kGoldHardMin) s.goldGoal = settings::kHardcoreGoldGoal;
                if (s.childCloseMs == settings::kCloseNormalDefault)
                    s.childCloseMs = settings::kCloseHardDefault;
            }
            else
            {
                if (s.goldGoal > settings::kGoldMax) s.goldGoal = settings::kDefaultGoldGoal;
                if (s.childCloseMs == settings::kCloseHardDefault)
                    s.childCloseMs = settings::kCloseNormalDefault;
            }
        }

        // ---- 把编辑中的值推给子系统（实时生效）----
        void PushLive()
        {
            settings::SetCurrent(g_edit);
            settings::Apply();
        }

        // ---- 画一条滑条 ----
        // hotKnob：第几颗珠子要高亮（0 起）；-1 = 都不亮。
        // enabled=false（只有硬核才能拖的那几条，在普通模式下）整体压暗。
        void DrawTrack(Graphics& g, const RectF& rc, const RECT& local,
                       int knobCount, int x1, int x2, int fillL, int fillR,
                       int hotKnob, bool enabled = true)
        {
            const REAL cy = rc.Y + (REAL)local.top + (REAL)kTrackH * 0.5f;
            const REAL x0 = rc.X + (REAL)local.left;
            const REAL xN = rc.X + (REAL)local.right;

            const Color bgC = enabled ? kTrackBg : Color(120, 34, 36, 42);
            const Color fillC = enabled ? kTrackFill : Color(110, 96, 98, 106);
            const Color knobC = enabled ? kKnob : Color(255, 120, 122, 132);

            // 槽
            {
                RectF t(x0, cy - 3.0f, xN - x0, 6.0f);
                FillRound(g, t, 3.0f, bgC);
            }
            // 已选段（两条音量条就是 0 -> 当前值；区间条是 下限 -> 上限）
            if (fillR > fillL)
            {
                RectF f(rc.X + (REAL)fillL, cy - 3.0f, (REAL)(fillR - fillL), 6.0f);
                FillRound(g, f, 3.0f, fillC);
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
                const Color c = (enabled && i == hotKnob) ? kKnobActive : knobC;

                SolidBrush b(c);
                g.FillEllipse(&b, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);

                Pen edge(Color(200, 30, 20, 24), 1.6f);
                g.DrawEllipse(&edge, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);
            }
        }

        // ---- 画一整行滑条 ----
        // 标签 + 右侧读数 + 条带 + 两端（可选中点）刻度，都按 rowY 算出来。
        // midText 传 nullptr 就不画中点刻度；enabled=false 时整行压暗。
        void DrawSliderRow(Graphics& g, const RectF& rc, int rowY,
                           const wchar_t* label, const wchar_t* value,
                           const Color& valueColor, const RECT& track,
                           int knobCount, int x1, int x2, int fillL, int fillR,
                           int hotKnob, bool enabled,
                           const wchar_t* loText, const wchar_t* hiText,
                           const wchar_t* midText, int midX)
        {
            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)(kContentW - kM * 2);
            const REAL y = rc.Y + (REAL)(ViewOffset() + rowY);

            RectF t(xL, y, w, (REAL)kLabelH);
            DrawTextCjk(g, label, t, 14.0f, enabled ? kTextMain : kTextFaint);

            RectF v(xL, y, w, (REAL)kLabelH);
            DrawTextMono(g, value, v, 14.0f,
                enabled ? valueColor : kTextFaint, StringAlignmentFar);

            DrawTrack(g, rc, track, knobCount, x1, x2, fillL, fillR, hotKnob, enabled);

            RectF lo(xL, rc.Y + (REAL)(track.bottom + 2), w, 14.0f);
            DrawTextMono(g, loText, lo, 11.0f, kTextFaint);
            RectF hi(xL, rc.Y + (REAL)(track.bottom + 2), w, 14.0f);
            DrawTextMono(g, hiText, hi, 11.0f, kTextFaint, StringAlignmentFar);

            if (midText && midX > 0)
            {
                const REAL mx = rc.X + (REAL)midX;
                const REAL my = rc.Y + (REAL)track.top + (REAL)kTrackH * 0.5f;
                SolidBrush tick(kTextFaint);
                g.FillRectangle(&tick, mx, my - 9.0f, 1.0f, 18.0f);

                RectF mid(xL + w * 0.5f - 40.0f,
                    rc.Y + (REAL)(track.bottom + 2), 80.0f, 14.0f);
                DrawTextMono(g, midText, mid, 11.0f, kTextFaint, StringAlignmentCenter);
            }
        }

        // ---- 假币形态配比：一条轨道 + **两颗珠子** ----
        //
        // 和"遭遇战间隔"那条双珠条一个路子。两颗珠子是两个分界点，
        // 划出三段：
        //
        //     [0 .. b1)   前缀改        —— 红
        //     [b1 .. b2)  后缀改        —— 绿（两颗珠子之间）
        //     [b2 .. 100] 两个都改      —— 红（吃掉剩下的，没有第三颗珠子）
        //
        // 配色是用户指定的：**中间那段绿、两边红**。
        // 第三段吃掉剩余这一条很关键 —— 它保证三种形态不可能同时为 0。
        void DrawFakeMixRow(Graphics& g, const RectF& rc, bool enabled)
        {
            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)(kContentW - kM * 2);
            const REAL y = rc.Y + (REAL)(ViewOffset() + kRowFakeMix);

            int b1, b2;
            FakeMixBounds(b1, b2);

            const int bothPct = kMixMax - b2;

            // 标签
            RectF t(xL, y, w, (REAL)kLabelH);
            DrawTextCjk(g, L"假币形态配比（前缀 / 后缀 / 都改）", t, 14.0f,
                enabled ? kTextMain : kTextFaint);

            // 右侧读数：三个比例一起报，一眼对得上
            wchar_t val[64];
            swprintf_s(val, L"%d / %d / %d", b1, b2 - b1, bothPct);
            RectF v(xL, y, w, (REAL)kLabelH);
            DrawTextMono(g, val, v, 14.0f, enabled ? kTextDim : kTextFaint, StringAlignmentFar);

            const RECT tr = FakeMixTrack();
            const REAL cy = rc.Y + (REAL)tr.top + (REAL)kTrackH * 0.5f;
            const REAL x0 = rc.X + (REAL)tr.left;
            const REAL xN = rc.X + (REAL)tr.right;

            // 槽
            {
                RectF track(x0, cy - 3.0f, xN - x0, 6.0f);
                FillRound(g, track, 3.0f, enabled ? kTrackBg : Color(120, 34, 36, 42));
            }

            // 三段：红 / 绿 / 红
            if (enabled)
            {
                const REAL xb1 = rc.X + (REAL)MixToX(b1);
                const REAL xb2 = rc.X + (REAL)MixToX(b2);

                const Color kRed(255, 220, 60, 60);
                const Color kGreen(255, 60, 200, 90);

                const REAL seg[4] = { x0, xb1, xb2, xN };
                const Color segC[3] = { kRed, kGreen, kRed };

                for (int i = 0; i < 3; ++i)
                {
                    if (seg[i + 1] - seg[i] < 0.5f) continue;
                    RectF f(seg[i], cy - 3.0f, seg[i + 1] - seg[i], 6.0f);
                    FillRound(g, f, 3.0f, segC[i]);
                }
            }

            // 端点刻度
            {
                SolidBrush dim(kTextFaint);
                g.FillRectangle(&dim, x0, cy - 6.0f, 1.0f, 12.0f);
                g.FillRectangle(&dim, xN - 1.0f, cy - 6.0f, 1.0f, 12.0f);
            }

            // 两颗珠子
            for (int i = 0; i < 2; ++i)
            {
                const REAL kx = rc.X + (REAL)MixKnobX(i);
                const bool hot = enabled && g_drag == DRAG_FAKE_MIX && g_dragMixIdx == i;
                const Color c = enabled ? (hot ? kKnobActive : kKnob)
                    : Color(255, 120, 122, 132);

                SolidBrush b(c);
                g.FillEllipse(&b, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);

                Pen edge(Color(200, 30, 20, 24), 1.6f);
                g.DrawEllipse(&edge, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);
            }

            // 两端刻度文字
            RectF lo(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
            DrawTextMono(g, L"0", lo, 11.0f, kTextFaint);
            RectF hi(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
            DrawTextMono(g, L"100（右段=都改）", hi, 11.0f, kTextFaint, StringAlignmentFar);
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
            const REAL w = (REAL)(kContentW - kM * 2);

            // ================= 头部（固定，不滚动）=================
            {
                SolidBrush bar(kAccent);
                g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 18.0f);
                RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 30.0f);
                DrawTextCjk(g, L"启动设置", t, 20.0f, kTextMain, StringAlignmentNear, FontStyleBold);
            }
            {
                RectF t(xL, rc.Y + 42.0f, w, 16.0f);
                DrawTextCjk(g, L"请在这里修改你想要的参数，会自动保存。",
                    t, 12.0f, kTextHint);
            }

            wchar_t buf[96];

            // ================= 视口（可滚动）=================
            //
            // 裁到视口范围再画：不然滚出去的行会糊到头部和底部上去。
            g.SetClip(RectF(rc.X, rc.Y + (REAL)kViewTop, (REAL)kContentW, (REAL)kViewH));

            // ---- 光敏安全（癫痫模式）—— 排在最上面 ----
            {
                DrawCheckbox(g, rc, SafeBox(), g_edit.photosensitiveSafe, false);

                const RECT b = SafeBox();
                RectF t(rc.X + (REAL)(b.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowSafe), 300.0f, (REAL)kCheckH);
                DrawTextCjk(g, L"光敏安全模式（癫痫模式）", t, 14.0f, kTextMain);

                RectF d(rc.X + (REAL)(b.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowSafe + 18), 420.0f, 16.0f);
                DrawTextCjk(g, L"压低整屏亮度跳变与闪烁", d, 11.0f, kTextHint);
            }

            // ---- 背景音乐 ----
            {
                const RECT tr = BgmTrack();
                const int kx = VolToX(g_edit.bgmVol, tr);
                swprintf_s(buf, L"%d%%", g_edit.bgmVol);
                DrawSliderRow(g, rc, kRowBgm, L"背景音乐", buf,
                    g_edit.bgmVol > 100 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_BGM ? 0 : -1, true, L"0", L"200%", nullptr, 0);
            }

            // ---- 音效 ----
            {
                const RECT tr = SfxTrack();
                const int kx = VolToX(g_edit.sfxVol, tr);
                swprintf_s(buf, L"%d%%", g_edit.sfxVol);
                DrawSliderRow(g, rc, kRowSfx, L"音效", buf,
                    g_edit.sfxVol > 100 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_SFX ? 0 : -1, true, L"0", L"200%", nullptr, 0);
            }

            // ---- 硬核模式 —— 紧跟音效，因为它是下面那两条假金币滑条的总闸 ----
            {
                DrawCheckbox(g, rc, HardBox(), g_edit.hardcore, false);

                const RECT hb = HardBox();
                RectF t(rc.X + (REAL)(hb.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowHard), 300.0f, (REAL)kCheckH);
                DrawTextCjk(g, L"硬核模式", t, 14.0f,
                    g_edit.hardcore ? kAccent : kTextMain, StringAlignmentNear, FontStyleBold);

                RectF d(rc.X + (REAL)(hb.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowHard + 20), 440.0f, 16.0f);
                DrawTextCjk(g,
                    g_edit.hardcore
                    ? L"已开启"
                    : L"开启后",
                    d, 11.0f, kTextHint);
            }

            // ---- 遭遇战间隔（随机区间，双滑块）----
            {
                const double f1 = (double)g_edit.minMs / 1000.0;
                const double f2 = (double)g_edit.maxMs / 1000.0;
                if (g_edit.minMs == g_edit.maxMs)
                    swprintf_s(buf, L"%d-%d ms（固定）", g_edit.minMs, g_edit.maxMs);
                else
                    swprintf_s(buf, L"%d-%d ms（%.2f-%.2f 秒）",
                        g_edit.minMs, g_edit.maxMs, f1, f2);

                const RECT tr = IntTrack();
                const int x1 = MsToX(g_edit.minMs);
                const int x2 = MsToX(g_edit.maxMs);
                const int hotKnob = (g_drag == DRAG_IDLE_LO) ? 0
                    : (g_drag == DRAG_IDLE_HI ? 1 : -1);

                DrawSliderRow(g, rc, kRowIdle, L"每次跳杀间隔（随机区间）", buf, kTextDim, tr,
                    2, x1, x2, x1, x2, hotKnob, true,
                    L"20ms", L"90s",
                    // 中点刻度标 45s —— 区间跨度到 90 秒之后，光有两端读数
                    // 很难估出"我这一拖大概落在多少秒"。
                    L"45s", (tr.left + tr.right) / 2);
            }

            // ---- 赎金目标 ----
            //
            // **范围随模式变、共用同一条滑条**：普通 10-1000，硬核 1000-9999。
            // 切开关时 ApplyModeDefaults 会把够不着的值抬/压到这一段的默认值，
            // 所以不会出现"珠子贴在两端不动"的错觉。
            // 超过 9999 的部分由 settings::GoldGoal() 夹掉。
            {
                const RECT tr = GoldTrack();
                const int shown = g_edit.goldGoal;
                const int kx = GoldToX(shown);

                swprintf_s(buf, L"%d", shown);
                DrawSliderRow(g, rc, kRowGold,
                    g_edit.hardcore ? L"赎金目标金币（硬核区间 1000-9999）" : L"赎金目标金币",
                    buf, shown > 500 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_GOLD ? 0 : -1, true,
                    g_edit.hardcore ? L"1000" : L"10",
                    g_edit.hardcore ? L"9999" : L"1000",
                    // 中点刻度标这一段的默认值：普通 500（原作）、硬核 5000
                    g_edit.hardcore ? L"5000" : L"500",
                    g_edit.hardcore ? GoldToX(5000) : GoldToX(500));
            }

            // ---- 关窗惩罚时长 ----
            // 同样是"一条滑条、范围随模式变"：普通 0-18 秒，硬核 0-30 秒。
            {
                const RECT tr = CloseTrack();
                const int ms = g_edit.childCloseMs;
                const int kx = CloseToX(ms);

                if (ms == 0)
                    swprintf_s(buf, L"0（关窗不扣时间）");
                else
                    swprintf_s(buf, L"%.1f 秒", ms / 1000.0);

                DrawSliderRow(g, rc, kRowClose, L"关窗惩罚时长", buf,
                    ms > 10000 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_CLOSE ? 0 : -1, true,
                    L"0", g_edit.hardcore ? L"30s" : L"18s",
                    g_edit.hardcore ? L"30s" : L"18s",
                    g_edit.hardcore ? CloseToX(30000) : CloseToX(18000));
            }

            // ---- 假金币比例 + 三种形态的配比（**只有硬核能拖**）----
            //
            // 普通模式下整行压暗且拖不动（FakeEnabled() 决定），值本身留着
            // —— 打开硬核开关就接着用。
            {
                const bool on = FakeEnabled();

                {
                    const RECT tr = FakePctTrack();
                    const int kx = PctToX(g_edit.fakePercent, tr);
                    swprintf_s(buf, L"%d%%", g_edit.fakePercent);
                    DrawSliderRow(g, rc, kRowFakePct, L"假金币比例", buf,
                        kTextDim, tr, 1, kx, kx, tr.left, kx,
                        g_drag == DRAG_FAKE_PCT ? 0 : -1, on,
                        L"0%", L"100%", nullptr, 0);
                }

                // 两颗珠子一条道：见 FakeMixBounds 那一带的说明
                DrawFakeMixRow(g, rc, on);
            }

            g.ResetClip();

            // ================= 滚动条 =================
            {
                const RECT t = ScrollTrackRect();
                const RECT th = ScrollThumbRect();

                RectF track(xL + (REAL)(t.left - xL), rc.Y + (REAL)t.top,
                    (REAL)(t.right - t.left), (REAL)(t.bottom - t.top));
                FillRound(g, track, 6.0f, Color(120, 26, 28, 34));

                // 内容没超出视口就不画滑块（也没得滚）
                if (MaxScroll() > 0)
                {
                    RectF thumb(rc.X + (REAL)th.left, rc.Y + (REAL)th.top,
                        (REAL)(th.right - th.left), (REAL)(th.bottom - th.top));
                    FillRound(g, thumb, 6.0f,
                        (g_drag == DRAG_SCROLL) ? kAccentSoft : Color(190, 120, 122, 132));
                }
            }

            // ================= 底部（固定，不滚动）=================
            DrawButton(g, rc, ResetBtn(), L"恢复默认", g_hot == HOT_RESET, false);
            DrawButton(g, rc, StartBtn(), L"开 始", g_hot == HOT_START, true);

            // 硬核模式下安全阀**也是按一次就停**（曾经打算做成"连按两次"，
            // 后来放弃了），所以这句话在两种模式下都成立，不用分支。
            {
                wchar_t hint[192];
                swprintf_s(hint,
                    L"开始后随时可以按 Ctrl + Alt + Shift + %c 立刻退出并全部还原。",
                    (wchar_t)g_panicVk);
                RectF t(xL, rc.Y + (REAL)kHintTop, w, 16.0f);
                DrawTextCjk(g, hint, t, 12.0f, kTextHint);
            }
        }

        void Paint(Graphics& g, const RectF& rc, DWORD, void*)
        {
            PaintContent(g, rc);
        }

        // ---- 拖拽 ----
        //
        // 注意：这里只用到 p.x（横向拖滑条），纵向的位置在命中判定那一步
        // 已经算过滚动偏移了，所以**不要**在这里再减一次 ViewOffset。
        void ApplyDrag(POINT p)
        {
            switch (g_drag)
            {
            case DRAG_BGM:
                g_edit.bgmVol = VolFromX(p.x);
                audio::SetBgmLevel(g_edit.bgmVol);
                break;

            case DRAG_SFX:
                g_edit.sfxVol = VolFromX(p.x);
                audio::SetSfxLevel(g_edit.sfxVol);
                break;

            case DRAG_IDLE_LO:      // 下限：不许越过上限
            {
                int v = MsFromX(p.x);
                if (v > g_edit.maxMs) v = g_edit.maxMs;
                g_edit.minMs = v;
                break;
            }
            case DRAG_IDLE_HI:      // 上限：不许越过下限
            {
                int v = MsFromX(p.x);
                if (v < g_edit.minMs) v = g_edit.minMs;
                g_edit.maxMs = v;
                break;
            }

            // 赎金 / 关窗惩罚：**两种模式都能拖**，只是范围不同。
            // GoldFromX / CloseFromX 内部按 g_edit.hardcore 取范围。
            case DRAG_GOLD:
                g_edit.goldGoal = GoldFromX(p.x);
                break;

            case DRAG_CLOSE:
                g_edit.childCloseMs = CloseFromX(p.x);
                break;

            // 下面四条只有硬核能拖（普通模式下整行是压暗的，也不该响应）
            case DRAG_FAKE_PCT:
                if (!FakeEnabled()) return;
                g_edit.fakePercent = PctFromX(p.x, FakePctTrack());
                break;

            case DRAG_FAKE_MIX:
                if (!FakeEnabled()) return;
                if (g_dragMixIdx < 0 || g_dragMixIdx > 1) return;   // 只有两颗珠子
                SetFakeMixBound(g_dragMixIdx, MixFromX(p.x));
                break;

            case DRAG_SCROLL:
            {
                const RECT t = ScrollTrackRect();
                const RECT th = ScrollThumbRect();
                const int span = (t.bottom - t.top) - (th.bottom - th.top);
                const int m = MaxScroll();
                if (span > 0 && m > 0)
                {
                    const int y = p.y - g_scrollGrab - t.top;
                    g_scroll = ClampScroll((int)((double)y * (double)m / (double)span));
                }
                break;
            }

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

                    // 两套区间不一样，切过来时把"够不着"或"还是另一套默认"的项
                    // 换成这一套的默认值（用户手调过的不动）。
                    // 不这么做的话，普通模式下的 500 切到硬核会变成珠子贴在
                    // 最左边、拖半天没反应。
                    ApplyModeDefaults(g_edit, g_edit.hardcore);

                    PushLive();

                    // 打开开关时整扇窗抖一下，给个"这东西很重"的反馈。
                    // AnimateJolt 走的是真实窗口位移（SetWindowPos），
                    // 和 SetWobble 那种绘制层微抖不是一回事；有动画在跑时
                    // 它会排队，不会把别的动画掐掉。
                    aero::AnimateJolt(hwnd, 9.0f, 380);

                    elog::Write(L"[setup] 硬核模式 %s（赎金 %d，关窗惩罚 %dms，倒计时 %d 秒，假币 %d%%）",
                        g_edit.hardcore ? L"开" : L"关",
                        g_edit.goldGoal, g_edit.childCloseMs,
                        g_edit.hardcore ? settings::kHardcoreRansomMs / 1000 : 90,
                        g_edit.fakePercent);
                    aero::Repaint(hwnd);
                    return;
                }

                // ---- 滚动条 ----
                // 点在滑块上就抓着拖；点在轨道空白处就翻一页。
                // 放在滑条判定之前：滚动条贴在右边缘，和滑条不重叠。
                if (Hit(ScrollTrackRect(), p) && MaxScroll() > 0)
                {
                    const RECT th = ScrollThumbRect();
                    if (Hit(th, p))
                    {
                        g_drag = DRAG_SCROLL;
                        g_scrollGrab = p.y - th.top;
                    }
                    else
                    {
                        const RECT t = ScrollTrackRect();
                        const double f = (double)(p.y - t.top) / (double)(t.bottom - t.top);
                        g_scroll = ClampScroll((int)(f * MaxScroll()));
                    }
                    SetCapture(hwnd);
                    aero::Repaint(hwnd);
                    return;
                }

                // 视口之外的空白（头部 / 底部）不处理
                if (!InView(p)) return;

                // ---- 滑条（一行行判）----
                const RECT bt = BgmTrack();
                if (TrackHit(bt, p)) { g_drag = DRAG_BGM; SetCapture(hwnd); ApplyDrag(p); return; }

                const RECT st = SfxTrack();
                if (TrackHit(st, p)) { g_drag = DRAG_SFX; SetCapture(hwnd); ApplyDrag(p); return; }

                const RECT it = IntTrack();
                if (TrackHit(it, p))
                {
                    // 两颗珠子重合时按距离选一颗，跟手的那颗才会动。
                    const int x1 = MsToX(g_edit.minMs);
                    const int x2 = MsToX(g_edit.maxMs);
                    const int d1 = (p.x > x1) ? (p.x - x1) : (x1 - p.x);
                    const int d2 = (p.x > x2) ? (p.x - x2) : (x2 - p.x);
                    g_drag = (d1 <= d2) ? DRAG_IDLE_LO : DRAG_IDLE_HI;
                    SetCapture(hwnd);
                    ApplyDrag(p);
                    return;
                }

                const RECT gt = GoldTrack();
                if (TrackHit(gt, p)) { g_drag = DRAG_GOLD; SetCapture(hwnd); ApplyDrag(p); return; }

                const RECT ct = CloseTrack();
                if (TrackHit(ct, p)) { g_drag = DRAG_CLOSE; SetCapture(hwnd); ApplyDrag(p); return; }

                // 假金币那两条：普通模式下拖不动（ApplyDrag 里还有一道保险）
                if (FakeEnabled())
                {
                    const RECT fp = FakePctTrack();
                    if (TrackHit(fp, p)) { g_drag = DRAG_FAKE_PCT; SetCapture(hwnd); ApplyDrag(p); return; }

                    const RECT fm = FakeMixTrack();
                    if (TrackHit(fm, p))
                    {
                        // 三颗珠子在同一条道上，按下时挑离光标最近的那颗
                        g_dragMixIdx = MixNearestKnob(p.x);
                        g_drag = DRAG_FAKE_MIX;
                        SetCapture(hwnd);
                        ApplyDrag(p);
                        return;
                    }
                }

                return;
            }

            case WM_LBUTTONUP:
                if (g_drag)
                {
                    g_drag = DRAG_NONE;
                    ReleaseCapture();
                    aero::Repaint(hwnd);
                }
                return;

            default:
                return;
            }
        }

        // 滚轮：指针压在控件上就微调那个控件；压在空白处（或者滚动条上）
        // 就滚动整屏 —— 内容比视口高，没有滚动很难受。
        //
        // 只靠拖动的话，想把 100% 改成 105% 得拖半天。
        void OnWheel(HWND hwnd, POINT p, int delta, void*)
        {
            const int step = (delta > 0) ? 1 : -1;

            // 滚动一格走多少：大约是"小半行"，滚起来不至于一格跳一整屏
            const int kScrollStep = 34;
            const bool overScrollbar = Hit(ScrollTrackRect(), p);

            if (!overScrollbar && InView(p))
            {
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
                const RECT ct = CloseTrack();

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
                    const int mstep = MsStep() * ((delta > 0) ? 1 : -1);
                    const int x1 = MsToX(g_edit.minMs);
                    const int x2 = MsToX(g_edit.maxMs);
                    if (p.x < (x1 + x2) / 2)
                    {
                        g_edit.minMs += mstep;
                        if (g_edit.minMs < kLoMs)        g_edit.minMs = kLoMs;
                        if (g_edit.minMs > g_edit.maxMs) g_edit.minMs = g_edit.maxMs;
                    }
                    else
                    {
                        g_edit.maxMs += mstep;
                        if (g_edit.maxMs > kHiMs)        g_edit.maxMs = kHiMs;
                        if (g_edit.maxMs < g_edit.minMs) g_edit.maxMs = g_edit.minMs;
                    }
                }
                else if (p.y >= gt.top - 10 && p.y < gt.bottom + 10)
                {
                    // 一格 1%：普通 10-1000 是 ±10，硬核 1000-9999 是 ±90。
                    // 两种模式的跨度差 10 倍，所以步长也按跨度算，手感才一致。
                    const int gstep = (GoldHi() - GoldLo()) / 100;
                    g_edit.goldGoal += step * ((gstep > 0) ? gstep : 10);
                    if (g_edit.goldGoal < GoldLo()) g_edit.goldGoal = GoldLo();
                    if (g_edit.goldGoal > GoldHi()) g_edit.goldGoal = GoldHi();
                    // 吸到 10 的整倍数，和拖拽保持一致
                    g_edit.goldGoal = (g_edit.goldGoal / 10) * 10;
                }
                else if (p.y >= ct.top - 10 && p.y < ct.bottom + 10)
                {
                    // 一格 1%（普通 180ms / 硬核 300ms），吸到 100ms
                    const int cstep = (CloseHi() / 100 > 0) ? (CloseHi() / 100) : 100;
                    g_edit.childCloseMs += step * cstep;
                    if (g_edit.childCloseMs < 0) g_edit.childCloseMs = 0;
                    if (g_edit.childCloseMs > CloseHi()) g_edit.childCloseMs = CloseHi();
                    g_edit.childCloseMs = (g_edit.childCloseMs / 100) * 100;
                }
                else if (FakeEnabled() &&
                         p.y >= FakePctTrack().top - 10 && p.y < FakePctTrack().bottom + 10)
                {
                    g_edit.fakePercent += step;
                    if (g_edit.fakePercent < settings::kFakePctMin) g_edit.fakePercent = settings::kFakePctMin;
                    if (g_edit.fakePercent > settings::kFakePctMax) g_edit.fakePercent = settings::kFakePctMax;
                }
                else if (FakeEnabled() &&
                         p.y >= FakeMixTrack().top - 10 && p.y < FakeMixTrack().bottom + 10)
                {
                    // 三颗珠子一条道：滚轮调离光标最近的那颗
                    // （按键的时候也是这么选的，手感一致）
                    SetFakeMixBound(MixNearestKnob(p.x),
                        MixFromX(MixKnobX(MixNearestKnob(p.x)) + step * 2));
                }
                else
                {
                    // 压在这一行空白（比如标签右边）上：当成滚动处理
                    g_scroll = ClampScroll(g_scroll + ((delta > 0) ? -kScrollStep : kScrollStep));
                }
            }
            else
            {
                // 头部 / 底部 / 滚动条上滚：滚动整屏
                g_scroll = ClampScroll(g_scroll + ((delta > 0) ? -kScrollStep : kScrollStep));
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
        bool  g_hotBtn = false;             // 「确认」按钮高亮
        bool  g_hotBack = false;            // 「返回上一级」按钮高亮
        bool  g_armed = false;              // 在本按钮上按下过左键
        bool  g_armedBack = false;          // 在「返回」上按下过左键
        bool  g_confirm = false;            // 点了「我知道了」（区别于关窗口）
        bool  g_back = false;               // 点了「返回上一级」-> 回设置界面
        setup_ui::Verdict g_verdict = setup_ui::VERDICT_ABORT;
        int   g_panicVk = 'Q';

        // 这一屏画哪一套内容：
        //   0 = 普通模式的通用提示（"这一步之前，先说清楚"）
        //   1 = **硬核模式专用警告**（"硬核模式：先把后果说清楚"）
        // 两者共用同一套窗口管道（建窗 / 模态循环 / 关窗 / 热键），
        // 只有标题、正文、配色和按钮文案不同。整屏尺寸也共用，
        // 所以硬核那套的正文必须塞进 kLede1Top..kExitTop 这段（见下）。
        int   g_mode = 0;

        RECT OkBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        // 「返回上一级」：摆在「开始」左边。点它 = 回到设置界面改设置重来，
        // 不是退出程序（那是关窗口那条路）。
        RECT BackBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW * 2 - 14, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        // 和设置窗口一样：绘制逻辑单独一个 PaintContent，活窗口和
        // `--notice-ui` 离线导出共用同一份。
        void PaintContent(Graphics& g, const RectF& rc)
        {
            DrawPanel(g, rc);

            // 硬核那一屏把整圈边框描红：一眼就能看出"这不是平时那一屏"。
            if (g_mode == 1)
                StrokeRound(g, rc, 10.0f, Color(210, 232, 40, 40), 2.0f);

            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)kTextW;

            if (g_mode == 1)
            {
                // ================= 硬核模式专用警告 =================
                //
                // 版面和通用那屏共用（同样的 kW/kH、同一个退出高亮框、
                // 同一个按钮），正文必须塞进 [kLede1Top, kExitTop) 之间 ——
                // 现在是 1 行引导 + 6 条要点，17px 行距，最后一行收在 195 附近。
                {
                    SolidBrush bar(Color(255, 232, 40, 40));
                    g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 20.0f);
                    RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 34.0f);
                    DrawTextCjk(g, L"硬核模式：先把后果说清楚", t, 21.0f, kTextMain,
                        StringAlignmentNear, FontStyleBold);
                }

                {
                    RectF t(xL, rc.Y + (REAL)kLede1Top, w, 20.0f);
                    DrawTextCjk(g, L"你开了硬核模式，这一轮和平时完全不一样：",
                        t, 13.0f, kAccent);
                }

                // 要点行。用亮色（kTextHint）而不是灰字：这一屏是要人**读**的，
                // 不是装饰。
                //
                // **内容要跟着用户在上一屏选的值走** —— 这一屏是最后一道
                // "确认你真的知道会发生什么"，写死数字就等于骗人。
                const settings::Set& s = settings::Current();

                wchar_t l1[160], l4[160], l6[160];
                swprintf_s(l1, L"· 倒计时 3 分钟；赎金 %d Gold、关窗惩罚 %.1f 秒都是你刚选的",
                    settings::GoldGoal(), settings::ChildCloseMs() / 1000.0);

                swprintf_s(l4, L"· 假币占真币的 %d%%：前缀改 %d / 后缀改 %d / 两个都改 %d",
                    settings::FakePercent(),
                    s.fakePrefixPct, s.fakeSuffixPct, s.fakeBothPct);

                swprintf_s(l6, L"· 桌面文件夹/文件会被随机锁 0.9~9 秒；中途不能切回普通模式");

                const wchar_t* kLines[] = {
                    l1,
                    L"· 桌面上的文件夹和文件会被随机锁住 0.9~9 秒，期间点不开也拖不动",
                    L"· 金币会撒到 C:\\ D:\\ 这类固定盘的顶层目录，还会混进假金币",
                    l4,
                    L"· 弹窗最多 22 个，还会贴着你的鼠标生成，专门挡你点击",
                    l6,
                };
                const int n = (int)(sizeof(kLines) / sizeof(kLines[0]));

                for (int i = 0; i < n; ++i)
                {
                    RectF t(xL, rc.Y + (REAL)(kLede1Top + 22 + i * 17), w, 17.0f);
                    DrawTextCjk(g, kLines[i], t, 12.0f, kTextHint);
                }
            }
            else
            {
                // ================= 普通模式的通用提示 =================
                {
                    SolidBrush bar(Color(255, 236, 92, 92));
                    g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 20.0f);
                    RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 34.0f);
                    DrawTextCjk(g, L"开始之前，先说清楚", t, 21.0f, kTextMain,
                        StringAlignmentNear, FontStyleBold);
                }
                {
                    RectF t(xL, rc.Y + (REAL)kLede1Top, w, 20.0f);
                    DrawTextCjk(g, L"这个版本会真的动你的桌面，不是玩笑：",
                        t, 13.0f, kAccent);
                }
                {
                    RectF t(xL, rc.Y + (REAL)kLede2Top, w, 20.0f);
                    DrawTextCjk(g, L"桌面图标会被标记成「已加密」、目前开着的窗口会被收进任务栏、",
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

                // 把用户刚在设置里定的关键数值回声一遍：这一屏除了讲后果，
                // 也该让人确认"我选的确实是这个"。
                {
                    wchar_t line[192];
                    swprintf_s(line,
                        L"本轮：赎金 %d Gold ／ 跳杀间隔 %d-%dms ／ 关窗惩罚 %.1f 秒。",
                        settings::GoldGoal(), settings::MinMs(), settings::MaxMs(),
                        settings::ChildCloseMs() / 1000.0);

                    RectF t(xL, rc.Y + (REAL)(kLede2Top + 66), w, 20.0f);
                    DrawTextCjk(g, line, t, 12.0f, kTextHint);
                }
            }

            // ---- 更要紧的：怎么退 ----
            {
                const REAL top = rc.Y + (REAL)kExitTop;
                RectF box(xL, top, w, 68.0f);
                FillRound(g, box, 8.0f, Color(150, 120, 16, 18));
                StrokeRound(g, box, 8.0f, Color(160, 200, 26, 30), 1.4f);

                RectF t1(xL + 14.0f, top + 8.0f, w - 28.0f, 24.0f);
                wchar_t line[160];
                swprintf_s(line, L"想逃课：按 Ctrl + Alt + Shift + %c",
                    (wchar_t)g_panicVk);
                DrawTextCjk(g, line, t1, 16.0f, kTextMain,
                    StringAlignmentNear, FontStyleBold);

                RectF t2(xL + 14.0f, top + 32.0f, w - 28.0f, 20.0f);
                DrawTextCjk(g, L"这是全局热键，任何时候都有效，按下去立刻退出并全部还原。",
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

            // 两个按钮：「返回上一级」（次要样式）在左，「开始」在右。
            DrawButton(g, rc, BackBtn(), L"← 返回上一级", g_hotBack, false);
            DrawButton(g, rc, OkBtn(),
                g_mode == 1 ? L"我明白，开始" : L"我知道了，开始", g_hotBtn, true);
        }

        void Paint(Graphics& g, const RectF& rc, DWORD, void*)
        {
            PaintContent(g, rc);
        }

        void OnMouse(HWND hwnd, UINT msg, POINT p, WPARAM, void*)
        {
            if (msg == WM_MOUSEMOVE)
            {
                const bool hotOk = Hit(OkBtn(), p);
                const bool hotBack = Hit(BackBtn(), p);
                if (hotOk != g_hotBtn || hotBack != g_hotBack)
                {
                    g_hotBtn = hotOk;
                    g_hotBack = hotBack;
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
                g_armedBack = Hit(BackBtn(), p);
                return;
            }

            if (msg == WM_LBUTTONUP)
            {
                const bool hitOk = Hit(OkBtn(), p);
                const bool hitBack = Hit(BackBtn(), p);

                if (g_armed && hitOk && !g_done)
                {
                    g_confirm = true;
                    g_done = true;

                    // 这里以前**只**置了标志，没请求关闭动画 ——
                    // 于是窗口被模态循环后面那句 aero::Destroy() 直接销毁，
                    // 玩家点「我知道了」看到的是一下子消失。补上这一句，
                    // 让窗口走和其它路径一致的 180ms 淡出。
                    aero::AnimateClose(hwnd);
                }
                else if (g_armedBack && hitBack && !g_done)
                {
                    // 返回上一级：回设置界面重来（不是退出程序）。
                    g_back = true;
                    g_done = true;
                    aero::AnimateClose(hwnd);
                }

                g_armed = false;
                g_armedBack = false;
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

    // 提示屏的离线导出。和 setup::RenderPreview 同一套做法。
    // 导哪一屏由 notice::g_mode 决定 —— 调用方（entity_main）先设好再调，
    // 或者直接用下面两个包装函数。
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
        g_drag = DRAG_NONE;
        g_hot = HOT_NONE;
        g_scroll = 0;                     // 每次打开都从顶上开始
        g_scrollGrab = 0;
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

    // 提示屏只有一个实现，两个模式共用（见 notice::g_mode 的说明）：
    //   mode 0 = 普通模式的通用提示
    //   mode 1 = 硬核模式专用警告
    // entity_main 按 settings::Hardcore() 决定调哪一个。
    static Verdict ShowNotice(HINSTANCE hInst, int panicVk, int mode)
    {
        using namespace notice;

        g_mode = mode;
        g_hwnd = nullptr;
        g_done = false;
        g_hotBtn = false;
        g_hotBack = false;
        g_armed = false;
        g_armedBack = false;
        g_confirm = false;
        g_back = false;
        g_verdict = VERDICT_ABORT;      // 默认「不演」——只有明确点了按钮才继续
        g_panicVk = panicVk;

        aero::Options opt;
        opt.title = (mode == 1)
            ? L"Ransom_dev — 硬核模式：你会喜欢的"
            : L"Ransom_dev — 开始前请读这里";
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
            elog::Write(L"[setup] %s窗口创建失败", mode == 1 ? L"硬核警告" : L"应急提示");
            return VERDICT_ERROR;
        }
        g_hwnd = h;
        SetWindowTextW(h, L"Ransom_dev");

        elog::Write(L"[setup] %s已弹出（安全阀 Ctrl+Alt+Shift+%c）",
            mode == 1 ? L"硬核模式警告" : L"应急提示", (wchar_t)panicVk);

        // 同上：跑到窗口消失为止，让关闭动画播完。
        // 这里有两处会把 g_done 置位（点按钮 / 点 X / 按热键），
        // 都以 g_done 为循环条件的话，关闭动画全都会被跳过去。
        MSG msg;
        while (aero::IsAlive(h) && GetMessageW(&msg, h, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 返回优先：点了「返回上一级」就回设置界面（调用方按 VERDICT_BACK 重新弹设置）。
        if (g_back)         g_verdict = VERDICT_BACK;
        else if (g_confirm) g_verdict = VERDICT_START;   // 明确点了按钮才继续

        if (aero::IsAlive(h)) aero::Destroy(h);
        g_hwnd = nullptr;
        ClearHotkeyHandler();

        elog::Write(L"[setup] %s关闭，结果 %d",
            mode == 1 ? L"硬核模式警告" : L"应急提示", (int)g_verdict);
        return g_verdict;
    }

    Verdict ShowSafetyNotice(HINSTANCE hInst, int panicVk)
    {
        return ShowNotice(hInst, panicVk, 0);
    }

    Verdict ShowHardcoreNotice(HINSTANCE hInst, int panicVk)
    {
        return ShowNotice(hInst, panicVk, 1);
    }

    bool DumpSettingsPreview(const wchar_t* path, const settings::Set& s, bool grid)
    {
        return setup::RenderPreview(path, s, grid);
    }

    bool DumpNoticePreview(const wchar_t* path, bool grid)
    {
        notice::g_mode = 0;
        return RenderNoticePreview(path, grid);
    }

    bool DumpHardcoreNoticePreview(const wchar_t* path, bool grid)
    {
        notice::g_mode = 1;
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
