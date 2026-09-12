// ============================================================================
//  fx.h
//
//  屏幕特效：闪屏、反色、雪花噪声。
//
//   * 闪屏 / 雪花走一块全屏的分层覆盖窗口，逐像素预乘 alpha 呈现，
//     鼠标穿透，不影响正常操作。
//   * 反色用系统 Magnification API 的真实全屏色彩矩阵（就是「放大镜」那套），
//     是**真·全屏反色**而不是贴一张截图；退出时把矩阵还原成单位矩阵。
//     不支持该 API 的环境会退化为「不反色」，绝不留下改不回来的状态。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fx {

// 建立覆盖窗口与驱动。需要有消息循环的线程。
bool Start(HINSTANCE hInst);

// 停止：关掉所有特效并把反色还原。
void Stop();

// ---- 闪屏 ----
// color 闪什么颜色；times 闪几次；onMs/offMs 每次亮/灭持续多久。
void Flash(COLORREF color, int times, DWORD onMs, DWORD offMs);
void StopFlash();
bool Flashing();

// ---- 雪花噪声 ----
// level 是每像素被噪声覆盖的概率（0-255）。0 = 关闭。
void SetNoise(int level);
int  Noise();

// ---- 全屏纯色底 ----
// 完全不透明地铺满整屏（不是半透明衬底，桌面完全看不见）。
// 用于停牌显示与 jumpscare 时的纯红背景。
void SetSolid(bool on, COLORREF color = RGB(200, 0, 0));
bool Solid();

// ---- 四角红光 ----
// 「已加密」阶段的红幕：**集中在四个角**的红色大噪点像素。
// 中间完全透明，四条边的中段也不亮——只有四个角有。
//
// 不是铺满整屏的纯色底（那会把桌面盖死、玩家就找不到金币了），
// 也不是沿四边的一圈渐变（那是「四边泛光」）。
// 实现上是 4px 的大块、每块随机决定亮不亮和有多亮，所以是噪点不是光晕。
// strength 是角上的最大 alpha（0-255）。
void SetEdgeGlow(bool on, COLORREF color = RGB(210, 16, 16), int strength = 170);
bool EdgeGlow();

// ---- 反色 ----
// 成功返回 true。环境不支持时返回 false 且不会改变状态。
bool SetInvert(bool on);
bool Invert();

// 反色脉冲：开启 ms 毫秒后自动还原。用于「瞬间闪一下反色」的演出。
void InvertPulse(DWORD ms);

// 一口气关掉全部特效（退出/终局用）。
void ClearAll();

// 调外观用：把当前 fx 图层渲染一遍并存成 PNG（含 alpha，颜色已反预乘）。
// 因为 fx 是全屏置顶的分层窗口，截屏时会被底下的桌面内容干扰，
// 只有单独导出这一层才能数得清红光范围和彩色噪点。
bool DumpLayer(const wchar_t* path);
} // namespace fx
