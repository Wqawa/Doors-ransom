
















#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace motion {


bool Start(HINSTANCE hInst, UINT panicVK,
           bool needCtrl, bool needAlt, bool needShift);
void Stop();


void Arm();


void Disarm();

bool Armed();


bool Moved();


bool MovedByMouse();
bool MovedByKeyboard();


int MouseDrift();


UINT LastKey();


void SetTolerance(int px);
int  Tolerance();

}
