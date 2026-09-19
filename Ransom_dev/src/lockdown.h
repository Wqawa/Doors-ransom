





































#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lockdown {




int Start();



int Restore();


bool Active();


int LockedCount();



int RepressedCount();



void Tick();



void AddOwnClass(const wchar_t* className);



void SetBlockKeys(bool on);
bool BlockKeys();


void SetEnabled(bool on);
bool Enabled();



bool ShouldSwallowKey(DWORD vkCode);

}
