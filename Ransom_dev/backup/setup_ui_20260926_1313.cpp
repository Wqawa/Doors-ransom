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
//
//  ---- 配色 ----
//
//  本文件的全部颜色都集中在下面那张「统一配色表」里。想换风格只改那一块，
//  下面的绘制代码一行都不用动。方案 v3 —— 中性灰 + 纯红。
//
//  ---- 缓动 ----
//
//  滑条数值、复选框进度、滚动偏移都挂了 Tween（见文件上半部分的缓动工具）。
//  **所有**数值变化都走缓动：
//    * 拖拽     —— 短缓动（80ms），珠子"追"鼠标而不是贴死；
//    * 滚轮 / 恢复默认 / 切换模式 —— 长缓动（180ms），滑过去；
//    * 复选框勾选 —— 进度补间，对勾从中心"长"出来。
//
//  逻辑判定（比如 FakeEnabled()、goldGoal 的有效值）始终读 g_edit（目标值），
//  绘制读 Tween。两者在缓动期间会短暂分离 —— 用户看到的是"正在滑过去的
//  中间态"，逻辑上它已经是终值。
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
#include <shellapi.h>          // SHGetFileInfoW：盘符页要 shell 的盘图标

// shell32：盘符图标从 shell 拿（settings.cpp / gold.cpp 也各自引了一次，
// 重复的 pragma 无副作用）。
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

namespace {

    // ========================================================================
    //  统一配色表 —— 想换风格只改这一块
    // ========================================================================
    //
    //  方案 v3 —— 中性灰 + 纯红
    //
    //  三条设计原则：
    //
    //   * **背景 / 文字全部中性灰**（R = G = B），不带任何暖色或冷色倾向。
    //   * **强调色是纯红 (255,0,0)**，和 A-90 停牌的红同源。
    //   * 底板 alpha 240：桌面只贡献 6%，明度关系可控。
    //
    //  ---- 速查 ----
    //
    //   底板     ■ #121212  近黑中性灰（alpha 240）
    //   描边     ■ #505050  中性灰
    //
    //   文字四级（全部 R=G=B）
    //            ■ #F5F5F5  标签 / 标题
    //            ■ #DCDCDC  说明文字
    //            ■ #A5A5A5  数值
    //            ■ #828282  刻度
    //
    //   强调     ■ #FF0000  纯红
    //   滑条     ■ #2D2D2D  滑槽 / ■ #FF0000 已选段 / ○ #F5F5F5 滑块
    //   假币条   ■ #FF0000  红段 / ■ #28C83C 绿段
    // ========================================================================

    // ---- 底板 ----
    const Color kPanelBg(240, 18, 18, 18);
    const Color kPanelEdge(90, 80, 80, 80);

    // ---- 文字四级（亮 → 弱，全部中性灰）----
    const Color kTextMain(255, 245, 245, 245);
    const Color kTextHint(250, 220, 220, 220);
    const Color kTextDim(235, 165, 165, 165);
    const Color kTextFaint(185, 130, 130, 130);

    // ---- 强调色：纯红 ----
    const Color kAccent(255, 255, 0, 0);
    const Color kAccentSoft(150, 255, 0, 0);

    // ---- 滑条 ----
    const Color kTrackBg(230, 45, 45, 45);
    const Color kTrackFill(255, 255, 0, 0);
    const Color kKnob(255, 245, 245, 245);
    const Color kKnobActive(255, 255, 80, 80);

    // ---- 按钮 ----
    const Color kBtnPrimary(200, 180, 0, 0);
    const Color kBtnPrimaryHot(240, 220, 0, 0);
    const Color kBtnPrimaryEdge(255, 255, 0, 0);
    const Color kBtnSecondary(100, 50, 50, 50);
    const Color kBtnSecondaryHot(150, 80, 80, 80);
    const Color kBtnSecondaryEdge(140, 130, 130, 130);
    const Color kBtnSecondaryEdgeHot(220, 200, 200, 200);

    // ---- 复选框 ----
    const Color kCheckEdge(180, 150, 150, 150);
    const Color kCheckEdgeHot(255, 255, 0, 0);

    // ---- 滚动条 ----
    const Color kScrollTrack(120, 26, 26, 26);
    const Color kScrollThumb(190, 120, 120, 120);
    const Color kScrollThumbActive(150, 255, 0, 0);

    // ---- 禁用态 ----
    const Color kTrackBgOff(150, 60, 60, 60);
    const Color kTrackFillOff(120, 120, 120, 120);
    const Color kKnobOff(255, 150, 150, 150);

    // ---- 语义色：假币条 ----
    const Color kMixRed(255, 255, 0, 0);
    const Color kMixGreen(255, 40, 200, 60);

    // ---- 应急提示窗口 ----
    const Color kHardcoreFrame(220, 255, 0, 0);
    const Color kNoticeBar(255, 255, 0, 0);
    const Color kExitBoxBg(170, 100, 100, 100);
    const Color kExitBoxEdge(200, 255, 0, 0);

    // ---- 开发用 ----
    const Color kGridThin(60, 120, 200, 240);
    const Color kGridBold(150, 120, 200, 240);
    const Color kGridLabel(190, 160, 220, 255);
    const Color kPreviewCanvas(255, 18, 18, 18);

    // ========================================================================
    //  文案表 —— 界面上所有能看到的字都集中在这一块
    // ========================================================================
    //
    //  润色文案只改这一块，下面的绘制代码一行都不用动。
    //
    //  约定：
    //    * `TXT_` 开头的是**整句**，拿去直接画；
    //    * `FMT_` 开头的是**格式串**，里面有 %d / %.1f / %s / %c 这些占位符。
    //      占位符的**个数、顺序、类型不能动**（动了那行数字就会错位甚至崩），
    //      词句部分随便改，占位符原样留着就行。
    //    * 一律 `const wchar_t* const`，别换成 std::wstring —— 这层不做动态分配。
    //
    //  ---- 改文案时的两个雷区 ----
    //    1. **整句里不要写百分号**（TXT_ 那些）。它们是被当作格式串传给
    //       swprintf_s 的，多一个 % 就会去吃一个不存在的参数。要写百分号
    //       请写成 %% 或者用 FMT_ 里现成的那几个。
    //    2. **反斜杠要写两个**。比如硬核警告里那条 `C:\\ D:\\`，写成单反斜杠
    //       就成了 C++ 的转义序列，编译过不去。
    //
    //  注：`elog::Write` 里的中文**没进这张表**：那是写进日志文件的诊断信息，
    //  玩家看不到。要改那些请直接在调用处改。
    //
    //  ---- 窗口标题 ----
    const wchar_t* const TXT_WIN_SETTINGS  = L"Ransom_dev — 启动设置";
    const wchar_t* const TXT_WIN_DRIVES    = L"Ransom_dev — 金币生成范围";
    const wchar_t* const TXT_WIN_NOTICE    = L"Ransom_dev — 用前注意";
    const wchar_t* const TXT_WIN_HARDCORE  = L"Ransom_dev — 硬核模式：Welcome";

    // ---- 设置窗口：头部 ----
    const wchar_t* const TXT_SET_TITLE     = L"启动设置";
    const wchar_t* const TXT_SET_LEDE      = L"请在这里修改具体参数";

    // ---- 设置窗口：行标签 ----
    const wchar_t* const TXT_LBL_SAFE      = L"光敏安全模式（癫痫模式）";
    const wchar_t* const TXT_DESC_SAFE     = L"压低整屏亮度跳变与闪烁";
    const wchar_t* const TXT_LBL_BGM       = L"背景音乐";
    const wchar_t* const TXT_LBL_SFX       = L"音效";
    const wchar_t* const TXT_LBL_HARD      = L"硬核模式";
    const wchar_t* const TXT_HARD_ON       = L"已开启";
    const wchar_t* const TXT_HARD_OFF      = L"更难的挑战";

    const wchar_t* const TXT_LBL_IDLE      = L"跳杀间隔（随机区间）";
    const wchar_t* const TXT_LBL_GOLD      = L"赎金目标金币（普通）";
    const wchar_t* const TXT_LBL_GOLD_HC   = L"赎金目标金币（硬核）";
    const wchar_t* const TXT_LBL_CLOSE     = L"关窗惩罚时长";
    const wchar_t* const TXT_LBL_FAKE_PCT  = L"假金币比例";
    const wchar_t* const TXT_LBL_FAKE_MIX  = L"假币形态配比（前 / 后 / 全）";

    const wchar_t* const TXT_LBL_HARDWIN   = L"硬核模式同时最多弹窗数";
    const wchar_t* const TXT_LBL_CURSOR    = L"阻挡鼠标弹窗的生成间隔（随机区间）";
    const wchar_t* const TXT_LBL_LOCKNUM   = L"桌面锁定非INK的数量上限";
    const wchar_t* const TXT_LBL_LOCKDUR   = L"桌面锁定时长（随机区间）";

    const wchar_t* const TXT_LBL_DRIVES    = L"金币生成磁盘范围（硬核）";
    const wchar_t* const TXT_BTN_DRIVES    = L"选择生成金币的磁盘…";
    const wchar_t* const TXT_BTN_DRIVES_OFF= L"需开启硬核模式";

    // ---- 设置窗口：磁盘范围那一行的当前值 ----
    const wchar_t* const TXT_DRIVES_ALL    = L"全部固定盘";
    const wchar_t* const TXT_DRIVES_NONE   = L"只落桌面";
    const wchar_t* const FMT_DRIVES_SUM    = L"%s（%d 个盘）";

    // ---- 设置窗口：滑条两端 / 中点的刻度文字 ----
    const wchar_t* const TXT_SCALE_0       = L"0";
    const wchar_t* const TXT_SCALE_10      = L"10";
    const wchar_t* const TXT_SCALE_30      = L"30";
    const wchar_t* const TXT_SCALE_500     = L"500";
    const wchar_t* const TXT_SCALE_1000    = L"1000";
    const wchar_t* const TXT_SCALE_5000    = L"5000";
    const wchar_t* const TXT_SCALE_9999    = L"9999";
    const wchar_t* const TXT_SCALE_200PCT  = L"200%";
    const wchar_t* const TXT_SCALE_100PCT  = L"100%";
    const wchar_t* const TXT_SCALE_0PCT    = L"0%";
    const wchar_t* const TXT_SCALE_20MS    = L"20ms";
    const wchar_t* const TXT_SCALE_45S     = L"45s";
    const wchar_t* const TXT_SCALE_90S     = L"90s";
    const wchar_t* const TXT_SCALE_09S     = L"0.9s";
    const wchar_t* const TXT_SCALE_18S     = L"18s";
    const wchar_t* const TXT_SCALE_30S     = L"30s";
    const wchar_t* const TXT_SCALE_22DEF   = L"22（默认）";
    const wchar_t* const TXT_SCALE_MIXEND  = L"100（右段=都改）";

    // ---- 设置窗口：数值显示格式 ----
    const wchar_t* const FMT_PCT           = L"%d%%";
    const wchar_t* const FMT_MS_FIXED      = L"%d-%d ms（固定）";
    const wchar_t* const FMT_MS_RANGE      = L"%d-%d ms（%.2f-%.2f 秒）";
    const wchar_t* const FMT_SEC_FIXED     = L"%.1f 秒（固定）";
    const wchar_t* const FMT_SEC_RANGE     = L"%.1f-%.1f 秒";
    const wchar_t* const FMT_SEC_ONE       = L"%.1f 秒";
    const wchar_t* const FMT_CLOSE_ZERO    = L"0（关窗不扣时间）";
    const wchar_t* const FMT_COUNT         = L"%d 个";
    const wchar_t* const FMT_MIX_TRIPLE    = L"%d / %d / %d";
    const wchar_t* const FMT_LOCKCAP       = L"%d（桌面可锁 %d 项）";

    // ---- 设置窗口：底部按钮与提示 ----
    const wchar_t* const TXT_BTN_RESET     = L"恢复默认";
    const wchar_t* const TXT_BTN_START     = L"开 始";
    const wchar_t* const FMT_KEY_SETTINGS  =
        L"可以按 Ctrl + Alt + Shift + %c 抢救。";

    // ---- 盘符页（设置窗口的第二个界面）----
    const wchar_t* const TXT_DRV_TITLE     = L"金币生成磁盘范围";
    const wchar_t* const TXT_DRV_SUB1      =
        L"勾上的固定盘，硬核模式会生成金币。";
    const wchar_t* const TXT_DRV_SUB2      =
        L"U 盘 / 光驱 / 网络盘不参与；不选 = 只落桌面。";
    const wchar_t* const TXT_DRV_EMPTY     = L"本设备上没有固定盘，金币只能在桌面上。";
    const wchar_t* const TXT_DRV_PICKED    = L"已选";
    const wchar_t* const TXT_DRV_SKIPPED   = L"不选";
    const wchar_t* const FMT_DRV_LETTER    = L"%c:";
    const wchar_t* const TXT_DRV_BACK      = L"← 返回设置";
    const wchar_t* const TXT_DRV_NONE      = L"反选";
    const wchar_t* const TXT_DRV_ALL       = L"全选";
    const wchar_t* const FMT_KEY_DRIVES    =
        L"设置硬核金币拓展位置；可以按 Ctrl + Alt + Shift + %c 抢救。";

    // ---- 二级警告屏：硬核（mode 1）----
    const wchar_t* const TXT_HC_TITLE      = L"硬核模式：用前注意";
    const wchar_t* const TXT_HC_LEDE       = L"这是硬核模式，基于原版的困难版：";
    const wchar_t* const FMT_HC_L1         =
        L"· 倒计时 3 分钟；赎金 %d Gold、关窗惩罚 %.1f 秒";
    const wchar_t* const FMT_HC_L2         =
        L"· 桌面文件和文件夹会被随机锁住 %.1f-%.1f 秒（同时最多 %d 个）";
    const wchar_t* const TXT_HC_L3         =
        L"· 金币会拓展放入 C:\\ D:\\ 这类固定盘内，会有假金币";
    const wchar_t* const FMT_HC_L4         =
        L"· 假币占真币的 %d%%：前缀改 %d / 后缀改 %d / 两个都改 %d";
    const wchar_t* const FMT_HC_L5         =
        L"· 弹窗最多 %d 个，每 %.1f-%.1f 秒会出来一个弹窗阻挡你鼠标";
    const wchar_t* const TXT_HC_L6         = L"· 祝你好运";

