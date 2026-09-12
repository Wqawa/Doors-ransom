// ============================================================================
//  desktop_overlay.h
//
//  桌面快捷方式覆盖层：给桌面上每个 .lnk / .url 画一个尺寸自适应、圆角、
//  透明、带外发光的方框。从原 desktop_shortcut_frame.cpp 抽出成模块，
//  由 entity_main 的消息循环统一驱动，不再自带 main() 和消息泵。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace overlay {

// 覆盖层外观
// LOOK_FRAME  = 普通标记框（冷色描边）
// LOOK_LOCKED = 已被 Ransom「加密」：红色描边 + 暗红填充 + 一把锁
enum Look {
    LOOK_FRAME = 0,
    LOOK_LOCKED,
};

struct Style {
    BYTE     alpha     = 220;                    // 轮廓不透明度 0-255
    COLORREF color     = RGB(0x2E, 0xB8, 0xFF);  // 轮廓颜色
    float    thickness = 2.0f;                   // 线宽(px)
    float    radius    = 10.0f;                  // 圆角半径(px)
    float    pad       = 4.0f;                   // 方框比图标外扩多少(px)
    bool     glow      = true;                   // 外发光
    int      boxMode   = 2;                      // 0=只框图标 1=只框文字 2=图标+文字
    int      interval  = 300;                    // 重扫间隔(ms)
    Look     look      = LOOK_FRAME;
};

// 启动覆盖层：注册窗口类、创建覆盖窗口、挂上重扫定时器。
// topmost=false 时贴在桌面层（会被普通窗口遮住），true 时浮在最上面。
// 顺带把自己和兄弟模块的窗口类名登记进 lockdown（清场时不会把自己收走），
// 并驱动 lockdown 的守望定时器。
bool Start(HINSTANCE hInst, bool topmost);

// 停止并销毁覆盖层，恢复一切被改动的东西。
void Stop();

// 外观设置（立即生效）。
void SetStyle(const Style& s);
Style GetStyle();

// ---- 外观模式 ----
// LOOK_LOCKED 把方框画成「已加密」：纯红边框 + 很淡的红底衬
// + 每框中央一个角度抖动的小停牌。
void SetLook(Look l);
Look GetLook();

// ---- 全屏红底 ----
// 锁定态下铺满整个覆盖层的红色底衬。它画在方框**之前**，
// 所以停牌标识天然压在上面，不会被盖住。
// alpha 传 0 关闭。颜色默认纯红。
void SetVeil(BYTE alpha, COLORREF color = RGB(200, 20, 20));
BYTE VeilAlpha();

// ---- 图标操控 ----

// 抖动幅度（像素）。0 = 关闭。开启后每个方框每帧随机偏移，
// 看起来像桌面图标在「躁动」。
void SetJitter(int amplitudePx);
int  Jitter();

// 全局错位：把方框整体偏移固定的量，做出「图标被拖歪」的效果。
void SetOffset(int dx, int dy);
void GetOffset(int* dx, int* dy);

// ---- 右键菜单 / 拖动拦截 ----
//
// LOOK_LOCKED（已加密）期间，对被锁图标的三类操作会被吞掉：
//
//   1. 双击打开  —— 吞掉第二次按下，系统不生成 WM_LBUTTONDBLCLK。
//   2. 右键菜单  —— 鼠标右键的按下/抬起都吞，以及菜单键、Shift+F10
//                   这两条键盘通路（它们不经过鼠标钩子）。
//   3. 拖动改位置 —— **普通左键按下直接吞掉**。图标拿不起来，就既不
//                   选中、也进不了 OLE 拖放循环，位置动不了。
//                   为什么不在「抬起」时才拦（那样还能保留单击选中）：
//                   拖放循环是本进程 UI 线程在跑，跑超过系统给的钩子
//                   超时（约 1 秒）钩子就被摘掉，拖慢一点就能绕过去。
//                   吞按下是唯一不依赖时序的时机。
//                   副作用：普通左键点一下不再选中该图标。要并选请按住
//                   Ctrl / Shift 点（那条路放行，抬起时另有兜底）。
//
// 三条都是 LOOK_LOCKED 才生效，默认开着，和画框一样属于加密效果的一部分。
// SetBlockContextMenu(false) 是给排查问题用的旁路：它关掉的是**整组输入
// 拦截**（右键菜单 + 拖动 + 吞左键），因为拦截都挂在同一个钩子上。
// 怀疑「点了没反应」是本模块造成的时候，先关掉它排除。
//
// 注意：低层钩子由系统管着，本进程主消息循环一旦卡住超过约 1 秒
// （模态弹窗、动画等待）系统会**静默摘钩**。overlay 的定时器每次
// 都会重挂，所以最长 1 个 tick（锁定态 70ms）之后自动恢复。
void SetBlockContextMenu(bool on);
bool BlockContextMenu();

// 累计拦截次数（诊断用）。
int  BlockedRightClicks();
int  BlockedClicks();       // 被吞掉的普通左键（拖动拦截就是靠它）
int  BlockedDrags();        // 兜底路径命中次数：>0 说明「吞按下」有漏，要查

// 显示 / 隐藏整个覆盖层。
void SetVisible(bool visible);
bool Visible();

// 立刻重扫一遍。force=true 时无条件重绘。
void Refresh(bool force);

// 覆盖层窗口句柄（未启动时为 nullptr）。
HWND Handle();

// 最近一次扫描命中的快捷方式数量。
int ShortcutCount();

// 诊断日志：图形界面程序没有控制台，排查时把过程写到文件。
// 传 nullptr 或空串关闭。路径为空时不写。
void SetLogFile(const wchar_t* path);

} // namespace overlay
