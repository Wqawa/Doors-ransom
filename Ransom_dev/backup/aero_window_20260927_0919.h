// ============================================================================
//  aero_window.h
//
//  透明 Aero 玻璃风格的弹出窗口（从最初的 main.cpp 改造而来）。
//
//  每个窗口都是独立实例，状态挂在 GWLP_USERDATA 上，所以可以同时开很多个。
//  保留了原来的观感：分层窗口逐像素透明、圆角、四周阴影、标题栏高光、
//  底部内阴影与反光、圆角标题栏按钮。
//
//  用途：Ransom 的主勒索窗口与子窗口。
//  主窗口用 buttons=false（**没有最大化/最小化/关闭**），子窗口用 buttons=true。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

// GDI+ 前置依赖（WIN32_LEAN_AND_MEAN 不会带进来）
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace aero {

// 内容绘制回调。
// content 是内容区矩形（窗口客户区坐标，已扣掉阴影与标题栏）。
// frame 每帧递增，用来驱动动画。
typedef void (*PaintFn)(Gdiplus::Graphics& g, const Gdiplus::RectF& content,
                        DWORD frame, void* user);

// 内容区鼠标回调。
// pt 已经换算成**内容区坐标**（左上角 = (0,0)，和 PaintFn 里的 content
// 同一套原点），所以自绘控件（滑条、复选框）可以直接比对布局矩形，
// 不用自己减阴影和标题栏的高度。
//
// 只转发客户区里的鼠标消息，标题栏上的拖动 / 标题栏按钮不会走到这里。
// 拖拽时记得自己 SetCapture/ReleaseCapture —— 这里不代管捕获，
// 因为「拖到窗口外还继续跟手」是控件自己的语义（滑条要，按钮不要）。
typedef void (*ContentMouseFn)(HWND hwnd, UINT msg, POINT pt,
                               WPARAM wp, void* user);

// 内容区滚轮回调。
// delta 是正负号处理过的滚轮量（+1 / -1），pt 同样是内容区坐标。
typedef void (*ContentWheelFn)(HWND hwnd, POINT pt, int delta, void* user);

struct Options {
        std::wstring title;
        int  width = 420;
        int  height = 260;
        int  x = CW_USEDEFAULT;   // 屏幕坐标（含阴影外扩）
        int  y = CW_USEDEFAULT;
        bool buttons = true;            // 是否显示 最小化/最大化/关闭
        bool topmost = true;
        bool resizable = true;          // 边缘可拖拽缩放
        bool animate = true;            // 开/关/最大化/还原/最小化 动画
        UINT tickMs = 33;               // 静止时的内容重绘间隔。
        // 窗口开得多的时候要调大，否则消息循环被
        // 重绘压满，动画定时器会被饿住（实测延迟近 200ms）

// 玩家**主动**要求关闭窗口时（点右上角 X 按钮，或系统菜单 / Alt+F4
// 里的「关闭」）回调。用途：勒索子窗口要区分「玩家关的」和
// 「寿命到了自己淡出的」—— 前者要扣倒计时，后者不扣。
//
// 回调发生在真正开始淡出动画**之前**，此时窗口仍然有效。
// 从 popup 那边调 `aero::AnimateClose()` 触发的关闭**不会**走这个
// 回调（那条路走的是 WM_CLOSE，跟玩家点关闭按钮的路不同），
// 所以「到寿命自动淡出」不会被误判成玩家关闭。
//
// 传 nullptr 表示不关心。
        void (*onUserClose)(HWND hwnd, void* user) = nullptr;
        void* onUserCloseUser = nullptr;

// 内容区鼠标回调（见上面的 ContentMouseFn）。用途：窗口内容里画的自绘
// 控件（滑条 / 复选框 / 按钮）要收点击，而 aero 本身只管标题栏那几个按钮。
//
// 传 nullptr 表示这个窗口不关心内容区鼠标消息，行为和以前完全一样。
// 消息过来的顺序和 Win32 一致：WM_MOUSEMOVE / WM_LBUTTONDOWN /
// WM_LBUTTONUP / WM_LBUTTONDBLCLK；wp 是按键与修饰键状态，直接透传。
        ContentMouseFn onContentMouse = nullptr;
        void* onContentMouseUser = nullptr;

// 内容区滚轮回调（见上面的 ContentWheelFn）。滑条要能用滚轮微调。
// 传 nullptr 表示不关心。
        ContentWheelFn onContentWheel = nullptr;
        void* onContentWheelUser = nullptr;
};

// 把客户区坐标换算成内容区坐标。
// 在客户区之外时返回 false（此时 out 无意义）。
bool ClientToContent(HWND hwnd, POINT clientPt, POINT& out);

// 内容区的**屏幕矩形**（不含阴影与标题栏）。
// 反投影用：窗口被移动 / 缩放之后，拿它把控件矩形对回屏幕。
RECT ContentRectOf(HWND hwnd);

// 创建窗口。成功返回句柄，失败返回 nullptr。
// 窗口带打开动画（缩放 + 淡入）。
HWND Create(HINSTANCE hInst, const Options& opt, PaintFn paint, void* user);

// ---- 外观资源（启动时调一次即可，之后的窗口都会带上）----

// 用**exe 自己的图标资源**当窗口图标。
// 图标早就编在 exe 里了（Ransom_dev.rc 的 `1 ICON`），所以单文件分发
// 也不需要外部 .ico。大图标给任务栏，小图标给标题栏。
void SetAppIconFromSelf();

// 从磁盘上的 .ico 注册窗口图标。空串 / nullptr = 不设。
// 只在想临时换图标时用；正常路径是上面那个。
void SetAppIcon(const wchar_t* icoPath);

// 从**内存**注册字体（素材内嵌，磁盘上没有 .ttf 文件了）。
// 内存必须一直有效（GDI+ 的 PrivateFontCollection 不拷贝），
// 所以内部会把字节留一份。
bool SetTitleFontFromMemory(const unsigned char* data, size_t size,
                            const wchar_t* family);

// 从磁盘上的字体文件注册。空串 = 恢复默认。
bool SetTitleFont(const wchar_t* fontFile, const wchar_t* family);

// 已注册 UI 字体的 FontFamily，没注册成功返回 nullptr。
// 弹窗模块要复用它——**不能按名字构造 Font**：FR_PRIVATE 注册的字体
// GDI+ 按名字查不到，会静默回退（踩过这个坑）。
Gdiplus::FontFamily* UiFontFamily();

// ---- 原地抖动 ----
// 轻微抖动 + 旋转（绕窗口中心）。振幅刻意压在「角点位移 < 3px」以内，
// 靠窗口自带的 14px 透明阴影边距吃掉位移，所以**不动窗口矩形**，
// 不影响鼠标命中与拖拽。静态时生效，动画播放期间自动让位。
void SetWobble(HWND hwnd, bool on);

// ---- 平滑换位置 ----
// 沿一条弧线平滑移动到新的左上角位置（**含阴影的屏幕坐标**，
// 和 RectOf() 同一套）。两端精确落位，中间走弧。
// w / h 传 0 表示保持当前尺寸；传具体值则移动过程中**同时改大小**。
// 已经有动画在跑、或窗口已最大化/最小化时忽略本次调用。
void AnimateMoveTo(HWND hwnd, int x, int y, DWORD ms, int w = 0, int h = 0);

// 分帧 + 过冲版本（付钱窗口飞向屏幕正中用的就是这个）。
//   steps     > 0：整段动画只走 steps 个离散位置，窗口一格一格地跳
//   overshoot    ：缓动换成 back-out，冲过目标约 12% 再弹回来
//   arc          ：是否走弧线；过冲时建议 false，两者叠加会把落点甩歪
// 时长和 AnimateMoveTo 是同一套语义（ms 是**整段**时长，不是每帧间隔）。
void AnimateMoveToStepped(HWND hwnd, int x, int y, DWORD ms, int w, int h,
                          int steps, bool overshoot = true, bool arc = false);

// 原地短促抖动：整扇窗绕当前位置做一次快速衰减的位移振荡。
// 用来给「重击 / 出场」配一下撞击感——和 SetWobble 的绘制层微抖不同，
// 这里走的是**真实的窗口位移**（SetWindowPos），幅度可以到几 px，
// 抖完在最后一帧精确回到原位（偏移量按 (1-t)^2 收敛到 0）。
//
// 与其它动画互斥：如果此刻有动画在跑（比如换位置 / 缩放），
// 本次调用会**排队**，等那段动画干干净净收完再抖——不会中途掐掉它，
// 免得带过冲的 move 停在半路、抖完就偏几像素回不来。
void AnimateJolt(HWND hwnd, float amplitudePx, DWORD ms);

// 立刻销毁（收尾用，不做动画）。
void Destroy(HWND hwnd);

// 播放关闭动画后再自行销毁（子窗口到寿命、点关闭按钮时用）。
void AnimateClose(HWND hwnd);

// 播放最大化 / 还原 / 最小化动画。
void AnimateMaximize(HWND hwnd);
void AnimateRestore(HWND hwnd);
void AnimateMinimize(HWND hwnd);

bool IsAlive(HWND hwnd);

// 四周阴影的厚度（px）。窗口外形尺寸 = 内容尺寸 + 2*它。
// 需要按「内容尺寸」反算窗口外形尺寸时用得上（比如付钱演出要把窗口
// 缩放到某个排版的内容区大小）。
int ShadowSize();

// 想让 paint 回调拿到的内容区正好是 cw x ch，Options 里该填多少？
// （Options 的 height **含标题栏**，width/height 还要各自扣 1px 边框；
//   直接填内容尺寸会得到一张纵向被压扁的画面。）
int OptionsWidthForContent(int cw);
int OptionsHeightForContent(int ch);

// 窗口当前屏幕矩形（含阴影）。
RECT RectOf(HWND hwnd);

// 请求重绘（内容变化时调）。
void Repaint(HWND hwnd);

// 当前存活的窗口数（调试用）。
int AliveCount();

// ============================================================================
//  aero::ui —— 自绘控件库
// ============================================================================
//
//  为什么在这儿：aero 的窗口是**分层窗口**，放不了 Win32 子控件（子控件画不出来），
//  所以设置界面那套（按钮 / 滑条 / 复选框 / 下拉框 / 侧边滚动条 / 盘符格）全是自绘的。
//  自绘代码一度全堆在 setup_ui.cpp 里 —— 那样 aero_window.cpp 只是个空壳，
//  别的窗口想用同一个按钮就得抄一遍。现在把它们连**命中判定、拖拽与滚轮的
//  数学、缓动动画、配色**一起搬到这里，setup_ui 只留"数据 + 布局 + 业务回调"。
//
//  约定（很重要，别改坏）：
//    * 所有控件的坐标都是**内容区坐标**（左上角 = (0,0)，和 PaintFn 的 content
//      同一个原点）。绘制时把 origin 传进来做平移，命中判定直接用裸坐标。
//    * 控件不持有窗口、不管消息循环、不读设置 —— 状态是纯数据，逻辑是纯函数。
//    * 值 <-> 像素、吸附、拖拽换算这些数学在控件里；"这个值代表什么"在调用方。
namespace ui {

// ---------------------------------------------------------------- 缓动 ----
// 每个可动的"显示值"挂一份：目标变了就快照当前值当起点，按时间插值到目标。
const DWORD kEaseDragMs = 80;    // 拖拽：珠子"追"鼠标，短缓动
const DWORD kEaseMs = 180;       // 滚轮 / 恢复默认 / 切模式：滑过去

struct Tween {
    double from = 0.0;
    double to = 0.0;
    DWORD  startMs = 0;
    DWORD  durMs = kEaseMs;

    // snap = true 直接把起点终点都设成目标（几乎瞬移，只用于初始化）。
    // dur = 0 用默认时长（kEaseMs）。
    void Set(double v, bool snap, DWORD dur = 0);
    double Value() const;
};

// ---------------------------------------------------------------- 主题 ----
// 全部颜色集中在这儿（原来是 setup_ui 顶部那张配色表）。
// 方案 v3：中性灰 + 纯红。
struct Theme {
    Gdiplus::Color panelBg, panelEdge;                        // 底板
    Gdiplus::Color textMain, textHint, textDim, textFaint;     // 文字四级
    Gdiplus::Color accent, accentSoft;                         // 强调（纯红）
    Gdiplus::Color trackBg, trackFill, knob, knobActive;       // 滑条
    Gdiplus::Color trackBgOff, trackFillOff, knobOff;          // 禁用态
    Gdiplus::Color btnPrimary, btnPrimaryHot, btnPrimaryEdge;
    Gdiplus::Color btnSecondary, btnSecondaryHot;
    Gdiplus::Color btnSecondaryEdge, btnSecondaryEdgeHot;
    Gdiplus::Color checkEdge, checkEdgeHot;                    // 复选框
    Gdiplus::Color scrollTrack, scrollThumb, scrollThumbActive; // 滚动条
    Gdiplus::Color mixRed, mixGreen;                           // 假币配比条
};

const Theme& DefaultTheme();

// ------------------------------------------------------- 小工具与文本 ----
float Clamp01(float v);
Gdiplus::Color LerpColor(const Gdiplus::Color& a, const Gdiplus::Color& b, float t);
// 按进度淡色（t = 1 原样，t = 0 全透明）—— 切模式时整行淡出用的就是它
Gdiplus::Color Fade(const Gdiplus::Color& c, float t);

void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
    const Gdiplus::Color& c);
void StrokeRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
    const Gdiplus::Color& c, float w);
