






#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace overlay {




enum Look {
    LOOK_FRAME = 0,
    LOOK_LOCKED,
};

struct Style {
    BYTE     alpha     = 220;
    COLORREF color     = RGB(0x2E, 0xB8, 0xFF);
    float    thickness = 2.0f;
    float    radius    = 10.0f;
    float    pad       = 4.0f;
    bool     glow      = true;
    int      boxMode   = 2;
    int      interval  = 300;
    Look     look      = LOOK_FRAME;
};





bool Start(HINSTANCE hInst, bool topmost);


void Stop();


void SetStyle(const Style& s);
Style GetStyle();




void SetLook(Look l);
Look GetLook();





void SetVeil(BYTE alpha, COLORREF color = RGB(200, 20, 20));
BYTE VeilAlpha();





void SetJitter(int amplitudePx);
int  Jitter();


void SetOffset(int dx, int dy);
void GetOffset(int* dx, int* dy);















void BeginPaidClear();

























void SetBlockContextMenu(bool on);
bool BlockContextMenu();


int  BlockedRightClicks();
int  BlockedClicks();
int  BlockedDrags();


void SetVisible(bool visible);
bool Visible();


void Refresh(bool force);


HWND Handle();


int ShortcutCount();



void SetLogFile(const wchar_t* path);

}
