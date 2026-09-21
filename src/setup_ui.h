// ============================================================================
//  setup_ui.h
//
//  开场那两个窗口。都建在**项目自带的透明窗模块**上（aero_window）：
//
//    1. 设置窗口（ShowSettings）
//       背景音乐 / 音效两条滑条（最大 200%）、光敏安全模式（「癫痫模式」）
//       勾选框、两次遭遇战之间潜伏时长的随机区间（双滑块）、
//       底部「恢复默认」与「开始」。
//
//    2. 应急提示窗口（ShowSafetyNotice）
//       设置确认之后、演出开始之前弹一次：把「这个程序会做什么」和
//       立刻退出的快捷键（Ctrl+Alt+Shift+Q）讲清楚。
//       程序不会主动动用户的东西，但**要动之前必须让人知道怎么喊停**。
//
//  两个窗口都在主线程上以**阻塞式**方式跑（内部自带一个消息循环），
//  因为这时候还没启动 director / overlay / guardian——演出必须等到
//  设置确定之后才能开始。返回值决定要不要继续。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// DumpSettingsPreview 要收一个 settings::Set
#include "settings.h"

namespace setup_ui {

	// ---- 安全阀热键的转接 ----
	//
	// 开场这两屏的模态循环里，WM_HOTKEY 会被 DispatchMessage 投给
	// **注册它的那个窗口**（entity_main 里隐藏的 IPC 窗口），而不是这两屏
	// 自己。所以 IPC 窗口收到热键之后要转一手，才能让「按这个键就停」
	// 在演出开始之前也是真的。
	//
	// 回调在主线程（模态循环）里被调用，实现者负责把它变成「中止」。
	typedef void (*HotkeyFn)(void* user);

	void SetHotkeyHandler(HotkeyFn fn, void* user);
	void ClearHotkeyHandler();

	// 供 entity_main 里的 IPC 窗口调用：有设置界面开着就转给它。
	// 返回 true 表示这次热键已经被接管（调用方不必再处理）。
	bool DispatchHotkey();

	enum Verdict {
		VERDICT_START = 0,   // 用户点了「开始」
		VERDICT_ABORT,       // 用户关掉了窗口 —— 整个程序退出，不演出
		VERDICT_ERROR,       // 窗口建不出来（极罕见）—— 调用方应该直接开始演出
	};

	// 弹出设置窗口，阻塞到用户点「开始」或关掉它。
	//
	// 期间会**实时**把改动推给 audio / fx（拖滑条能听到音量变化），
	// 点「开始」时才落盘（settings::Save）。
	//
	// hPanicVk 是安全阀热键的虚拟键码，设置窗口里按它等同于点关闭：
	// 这一步用户看到的就是"想停就按这个"，那就得真的有效。
	Verdict ShowSettings(HINSTANCE hInst, int panicVk);

	// 应急提示窗口：把后果和退出快捷键讲一遍，等用户点「我知道了」。
	// 返回 VERDICT_START（继续演出）或 VERDICT_ABORT。
	Verdict ShowSafetyNotice(HINSTANCE hInst, int panicVk);

	// ---- 开发用：离线导出排版 ----
	//
	// 把两个窗口按给定设置渲染成 PNG 后退出。和 --ui-preview / --fx-demo
	// 一个路子：这两屏有一大半是手算的绝对坐标，盖在桌面上截屏会被壁纸和
	// 别的窗口污染，只有单独导出一张才能按像素核对。
	//
	// 前提：GDI+ 已初始化（调用点在 entity_main 里的 GDI+ 之后）。
	// grid = true 会叠一层 20px 网格 + 每 100px 的坐标标注。
	bool DumpSettingsPreview(const wchar_t* path, const settings::Set& s, bool grid);
	bool DumpNoticePreview(const wchar_t* path, bool grid);

} // namespace setup_ui