void DrawPanel(Gdiplus::Graphics& g, const Gdiplus::RectF& rc);

bool Hit(const RECT& r, const POINT& p);
void SetRectLocal(RECT& r, int x, int y, int w, int h);

void DrawTextCjk(Gdiplus::Graphics& g, const wchar_t* s, const Gdiplus::RectF& rc,
    float px, const Gdiplus::Color& c,
    Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear,
    int style = Gdiplus::FontStyleRegular);
void DrawTextMono(Gdiplus::Graphics& g, const wchar_t* s, const Gdiplus::RectF& rc,
    float px, const Gdiplus::Color& c,
    Gdiplus::StringAlignment align = Gdiplus::StringAlignmentNear,
    int style = Gdiplus::FontStyleRegular);

// ------------------------------------------------------------- 按钮 ----
struct Button {
    RECT rc = {};                    // 内容区坐标
    const wchar_t* label = L"";
    bool  primary = false;           // 主按钮（红底）
    bool  enabled = true;
    bool  hot = false;
    float alpha = 1.0f;

    bool Hit(POINT p) const;
    void Draw(Gdiplus::Graphics& g, const Gdiplus::RectF& origin) const;
};

// ----------------------------------------------------------- 复选框 ----
struct Checkbox {
    RECT  rc = {};
    bool  hot = false;
    float alpha = 1.0f;