    // ---- 二级警告屏：普通（mode 0）----
    const wchar_t* const TXT_N_TITLE       = L"用前注意";
    const wchar_t* const TXT_N_LEDE        = L"这个版本会导致桌面INK问题：";
    const wchar_t* const TXT_N_LINE1       =
        L"桌面INK会被标记成「已加密」、目前开着的窗口会被收进任务栏、";
    const wchar_t* const TXT_N_LINE2       = L"满屏弹窗会挡住你正在做的事。超时没付清的话，";
    const wchar_t* const TXT_N_LINE3       = L"桌面INK会被丢进回收站（可以还原）。";
    const wchar_t* const FMT_N_SUMMARY     =
        L"本轮：赎金 %d Gold ／ 跳杀间隔 %d-%dms ／ 关窗惩罚 %.1f 秒。";

    // ---- 二级警告屏：两屏共用（逃课框 / 脚注 / 按钮）----
    const wchar_t* const FMT_ESCAPE        = L"抢救：Ctrl + Alt + Shift + %c";
    const wchar_t* const TXT_ESCAPE_DESC   =
        L"这是全局热键，用于抢救。";
    const wchar_t* const TXT_FOOT1         =
        L"如果意外导致Ransom未还原INK：Ransom_dev.exe --restore ，";
    const wchar_t* const TXT_FOOT2         = L"--clean-gold 清掉残留的金币文件。";
    const wchar_t* const TXT_BTN_BACK      = L"← 返回上一级";
    const wchar_t* const TXT_BTN_OK        = L"开 始";
    const wchar_t* const TXT_BTN_OK_HC     = L"开 始";

    // ========================================================================
    //  缓动动画
    // ========================================================================
    //
    //  每个可动的"显示值"（滑条珠子位置、滚动偏移、复选框勾选进度）都挂一份
    //  Tween：目标变了就快照当前值当起点，每次重绘按时间插值到目标。
    //
    //  两种时长：
    //    * kEaseDragMs = 80   —— **拖拽**。珠子"追"鼠标而不是瞬移：
    //                            视觉上有平滑感，但因为时长很短，仍然跟手。
    //                            早先版本拖拽是 snap（完全不缓动），用户反馈
    //                            "手动拖动所有滑条没有任何缓动曲线" —— 就是
    //                            这条路径。改成短缓动之后珠子会"追上来"，
    //                            数值文字也跟着一起平滑变化。
    //
    //    * kEaseMs = 180      —— **非拖拽**（滚轮 / 恢复默认 / 切换模式 /
    //                            滚动翻页）。一个离散事件触发一次较大幅度的
    //                            变化，用长缓动"滑过去"才有缓冲感。
    //
    //  时长存在**每份 Tween 自己身上**（durMs），所以同一个数值在"拖拽时"
    //  和"滚轮时"可以用不同时长 —— 调用时传第三个参数即可，不传就是默认
    //  180ms。
    const DWORD kEaseDragMs = 80;
    const DWORD kEaseMs = 180;

    struct Tween {
        double from = 0.0;
        double to = 0.0;
        DWORD  startMs = 0;
        DWORD  durMs = kEaseMs;   // 本份 Tween 当前的时长

        // snap = true ：直接把起点和终点都设成目标（**几乎瞬移**，
        //               只用于窗口打开时的初始化）。
        // snap = false：正常补间，durMs 毫秒内走完。
        // dur = 0     ：用默认时长（kEaseMs）。
        void Set(double v, bool snap, DWORD dur = 0)
        {
            if (snap) { from = v; to = v; startMs = 0; return; }
            if (v == to) return;                     // 已经在去这个目标的路上
            from = Value();
            to = v;
            durMs = (dur < 30) ? kEaseMs : dur;      // 太短的时长跟瞬时没区别
            startMs = GetTickCount();
        }

        double Value() const
        {
            if (startMs == 0) return to;
            const DWORD el = GetTickCount() - startMs;
            if (el >= durMs) return to;

            const float p = (float)el / (float)durMs;
            const float u = 1.0f - p;
            const float t = 1.0f - u * u * u;        // ease-out cubic
            return from + (to - from) * (double)t;
        }
    };

    // 颜色线性插值（ARGB 逐通道）。
    Color LerpColor(const Color& a, const Color& b, float t)
    {
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        const BYTE A = (BYTE)(a.GetA() + (b.GetA() - a.GetA()) * t + 0.5f);
        const BYTE R = (BYTE)(a.GetR() + (b.GetR() - a.GetR()) * t + 0.5f);
        const BYTE G = (BYTE)(a.GetG() + (b.GetG() - a.GetG()) * t + 0.5f);
        const BYTE B = (BYTE)(a.GetB() + (b.GetB() - a.GetB()) * t + 0.5f);
        return Color(A, R, G, B);
    }

