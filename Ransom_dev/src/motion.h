// ============================================================================
//  motion.h
//
//  移动检测：Ransom 的核心机制是「它出现时你必须停下不动」。
//
//  本模块负责判定「动了没有」：
//    * 鼠标 —— 与锚点的偏移超过容差
//    * 键盘 —— 任何一个按键按下
//
//  ⚠ 安全阀豁免（这是本模块最要紧的一条）：
//    强制退出是 Ctrl+Alt+Shift+Q，纯键盘操作。如果「按键=移动」，玩家按
//    Ctrl 的那一下就立刻被抓，安全阀永远按不完。
//    所以：
//      1. 修饰键（Ctrl / Alt / Shift / Win）本身不计入移动；
//      2. 当安全阀要求的修饰键全部按住时，安全阀主键也不计入。
//    换句话说，逃生组合键在任何时候都能按出来。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace motion {

// panicVK / needCtrl / needAlt / needShift 描述安全阀组合键，用于豁免。
bool Start(HINSTANCE hInst, UINT panicVK,
           bool needCtrl, bool needAlt, bool needShift);
void Stop();

// 开始监视：把当前鼠标位置设为锚点，清空「动过」标记。
void Arm();

// 停止监视（不再累积新的移动）。
void Disarm();

bool Armed();

// 自从 Arm() 以来有没有动过。
bool Moved();

// 是鼠标动的还是键盘动的（调试/日志用）。
bool MovedByMouse();
bool MovedByKeyboard();

// 鼠标相对锚点的最大偏移像素数。
int MouseDrift();

// 触发移动的那个键（0 = 不是键盘触发的）。
UINT LastKey();

// 鼠标容差（像素）。太小会被手抖误判，太大又失去意义。默认 10。
void SetTolerance(int px);
int  Tolerance();

} // namespace motion
