




























#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>


#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace ui_layout {


struct Status {
    int   gold     = 0;
    int   goal     = 500;
    DWORD remainMs = 0;
};





bool Load(const wchar_t* mainIniName, const wchar_t* payupIniName);


int      Width();
int      Height();
COLORREF Background();
const wchar_t* SourcePath();


bool PayupReady();
int  PayupWidth();
int  PayupHeight();



void SetActive(int which);
int  Active();



void SetTravel(bool on);


void RestartAnim();
DWORD AnimElapsed();


void Render(Gdiplus::Graphics& g, const Gdiplus::RectF& content, const Status& st);



bool Preview(const wchar_t* pngPath, const Status& st, bool withGrid);


void Shutdown();

}