    float Clamp01(float v)
    {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    // ========================================================================
    //  小工具
    // ========================================================================

    bool Hit(const RECT& r, const POINT& p)
    {
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }

    // 注意**不叫** Rect：windows.h 里已经有一个同名的 GDI 函数。
    void SetRectLocal(RECT& r, int x, int y, int w, int h)
    {
        r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    }

    FontFamily* MonoFamily()
    {
        static FontFamily* fam = nullptr;
        static bool tried = false;
        if (!tried) { tried = true; fam = aero::UiFontFamily(); }
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
    void* g_hotkeyUser = nullptr;

    namespace setup { void Commit(setup_ui::Verdict v); }

    void HotkeyWhileSetup(void*)
    {
        elog::Write(L"[setup] 设置窗口里按下了安全阀热键，直接退出");
        setup::Commit(setup_ui::VERDICT_ABORT);
    }

    // ========================================================================
    //  模态消息泵 —— 设置窗口 / 二级警告窗口共用
    // ========================================================================
    //
    //  这两屏以前各自写的是：
    //      while (aero::IsAlive(h) && GetMessageW(&msg, h, 0, 0) > 0) { ... }
    //  两个坑（2026-09-26 用户报「对设置窗口用 QQ 截屏之后，整个 Ransom 拖动
    //  没反应、点不动、连关闭都点不了」时暴露出来的）：
    //
    //   1. **带窗口过滤**的 GetMessage 只取这一扇窗的消息，投给别的窗口的消息
    //      永远躺在队列里。最要命的是 IPC 窗口的 WM_HOTKEY —— 安全阀
    //      Ctrl+Alt+Shift+Q 正是注册在它身上（见 entity_main 的 IpcProc，
    //      那里专门写了「转给正在开着的设置界面」），于是设置屏上按热键
    //      **毫无反应**：最后一条逃路被自己堵死了。
    //
    //   2. 只要这扇窗不再收到消息（第三方截屏工具的全屏覆盖层压在上面、
    //      窗口已经不在……），GetMessage 就会**一直阻塞**，整个进程表现成
    //      「拖不动、点不动、关不掉」。
    //
    //  现在改成「等消息、最多 100ms 超时」：谁的消息都取、取完就派发，
    //  超时就回来看一眼窗口状态。结构上不可能再卡死，安全阀热键也恢复有效。
    //
    //  sawQuit：收到 WM_QUIT 时置位（并把它**放回队列**，交给主循环去处理），
    //  调用方据此结束这一屏。
    const DWORD kPumpTimeoutMs = 100;

    // 诊断：鼠标明明在我们窗口上，这点却被**别的进程**的窗口压着 ——
    // 那正是「点击被吃掉」的样子（截屏工具的全屏覆盖层最典型）。
    // 每秒最多写一行，只在真的发生时写，方便下次照着日志定位。
    void LogIfInputStarved(HWND h)
    {
        static DWORD s_last = 0;
        const DWORD now = GetTickCount();
        if (now - s_last < 1000) return;
        s_last = now;

        if (!IsWindowVisible(h)) return;

        POINT m = {};
        if (!GetCursorPos(&m)) return;

        RECT wr = {};
        if (!GetWindowRect(h, &wr)) return;
        if (!PtInRect(&wr, m)) return;          // 光标不在我们身上，不关心

        HWND under = WindowFromPoint(m);
        if (!under || under == h) return;
        if (GetAncestor(under, GA_ROOT) == h) return;

        DWORD pid = 0;
        GetWindowThreadProcessId(under, &pid);
        if (pid == GetCurrentProcessId()) return;   // 自家的窗口，不算

        wchar_t cls[128] = {};
        GetClassNameW(under, cls, 128);

        elog::Write(L"[setup] !! 光标在窗口上却被别的进程的窗口压着（class=%s pid=%lu）"
            L" —— 点击会被它吃掉；按 Ctrl+Alt+Shift+Q 可以直接退出",
            cls, (unsigned long)pid);
    }

    // 泵一轮消息：有消息就全部派发掉，没有就等最多 100ms。
    // sawQuit 置位 = 收到 WM_QUIT（已放回队列），调用方该收场了。
    void PumpModal(HWND h, bool& sawQuit)
    {
        sawQuit = false;

        const DWORD w = MsgWaitForMultipleObjectsEx(
            0, nullptr, kPumpTimeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (w == WAIT_TIMEOUT || w == WAIT_FAILED)
        {
            // 这一轮没有消息：顺手看一眼是不是被别的窗口压住了（诊断用）
            LogIfInputStarved(h);
            return;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                // 把退出请求放回队列：这一层只负责结束本屏，
                // 整个进程的退出去主循环那边走。
                PostQuitMessage((int)msg.wParam);
                sawQuit = true;
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void DrawGrid(Graphics& g, const RectF& rc)
    {
        Pen thin(kGridThin, 1.0f);
        Pen bold(kGridBold, 1.0f);
        Font f = MakeMonoFont(9.0f);
        SolidBrush label(kGridLabel);

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
        const wchar_t* label, bool hot, bool primary, bool enabled = true)
    {
        RectF b(rc.X + (REAL)local.left, rc.Y + (REAL)local.top,
            (REAL)(local.right - local.left), (REAL)(local.bottom - local.top));

        // 灰掉的按钮：不参与高亮，底色和文字一起压暗。
        // 用途：「金币生成磁盘范围」那条只有在硬核开着时才点得进去。
        Color fill, edge, text;
        if (!enabled)
        {
            fill = kTrackBgOff;
            edge = kBtnSecondaryEdge;
            text = kTextFaint;
        }
        else
        {
            fill = primary
                ? (hot ? kBtnPrimaryHot : kBtnPrimary)
                : (hot ? kBtnSecondaryHot : kBtnSecondary);
            edge = primary
                ? kBtnPrimaryEdge
                : (hot ? kBtnSecondaryEdgeHot : kBtnSecondaryEdge);
            text = kTextMain;
        }

        FillRound(g, b, 7.0f, fill);
        StrokeRound(g, b, 7.0f, edge, 1.4f);

        DrawTextCjk(g, label, b, 15.0f, text,
            StringAlignmentCenter, FontStyleBold);
    }

    // ==========================================================================
    //  设置窗口
    // ==========================================================================
    namespace setup {

        // ---- 纯数据布局（相对内容区左上角）----
        //
        // **正方形**：内容区宽 = 高 = 600。以前是 520x600 的竖长方形，
        // 加了四条硬核滑条之后左边那一列太窄、刻度文字挤成一团，索性把
        // 横宽同步到竖长 —— 宽出来的 80px 全给了滑条（轨道从 426 涨到 506）。
        const int kW = 600;
        const int kH = 600;
        const int kM = 22;
        const int kLabelH = 18;
        const int kTrackH = 18;
        const int kKnobR = 8;

        const int kHeaderH = 62;
        const int kFooterH = 96;
        const int kViewTop = kHeaderH;
        const int kViewH = kH - kHeaderH - kFooterH;
        const int kScrollW = 14;
        const int kContentW = kW - kScrollW;

        const int kRowH = 56;
        const int kCheckH = 34;

        const int kRowSafe = 0;
        const int kRowBgm = 40;
        const int kRowSfx = 96;
        const int kRowHard = 152;
        const int kRowIdle = 194;
        const int kRowGold = 250;
        const int kRowClose = 306;
        const int kRowFakePct = 362;
        const int kRowFakeMix = 418;

        // ---- 硬核专属的四条（接在假币那两条后面）----
        //
        // 为什么接在末尾而不是插在「硬核模式」开关下面：v0.4 的排版刚定稿，
        // 插在中间会把下面所有行的位置整体顶下去，用户已经记住的行位置全乱；
        // 接在末尾则一行都不动，只是往下多出四条（滚动条本来就一直在）。
        // 这四条和假金币那两条一样，只在硬核开着时能拖。
        const int kRowHardWin = 474;    // 硬核同时最多几个弹窗（10-30）
        const int kRowCursor = 530;     // 阻挡鼠标弹窗的生成间隔（0.9~18 秒）
        const int kRowLockNum = 586;    // 桌面锁定数量上限（0~min(90, 桌面实有））
        const int kRowLockDur = 642;    // 桌面锁定时长（0.9~18 秒）
        const int kRowDrives = 698;     // 金币生成磁盘范围（点进去是 2D 盘符页）

        const int kContentH = 754;

        // ---- 盘符页（设置窗口的第二个界面）----
        //
        // 一屏 6 列 x 5 行 = 30 格，A:..Z: 全装得下；实机一般只有 2~4 个。
        // 只列**固定盘** —— 正好就是"能生成金币"的那些（U 盘 / 光驱 / 网络盘
        // 从来不撒金币，列出来只会让人以为勾了就有用）。
        const int kDrvTitleTop = 20;
        const int kDrvSubTop = 48;
        const int kDrvGridTop = 82;
        const int kDrvCols = 6;
        const int kDrvRows = 5;
        const int kDrvMax = kDrvCols * kDrvRows;
        const int kDrvGap = 10;
        const int kDrvTileH = 78;
        const int kDrvTileW =
            (kContentW - kM * 2 - kDrvGap * (kDrvCols - 1)) / kDrvCols;

        // 盘符页的绘制在文件后半段（要等所有绘制小工具都定义完），
        // 但设置页的 PaintContent 要先分支过去，所以这里先声明一下。
        void PaintDrives(Gdiplus::Graphics& g, const Gdiplus::RectF& rc);

        const int kTrackLeft = kM + 18;
        const int kTrackRight = kContentW - kM - 18;
        const int kTrackW = kTrackRight - kTrackLeft;

        const int kBtnH = 42;
        const int kBtnGap = 14;
        const int kBtnW = 150;
        const int kBtnTop = kH - 74;
        const int kHintTop = kH - 26;

        const int kLoMs = settings::kIntervalMinMs;
        const int kHiMs = settings::kIntervalMaxMs;

        const int kVolStep = 5;

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

        enum Hot { HOT_NONE = 0, HOT_RESET, HOT_START, HOT_DRIVES, HOT_DRV_ALL, HOT_DRV_NONE, HOT_DRV_BACK };
        int   g_hot = HOT_NONE;

        // 当前是哪个界面：0 = 设置列表，1 = 2D 盘符页。
        // 两个界面共用同一个窗口、同一套 aero 绘制/输入管道，切换只是换一份画法
        // 和一份命中判定 —— 不另开窗口（分层窗口本来也不适合开一堆子窗）。
        int   g_page = 0;

        // 待切换到的页面（-1 = 不切）。点「选择磁盘…」/「返回设置」时先把它记下来，
        // 让**当前这扇窗淡出完之后**才真的换页 —— 直接改 g_page 的话，
        // 淡出动画那几百毫秒里画的就是新页面了。
        int   g_pendingPage = -1;

        // 盘符页的鼠标悬停格子（-1 = 没有）
        int   g_drvHot = -1;

        // 盘符页面上的一个格子。icon 由 shell 给（和"此电脑"里看到的一致），
        // 拿不到就是空指针、那格只画盘符字母。
        struct DriveItem {
            wchar_t           letter = 0;        // 'C'
            std::wstring      root;              // "C:\\"
            Gdiplus::Bitmap*  icon = nullptr;    // 可能为空
            Tween             anim;              // 勾选动画进度（0=没勾 1=勾了）
        };
        std::vector<DriveItem> g_drives;

        enum Drag {
            DRAG_NONE = 0,
            DRAG_BGM, DRAG_SFX, DRAG_IDLE_LO, DRAG_IDLE_HI,
            DRAG_GOLD, DRAG_CLOSE,
            DRAG_FAKE_PCT, DRAG_FAKE_MIX,
            DRAG_HARDWIN, DRAG_CURSOR_LO, DRAG_CURSOR_HI,
            DRAG_LOCKNUM, DRAG_LOCKDUR_LO, DRAG_LOCKDUR_HI,
            DRAG_SCROLL,
        };
        int   g_drag = DRAG_NONE;
        int   g_dragMixIdx = -1;
        int   g_scrollGrab = 0;
        int   g_scroll = 0;

        // 编辑中的一份副本。g_edit 是**目标值**（逻辑上的当前值）；
        // 下面是显示值（Tween）。两者在缓动期间会短暂分离。
        settings::Set g_edit;

        // ---- 显示值 Tween ----
        Tween g_tBgm, g_tSfx;
        Tween g_tIdleMin, g_tIdleMax;
        Tween g_tGold, g_tClose;
        Tween g_tFakePct, g_tFakePre, g_tFakeSuf;
        Tween g_tSafe, g_tHard;
        // 硬核专属四条：弹窗上限 / 阻挡弹窗间隔（两珠）/ 锁定数量 / 锁定时长（两珠）
        Tween g_tHardWin;
        Tween g_tCursorMin, g_tCursorMax;
        Tween g_tLockNum;
        Tween g_tLockMin, g_tLockMax;
        Tween g_tScroll;

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

        // 本帧的滚动显示位置（从 Tween 取，会和 g_scroll 短暂分离）。
        int ScrollValue() { return (int)lround(g_tScroll.Value()); }

        int ViewOffset() { return kViewTop - ScrollValue(); }

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
        RECT HardWinTrack() { return RowTrack(kRowHardWin); }
        RECT CursorTrack() { return RowTrack(kRowCursor); }
        RECT LockNumTrack() { return RowTrack(kRowLockNum); }
        RECT LockDurTrack() { return RowTrack(kRowLockDur); }

        RECT CheckBoxAt(int rowY)
        {
            RECT r;
            SetRectLocal(r, kM, ViewOffset() + rowY + (kCheckH - 20) / 2, 20, 20);
            return r;
        }

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

        bool TrackHit(const RECT& t, POINT p)
        {
            return p.y >= t.top - 8 && p.y < t.bottom + 8 &&
                p.x >= t.left - 10 && p.x < t.right + 10;
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

        // ---- 设置列表里那条「金币生成磁盘范围」----
        //
        // 整行可点（和复选框那几行一个待遇），行内右半边画成一个按钮的样子。
        RECT DrivesBtn()
        {
            RECT r;
            SetRectLocal(r, kTrackLeft, ViewOffset() + kRowDrives + 20, kTrackW, kCheckH);
            return r;
        }

        RECT DrivesHit()
        {
            RECT r;
            SetRectLocal(r, kM - 4, ViewOffset() + kRowDrives, kContentW - (kM - 4) * 2, 56);
            return r;
        }

        // ---- 盘符页的格子与按钮 ----
        //
        // 盘符页**不滚动**（30 格一屏放得下），所以这里的坐标是裸的内容区坐标，
        // 不带 ViewOffset —— 和设置页那些行不一样，别在这儿加偏移。
        RECT DriveTileRect(int idx)
        {
            const int col = idx % kDrvCols;
            const int row = idx / kDrvCols;

            RECT r;
            SetRectLocal(r,
                kM + col * (kDrvTileW + kDrvGap),
                kDrvGridTop + row * (kDrvTileH + kDrvGap),
                kDrvTileW, kDrvTileH);
            return r;
        }

        RECT DrvAllBtn()
        {
            // 右对齐：和设置页的「恢复默认 / 开始」同一套排法。
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        RECT DrvNoneBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW * 2 - kBtnGap, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        RECT DrvBackBtn()
        {
            RECT r;
            SetRectLocal(r, kM, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        // 「金币生成磁盘范围」那一行的值（右对齐那串字）。
        void DrivesSummary(wchar_t* buf, size_t n)
        {
            const std::vector<std::wstring>& dv = g_edit.coinDrives;

            if (dv.size() == 1 && dv[0] == settings::kCoinDrivesNoneToken)
            {
                swprintf_s(buf, n, TXT_DRIVES_NONE);
                return;
            }
            if (dv.empty())
            {
                swprintf_s(buf, n, TXT_DRIVES_ALL);
                return;
            }

            std::wstring s;
            for (size_t i = 0; i < dv.size(); ++i)
            {
                if (i) s += L" / ";
                s += dv[i];
                s += L":";
            }
            swprintf_s(buf, n, FMT_DRIVES_SUM, s.c_str(), (int)dv.size());
        }

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
                ? (int)((double)span * (double)ScrollValue() / (double)m) : 0);

            RECT r;
            SetRectLocal(r, t.left, y, t.right - t.left, h);
            return r;
        }

        // ---- 数值 <-> 像素 ----
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

        int GoldLo() { return g_edit.hardcore ? settings::kGoldHardMin : settings::kGoldMin; }
        int GoldHi() { return g_edit.hardcore ? settings::kGoldHardMax : settings::kGoldMax; }

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

        int CloseHi()
        {
            return g_edit.hardcore ? settings::kCloseHardMax : settings::kCloseNormalMax;
        }

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

        // 硬核开关。下面四条硬核专属滑条全部以它为"可不可拖"的门。
        // FakeEnabled 是它的旧名（假币那两条先用的），保留下来少改一处。
        bool HardcoreOn() { return g_edit.hardcore; }

        bool FakeEnabled() { return g_edit.hardcore; }

        // ---- 硬核：同时最多几个弹窗（10-30，单珠）----
        int HardWinFromX(int x)
        {
            return ValueFromX(x, HardWinTrack(), settings::kHardPopupMin, settings::kHardPopupMax);
        }

        int HardWinToX(int v)
        {
            return XFromValue(v, HardWinTrack(), settings::kHardPopupMin, settings::kHardPopupMax);
        }

        // ---- 时间类的两条（阻挡弹窗间隔 / 锁定时长）：两珠、毫秒、0.1 秒粒度 ----
        //
        // 吸附到 100ms：显示是 "%.1f 秒"，如果内部值是 1234ms，用户看到 1.2 秒
        // 而实际生效的是 1.234 秒 —— 读数对不上。吸到 100ms 之后所见即所得。
        int SnapMs(int v, int lo, int hi)
        {
            v = (v / 100) * 100;
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            return v;
        }

        int CursorMsFromX(int x)
        {
            return SnapMs(ValueFromX(x, CursorTrack(),
                settings::kCursorGapFloorMs, settings::kCursorGapCeilMs),
                settings::kCursorGapFloorMs, settings::kCursorGapCeilMs);
        }

        int CursorMsToX(int ms)
        {
            return XFromValue(ms, CursorTrack(),
                settings::kCursorGapFloorMs, settings::kCursorGapCeilMs);
        }

        int LockDurMsFromX(int x)
        {
            return SnapMs(ValueFromX(x, LockDurTrack(),
                settings::kExtraLockFloorMs, settings::kExtraLockCeilMs),
                settings::kExtraLockFloorMs, settings::kExtraLockCeilMs);
        }

        int LockDurMsToX(int ms)
        {
            return XFromValue(ms, LockDurTrack(),
                settings::kExtraLockFloorMs, settings::kExtraLockCeilMs);
        }

        // ---- 硬核：桌面锁定数量上限（单珠）----
        //
        // 右端**不是常数 90**：桌面上一共只有 N 个非快捷方式条目时，拖到 90
        // 也没意义。所以右端 = settings::ExtraLockCapacity()
        // = min(90, 桌面上真实存在的非快捷方式条目数)。
        int LockCap() { return settings::ExtraLockCapacity(); }

        int LockNumFromX(int x)
        {
            return ValueFromX(x, LockNumTrack(), 0, LockCap());
        }

        int LockNumToX(int n)
        {
            return XFromValue(n, LockNumTrack(), 0, LockCap());
        }

        // "0.9-9.0 秒" / 两端重合时 "3.0 秒（固定）"
        void FormatSpanSecs(wchar_t* buf, size_t n, int loMs, int hiMs)
        {
            if (loMs == hiMs)
                swprintf_s(buf, n, FMT_SEC_FIXED, loMs / 1000.0);
            else
                swprintf_s(buf, n, FMT_SEC_RANGE, loMs / 1000.0, hiMs / 1000.0);
        }

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
        const int kMixMin = 0;
        const int kMixMax = 100;

        // 两颗珠子重合的判定阈值（像素）。**只用于拖拽按下时**判断
        // "点的是哪颗"（见 MixNearestKnob）。
        //
        // 滚轮那一路**不用**这个阈值 —— 它按光标在中点的哪一侧决定调
        // 哪颗珠子（照搬「遭遇战间隔」的判定），重合和分开是同一套逻辑。
        const int kOverlapEps = 6;

        // 两个权重 -> 两个分界点（b1 = 前缀，b2 = 前缀+后缀）。
        // **目标值**版本，输入处理用（读 g_edit）。
        void FakeMixBounds(int& b1, int& b2)
        {
            b1 = ClampI(g_edit.fakePrefixPct, kMixMin, kMixMax);
            b2 = b1 + ClampI(g_edit.fakeSuffixPct, kMixMin, kMixMax);
            if (b2 > kMixMax) b2 = kMixMax;
        }

        // **显示值**版本，绘制用（读 Tween）。
        void DisplayFakeBounds(int& b1, int& b2)
        {
            b1 = ClampI((int)lround(g_tFakePre.Value()), kMixMin, kMixMax);
            b2 = b1 + ClampI((int)lround(g_tFakeSuf.Value()), kMixMin, kMixMax);
            if (b2 > kMixMax) b2 = kMixMax;
            if (b1 > b2) b1 = b2;
            if (b1 < kMixMin) b1 = kMixMin;
        }

        int MixToX(int v)
        {
            return XFromValue(v, FakeMixTrack(), kMixMin, kMixMax);
        }

        int MixFromX(int x)
        {
            return ValueFromX(x, FakeMixTrack(), kMixMin, kMixMax);
        }

        // 第 idx 颗珠子（0=前缀边界 1=后缀边界）的 x。
        // 用**显示值**：拖拽时珠子正在追鼠标，显示值就是"用户看到的"。
        int MixKnobX(int idx)
        {
            int b1, b2;
            DisplayFakeBounds(b1, b2);
            return MixToX(idx == 0 ? b1 : b2);
        }

        // 离光标最近的是哪一颗珠子（**拖拽**按下时用）。
        //
        // **两颗珠子重合（或几乎重合）时一律返回 0（左珠）**，不管鼠标点哪儿。
        // 理由：完全重合时按距离选，鼠标偏右几个像素就会选中右珠，而右珠被
        // 夹在左珠上、往左拖不动 —— 用户看到"有时能拖、有时拖不动"。
        //
        // 滚轮那一路**不**走这个函数：滚轮有明确方向（滚上/滚下），
        // 按方向选珠才是对的（见 OnWheel 里那段）。
        int MixNearestKnob(int x)
        {
            const int xa = MixKnobX(0);
            const int xb = MixKnobX(1);

            if (xb - xa <= kOverlapEps) return 0;

            int best = 0, bestD = -1;
            for (int i = 0; i < 2; ++i)
            {
                const int d = MixKnobX(i) - x;
                const int ad = (d < 0) ? -d : d;
                if (bestD < 0 || ad < bestD) { bestD = ad; best = i; }
            }
            return best;
        }

        // 通用的"两颗珠子选哪颗"（按下时用）：重合（或几乎重合）时一律选左珠，
        // 其余按距离。跳过间隔那条和假币那条各有各的写法（它们各自要处理
        // "显示值 vs 目标值"），**新增**的两条两珠滑条（阻挡弹窗间隔、
        // 桌面锁定时长）走这一个 —— 它们没有第三颗珠、也不需要吸附边界。
        int NearestKnob(int x1, int x2, int x)
        {
            if (x2 - x1 <= kOverlapEps) return 0;

            const int d1 = (x > x1) ? (x - x1) : (x1 - x);
            const int d2 = (x > x2) ? (x - x2) : (x2 - x);
            return (d1 <= d2) ? 0 : 1;
        }

        // 拖动第 idx 个分界点。两颗珠子互相不能越过（b1 <= b2）。
        //
        // dur = 0 -> 用默认时长（180ms）。
        // 拖拽路径会传 kEaseDragMs（80ms）—— 珠子"追"鼠标而不是贴死。
        void SetFakeMixBound(int idx, int v, bool snap, DWORD dur = 0)
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

            g_edit.fakePrefixPct = b1;
            g_edit.fakeSuffixPct = b2 - b1;
            g_edit.fakeBothPct = kMixMax - b2;

            g_tFakePre.Set(g_edit.fakePrefixPct, snap, dur);
            g_tFakeSuf.Set(g_edit.fakeSuffixPct, snap, dur);
        }

        // ---- 一次性把所有 Tween 对齐到 g_edit ----
        void SyncAllTweens(bool snap)
        {
            g_tBgm.Set(g_edit.bgmVol, snap);
            g_tSfx.Set(g_edit.sfxVol, snap);
            g_tIdleMin.Set(g_edit.minMs, snap);
            g_tIdleMax.Set(g_edit.maxMs, snap);
            g_tGold.Set(g_edit.goldGoal, snap);
            g_tClose.Set(g_edit.childCloseMs, snap);
            g_tFakePct.Set(g_edit.fakePercent, snap);
            g_tFakePre.Set(g_edit.fakePrefixPct, snap);
            g_tFakeSuf.Set(g_edit.fakeSuffixPct, snap);
            g_tSafe.Set(g_edit.photosensitiveSafe ? 1.0 : 0.0, snap);
            g_tHard.Set(g_edit.hardcore ? 1.0 : 0.0, snap);
            // 硬核专属四条
            g_tHardWin.Set(g_edit.hardPopupMax, snap);
            g_tCursorMin.Set(g_edit.cursorGapMinMs, snap);
            g_tCursorMax.Set(g_edit.cursorGapMaxMs, snap);
            g_tLockNum.Set(g_edit.extraLockCount, snap);
            g_tLockMin.Set(g_edit.extraLockMinMs, snap);
            g_tLockMax.Set(g_edit.extraLockMaxMs, snap);
        }

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

        void PushLive()
        {
            settings::SetCurrent(g_edit);
            settings::Apply();
        }

        // ---- 画一条滑条 ----
        void DrawTrack(Graphics& g, const RectF& rc, const RECT& local,
            int knobCount, int x1, int x2, int fillL, int fillR,
            int hotKnob, bool enabled = true)
        {
            const REAL cy = rc.Y + (REAL)local.top + (REAL)kTrackH * 0.5f;
            const REAL x0 = rc.X + (REAL)local.left;
            const REAL xN = rc.X + (REAL)local.right;

            const Color bgC = enabled ? kTrackBg : kTrackBgOff;
            const Color fillC = enabled ? kTrackFill : kTrackFillOff;
            const Color knobC = enabled ? kKnob : kKnobOff;

            {
                RectF t(x0, cy - 3.0f, xN - x0, 6.0f);
                FillRound(g, t, 3.0f, bgC);
            }
            if (fillR > fillL)
            {
                RectF f(rc.X + (REAL)fillL, cy - 3.0f, (REAL)(fillR - fillL), 6.0f);
                FillRound(g, f, 3.0f, fillC);
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
                const Color c = (enabled && i == hotKnob) ? kKnobActive : knobC;

                SolidBrush b(c);
                g.FillEllipse(&b, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);

                Pen edge(Color(200, 30, 20, 24), 1.6f);
                g.DrawEllipse(&edge, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);
            }
        }

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

        void DrawFakeMixRow(Graphics& g, const RectF& rc, bool enabled,
            int b1, int b2)
        {
            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)(kContentW - kM * 2);
            const REAL y = rc.Y + (REAL)(ViewOffset() + kRowFakeMix);

            const int bothPct = kMixMax - b2;

            RectF t(xL, y, w, (REAL)kLabelH);
            DrawTextCjk(g, TXT_LBL_FAKE_MIX, t, 14.0f,
                enabled ? kTextMain : kTextFaint);

            wchar_t val[64];
            swprintf_s(val, FMT_MIX_TRIPLE, b1, b2 - b1, bothPct);
            RectF v(xL, y, w, (REAL)kLabelH);
            DrawTextMono(g, val, v, 14.0f, enabled ? kTextDim : kTextFaint, StringAlignmentFar);

            const RECT tr = FakeMixTrack();
            const REAL cy = rc.Y + (REAL)tr.top + (REAL)kTrackH * 0.5f;
            const REAL x0 = rc.X + (REAL)tr.left;
            const REAL xN = rc.X + (REAL)tr.right;

            {
                RectF track(x0, cy - 3.0f, xN - x0, 6.0f);
                FillRound(g, track, 3.0f, enabled ? kTrackBg : kTrackBgOff);
            }

            if (enabled)
            {
                const REAL xb1 = rc.X + (REAL)MixToX(b1);
                const REAL xb2 = rc.X + (REAL)MixToX(b2);

                const REAL seg[4] = { x0, xb1, xb2, xN };
                const Color segC[3] = { kMixRed, kMixGreen, kMixRed };

                for (int i = 0; i < 3; ++i)
                {
                    if (seg[i + 1] - seg[i] < 0.5f) continue;
                    RectF f(seg[i], cy - 3.0f, seg[i + 1] - seg[i], 6.0f);
                    FillRound(g, f, 3.0f, segC[i]);
                }
            }

            {
                SolidBrush dim(kTextFaint);
                g.FillRectangle(&dim, x0, cy - 6.0f, 1.0f, 12.0f);
                g.FillRectangle(&dim, xN - 1.0f, cy - 6.0f, 1.0f, 12.0f);
            }

            for (int i = 0; i < 2; ++i)
            {
                const REAL kx = rc.X + (REAL)MixToX(i == 0 ? b1 : b2);
                const bool hot = enabled && g_drag == DRAG_FAKE_MIX && g_dragMixIdx == i;
                const Color c = enabled ? (hot ? kKnobActive : kKnob) : kKnobOff;

                SolidBrush b(c);
                g.FillEllipse(&b, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);

                Pen edge(Color(200, 30, 20, 24), 1.6f);
                g.DrawEllipse(&edge, kx - kKnobR, cy - kKnobR, kKnobR * 2.0f, kKnobR * 2.0f);
            }

            RectF lo(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
            DrawTextMono(g, L"0", lo, 11.0f, kTextFaint);
            RectF hi(xL, rc.Y + (REAL)(tr.bottom + 2), w, 14.0f);
            DrawTextMono(g, TXT_SCALE_MIXEND, hi, 11.0f, kTextFaint, StringAlignmentFar);
        }

        // ---- 一个复选框（带勾选进度动画）----
        void DrawCheckbox(Graphics& g, const RectF& rc, const RECT& b,
            float progress, bool hot)
        {
            RectF box(rc.X + (REAL)b.left, rc.Y + (REAL)b.top, 20.0f, 20.0f);

            const Color fill = LerpColor(kTrackBg, kAccentSoft, progress);
            FillRound(g, box, 4.0f, fill);

            const Color edge = hot
                ? kCheckEdgeHot
                : LerpColor(kCheckEdge, kCheckEdgeHot, progress);
            StrokeRound(g, box, 4.0f, edge, 1.6f);

            if (progress <= 0.02f) return;

            const float s1 = Clamp01(progress / 0.55f);
            const float s2 = Clamp01((progress - 0.55f) / 0.45f);

            const REAL x0 = box.X + 5.0f, y0 = box.Y + 10.5f;
            const REAL x1 = box.X + 8.5f, y1 = box.Y + 14.0f;
            const REAL x2 = box.X + 15.0f, y2 = box.Y + 6.0f;

            Pen pen(Color(255, 245, 245, 250), 2.6f);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);

            if (s1 > 0.0f)
            {
                const REAL mx = x0 + (x1 - x0) * s1;
                const REAL my = y0 + (y1 - y0) * s1;
                g.DrawLine(&pen, x0, y0, mx, my);
            }
            if (s2 > 0.0f)
            {
                const REAL mx = x1 + (x2 - x1) * s2;
                const REAL my = y1 + (y2 - y1) * s2;
                g.DrawLine(&pen, x1, y1, mx, my);
            }
        }

        // ---- 主绘制 ----
        void PaintContent(Graphics& g, const RectF& rc)
        {
            // 盘符页是同一个窗口的第二个界面，画法整套换掉。
            if (g_page == 1) { PaintDrives(g, rc); return; }

            DrawPanel(g, rc);

            // ---- 本帧显示值（全部从 Tween 取）----
            //
            // 下面所有滑条、复选框、假币配比的绘制都读这些**局部变量**。
            // 拖拽走短缓动（80ms），滚轮 / 恢复默认 / 切模式走长缓动（180ms），
            // 所以用户在任何一条路径上都能看到"珠子滑过去"的过程。
            //
            // 逻辑分支（FakeEnabled()、g_drag 比较）仍然读 g_edit —— 那是
            // **目标状态**，不该有中间态。
            const int dispBgm = (int)lround(g_tBgm.Value());
            const int dispSfx = (int)lround(g_tSfx.Value());
            const int dispMinMs = (int)lround(g_tIdleMin.Value());
            const int dispMaxMs = (int)lround(g_tIdleMax.Value());
            const int dispGold = (int)lround(g_tGold.Value());
            const int dispClose = (int)lround(g_tClose.Value());
            const int dispFakePct = (int)lround(g_tFakePct.Value());

            const float safeP = (float)g_tSafe.Value();
            const float hardP = (float)g_tHard.Value();

            // 硬核专属四条（同样读 Tween）
            const int dispHardWin = (int)lround(g_tHardWin.Value());
            const int dispCursorMin = (int)lround(g_tCursorMin.Value());
            const int dispCursorMax = (int)lround(g_tCursorMax.Value());
            const int dispLockNum = (int)lround(g_tLockNum.Value());
            const int dispLockMin = (int)lround(g_tLockMin.Value());
            const int dispLockMax = (int)lround(g_tLockMax.Value());

            int dispB1, dispB2;
            DisplayFakeBounds(dispB1, dispB2);

            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)(kContentW - kM * 2);

            // ================= 头部 =================
            {
                SolidBrush bar(kAccent);
                g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 18.0f);
                RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 30.0f);
                DrawTextCjk(g, TXT_SET_TITLE, t, 20.0f, kTextMain, StringAlignmentNear, FontStyleBold);
            }
            {
                RectF t(xL, rc.Y + 42.0f, w, 16.0f);
                DrawTextCjk(g, TXT_SET_LEDE,
                    t, 12.0f, kTextHint);
            }

            wchar_t buf[96];

            // ================= 视口 =================
            g.SetClip(RectF(rc.X, rc.Y + (REAL)kViewTop, (REAL)kContentW, (REAL)kViewH));

            // ---- 光敏安全 ----
            {
                DrawCheckbox(g, rc, SafeBox(), safeP, false);

                const RECT b = SafeBox();
                RectF t(rc.X + (REAL)(b.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowSafe), 300.0f, (REAL)kCheckH);
                DrawTextCjk(g, TXT_LBL_SAFE, t, 14.0f, kTextMain);

                RectF d(rc.X + (REAL)(b.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowSafe + 18), 420.0f, 16.0f);
                DrawTextCjk(g, TXT_DESC_SAFE, d, 11.0f, kTextDim);
            }

            // ---- 背景音乐 ----
            {
                const RECT tr = BgmTrack();
                const int kx = VolToX(dispBgm, tr);
                swprintf_s(buf, FMT_PCT, dispBgm);
                DrawSliderRow(g, rc, kRowBgm, TXT_LBL_BGM, buf,
                    dispBgm > 100 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_BGM ? 0 : -1, true,
                    TXT_SCALE_0, TXT_SCALE_200PCT,
                    TXT_SCALE_100PCT, VolToX(100, tr));
            }

            // ---- 音效 ----
            {
                const RECT tr = SfxTrack();
                const int kx = VolToX(dispSfx, tr);
                swprintf_s(buf, FMT_PCT, dispSfx);
                DrawSliderRow(g, rc, kRowSfx, TXT_LBL_SFX, buf,
                    dispSfx > 100 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_SFX ? 0 : -1, true,
                    TXT_SCALE_0, TXT_SCALE_200PCT,
                    TXT_SCALE_100PCT, VolToX(100, tr));
            }

            // ---- 硬核模式 ----
            {
                DrawCheckbox(g, rc, HardBox(), hardP, false);

                const RECT hb = HardBox();
                RectF t(rc.X + (REAL)(hb.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowHard), 300.0f, (REAL)kCheckH);
                DrawTextCjk(g, TXT_LBL_HARD, t, 14.0f,
                    g_edit.hardcore ? kAccent : kTextMain, StringAlignmentNear, FontStyleBold);

                RectF d(rc.X + (REAL)(hb.right + 10),
                    rc.Y + (REAL)(ViewOffset() + kRowHard + 20), 440.0f, 16.0f);
                DrawTextCjk(g,
                    g_edit.hardcore ? TXT_HARD_ON : TXT_HARD_OFF,
                    d, 11.0f, kTextDim);
            }

            // ---- 遭遇战间隔 ----
            {
                const double f1 = (double)dispMinMs / 1000.0;
                const double f2 = (double)dispMaxMs / 1000.0;
                if (dispMinMs == dispMaxMs)
                    swprintf_s(buf, FMT_MS_FIXED, dispMinMs, dispMaxMs);
                else
                    swprintf_s(buf, FMT_MS_RANGE,
                        dispMinMs, dispMaxMs, f1, f2);

                const RECT tr = IntTrack();
                const int x1 = MsToX(dispMinMs);
                const int x2 = MsToX(dispMaxMs);
                const int hotKnob = (g_drag == DRAG_IDLE_LO) ? 0
                    : (g_drag == DRAG_IDLE_HI ? 1 : -1);

                DrawSliderRow(g, rc, kRowIdle, TXT_LBL_IDLE, buf, kTextDim, tr,
                    2, x1, x2, x1, x2, hotKnob, true,
                    TXT_SCALE_20MS, TXT_SCALE_90S,
                    TXT_SCALE_45S, (tr.left + tr.right) / 2);
            }

            // ---- 赎金目标 ----
            {
                const RECT tr = GoldTrack();
                const int shown = dispGold;
                const int kx = GoldToX(shown);

                swprintf_s(buf, L"%d", shown);
                DrawSliderRow(g, rc, kRowGold,
                    g_edit.hardcore ? TXT_LBL_GOLD_HC : TXT_LBL_GOLD,
                    buf, shown > 500 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_GOLD ? 0 : -1, true,
                    g_edit.hardcore ? TXT_SCALE_1000 : TXT_SCALE_10,
                    g_edit.hardcore ? TXT_SCALE_9999 : TXT_SCALE_1000,
                    g_edit.hardcore ? TXT_SCALE_5000 : TXT_SCALE_500,
                    g_edit.hardcore ? GoldToX(5000) : GoldToX(500));
            }

            // ---- 关窗惩罚时长 ----
            {
                const RECT tr = CloseTrack();
                const int ms = dispClose;
                const int kx = CloseToX(ms);

                if (ms == 0)
                    swprintf_s(buf, FMT_CLOSE_ZERO);
                else
                    swprintf_s(buf, FMT_SEC_ONE, ms / 1000.0);

                DrawSliderRow(g, rc, kRowClose, TXT_LBL_CLOSE, buf,
                    ms > 10000 ? kAccent : kTextDim, tr,
                    1, kx, kx, tr.left, kx,
                    g_drag == DRAG_CLOSE ? 0 : -1, true,
                    TXT_SCALE_0, g_edit.hardcore ? TXT_SCALE_30S : TXT_SCALE_18S,
                    g_edit.hardcore ? TXT_SCALE_30S : TXT_SCALE_18S,
                    g_edit.hardcore ? CloseToX(30000) : CloseToX(18000));
            }

            // ---- 假金币比例 + 假币形态配比 ----
            {
                const bool on = FakeEnabled();

                {
                    const RECT tr = FakePctTrack();
                    const int kx = PctToX(dispFakePct, tr);
                    swprintf_s(buf, FMT_PCT, dispFakePct);
                    DrawSliderRow(g, rc, kRowFakePct, TXT_LBL_FAKE_PCT, buf,
                        kTextDim, tr, 1, kx, kx, tr.left, kx,
                        g_drag == DRAG_FAKE_PCT ? 0 : -1, on,
                        TXT_SCALE_0PCT, TXT_SCALE_100PCT, nullptr, 0);
                }

                DrawFakeMixRow(g, rc, on, dispB1, dispB2);
            }

            // ---- 硬核专属四条（硬核没开时整条灰掉、拖不动）----
            //
            // 顺序照用户提的来：弹窗上限 → 阻挡鼠标弹窗的间隔 → 桌面锁定
            // 数量上限 → 桌面锁定时长。全部是滑条，全部挂 Tween，
            // 所以拖拽 / 滚轮 / 恢复默认 / 切模式都和其他滑条一样有缓动。
            {
                const bool on = HardcoreOn();

                // ---- 硬核：同时最多几个弹窗（10-30）----
                {
                    const RECT tr = HardWinTrack();
                    const int n = dispHardWin;
                    const int kx = HardWinToX(n);

                    swprintf_s(buf, FMT_COUNT, n);
                    DrawSliderRow(g, rc, kRowHardWin, TXT_LBL_HARDWIN, buf,
                        kTextDim, tr, 1, kx, kx, tr.left, kx,
                        g_drag == DRAG_HARDWIN ? 0 : -1, on,
                        TXT_SCALE_10, TXT_SCALE_30,
                        TXT_SCALE_22DEF, HardWinToX(settings::kDefaultHardPopupMax));
                }

                // ---- 硬核：阻挡鼠标弹窗的生成间隔（0.9~18 秒，两珠）----
                {
                    const RECT tr = CursorTrack();
                    FormatSpanSecs(buf, 96, dispCursorMin, dispCursorMax);

                    const int x1 = CursorMsToX(dispCursorMin);
                    const int x2 = CursorMsToX(dispCursorMax);
                    const int hotKnob = (g_drag == DRAG_CURSOR_LO) ? 0
                        : (g_drag == DRAG_CURSOR_HI ? 1 : -1);

                    DrawSliderRow(g, rc, kRowCursor, TXT_LBL_CURSOR,
                        buf, kTextDim, tr, 2, x1, x2, x1, x2, hotKnob, on,
                        TXT_SCALE_09S, TXT_SCALE_18S, nullptr, 0);
                }

                // ---- 硬核：桌面锁定非快捷方式的数量上限 ----
                //
                // 右端是**动态**的：桌面上真有几个能锁的非快捷方式条目，
                // 滑条就只能拖到几（硬顶 90）。刻度文字直接把这两个数写出来，
                // 免得用户以为"怎么拖不到 90"。
                {
                    const RECT tr = LockNumTrack();
                    const int n = dispLockNum;
                    const int kx = LockNumToX(n);

                    wchar_t hi[64];
                    swprintf_s(hi, FMT_LOCKCAP,
                        LockCap(), settings::DesktopItemCount());

                    swprintf_s(buf, FMT_COUNT, n);
                    DrawSliderRow(g, rc, kRowLockNum, TXT_LBL_LOCKNUM, buf,
                        kTextDim, tr, 1, kx, kx, tr.left, kx,
                        g_drag == DRAG_LOCKNUM ? 0 : -1, on,
                        TXT_SCALE_0, hi, nullptr, 0);
                }

                // ---- 硬核：桌面锁定时长（0.9~18 秒，两珠）----
                {
                    const RECT tr = LockDurTrack();
                    FormatSpanSecs(buf, 96, dispLockMin, dispLockMax);

                    const int x1 = LockDurMsToX(dispLockMin);
                    const int x2 = LockDurMsToX(dispLockMax);
                    const int hotKnob = (g_drag == DRAG_LOCKDUR_LO) ? 0
                        : (g_drag == DRAG_LOCKDUR_HI ? 1 : -1);

                    DrawSliderRow(g, rc, kRowLockDur, TXT_LBL_LOCKDUR,
                        buf, kTextDim, tr, 2, x1, x2, x1, x2, hotKnob, on,
                        TXT_SCALE_09S, TXT_SCALE_18S, nullptr, 0);
                }

                // ---- 金币生成磁盘范围（点进去是 2D 盘符页）----
                //
                // 这一条不是滑条而是一个入口按钮：范围是"勾哪几个盘"，
                // 塞不进一条轨道里。行内左侧照旧是标签 + 当前值，右半边是按钮。
                //
                // **只在硬核开着时点得进去**（磁盘范围本来就是硬核的参数）：
                // 关着的时候按钮画成灰的，命中判定那边也会挡住。
                {
                    wchar_t sum[160];
                    DrivesSummary(sum, 160);

                    RectF t(xL, rc.Y + (REAL)(ViewOffset() + kRowDrives),
                        (REAL)(kContentW - kM * 2), (REAL)kLabelH);
                    DrawTextCjk(g, TXT_LBL_DRIVES, t, 14.0f,
                        on ? kTextMain : kTextFaint);
                    DrawTextMono(g, sum, t, 13.0f,
                        on ? kTextDim : kTextFaint, StringAlignmentFar);

                    DrawButton(g, rc, DrivesBtn(),
                        on ? TXT_BTN_DRIVES : TXT_BTN_DRIVES_OFF,
                        g_hot == HOT_DRIVES, false, on);
                }
            }

            g.ResetClip();

            // ================= 滚动条 =================
            {
                const RECT t = ScrollTrackRect();
                const RECT th = ScrollThumbRect();

                RectF track(xL + (REAL)(t.left - xL), rc.Y + (REAL)t.top,
                    (REAL)(t.right - t.left), (REAL)(t.bottom - t.top));
                FillRound(g, track, 6.0f, kScrollTrack);

                if (MaxScroll() > 0)
                {
                    RectF thumb(rc.X + (REAL)th.left, rc.Y + (REAL)th.top,
                        (REAL)(th.right - th.left), (REAL)(th.bottom - th.top));
                    FillRound(g, thumb, 6.0f,
                        (g_drag == DRAG_SCROLL) ? kScrollThumbActive : kScrollThumb);
                }
            }

            // ================= 底部 =================
            DrawButton(g, rc, ResetBtn(), TXT_BTN_RESET, g_hot == HOT_RESET, false);
            DrawButton(g, rc, StartBtn(), TXT_BTN_START, g_hot == HOT_START, true);

            {
                wchar_t hint[192];
                swprintf_s(hint, FMT_KEY_SETTINGS, (wchar_t)g_panicVk);
                RectF t(xL, rc.Y + (REAL)kHintTop, w, 16.0f);
                DrawTextCjk(g, hint, t, 12.0f, kTextHint);
            }
        }

        void Paint(Graphics& g, const RectF& rc, DWORD, void*)
        {
            PaintContent(g, rc);
        }

        // ==================================================================
        //  盘符页（第二个界面）
        // ==================================================================
        //
        //  设置列表里那条「金币生成磁盘范围」点进来就是这一页：一屏 2D 网格，
        //  每格一个盘符 + shell 给的盘图标，勾上的盘才是硬核撒金币的地方。
        //
        //  ---- 为什么只列固定盘 ----
        //
        //  gold.cpp 只往 DRIVE_FIXED 的顶层目录撒（U 盘拔了就没了、光驱写不进、
        //  网络盘动别人的东西）。所以"能生成金币的盘符" = 固定盘，这里也只列它们。
        //
        //  ---- 三态怎么落到勾上 ----
        //
        //  设置里存的是"勾了哪几个盘"（见 settings::Set::coinDrives）：
        //    全不勾   -> 存 "-"（空集：金币只落桌面）
        //    勾一部分 -> 存那几个字母
        //    全勾     -> 存空 = 未配置 = 全部固定盘（老行为，也是默认态）
        //  "全勾"和"没配过"合并成一种状态是故意的：以后插了新硬盘，默认态自然
        //  把它算进去，不需要用户回来重新勾一遍。

        // 盘符图标：直接问 shell 要，和「此电脑」里看到的一模一样。
        // 拿不到就返回 nullptr，那一格只画盘符字母（不影响勾选）。
        Gdiplus::Bitmap* ShellDriveIcon(const wchar_t* root)
        {
            SHFILEINFOW sfi = {};
            if (!SHGetFileInfoW(root, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON))
                return nullptr;
            if (!sfi.hIcon) return nullptr;

            Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHICON(sfi.hIcon);
            DestroyIcon(sfi.hIcon);

            if (bmp && bmp->GetLastStatus() != Ok) { delete bmp; bmp = nullptr; }
            return bmp;
        }

        void FreeDrives()
        {
            for (size_t i = 0; i < g_drives.size(); ++i)
            {
                delete g_drives[i].icon;
                g_drives[i].icon = nullptr;
            }
            g_drives.clear();
            g_drvHot = -1;
        }

        // 这个盘现在该不该是勾上的？（读 g_edit —— **目标值**，不是动画值）
        bool DriveSelected(wchar_t letter)
        {
            const std::vector<std::wstring>& dv = g_edit.coinDrives;

            if (dv.size() == 1 && dv[0] == settings::kCoinDrivesNoneToken)
                return false;                    // 显式空集
            if (dv.empty())
                return true;                     // 未配置 = 全部

            for (size_t i = 0; i < dv.size(); ++i)
                if (dv[i].size() == 1 && dv[i][0] == letter) return true;
            return false;
        }

        // 把所有格子的动画对齐到当前勾选状态（snap=true 用于刚进页面那一下）
        void SyncDriveTweens(bool snap)
        {
            for (size_t i = 0; i < g_drives.size(); ++i)
                g_drives[i].anim.Set(
                    DriveSelected(g_drives[i].letter) ? 1.0 : 0.0, snap);
        }

        // 扫一遍固定盘，建好格子（进页面时调一次；演出中途不重扫）
        void RefreshDrives()
        {
            FreeDrives();

            const DWORD mask = GetLogicalDrives();
            for (int d = 0; d < 26 && (int)g_drives.size() < kDrvMax; ++d)
            {
                if (!(mask & (1u << d))) continue;

                wchar_t root[8] = { (wchar_t)(L'A' + d), L':', L'\\', 0 };

                // 和 gold.cpp 的口径完全一致：只有固定盘能撒金币
                if (GetDriveTypeW(root) != DRIVE_FIXED) continue;

                DriveItem it;
                it.letter = (wchar_t)(L'A' + d);
                it.root = root;
                it.icon = ShellDriveIcon(root);
                g_drives.push_back(it);
            }

            SyncDriveTweens(true);

            elog::Write(L"[setup] 盘符页：找到 %d 个固定盘", (int)g_drives.size());
        }

        // 把「哪些盘勾了」写回设置。sel[i] = 第 i 个格子
        void CommitDriveSelection(const std::vector<bool>& sel)
        {
            int on = 0;
            for (size_t i = 0; i < sel.size(); ++i) if (sel[i]) ++on;

            std::vector<std::wstring> out;

            if (on == 0)
            {
                // 一个都没勾 = 显式空集（金币只落桌面）
                out.push_back(settings::kCoinDrivesNoneToken);
            }
            else if (on < (int)sel.size())
            {
                // 勾了一部分：只存勾上的那几个字母
                for (size_t i = 0; i < sel.size(); ++i)
                    if (sel[i]) out.push_back(std::wstring(1, g_drives[i].letter));
            }
            // on == 全部 -> 留空（未配置 = 全部固定盘）

            g_edit.coinDrives = out;
            SyncDriveTweens(false);       // 勾选动画走默认缓动
            PushLive();
        }

        std::vector<bool> CurrentDriveSelection()
        {
            std::vector<bool> sel(g_drives.size(), false);
            for (size_t i = 0; i < g_drives.size(); ++i)
                sel[i] = DriveSelected(g_drives[i].letter);
            return sel;
        }

        void SetAllDrives(bool on)
        {
            std::vector<bool> sel(g_drives.size(), on);
            CommitDriveSelection(sel);
        }

        int DriveTileAt(POINT p)
        {
            for (size_t i = 0; i < g_drives.size(); ++i)
                if (Hit(DriveTileRect((int)i), p)) return (int)i;
            return -1;
        }

        // 换页：**关掉当前这扇窗、再开一扇新的**。
        //
        // 为什么不在这扇窗里换画法：用户要的动效就是「点开始」「警告界面点返回」
        // 那一下 —— 旧窗淡出缩放 + 新窗淡入缩放，那两处用的都是 aero 自己的
        // 开关窗动画。这里照搬同一条路，动效天然一致，也不用另造一套内容动画。
        // 真正换 g_page 的时机在 ShowSettings 的循环里（见 g_pendingPage）。
        void SwitchPage(int page)
        {
            g_pendingPage = page;
            aero::AnimateClose(g_hwnd);
        }

        void PaintDrives(Graphics& g, const RectF& rc)
        {
            DrawPanel(g, rc);

            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)(kContentW - kM * 2);

            // ---- 头部 ----
            {
                SolidBrush bar(kAccent);
                g.FillRectangle(&bar, xL, rc.Y + (REAL)kDrvTitleTop + 2.0f, 4.0f, 18.0f);
                RectF t(xL + 12.0f, rc.Y + (REAL)kDrvTitleTop - 4.0f, w - 12.0f, 30.0f);
                DrawTextCjk(g, TXT_DRV_TITLE, t, 20.0f, kTextMain,
                    StringAlignmentNear, FontStyleBold);
            }
            {
                RectF a(xL, rc.Y + (REAL)kDrvSubTop, w, 16.0f);
                DrawTextCjk(g, TXT_DRV_SUB1, a, 12.0f, kTextHint);

                RectF b(xL, rc.Y + (REAL)kDrvSubTop + 18.0f, w, 16.0f);
                DrawTextCjk(g, TXT_DRV_SUB2, b, 12.0f, kTextDim);
            }

            // ---- 网格 ----
            if (g_drives.empty())
            {
                RectF t(xL, rc.Y + (REAL)(kDrvGridTop + 20), w, 20.0f);
                DrawTextCjk(g, TXT_DRV_EMPTY, t, 13.0f, kAccent);
            }

            for (size_t i = 0; i < g_drives.size(); ++i)
            {
                const DriveItem& dv = g_drives[i];
                const RECT lr = DriveTileRect((int)i);
                const RectF t(rc.X + (REAL)lr.left, rc.Y + (REAL)lr.top,
                    (REAL)(lr.right - lr.left), (REAL)(lr.bottom - lr.top));

                const float p = (float)dv.anim.Value();
                const bool hot = ((int)i == g_drvHot);

                FillRound(g, t, 8.0f, LerpColor(kTrackBg, kAccentSoft, p));
                StrokeRound(g, t, 8.0f,
                    hot ? kCheckEdgeHot
                        : LerpColor(kBtnSecondaryEdge, kBtnPrimaryEdge, p), 1.4f);

                // 图标（32x32 居中；拿不到就不画，只留盘符）
                if (dv.icon)
                {
                    RectF ib(t.X + (t.Width - 32.0f) * 0.5f, t.Y + 7.0f, 32.0f, 32.0f);
                    g.DrawImage(dv.icon, ib);
                }

                // 盘符 + 勾选状态
                {
                    wchar_t s[8];
                    swprintf_s(s, FMT_DRV_LETTER, dv.letter);

                    RectF lt(t.X, t.Y + 41.0f, t.Width, 20.0f);
                    DrawTextMono(g, s, lt, 17.0f,
                        (p > 0.5f || hot) ? kTextMain : kTextDim,
                        StringAlignmentCenter, FontStyleBold);

                    RectF tt(t.X, t.Y + 60.0f, t.Width, 14.0f);
                    DrawTextCjk(g, p > 0.5f ? TXT_DRV_PICKED : TXT_DRV_SKIPPED, tt, 10.0f,
                        p > 0.5f ? kAccent : kTextFaint, StringAlignmentCenter);
                }
            }

            // ---- 底部 ----
            DrawButton(g, rc, DrvBackBtn(), TXT_DRV_BACK, g_hot == HOT_DRV_BACK, false);
            DrawButton(g, rc, DrvNoneBtn(), TXT_DRV_NONE, g_hot == HOT_DRV_NONE, false);
            DrawButton(g, rc, DrvAllBtn(), TXT_DRV_ALL, g_hot == HOT_DRV_ALL, true);

            {
                wchar_t hint[192];
                swprintf_s(hint, FMT_KEY_DRIVES, (wchar_t)g_panicVk);
                RectF t(xL, rc.Y + (REAL)kHintTop, w, 16.0f);
                DrawTextCjk(g, hint, t, 12.0f, kTextHint);
            }
        }

        // ---- 拖拽 ----
        //
        // **所有分支都走短缓动**（kEaseDragMs = 80ms），不再 snap。
        //
        // 早先版本拖拽是 snap（完全瞬时），用户反馈"手动拖动所有滑条没有
        // 任何缓动曲线"。改成短缓动之后：
        //   * 珠子会"追"鼠标 —— 视觉上有平滑感，不是贴死；
        //   * 数值文字也跟着平滑过渡（因为它读的是 Tween 值）；
        //   * 80ms 足够短，快速拖动时仍然跟手，不会有明显滞后。
        //
        // 注意：这里只用到 p.x（横向拖滑条），纵向的位置在命中判定那一步
        // 已经算过滚动偏移了，所以**不要**在这里再减一次 ViewOffset。
        void ApplyDrag(POINT p)
        {
            switch (g_drag)
            {
            case DRAG_BGM:
                g_edit.bgmVol = VolFromX(p.x);
                g_tBgm.Set(g_edit.bgmVol, false, kEaseDragMs);
                audio::SetBgmLevel(g_edit.bgmVol);
                break;

            case DRAG_SFX:
                g_edit.sfxVol = VolFromX(p.x);
                g_tSfx.Set(g_edit.sfxVol, false, kEaseDragMs);
                audio::SetSfxLevel(g_edit.sfxVol);
                break;

            case DRAG_IDLE_LO:
            {
                int v = MsFromX(p.x);
                if (v > g_edit.maxMs) v = g_edit.maxMs;
                g_edit.minMs = v;
                g_tIdleMin.Set(g_edit.minMs, false, kEaseDragMs);
                break;
            }
            case DRAG_IDLE_HI:
            {
                int v = MsFromX(p.x);
                if (v < g_edit.minMs) v = g_edit.minMs;
                g_edit.maxMs = v;
                g_tIdleMax.Set(g_edit.maxMs, false, kEaseDragMs);
                break;
            }

            case DRAG_GOLD:
                g_edit.goldGoal = GoldFromX(p.x);
                g_tGold.Set(g_edit.goldGoal, false, kEaseDragMs);
                break;

            case DRAG_CLOSE:
                g_edit.childCloseMs = CloseFromX(p.x);
                g_tClose.Set(g_edit.childCloseMs, false, kEaseDragMs);
                break;

            case DRAG_FAKE_PCT:
                if (!FakeEnabled()) return;
                g_edit.fakePercent = PctFromX(p.x, FakePctTrack());
                g_tFakePct.Set(g_edit.fakePercent, false, kEaseDragMs);
                break;

            case DRAG_FAKE_MIX:
                if (!FakeEnabled()) return;
                if (g_dragMixIdx < 0 || g_dragMixIdx > 1) return;
                // 拖拽时珠子跟鼠标 —— 用短缓动
                SetFakeMixBound(g_dragMixIdx, MixFromX(p.x),
                    false, kEaseDragMs);
                break;

            // ---- 硬核专属四条 ----
            //
            // 两条两珠滑条的规矩和「每次跳杀间隔」一致：两珠互相不能越过
            // （左珠 <= 右珠），拖到对方身上就顶住。
            case DRAG_HARDWIN:
                if (!HardcoreOn()) return;
                g_edit.hardPopupMax = HardWinFromX(p.x);
                g_tHardWin.Set(g_edit.hardPopupMax, false, kEaseDragMs);
                break;

            case DRAG_CURSOR_LO:
            {
                if (!HardcoreOn()) return;
                int v = CursorMsFromX(p.x);
                if (v > g_edit.cursorGapMaxMs) v = g_edit.cursorGapMaxMs;
                g_edit.cursorGapMinMs = v;
                g_tCursorMin.Set(g_edit.cursorGapMinMs, false, kEaseDragMs);
                break;
            }
            case DRAG_CURSOR_HI:
            {
                if (!HardcoreOn()) return;
                int v = CursorMsFromX(p.x);
                if (v < g_edit.cursorGapMinMs) v = g_edit.cursorGapMinMs;
                g_edit.cursorGapMaxMs = v;
                g_tCursorMax.Set(g_edit.cursorGapMaxMs, false, kEaseDragMs);
                break;
            }

            case DRAG_LOCKNUM:
                if (!HardcoreOn()) return;
                g_edit.extraLockCount = LockNumFromX(p.x);
                g_tLockNum.Set(g_edit.extraLockCount, false, kEaseDragMs);
                break;

            case DRAG_LOCKDUR_LO:
            {
                if (!HardcoreOn()) return;
                int v = LockDurMsFromX(p.x);
                if (v > g_edit.extraLockMaxMs) v = g_edit.extraLockMaxMs;
                g_edit.extraLockMinMs = v;
                g_tLockMin.Set(g_edit.extraLockMinMs, false, kEaseDragMs);
                break;
            }
            case DRAG_LOCKDUR_HI:
            {
                if (!HardcoreOn()) return;
                int v = LockDurMsFromX(p.x);
                if (v < g_edit.extraLockMinMs) v = g_edit.extraLockMinMs;
                g_edit.extraLockMaxMs = v;
                g_tLockMax.Set(g_edit.extraLockMaxMs, false, kEaseDragMs);
                break;
            }

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
                    // 拖滚动条也走短缓动，内容跟手但不硬贴
                    g_tScroll.Set((double)g_scroll, false, kEaseDragMs);
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

            if (g_page == 1)
            {
                if (Hit(DrvBackBtn(), p))      h = HOT_DRV_BACK;
                else if (Hit(DrvNoneBtn(), p)) h = HOT_DRV_NONE;
                else if (Hit(DrvAllBtn(), p))  h = HOT_DRV_ALL;

                // 格子悬停：只有固定盘才高亮（本来就只列固定盘，这层是防将来
                // 加了不可选的格子时忘了改）
                const int ti = DriveTileAt(p);
                if (ti != g_drvHot)
                {
                    g_drvHot = ti;
                    aero::Repaint(g_hwnd);
                }

                if (h != g_hot)
                {
                    g_hot = h;
                    aero::Repaint(g_hwnd);
                }
                return;
            }

            if (Hit(ResetBtn(), p)) h = HOT_RESET;
            else if (Hit(StartBtn(), p)) h = HOT_START;
            // 灰掉的按钮不高亮，否则鼠标一上去就像能点
            else if (g_edit.hardcore && Hit(DrivesHit(), p)) h = HOT_DRIVES;

            if (h != g_hot)
            {
                g_hot = h;
                aero::Repaint(g_hwnd);
            }
        }

        // 盘符页的鼠标消息。返回 true = 已经处理掉了。
        bool OnMouseDrives(HWND hwnd, UINT msg, POINT p)
        {
            switch (msg)
            {
            case WM_MOUSEMOVE:
                RefreshHot(p);
                return true;

            case WM_LBUTTONDOWN:
                if (Hit(DrvBackBtn(), p))
                {
                    // 返回设置列表：走和「点开始」同一条关闭动画，
                    // 回来时再开一扇设置窗（淡入缩放）
                    SwitchPage(0);
                    return true;
                }
                if (Hit(DrvAllBtn(), p))  { SetAllDrives(true);  aero::Repaint(hwnd); return true; }
                if (Hit(DrvNoneBtn(), p)) { SetAllDrives(false); aero::Repaint(hwnd); return true; }

                {
                    const int ti = DriveTileAt(p);
                    if (ti >= 0)
                    {
                        std::vector<bool> sel = CurrentDriveSelection();
                        sel[ti] = !sel[ti];
                        CommitDriveSelection(sel);
                        elog::Write(L"[setup] 盘符页：%c: %s",
                            g_drives[ti].letter, sel[ti] ? L"勾上" : L"取消");
                        aero::Repaint(hwnd);
                    }
                }
                return true;

            default:
                return true;      // 盘符页不吃别的鼠标消息（尤其不吃滚动）
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
            // 盘符页是另一套命中判定，整块截走
            if (g_page == 1) { OnMouseDrives(hwnd, msg, p); return; }

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
                    // 缓动：所有滑条、复选框一起"滑"到默认值。
                    SyncAllTweens(false);
                    PushLive();
                    elog::Write(L"[setup] 已恢复默认设置");
                    aero::Repaint(hwnd);
                    return;
                }

                if (Hit(SafeHit(), p))
                {
                    g_edit.photosensitiveSafe = !g_edit.photosensitiveSafe;
                    g_tSafe.Set(g_edit.photosensitiveSafe ? 1.0 : 0.0, false);
                    PushLive();
                    aero::Repaint(hwnd);
                    return;
                }

                if (Hit(HardHit(), p))
                {
                    g_edit.hardcore = !g_edit.hardcore;
                    ApplyModeDefaults(g_edit, g_edit.hardcore);

                    g_tHard.Set(g_edit.hardcore ? 1.0 : 0.0, false);
                    g_tGold.Set(g_edit.goldGoal, false);
                    g_tClose.Set(g_edit.childCloseMs, false);

                    PushLive();

                    aero::AnimateJolt(hwnd, 9.0f, 380);

                    elog::Write(L"[setup] 硬核模式 %s（赎金 %d，关窗惩罚 %dms，倒计时 %d 秒，假币 %d%%，弹窗上限 %d，阻挡弹窗 %d-%dms，桌面锁定 %d 个 %d-%dms）",
                        g_edit.hardcore ? L"开" : L"关",
                        g_edit.goldGoal, g_edit.childCloseMs,
                        g_edit.hardcore ? settings::kHardcoreRansomMs / 1000 : 90,
                        g_edit.fakePercent,
                        g_edit.hardPopupMax,
                        g_edit.cursorGapMinMs, g_edit.cursorGapMaxMs,
                        g_edit.extraLockCount,
                        g_edit.extraLockMinMs, g_edit.extraLockMaxMs);
                    aero::Repaint(hwnd);
                    return;
                }

                // ---- 滚动条 ----
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
                        g_tScroll.Set((double)g_scroll, false);
                    }
                    SetCapture(hwnd);
                    aero::Repaint(hwnd);
                    return;
                }

                if (!InView(p)) return;

                // ---- 金币生成磁盘范围：点进盘符页 ----
                //
                // **只有硬核开着才进得去**（磁盘范围是硬核的参数，普通模式
                // 根本不往磁盘撒金币）。关着时点这里什么都不做 —— 那行按钮
                // 本来就画成灰的了。
                //
                // 放在 InView 之后：盘符行的矩形带着滚动偏移，滚出视口之后
                // 它可能压到页头/页脚上，先过 InView 才不会误触。
                if (g_edit.hardcore && Hit(DrivesHit(), p))
                {
                    SwitchPage(1);
                    return;
                }

                // ---- 滑条（一行行判）----
                const RECT bt = BgmTrack();
                if (TrackHit(bt, p)) { g_drag = DRAG_BGM; SetCapture(hwnd); ApplyDrag(p); return; }

                const RECT st = SfxTrack();
                if (TrackHit(st, p)) { g_drag = DRAG_SFX; SetCapture(hwnd); ApplyDrag(p); return; }

                const RECT it = IntTrack();
                if (TrackHit(it, p))
                {
                    // 两颗珠子怎么选：**重合（或几乎重合）时一律选左珠**。
                    // 理由见 MixNearestKnob 的注释。
                    const int x1 = MsToX(g_edit.minMs);
                    const int x2 = MsToX(g_edit.maxMs);

                    if (x2 - x1 <= kOverlapEps)
                    {
                        g_drag = DRAG_IDLE_LO;
                    }
                    else
                    {
                        const int d1 = (p.x > x1) ? (p.x - x1) : (x1 - p.x);
                        const int d2 = (p.x > x2) ? (p.x - x2) : (x2 - p.x);
                        g_drag = (d1 <= d2) ? DRAG_IDLE_LO : DRAG_IDLE_HI;
                    }
                    SetCapture(hwnd);
                    ApplyDrag(p);
                    return;
                }

                const RECT gt = GoldTrack();
                if (TrackHit(gt, p)) { g_drag = DRAG_GOLD; SetCapture(hwnd); ApplyDrag(p); return; }

                const RECT ct = CloseTrack();
                if (TrackHit(ct, p)) { g_drag = DRAG_CLOSE; SetCapture(hwnd); ApplyDrag(p); return; }

                if (FakeEnabled())
                {
                    const RECT fp = FakePctTrack();
                    if (TrackHit(fp, p)) { g_drag = DRAG_FAKE_PCT; SetCapture(hwnd); ApplyDrag(p); return; }

                    const RECT fm = FakeMixTrack();
                    if (TrackHit(fm, p))
                    {
                        g_dragMixIdx = MixNearestKnob(p.x);
                        g_drag = DRAG_FAKE_MIX;
                        SetCapture(hwnd);
                        ApplyDrag(p);
                        return;
                    }
                }

                // ---- 硬核专属四条 ----
                if (HardcoreOn())
                {
                    const RECT hw = HardWinTrack();
                    if (TrackHit(hw, p)) { g_drag = DRAG_HARDWIN; SetCapture(hwnd); ApplyDrag(p); return; }

                    const RECT cu = CursorTrack();
                    if (TrackHit(cu, p))
                    {
                        g_drag = (NearestKnob(CursorMsToX(g_edit.cursorGapMinMs),
                            CursorMsToX(g_edit.cursorGapMaxMs), p.x) == 0)
                            ? DRAG_CURSOR_LO : DRAG_CURSOR_HI;
                        SetCapture(hwnd);
                        ApplyDrag(p);
                        return;
                    }

                    const RECT ln = LockNumTrack();
                    if (TrackHit(ln, p)) { g_drag = DRAG_LOCKNUM; SetCapture(hwnd); ApplyDrag(p); return; }

                    const RECT ld = LockDurTrack();
                    if (TrackHit(ld, p))
                    {
                        g_drag = (NearestKnob(LockDurMsToX(g_edit.extraLockMinMs),
                            LockDurMsToX(g_edit.extraLockMaxMs), p.x) == 0)
                            ? DRAG_LOCKDUR_LO : DRAG_LOCKDUR_HI;
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

            // 鼠标捕获被别人抢走（第三方截屏工具的全屏覆盖层最典型）：
            // 我们再也等不到 WM_LBUTTONUP 了，必须在这儿把拖拽状态清掉，
            // 否则之后每一次鼠标移动都会被 ApplyDrag 当成"还在拖那根滑条"。
            // 注意这条消息**不带坐标**，p 是 (0,0) 占位 —— 不能拿它做命中判定。
            case WM_CAPTURECHANGED:
                if (g_drag)
                {
                    elog::Write(L"[setup] 鼠标捕获被抢走（drag=%d），强制结束拖拽", g_drag);
                    g_drag = DRAG_NONE;
                    g_dragMixIdx = -1;
                    g_hot = HOT_NONE;
                    aero::Repaint(hwnd);
                }
                return;

            default:
                return;
            }
        }

        // ---- 滚轮 ----
        //
        // **所有滚轮触发的改动都走长缓动**（默认 180ms）。一格滚轮是一个
        // 离散事件，如果数值瞬跳，观感就像"点一下蹦一格"；缓动之后是
        // "滑一格"。
        void OnWheel(HWND hwnd, POINT p, int delta, void*)
        {
            // 盘符页不滚动：网格一屏放得下，滚轮在这一页没有意义
            if (g_page == 1) return;

            const int step = (delta > 0) ? 1 : -1;

            const int kScrollStep = 34;
            const bool overScrollbar = Hit(ScrollTrackRect(), p);

            if (!overScrollbar && InView(p))
            {
                if (Hit(SafeHit(), p))
                {
                    g_edit.photosensitiveSafe = !g_edit.photosensitiveSafe;
                    g_tSafe.Set(g_edit.photosensitiveSafe ? 1.0 : 0.0, false);
                    PushLive();
                    aero::Repaint(hwnd);
                    return;
                }

                // 硬核开关**不接受滚轮**：这么重的开关被滚轮误触翻掉太容易了。
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
                    g_tBgm.Set(g_edit.bgmVol, false);
                    audio::SetBgmLevel(g_edit.bgmVol);
                }
                else if (p.y >= st.top - 10 && p.y < st.bottom + 10)
                {
                    g_edit.sfxVol += step * kVolStep;
                    if (g_edit.sfxVol < settings::kVolMin) g_edit.sfxVol = settings::kVolMin;
                    if (g_edit.sfxVol > settings::kVolMax) g_edit.sfxVol = settings::kVolMax;
                    g_tSfx.Set(g_edit.sfxVol, false);
                    audio::SetSfxLevel(g_edit.sfxVol);
                }
                else if (p.y >= it.top - 10 && p.y < it.bottom + 10)
                {
                    const int mstep = MsStep() * ((delta > 0) ? 1 : -1);
                    const int x1 = MsToX(g_edit.minMs);
                    const int x2 = MsToX(g_edit.maxMs);
                    if (p.x < (x1 + x2) / 2)
                    {
                        g_edit.minMs += mstep;
                        if (g_edit.minMs < kLoMs)        g_edit.minMs = kLoMs;
                        if (g_edit.minMs > g_edit.maxMs) g_edit.minMs = g_edit.maxMs;
                        g_tIdleMin.Set(g_edit.minMs, false);
                    }
                    else
                    {
                        g_edit.maxMs += mstep;
                        if (g_edit.maxMs > kHiMs)        g_edit.maxMs = kHiMs;
                        if (g_edit.maxMs < g_edit.minMs) g_edit.maxMs = g_edit.minMs;
                        g_tIdleMax.Set(g_edit.maxMs, false);
                    }
                }
                else if (p.y >= gt.top - 10 && p.y < gt.bottom + 10)
                {
                    // 一格 1%：普通跨度 990 -> 9，硬核跨度 8999 -> 89。
                    // 但下面还要把结果吸到 10 的倍数，所以**步长自己也必须是
                    // 10 的倍数** —— 否则每次都会在吸附那一步丢步甚至归零。
                    int gstep = (GoldHi() - GoldLo()) / 100;
                    gstep = (gstep / 10) * 10;
                    if (gstep < 10) gstep = 10;

                    g_edit.goldGoal += step * gstep;
                    if (g_edit.goldGoal < GoldLo()) g_edit.goldGoal = GoldLo();
                    if (g_edit.goldGoal > GoldHi()) g_edit.goldGoal = GoldHi();
                    g_edit.goldGoal = (g_edit.goldGoal / 10) * 10;
                    g_tGold.Set(g_edit.goldGoal, false);
                }
                else if (p.y >= ct.top - 10 && p.y < ct.bottom + 10)
                {
                    const int cstep = (CloseHi() / 100 > 0) ? (CloseHi() / 100) : 100;
                    g_edit.childCloseMs += step * cstep;
                    if (g_edit.childCloseMs < 0) g_edit.childCloseMs = 0;
                    if (g_edit.childCloseMs > CloseHi()) g_edit.childCloseMs = CloseHi();
                    g_edit.childCloseMs = (g_edit.childCloseMs / 100) * 100;
                    g_tClose.Set(g_edit.childCloseMs, false);
                }
                else if (FakeEnabled() &&
                    p.y >= FakePctTrack().top - 10 && p.y < FakePctTrack().bottom + 10)
                {
                    // 步长 5（原来 1）：0-100 的范围内，1 只有滑条的 1% ≈
                    // 4px，看不出缓动。5 是 20px，能明显看到珠子"滑"过去。
                    g_edit.fakePercent += step * 5;
                    if (g_edit.fakePercent < settings::kFakePctMin) g_edit.fakePercent = settings::kFakePctMin;
                    if (g_edit.fakePercent > settings::kFakePctMax) g_edit.fakePercent = settings::kFakePctMax;
                    g_tFakePct.Set(g_edit.fakePercent, false);
                }
                else if (FakeEnabled() &&
                    p.y >= FakeMixTrack().top - 10 && p.y < FakeMixTrack().bottom + 10)
                {
                    // ---- 假币形态配比：滚轮 ----
                    //
                    // 判定逻辑**完全照搬「遭遇战间隔」那一条**：光标在中点
                    // 左边就调左珠、右边就调右珠，和滚动方向无关。
                    //
                    // 为什么是这一套：早先的"按方向选珠"版本有两个问题，
                    // 而"按光标位置"天然绕开了它们 ——
                    //
                    //   1. 两颗珠子重合时中点就是重合点。光标在左 → 调左珠，
                    //      光标在右 → 调右珠，**两个方向都能拉开**，不会
                    //      出现"只往一个方向走"。
                    //
                    //   2. 位置判定用**显示值**（MixKnobX 读 Tween），
                    //      保证"鼠标在哪边"和"珠子看起来在哪边"是一致的；
                    //      新值则在**目标值**上累加（b1/b2 读 g_edit），
                    //      避免"显示值还没追上 → 目标算出来没变 → 滚半天
                    //      没反应"。
                    //
                    // 用户已经熟悉了跳杀间隔那条的手感，这边照搬一套就行。
                    const int x1 = MixKnobX(0);
                    const int x2 = MixKnobX(1);
                    const int mid = (x1 + x2) / 2;
                    const int stepVal = step * 5;

                    int b1, b2;
                    FakeMixBounds(b1, b2);

                    if (p.x < mid)
                        SetFakeMixBound(0, b1 + stepVal, false /* 缓动 */);
                    else
                        SetFakeMixBound(1, b2 + stepVal, false /* 缓动 */);
                }

                // ---- 硬核专属四条（滚轮 = 离散事件 -> 长缓动）----
                //
                // 两条两珠滑条选珠的规矩**完全照搬「每次跳杀间隔」**：
                // 光标在珠子中点左边就调左珠、右边就调右珠，和滚动方向无关。
                // （理由见假币那一段的长注释。）
                else if (HardcoreOn() &&
                    p.y >= HardWinTrack().top - 10 && p.y < HardWinTrack().bottom + 10)
                {
                    // 一格 1 个：10-30 的区间只有 20 格，一格再细分就没手感了。
                    g_edit.hardPopupMax = ClampI(g_edit.hardPopupMax + step,
                        settings::kHardPopupMin, settings::kHardPopupMax);
                    g_tHardWin.Set(g_edit.hardPopupMax, false);
                }
                else if (HardcoreOn() &&
                    p.y >= CursorTrack().top - 10 && p.y < CursorTrack().bottom + 10)
                {
                    // 一格 0.5 秒：轨道 506px 走完 17.1 秒，0.5 秒约 15px，
                    // 缓动看得出来；再小就跟瞬移一样了。
                    const int d = step * 500;
                    const int mid = (CursorMsToX(g_edit.cursorGapMinMs) +
                        CursorMsToX(g_edit.cursorGapMaxMs)) / 2;

                    if (p.x < mid)
                    {
                        int v = g_edit.cursorGapMinMs + d;
                        if (v > g_edit.cursorGapMaxMs) v = g_edit.cursorGapMaxMs;
                        g_edit.cursorGapMinMs = SnapMs(v,
                            settings::kCursorGapFloorMs, settings::kCursorGapCeilMs);
                        g_tCursorMin.Set(g_edit.cursorGapMinMs, false);
                    }
                    else
                    {
                        int v = g_edit.cursorGapMaxMs + d;
                        if (v < g_edit.cursorGapMinMs) v = g_edit.cursorGapMinMs;
                        g_edit.cursorGapMaxMs = SnapMs(v,
                            settings::kCursorGapFloorMs, settings::kCursorGapCeilMs);
                        g_tCursorMax.Set(g_edit.cursorGapMaxMs, false);
                    }
                }
                else if (HardcoreOn() &&
                    p.y >= LockNumTrack().top - 10 && p.y < LockNumTrack().bottom + 10)
                {
                    // 一格 1 个。右端是动态的（桌面可锁项数），所以夹的是 LockCap()。
                    g_edit.extraLockCount = ClampI(g_edit.extraLockCount + step, 0, LockCap());
                    g_tLockNum.Set(g_edit.extraLockCount, false);
                }
                else if (HardcoreOn() &&
                    p.y >= LockDurTrack().top - 10 && p.y < LockDurTrack().bottom + 10)
                {
                    const int d = step * 500;
                    const int mid = (LockDurMsToX(g_edit.extraLockMinMs) +
                        LockDurMsToX(g_edit.extraLockMaxMs)) / 2;

                    if (p.x < mid)
                    {
                        int v = g_edit.extraLockMinMs + d;
                        if (v > g_edit.extraLockMaxMs) v = g_edit.extraLockMaxMs;
                        g_edit.extraLockMinMs = SnapMs(v,
                            settings::kExtraLockFloorMs, settings::kExtraLockCeilMs);
                        g_tLockMin.Set(g_edit.extraLockMinMs, false);
                    }
                    else
                    {
                        int v = g_edit.extraLockMaxMs + d;
                        if (v < g_edit.extraLockMinMs) v = g_edit.extraLockMinMs;
                        g_edit.extraLockMaxMs = SnapMs(v,
                            settings::kExtraLockFloorMs, settings::kExtraLockCeilMs);
                        g_tLockMax.Set(g_edit.extraLockMaxMs, false);
                    }
                }
                else
                {
                    g_scroll = ClampScroll(g_scroll + ((delta > 0) ? -kScrollStep : kScrollStep));
                    g_tScroll.Set((double)g_scroll, false);
                }
            }
            else
            {
                g_scroll = ClampScroll(g_scroll + ((delta > 0) ? -kScrollStep : kScrollStep));
                g_tScroll.Set((double)g_scroll, false);
            }

            aero::Repaint(hwnd);
        }

        // ---- 离线导出（`--setup-ui`）----
        bool RenderPreview(const wchar_t* path, const settings::Set& s, bool grid, int scroll)
        {
            const int CW = kW, CH = kH;

            Bitmap bmp(CW, CH, PixelFormat32bppARGB);
            if (bmp.GetLastStatus() != Ok) return false;

            {
                Graphics g(&bmp);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

                SolidBrush bg(kPreviewCanvas);
                g.FillRectangle(&bg, 0, 0, CW, CH);

                // 预览里 Tween 应该处于**终点状态**（"动画播完之后"），
                // 所以先把所有 Tween snap 到传入的样例值。
                const settings::Set saved = g_edit;
                const Tween savedBgm = g_tBgm;
                const Tween savedSfx = g_tSfx;
                const Tween savedIdleMin = g_tIdleMin;
                const Tween savedIdleMax = g_tIdleMax;
                const Tween savedGold = g_tGold;
                const Tween savedClose = g_tClose;
                const Tween savedFakePct = g_tFakePct;
                const Tween savedFakePre = g_tFakePre;
                const Tween savedFakeSuf = g_tFakeSuf;
                const Tween savedSafe = g_tSafe;
                const Tween savedHard = g_tHard;
                const Tween savedHardWin = g_tHardWin;
                const Tween savedCursorMin = g_tCursorMin;
                const Tween savedCursorMax = g_tCursorMax;
                const Tween savedLockNum = g_tLockNum;
                const Tween savedLockMin = g_tLockMin;
                const Tween savedLockMax = g_tLockMax;
                const Tween savedScroll = g_tScroll;

                g_edit = s;
                SyncAllTweens(true);
                // 预览里滚动条也应该是**静止**的：直接摆到目标位置（夹过之后）。
                g_tScroll.Set((double)ClampScroll(scroll), true);

                PaintContent(g, RectF(0.0f, 0.0f, (REAL)CW, (REAL)CH));

                g_edit = saved;
                g_tBgm = savedBgm;
                g_tSfx = savedSfx;
                g_tIdleMin = savedIdleMin;
                g_tIdleMax = savedIdleMax;
                g_tGold = savedGold;
                g_tClose = savedClose;
                g_tFakePct = savedFakePct;
                g_tFakePre = savedFakePre;
                g_tFakeSuf = savedFakeSuf;
                g_tSafe = savedSafe;
                g_tHard = savedHard;
                g_tHardWin = savedHardWin;
                g_tCursorMin = savedCursorMin;
                g_tCursorMax = savedCursorMax;
                g_tLockNum = savedLockNum;
                g_tLockMin = savedLockMin;
                g_tLockMax = savedLockMax;
                g_tScroll = savedScroll;

                if (grid) DrawGrid(g, RectF(0.0f, 0.0f, (REAL)CW, (REAL)CH));
            }

            return SavePng(bmp, path);
        }

    } // namespace setup

