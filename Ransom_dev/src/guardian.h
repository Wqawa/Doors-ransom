

































#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace guardian {




	bool Start(HINSTANCE hInst);



	void Disarm();



	void Tick();



	void Stop();




	bool IsGuardianMode();




	bool ParseGuardianArgs(int argc, wchar_t** argv, DWORD& targetPid, int& generation);


	int RunGuardian(HINSTANCE hInst, DWORD targetPid, int generation);



	void ClearPunishMark();

}