    bool Hit(POINT p) const;
    // progress = 勾选进度（0..1），对勾从中心"长"出来
    void Draw(Gdiplus::Graphics& g, const Gdiplus::RectF& origin, float progress) const;
};

// ------------------------------------------------------------- 滑条 ----
//
// 单珠和双珠共用一套：knobCount = 1 时只有左珠。
// 值 <-> 像素、吸附、选珠全在里面；拖拽只是"每帧拿鼠标 x 调 FromX"。
struct Slider {
    RECT  track = {};                // 轨道矩形（内容区坐标）
    int   knobCount = 1;
    int   lo = 0;
    int   hi = 100;
    int   snapStep = 1;              // 吸附粒度（1 = 整数，10 = 十位，100 = 百位）
    bool  enabled = true;
    float alpha = 1.0f;
    int   hotKnob = -1;              // 正在拖/悬停的珠子下标（-1 = 没有）

    int  FromX(int x) const;         // 像素 -> 值（夹 + 吸附）
    int  ToX(int v) const;           // 值 -> 像素
    // 两颗珠子选哪颗：重合（或几乎重合）时一律左珠
    static int NearestKnob(int x1, int x2, int x);
    // 轨道命中（带容差，方便点偏一点也能拖）
    bool HitTrack(POINT p, int padY = 8, int padX = 10) const;
    // x1/x2 是两颗珠子（单珠时忽略 x2）；fillL/fillR 是已选段
    void Draw(Gdiplus::Graphics& g, const Gdiplus::RectF& origin,
        int x1, int x2, int fillL, int fillR) const;
};