    // ==========================================================================
    //  应急提示窗口
    // ==========================================================================
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
        bool  g_hotBack = false;
        bool  g_armed = false;
        bool  g_armedBack = false;
        bool  g_confirm = false;
        bool  g_back = false;
        setup_ui::Verdict g_verdict = setup_ui::VERDICT_ABORT;
        int   g_panicVk = 'Q';
        int   g_mode = 0;

        RECT OkBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        RECT BackBtn()
        {
            RECT r;
            SetRectLocal(r, kW - kM - kBtnW * 2 - 14, kBtnTop, kBtnW, kBtnH);
            return r;
        }

        void PaintContent(Graphics& g, const RectF& rc)
        {
            DrawPanel(g, rc);

            if (g_mode == 1)
                StrokeRound(g, rc, 10.0f, kHardcoreFrame, 2.0f);

            const REAL xL = rc.X + (REAL)kM;
            const REAL w = (REAL)kTextW;

            if (g_mode == 1)
            {
                {
                    SolidBrush bar(kNoticeBar);
                    g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 20.0f);
                    RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 34.0f);
                    DrawTextCjk(g, TXT_HC_TITLE, t, 21.0f, kTextMain,
                        StringAlignmentNear, FontStyleBold);
                }

                {
                    RectF t(xL, rc.Y + (REAL)kLede1Top, w, 20.0f);
                    DrawTextCjk(g, TXT_HC_LEDE,
                        t, 13.0f, kAccent);
                }

