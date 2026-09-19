


#include "director.h"

#include "desktop_overlay.h"
#include "entity_log.h"
#include "audio.h"
#include "face.h"
#include "fx.h"
#include "gold.h"
#include "lockdown.h"
#include "motion.h"
#include "popup.h"
#include "recycle.h"
#include "settings.h"

#include <cstdlib>
#include <cwchar>

namespace {

    const wchar_t* kClassName = L"RansomDirectorWnd";
    const UINT_PTR kTickId = 1;
    const UINT     kTickMs = 16;





    const DWORD kIdleMs = 4000;
    const DWORD kFaceMs = 900;


    const DWORD kCenterMs = 200;
    const DWORD kStopMs = 800;
    const DWORD kEscapedMs = 200;





    const DWORD kStopShowMs = kCenterMs + kStopMs;






    const DWORD kJumpscareMs = 1000;
    const DWORD kLoadMs = 1300;
    const DWORD kFinishMs = 200;
    const DWORD kCatchLeadMs = kJumpscareMs + kLoadMs + kFinishMs;
    const DWORD kCaughtMs = 90000;
    const DWORD kPaidMs = 4800;
    const DWORD kPunishMs = 2200;

    const DWORD kCooldownPaidMs = 11000;
    const DWORD kCooldownPunishMs = 13000;



    const DWORD kRiserLeadMs = 15000;




    const DWORD kChildCloseCreditMs = 10000;


    const COLORREF kCenterVeil = RGB(0, 0, 0);
    const int      kCenterNoise = 80;
    const COLORREF kStopVeil = RGB(170, 0, 0);
    const COLORREF kAttackVeil = RGB(80, 0, 0);


    const COLORREF kLockGlow = RGB(210, 16, 16);
    const int      kLockGlowStrength = 170;




    HWND         g_hwnd = nullptr;
    director::Phase g_phase = director::PHASE_IDLE;
    DWORD        g_phaseStart = 0;
    DWORD        g_startTick = 0;
    bool         g_auto = true;

    bool         g_caught = false;
    int          g_gold = 0;
    DWORD        g_paidAt = 0;
    DWORD        g_caughtAt = 0;
    bool         g_ransomBegun = false;
    bool         g_loadShown = false;
    bool         g_riserFired = false;
    int          g_fadeLogged = 101;




    DWORD        g_timeCreditMs = 0;

    DWORD        g_idleMs = kIdleMs;
    DWORD        g_pendingIdleMs = kIdleMs;

    DWORD RansomElapsedMs()
    {
        const DWORD begin = g_caughtAt + kCatchLeadMs;
        const DWORD now = GetTickCount();
        const DWORD el = (now <= begin) ? 0 : (now - begin);




        return el + g_timeCreditMs;
    }

    void EnterPhase(director::Phase p);
    void Tick();


    void Verdict()
    {
        const bool moved = motion::Moved();
        g_caught = moved;

        elog::Write(L"[director] 判定（停牌期间）：%s（鼠标偏移 %dpx，键动=%d，键=0x%02X）",
            moved ? L"动了 → 被抓" : L"没动 → 避开",
            motion::MouseDrift(),
            (int)motion::MovedByKeyboard(),
            motion::LastKey());

        motion::Disarm();
    }




    void EnterPhase(director::Phase p)
    {
        g_phase = p;
        g_phaseStart = GetTickCount();

        elog::Write(L"[director] === 阶段 %d 「%s」 ===", (int)p, director::PhaseName(p));

        switch (p)
        {
        case director::PHASE_IDLE:
            g_idleMs = g_pendingIdleMs;
            g_pendingIdleMs = kIdleMs;

            lockdown::Restore();
            face::Hide();
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            motion::Disarm();
            popup::EndRansom();
            overlay::SetLook(overlay::LOOK_FRAME);
            overlay::SetVisible(false);
            audio::Silence();






            settings::Apply();

            elog::Write(L"[director] 本轮潜伏 %lu ms（按设置区间随机抽）",
                (unsigned long)g_idleMs);
            break;

        case director::PHASE_FACE:

            g_caught = false;
            g_ransomBegun = false;
            g_gold = 0;
            face::SpawnAnywhere(kFaceMs + kCenterMs + kStopMs);
            audio::SetGlitchBed(6);
            break;

        case director::PHASE_CENTER:










            face::MoveToCenterWithStop(kCenterMs + kStopMs + kEscapedMs,
                kStopShowMs);
            motion::Arm();
            audio::PlayGlitch();

            fx::SetSolid(true, kCenterVeil);
            fx::SetNoise(kCenterNoise);
            break;

        case director::PHASE_STOP:




            fx::SetSolid(true, kStopVeil);
            fx::SetNoise(0);
            break;

        case director::PHASE_ESCAPED:



            lockdown::Restore();
            face::ShowHeadAgain(kEscapedMs);
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            popup::EndRansom();
            overlay::SetLook(overlay::LOOK_FRAME);
            overlay::SetVisible(false);
            audio::SetGlitchBed(0);
            audio::SetTheme(false);
            break;
        case director::PHASE_CAUGHT:



            g_gold = 0;
            g_paidAt = 0;
            g_caughtAt = GetTickCount();
            g_ransomBegun = false;
            g_loadShown = false;
            g_timeCreditMs = 0;
            face::ShowAttackStill(kJumpscareMs);
            audio::PlayCaught();
            break;

        case director::PHASE_PAID:
            lockdown::Restore();










            overlay::BeginPaidClear();

            popup::BeginPayup();
            gold::Cleanup();
            face::Hide();
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            g_paidAt = GetTickCount();
            audio::SetGlitchBed(0);
            audio::SetTheme(false);
            break;

        case director::PHASE_PUNISH:
            lockdown::Restore();
            popup::EndRansom();
            gold::Cleanup();
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            face::ShowAttackStill(kPunishMs, false); 
            audio::PlayHit();
            audio::SetGlitchBed(0);
            audio::SetTheme(false);
            recycle::SendToBin();
            break;

        default:
            break;
        }
    }

