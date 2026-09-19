











#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>


#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace aero {




typedef void (*PaintFn)(Gdiplus::Graphics& g, const Gdiplus::RectF& content,
                        DWORD frame, void* user);









typedef void (*ContentMouseFn)(HWND hwnd, UINT msg, POINT pt,
                               WPARAM wp, void* user);



typedef void (*ContentWheelFn)(HWND hwnd, POINT pt, int delta, void* user);

struct Options {
        std::wstring title;
        int  width = 420;
        int  height = 260;
        int  x = CW_USEDEFAULT;
        int  y = CW_USEDEFAULT;
        bool buttons = true;
        bool topmost = true;
        bool resizable = true;
        bool animate = true;
        UINT tickMs = 33;













        void (*onUserClose)(HWND hwnd, void* user) = nullptr;
        void* onUserCloseUser = nullptr;







        ContentMouseFn onContentMouse = nullptr;
        void* onContentMouseUser = nullptr;



        ContentWheelFn onContentWheel = nullptr;
        void* onContentWheelUser = nullptr;
};



bool ClientToContent(HWND hwnd, POINT clientPt, POINT& out);



RECT ContentRectOf(HWND hwnd);



HWND Create(HINSTANCE hInst, const Options& opt, PaintFn paint, void* user);






void SetAppIconFromSelf();



void SetAppIcon(const wchar_t* icoPath);




bool SetTitleFontFromMemory(const unsigned char* data, size_t size,
                            const wchar_t* family);


bool SetTitleFont(const wchar_t* fontFile, const wchar_t* family);




Gdiplus::FontFamily* UiFontFamily();





void SetWobble(HWND hwnd, bool on);






void AnimateMoveTo(HWND hwnd, int x, int y, DWORD ms, int w = 0, int h = 0);






void AnimateMoveToStepped(HWND hwnd, int x, int y, DWORD ms, int w, int h,
                          int steps, bool overshoot = true, bool arc = false);









void AnimateJolt(HWND hwnd, float amplitudePx, DWORD ms);


void Destroy(HWND hwnd);


void AnimateClose(HWND hwnd);


void AnimateMaximize(HWND hwnd);
void AnimateRestore(HWND hwnd);
void AnimateMinimize(HWND hwnd);

bool IsAlive(HWND hwnd);




int ShadowSize();




int OptionsWidthForContent(int cw);
int OptionsHeightForContent(int ch);


RECT RectOf(HWND hwnd);


void Repaint(HWND hwnd);


int AliveCount();

}