                const settings::Set& s = settings::Current();

                // 文案里的数字**全部现读设置**，一个都不写死 —— 这几条现在
                // 都是用户自己拖出来的（滑条默认值一改，文案就跟着变）。
                // 文字部分在文件顶部的文案表里，这里只负责填数。
                wchar_t l1[160], l2[160], l4[160], l5[160];
                swprintf_s(l1, FMT_HC_L1,
                    settings::GoldGoal(), settings::ChildCloseMs() / 1000.0);

                swprintf_s(l2, FMT_HC_L2,
                    settings::ExtraLockMinMs() / 1000.0,
                    settings::ExtraLockMaxMs() / 1000.0,
                    settings::ExtraLockCount());

                swprintf_s(l4, FMT_HC_L4,
                    settings::FakePercent(),
                    s.fakePrefixPct, s.fakeSuffixPct, s.fakeBothPct);

                swprintf_s(l5, FMT_HC_L5,
                    settings::HardPopupMax(),
                    settings::CursorGapMinMs() / 1000.0,
                    settings::CursorGapMaxMs() / 1000.0);

                const wchar_t* kLines[] = {
                    l1,
                    l2,
                    TXT_HC_L3,
                    l4,
                    l5,
                    TXT_HC_L6,
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
                {
                    SolidBrush bar(kNoticeBar);
                    g.FillRectangle(&bar, xL, rc.Y + 22.0f, 4.0f, 20.0f);
                    RectF t(xL + 12.0f, rc.Y + 14.0f, w - 12.0f, 34.0f);
                    DrawTextCjk(g, TXT_N_TITLE, t, 21.0f, kTextMain,
                        StringAlignmentNear, FontStyleBold);
                }
                {
                    RectF t(xL, rc.Y + (REAL)kLede1Top, w, 20.0f);
                    DrawTextCjk(g, TXT_N_LEDE, t, 13.0f, kAccent);
                }
                {
                    RectF t(xL, rc.Y + (REAL)kLede2Top, w, 20.0f);
                    DrawTextCjk(g, TXT_N_LINE1, t, 13.0f, kTextDim);
                }
                {
                    RectF t(xL, rc.Y + (REAL)kLede2Top + 20.0f, w, 20.0f);
                    DrawTextCjk(g, TXT_N_LINE2, t, 13.0f, kTextDim);
                }
                {
                    RectF t(xL, rc.Y + (REAL)kLede2Top + 40.0f, w, 20.0f);
                    DrawTextCjk(g, TXT_N_LINE3, t, 13.0f, kTextDim);
                }

                {
                    wchar_t line[192];
                    swprintf_s(line, FMT_N_SUMMARY,
                        settings::GoldGoal(), settings::MinMs(), settings::MaxMs(),
                        settings::ChildCloseMs() / 1000.0);

                    RectF t(xL, rc.Y + (REAL)(kLede2Top + 66), w, 20.0f);
                    DrawTextCjk(g, line, t, 12.0f, kTextHint);
                }
            }

