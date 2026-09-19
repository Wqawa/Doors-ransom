


















#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>


#include "settings.h"

namespace setup_ui {









	typedef void (*HotkeyFn)(void* user);

	void SetHotkeyHandler(HotkeyFn fn, void* user);
	void ClearHotkeyHandler();



	bool DispatchHotkey();

	enum Verdict {
		VERDICT_START = 0,
		VERDICT_ABORT,
		VERDICT_ERROR,
	};








	Verdict ShowSettings(HINSTANCE hInst, int panicVk);



	Verdict ShowSafetyNotice(HINSTANCE hInst, int panicVk);









	bool DumpSettingsPreview(const wchar_t* path, const settings::Set& s, bool grid);
	bool DumpNoticePreview(const wchar_t* path, bool grid);

}
