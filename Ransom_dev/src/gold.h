


















#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace gold {


const int kGoal = 500;

bool Start(HINSTANCE hInst);


void Stop();



int Spawn(int goal);



int Consume(int token);


int SpawnedTotal();
int Alive();
int TotalCreated();


void Cleanup();


int CleanupStale();




bool IsOurCoinFile(const wchar_t* lnkPath);

}
