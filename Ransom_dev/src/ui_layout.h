// ============================================================================
//  ui_layout.h
//
//  勒索主窗口的**数据驱动**排版。
//
//  为什么要有这一层：主窗口的排版是要反复调的（图放哪、字放哪、块多大），
//  而调排版不该每次都改代码、重新编译。所以位置/尺寸/颜色全部写在
//  assets\main_window.ini 里，程序启动时读进来渲染。
//
//  配套有一个 --ui-preview 开关：把主窗口内容单独渲染成 PNG 输出，
//  不用启动整场演出就能看排版效果（还能叠一层坐标网格，方便读坐标）。
//
//  格式（宽容解析：大小写不敏感、空行与 # / ; 注释随便写、
//  认不出的键只记一行日志，不会中断）：
//
//      [window]
//      width  = 580
//      height = 340
//      bg     = 190,0,0
//
//      [element]
//      kind  = rect            ; rect | image | text
//      rect  = 20,136,540,108
//      color = 0,0,0
//      border = 255,255,255
//      borderw = 2
//
//  元素按**出现顺序**绘制，后面的压在前面上，所以底色那块写在最前面。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// GDI+ 前置依赖（WIN32_LEAN_AND_MEAN 不会带进来）
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace ui_layout {

// 主窗口上要显示的动态数值。
struct Status {
    int   gold     = 0;      // 已收集
    int   goal     = 500;    // 目标
    DWORD remainMs = 0;      // 倒计时剩余毫秒
};

// 载入排版。两个参数都是**文件名**（如 L"main_window.ini"），
// 字节从内嵌资源里取（见 assets.h）。
// main 那套读不到会用内置兜底；payup 那套可以为 nullptr，
// 读不到就标记不可用，付钱演出会被跳过，主窗口照常关掉，不会崩。
bool Load(const wchar_t* mainIniName, const wchar_t* payupIniName);

// 当前排版的内容区尺寸与底色（供 popup 决定窗口大小）。
int      Width();
int      Height();
COLORREF Background();
const wchar_t* SourcePath();      // 实际读到的文件路径（诊断用）

// ---- 付钱排版 ----
bool PayupReady();                // 付钱排版可不可用
int  PayupWidth();                // 付钱窗口的内容区尺寸（用于把窗口缩放过去）
int  PayupHeight();

// 切换当前排版（0=主 1=付钱）。切过去会**重置出场动画计时**。
// 目标排版没载入时什么都不做。
void SetActive(int which);
int  Active();

// travel 阶段：只画标记了 keep=1 的元素 + 强制黑底。
// 付完钱后「除了 A90 的头其他都删掉」用的就是这个。
void SetTravel(bool on);

// 重置出场动画计时（popin 从这一刻重新开始算）
void RestartAnim();
DWORD AnimElapsed();

// 把整窗内容画进 content 矩形（尺寸应与 Width/Height 一致）。
void Render(Gdiplus::Graphics& g, const Gdiplus::RectF& content, const Status& st);

// 调试预览：把当前排版渲染成 PNG。
// withGrid=true 时叠一层 20px 网格 + 每 100px 的坐标标注，方便读坐标。
bool Preview(const wchar_t* pngPath, const Status& st, bool withGrid);

// 释放图片缓存 / 反注册字体。退出时调。
void Shutdown();

} // namespace ui_layout
