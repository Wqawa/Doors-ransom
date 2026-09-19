


#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace face {

    enum Mode {
        FACE_HIDDEN = 0,
        FACE_IDLE,
        FACE_STOP,
        FACE_ATTACK,
        FACE_THANKS,
        FACE_LOADING,
    };

    bool Start(HINSTANCE hInst);
    void Stop();



    void SpawnAnywhere(DWORD lifeMs);


    void MoveToCenter(DWORD lifeMs);





    void MoveToCenterWithStop(DWORD headLifeMs, DWORD stopLifeMs);


    void ShowHeadAgain(DWORD lifeMs);


    void ShowStopSign(DWORD lifeMs);








    void ShowAttackStill(DWORD lifeMs, bool smallToBig = true);


    void ShowAttackShaking(DWORD lifeMs);
    void ShowAttack(DWORD lifeMs);


    void ShowLoading(DWORD fillMs, DWORD finishMs);


    void ShowThanks(DWORD lifeMs);

    void SpawnIdle(DWORD lifeMs);

    void Hide();

    Mode Current();
    bool Visible();
    bool LoadingDone();
    RECT IdleRect();


    bool UsingAssets();
    int  PopupImageCount();
    bool BlitPopupImage(HDC hdc, const RECT& rc, int index);
    void BlitFace(HDC hdc, const RECT& rc, bool gape, DWORD frame);



    void BlitFaceAlpha(HDC hdc, const RECT& rc, bool gape, float alpha);

    void BlitCrucified(HDC hdc, const RECT& rc, DWORD frame);




    void BlitStopSign(HDC hdc, const RECT& rc, float angleDeg, float alpha = 1.0f);


    bool DumpAssets(const wchar_t* dir);

}