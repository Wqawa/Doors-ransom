










#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fx {


bool Start(HINSTANCE hInst);


void Stop();













void SetPhotosensitiveSafe(bool on);
bool PhotosensitiveSafe();




void Flash(COLORREF color, int times, DWORD onMs, DWORD offMs);
void StopFlash();
bool Flashing();



void SetNoise(int level);
int  Noise();




void SetSolid(bool on, COLORREF color = RGB(200, 0, 0));
bool Solid();









void SetEdgeGlow(bool on, COLORREF color = RGB(210, 16, 16), int strength = 170);
bool EdgeGlow();



bool SetInvert(bool on);
bool Invert();


void InvertPulse(DWORD ms);


void ClearAll();




bool DumpLayer(const wchar_t* path);
}
