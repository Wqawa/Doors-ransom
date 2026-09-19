












#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace recycle {

bool Start(HINSTANCE hInst);
void Stop();


int LockedCount();



int SendToBin();



int Restore();


int ManifestCount();

}