            {
                const REAL top = rc.Y + (REAL)kExitTop;
                RectF box(xL, top, w, 68.0f);
                FillRound(g, box, 8.0f, kExitBoxBg);
                StrokeRound(g, box, 8.0f, kExitBoxEdge, 1.4f);

                RectF t1(xL + 14.0f, top + 8.0f, w - 28.0f, 24.0f);
                wchar_t line[160];
                swprintf_s(line, FMT_ESCAPE, (wchar_t)g_panicVk);
                DrawTextCjk(g, line, t1, 16.0f, kTextMain,
                    StringAlignmentNear, FontStyleBold);

                RectF t2(xL + 14.0f, top + 32.0f, w - 28.0f, 20.0f);
                DrawTextCjk(g, TXT_ESCAPE_DESC,
                    t2, 12.0f, kTextDim);
            }

            {
                RectF t(xL, rc.Y + (REAL)kFootTop, w, 18.0f);
                DrawTextCjk(g, TXT_FOOT1, t, 12.0f, kTextFaint);
            }
            {
                RectF t(xL, rc.Y + (REAL)kFootTop + 18.0f, w, 18.0f);
                DrawTextCjk(g, TXT_FOOT2, t, 12.0f, kTextFaint);
            }

            DrawButton(g, rc, BackBtn(), TXT_BTN_BACK, g_hotBack, false);
            DrawButton(g, rc, OkBtn(),
                g_mode == 1 ? TXT_BTN_OK_HC : TXT_BTN_OK, g_hotBtn, true);
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
                    aero::AnimateClose(hwnd);
                }
                else if (g_armedBack && hitBack && !g_done)
                {
                    g_back = true;
                    g_done = true;
                    aero::AnimateClose(hwnd);
                }

                g_armed = false;
                g_armedBack = false;
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

    } // namespace notice

    bool RenderNoticePreview(const wchar_t* path, bool grid)
    {
        using namespace notice;

        Bitmap bmp(kW, kH, PixelFormat32bppARGB);
        if (bmp.GetLastStatus() != Ok) return false;

        {
            Graphics g(&bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

            SolidBrush bg(kPreviewCanvas);
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
        g_scroll = 0;
        g_scrollGrab = 0;
        g_panicVk = panicVk;
        g_edit = settings::Current();

        // 一进来永远是设置列表那一页；盘符页的格子等用户点进去时再扫
        //（在那之前一个 HICON 都不取，不浪费启动时间）。
        g_page = 0;
        g_pendingPage = -1;
        g_drvHot = -1;
        FreeDrives();

        // 窗口第一帧就显示正确的值，不让它从 0 滑上来。
        SyncAllTweens(true);
        g_tScroll.Set(0.0, true);

        SetHotkeyHandler(HotkeyWhileSetup, nullptr);

        // ---- 开窗循环：设置列表 ↔ 盘符页 ----
        //
        // 换页不是在这扇窗里换画法，而是**关掉再开一扇**：
        // 旧窗走 aero::AnimateClose（淡出 + 缩放），新窗走 aero::Create 自带的
        // 打开动画（淡入 + 缩放）—— 和「点开始」「警告界面点返回」是同一条路，
        // 动效天然一致，不用另造一套内容动画。
        //
        // 循环退出条件：Commit() 落了 g_done（开始 / 中止 / 关窗），
        // 或者没人要求换页（正常只会是前者）。
        HWND h = nullptr;

        for (;;)
        {
            // 每扇窗都从干净状态开始
            g_hot = HOT_NONE;
            g_drvHot = -1;
            g_drag = DRAG_NONE;
            g_scrollGrab = 0;

            // 盘符页：建窗之前把格子扫好（RefreshDrives 里会先 FreeDrives）。
            // 设置页：上一扇（盘符页）已经销毁，图标现在才还 —— 不能在
            // SwitchPage 那一刻还，那扇窗还在播淡出动画、paint 还要用。
            if (g_page == 1) RefreshDrives();
            else             FreeDrives();

            aero::Options opt;
            opt.title = (g_page == 1) ? TXT_WIN_DRIVES : TXT_WIN_SETTINGS;
            opt.width = aero::OptionsWidthForContent(kW);
            opt.height = aero::OptionsHeightForContent(kH);
            opt.buttons = true;
            opt.topmost = true;
            opt.resizable = false;
            opt.animate = true;
            // tick 16ms（~60fps）：180ms 的缓动大约 11 帧，滑起来顺滑。
            opt.tickMs = 16;
            opt.onContentMouse = OnMouse;
            opt.onContentMouseUser = nullptr;
            opt.onContentWheel = OnWheel;
            opt.onContentWheelUser = nullptr;
            opt.onUserClose = [](HWND, void*) { Commit(VERDICT_ABORT); };
            opt.onUserCloseUser = nullptr;

            h = aero::Create(hInst, opt, Paint, nullptr);
            if (!h)
            {
                elog::Write(L"[setup] 设置窗口创建失败（页面 %d）", g_page);
                break;                     // g_verdict 仍是 VERDICT_ERROR
            }
            g_hwnd = h;
            SetWindowTextW(h, L"Ransom_dev");

            elog::Write(L"[setup] 窗口已打开：%s（背景音乐 %d%% / 音效 %d%% / 光敏安全 %s / 潜伏 %d-%dms / 内容区 %dx%d）",
                g_page == 1 ? L"金币生成范围" : L"设置列表",
                g_edit.bgmVol, g_edit.sfxVol,
                g_edit.photosensitiveSafe ? L"开" : L"关",
                g_edit.minMs, g_edit.maxMs, kW, kH);

            // 这个循环只收本窗口的消息（滤镜传了 h），所以换窗时新窗口的
            // 消息不会被上一轮吃掉。
            bool quit = false;
            while (aero::IsAlive(h) && !quit)
                PumpModal(h, quit);

            if (aero::IsAlive(h)) aero::Destroy(h);
            g_hwnd = nullptr;

            if (quit)
            {
                // 收到 WM_QUIT：按"中止"上报，主循环那边会干净退出
                g_verdict = VERDICT_ABORT;
                break;
            }
            if (g_done) break;                 // 开始 / 中止 / 关窗口
            if (g_pendingPage < 0) break;      // 什么都没要求，也收工

            // 换页：现在才真的翻页（见 g_pendingPage 的说明）
            g_page = g_pendingPage;
            g_pendingPage = -1;
        }

        ClearHotkeyHandler();

        // HICON 转出来的 Bitmap 是我们 new 的，退出前必须还回去
        FreeDrives();
        g_page = 0;
        g_pendingPage = -1;

        elog::Write(L"[setup] 设置窗口关闭，结果 %d", (int)g_verdict);
        return g_verdict;
    }

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
        g_verdict = VERDICT_ABORT;
        g_panicVk = panicVk;

        aero::Options opt;
        opt.title = (mode == 1) ? TXT_WIN_HARDCORE : TXT_WIN_NOTICE;
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
            elog::Write(L"[setup] %s窗口创建失败", mode == 1 ? L"硬核警告" : L"应急提示");
            return VERDICT_ERROR;
        }
        g_hwnd = h;
        SetWindowTextW(h, L"Ransom_dev");

        elog::Write(L"[setup] %s已弹出（安全阀 Ctrl+Alt+Shift+%c）",
            mode == 1 ? L"硬核模式警告" : L"应急提示", (wchar_t)panicVk);

        bool quit = false;
        while (aero::IsAlive(h) && !quit)
            PumpModal(h, quit);

        if (g_back)         g_verdict = VERDICT_BACK;
        else if (g_confirm) g_verdict = VERDICT_START;

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

    bool DumpSettingsPreview(const wchar_t* path, const settings::Set& s, bool grid, int scroll)
    {
        return setup::RenderPreview(path, s, grid, scroll);
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