// ----------------------------------------------------------- 下拉框 ----
//
// 自绘：闭合态一个胶囊（当前项 + 箭头），展开时列表**盖在下面的内容上**。
// 展开进度走 Tween，所以展开/收起都有动画。**收起时按进度判断要不要画**，
// 别按目标状态 —— 踩过：目标一翻 false 列表当场消失，收起就没动画了。
struct Combo {
    RECT   rc = {};                  // 闭合态（内容区坐标）
    int    itemH = 30;
    int    count = 0;
    int    current = 0;
    const wchar_t* const* names = nullptr;   // count 项
    const wchar_t* const* descs = nullptr;   // 可以为 nullptr
    bool   open = false;             // 目标状态
    Tween  anim;                     // 展开进度 0..1
    int    hotItem = -1;
    float  alpha = 1.0f;

    bool  Opened()  const { return open; }
    bool  Visible() const { return anim.Value() > 0.02; }
    float P() const { return (float)anim.Value(); }

    void Open()  { open = true;  anim.Set(1.0, false); hotItem = -1; }
    void Close() { open = false; anim.Set(0.0, false); hotItem = -1; }
    void Sync(bool snap);            // 进页面时把动画直接摆到目标

    RECT ItemRect(int i) const;
    bool HitBox(POINT p) const;
    int  HitItem(POINT p) const;     // -1 = 没点中任何一项

