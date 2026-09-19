












#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace director {

enum Phase {
    PHASE_IDLE = 0,
    PHASE_FACE,
    PHASE_CENTER,
    PHASE_STOP,
    PHASE_ESCAPED,
    PHASE_CAUGHT,
    PHASE_PAID,
    PHASE_PUNISH,
    PHASE_COUNT
};

bool Start(HINSTANCE hInst);
void Stop();

void JumpTo(Phase p);
Phase Current();
const wchar_t* PhaseName(Phase p);
Phase PhaseFromName(const wchar_t* name);

void SetAutoAdvance(bool on);
bool AutoAdvance();

DWORD PhaseElapsedMs();
DWORD TotalElapsedMs();



void CreditGold(int amount);
int  Gold();
int  GoldGoal();
DWORD RansomRemainMs();


bool CaughtThisRound();

}