    void Tick()
    {
        const DWORD now = GetTickCount();
        const DWORD inPhase = now - g_phaseStart;




        lockdown::Tick();

        if (g_phase == director::PHASE_CAUGHT && !g_ransomBegun)
        {
            const DWORD el = now - g_caughtAt;

            if (!g_loadShown && el >= kJumpscareMs)
            {
                g_loadShown = true;
                face::ShowLoading(kLoadMs, kFinishMs);
                elog::Write(L"[director] JUMPSCARE 演完，切到加载画面（走条 %lu ms + FINISH %lu ms）",
                    (unsigned long)kLoadMs, (unsigned long)kFinishMs);
            }
        }

        if (g_phase == director::PHASE_CAUGHT && !g_ransomBegun &&
            now - g_caughtAt >= kCatchLeadMs &&
            (!g_loadShown || face::LoadingDone()))
        {
            g_ransomBegun = true;
            g_riserFired = false;
            g_fadeLogged = 101;

            face::Hide();
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(true, kLockGlow, kLockGlowStrength);

            popup::BeginRansom(8);
            overlay::SetLook(overlay::LOOK_LOCKED);
            overlay::SetVisible(true);
            gold::Spawn(settings::GoldGoal());






            lockdown::Start();

            audio::PlayError();
            audio::SetGlitchBed(22);
            audio::SetTheme(true);

            elog::Write(L"[director] 加载结束，进入勒索阶段");
        }




        if (g_phase == director::PHASE_CAUGHT && g_ransomBegun)
        {
            const int n = popup::ConsumePlayerClosedCount();
            if (n > 0)
            {
                const DWORD credit = (DWORD)n * kChildCloseCreditMs;
                g_timeCreditMs += credit;




                audio::SeekThemeBy((double)credit / 1000.0);

                elog::Write(L"[director] 玩家关闭 %d 个子窗口，倒计时提前 %d 秒（累计提前 %.1f 秒）",
                    n, n * (int)(kChildCloseCreditMs / 1000),
                    g_timeCreditMs / 1000.0);
            }
        }
        if (g_phase == director::PHASE_CAUGHT && g_ransomBegun)
            popup::SetStatus(g_gold, settings::GoldGoal(),
                director::RansomRemainMs(), kCaughtMs);

        if (g_phase == director::PHASE_CAUGHT && g_gold >= settings::GoldGoal())
        {
            EnterPhase(director::PHASE_PAID);
            return;
        }

        if (g_phase == director::PHASE_CAUGHT && g_ransomBegun)
        {
            const DWORD remain = director::RansomRemainMs();

            if (remain > 0 && remain < kRiserLeadMs)
            {
                const int level = (int)(100.0 * (double)remain / (double)kRiserLeadMs);
                audio::SetThemeLevel(level);

                if (level <= g_fadeLogged - 10)
                {
                    g_fadeLogged = level;
                    elog::Write(L"[director] 主题曲渐隐至 %d%%（剩 %.1f 秒）",
                        level, remain / 1000.0);
                }

                if (!g_riserFired)
                {
                    g_riserFired = true;
                    audio::PlayRiser();
                    elog::Write(L"[director] 倒计时剩 %.1f 秒，叠加 riser（Ransom_encounter，满音量）",
                        remain / 1000.0);
                }
            }
        }

        if (!g_auto) return;

        if (g_phase == director::PHASE_CAUGHT)
        {
            if (!g_ransomBegun) return;
            if (RansomElapsedMs() < kCaughtMs) return;
            EnterPhase(director::PHASE_PUNISH);
            return;
        }

        DWORD dur = 0;
        switch (g_phase)
        {
        case director::PHASE_IDLE:    dur = g_idleMs;   break;
        case director::PHASE_FACE:    dur = kFaceMs;    break;
        case director::PHASE_CENTER:  dur = kCenterMs;  break;
        case director::PHASE_STOP:    dur = kStopMs;    break;
        case director::PHASE_ESCAPED: dur = kEscapedMs; break;
        case director::PHASE_PAID:    dur = kPaidMs;    break;
        case director::PHASE_PUNISH:  dur = kPunishMs;  break;
        default: break;
        }

        if (dur == 0 || inPhase < dur) return;

        director::Phase next = director::PHASE_IDLE;

        switch (g_phase)
        {
        case director::PHASE_IDLE:    next = director::PHASE_FACE;    break;
        case director::PHASE_FACE:    next = director::PHASE_CENTER;  break;
        case director::PHASE_CENTER:  next = director::PHASE_STOP;    break;
        case director::PHASE_STOP:
            Verdict();
            next = g_caught ? director::PHASE_CAUGHT
                : director::PHASE_ESCAPED;
            break;
        case director::PHASE_ESCAPED: g_pendingIdleMs = (DWORD)settings::PickIdleMs(); next = director::PHASE_IDLE; break;
        case director::PHASE_PAID:    g_pendingIdleMs = kCooldownPaidMs;   next = director::PHASE_IDLE; break;
        case director::PHASE_PUNISH:  g_pendingIdleMs = kCooldownPunishMs; next = director::PHASE_IDLE; break;
        default: break;
        }

        EnterPhase(next);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_TIMER:
            if (wp == kTickId) { Tick(); return 0; }
            break;
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

}

namespace director {