    // 闭合那一条（含左边的标签和右上角的说明文字）
    void DrawBox(Gdiplus::Graphics& g, const Gdiplus::RectF& origin,
        const wchar_t* label) const;
    // 展开的列表：画在所有内容之后（要盖住下面的行）
    void DrawList(Gdiplus::Graphics& g, const Gdiplus::RectF& origin) const;
};

// --------------------------------------------------------- 侧边滚动条 ----
struct ScrollBar {
    RECT rc = {};                    // 轨道（内容区坐标）
    int  contentH = 0;               // 内容总高
    int  viewH = 0;                  // 视口高
    int  maxOffset = 0;              // 最大滚动量
    int  offset = 0;                 // 当前滚动量
    int  grab = 0;                   // 按下时鼠标在滑块内的偏移
    bool hot = false;

    RECT ThumbRect() const;
    bool HitTrack(POINT p) const;
    bool HitThumb(POINT p) const;
    // 拖动：由鼠标 y 反算新的 offset（已夹）
    int  OffsetFromDrag(POINT p) const;
    // 点轨道空白：翻到大致位置
    int  OffsetFromClick(POINT p) const;
    // 滚轮一格：返回新的 offset
    int  OffsetFromWheel(int delta, int stepPx) const;

    void Draw(Gdiplus::Graphics& g, const Gdiplus::RectF& origin) const;
};

// ---------------------------------------------------------- 2D 盘符格 ----
// 盘符页那种"图标 + 主标题 + 副标题 + 选中态"的方块。
struct Tile {
    RECT  rc = {};
    const wchar_t* title = L"";      // 盘符
    const wchar_t* sub = L"";        // 已选 / 不选
    Gdiplus::Bitmap* icon = nullptr; // 可以为空
    bool  hot = false;
    float alpha = 1.0f;
    float progress = 0.0f;           // 选中进度（0..1，走 Tween）

    bool Hit(POINT p) const;
    void Draw(Gdiplus::Graphics& g, const Gdiplus::RectF& origin) const;
};

} // namespace ui

} // namespace aero
