













#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace popup {

bool Start(HINSTANCE hInst);
void Stop();


void BeginRansom(int childBurst);






void BeginPayup();


void EndRansom();




void SetStatus(int gold, int goal, DWORD remainMs, DWORD totalMs);

bool MainAlive();
int  Children();



int ConsumePlayerClosedCount();
int  TotalSpawned();

}