    bool Start(HINSTANCE hInst)
    {
        if (g_hwnd) return true;

        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = nullptr;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        g_hwnd = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, hInst, nullptr);
        if (!g_hwnd)
        {
            elog::Write(L"[director] 调度窗口创建失败, err=%lu", GetLastError());
            return false;
        }

        g_startTick = GetTickCount();
        SetTimer(g_hwnd, kTickId, kTickMs, nullptr);

        popup::Start(hInst);
        recycle::Start(hInst);




        g_idleMs = (DWORD)settings::StartupIdleMs();
        g_pendingIdleMs = g_idleMs;

        EnterPhase(PHASE_IDLE);
        return true;
    }

    void Stop()
    {
        if (g_hwnd)
        {
            KillTimer(g_hwnd, kTickId);
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
        }

        motion::Disarm();
        face::Hide();
        fx::SetNoise(0);
        popup::Stop();
        gold::Cleanup();
        lockdown::Restore();
        audio::Silence();

        elog::Write(L"[director] 已停止");
    }

    void JumpTo(Phase p)
    {
        if ((int)p < 0 || (int)p >= (int)PHASE_COUNT) return;
        EnterPhase(p);
    }

    Phase Current() { return g_phase; }

    const wchar_t* PhaseName(Phase p)
    {
        switch (p)
        {
        case PHASE_IDLE:    return L"潜伏";
        case PHASE_FACE:    return L"任意位置浮现";
        case PHASE_CENTER:  return L"瞬移到中央";
        case PHASE_STOP:    return L"停牌判定";
        case PHASE_ESCAPED: return L"避开";
        case PHASE_CAUGHT:  return L"被抓";
        case PHASE_PAID:    return L"付清";
        case PHASE_PUNISH:  return L"惩罚";
        default:            return L"?";
        }
    }

    Phase PhaseFromName(const wchar_t* name)
    {
        if (!name || !*name) return PHASE_COUNT;
        if (iswdigit(name[0]))
        {
            const int n = _wtoi(name);
            return (n >= 0 && n < (int)PHASE_COUNT) ? (Phase)n : PHASE_COUNT;
        }
        for (int i = 0; i < (int)PHASE_COUNT; ++i)
            if (wcscmp(name, PhaseName((Phase)i)) == 0) return (Phase)i;
        return PHASE_COUNT;
    }

    void SetAutoAdvance(bool on) { g_auto = on; }
    bool AutoAdvance() { return g_auto; }

    DWORD PhaseElapsedMs() { return GetTickCount() - g_phaseStart; }
    DWORD TotalElapsedMs() { return GetTickCount() - g_startTick; }

    void CreditGold(int amount)
    {
        if (g_phase != PHASE_CAUGHT) return;
        if (amount <= 0) return;

        g_gold += amount;
        elog::Write(L"[director] 收到 %d Gold（累计 %d / %d）",
            amount, g_gold, settings::GoldGoal());
    }

    int Gold() { return g_gold; }
    int GoldGoal() { return settings::GoldGoal(); }

    DWORD RansomRemainMs()
    {
        if (g_phase != PHASE_CAUGHT || !g_ransomBegun) return 0;
        const DWORD el = RansomElapsedMs();
        return (el >= kCaughtMs) ? 0 : (kCaughtMs - el);
    }

    bool CaughtThisRound() { return g_caught; }

}