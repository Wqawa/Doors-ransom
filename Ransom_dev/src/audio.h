






















#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace audio {




bool Start();
void Stop();
bool Active();


int  LoadedClips();
int  TotalClips();













void SetMaster(int level);
int  Master();
void SetBgmLevel(int percent);
int  BgmLevel();
void SetSfxLevel(int percent);
int  SfxLevel();


void SetTheme(bool on);
void SetThemeLevel(int percent);
void SetGlitchBed(int level);


void PlayGlitch();
void PlayCaught();
void PlayHit();
void PlayRiser();
void PlayError();
void PlayCoin();
void PlaySuccess();


void Silence();






void PreviewBgm(DWORD previewMs = 1200);




void PreviewSfx();












void SeekThemeBy(double seconds);



bool DumpMix(const wchar_t* path, int seconds);





bool DumpTheme(const wchar_t* path);

}
