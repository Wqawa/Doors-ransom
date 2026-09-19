


#include "popup.h"

#include "aero_window.h"
#include "assets.h"
#include "audio.h"
#include "entity_log.h"
#include "face.h"
#include "ui_layout.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

    const wchar_t* kDriverClass = L"RansomPopupDriver";
    const UINT_PTR kTickId = 1;
    const UINT     kTickMs = 100;


    const DWORD kBurstLifeMs = 12000;
    const DWORD kGapMinMs = 5000;
    const DWORD kGapMaxMs = 12000;
    const DWORD kChildLifeMin = 4000;
    const DWORD kChildLifeMax = 8000;
    const int   kMaxChildren = 14;


    const DWORD kMainMoveMinMs = 6000;
    const DWORD kMainMoveMaxMs = 13000;
    const DWORD kMainMoveDurMs = 900;







    const int   kMainMoveMargin = 0;




    const wchar_t* kUiMonoFont = L"Roboto Mono";







    const DWORD kPayTravelMs = 1000;
    const DWORD kPayCloseAtMs = 3200;





    const int kPayTravelFrames = 10;

    enum PayStage { PAY_NONE = 0, PAY_TRAVEL, PAY_SHOW, PAY_CLOSING };
    PayStage g_payStage = PAY_NONE;
    DWORD    g_payStageAt = 0;



    const DWORD kGapMinEndMs = 2500;
    const DWORD kGapMaxEndMs = 6000;


    const wchar_t* kTitles[] = {
        L"Untitled", L"Untitled (2)", L"Untitled (3)",
        L"RANSOM.exe", L"RANASOM", L"MOSNAR",
        L"I FOUND YOU", L"", L"RANSOMRANSOM", L"RRAANNSSOOMM",
        L"LACKLUSTER", L"INCOMPETENT", L"YOU ARE AN IDIOT",
        L"NONIMPRESSIVE", L"ENCRYPTED",
        L"AdWBXV Rk1PRVJNR09PUlRJVEVFU04=",
        L"times up", L"_____", L"YOUR GOLD IS VERY YUMMY!",
        L"YOURGOLDAREBELONGTOUS", L"ERROR", L"Error. Not found.",
        L"WHATWOULDSHETHINK",
    };
    const int kTitleCount = (int)(sizeof(kTitles) / sizeof(kTitles[0]));


    struct ChildData {
        int   variant = 0;

        DWORD born = 0;
        DWORD life = 0;
    };
    HWND      g_driver = nullptr;
    HINSTANCE g_hInst = nullptr;
    HWND      g_main = nullptr;
    int       g_total = 0;

    struct Child {
        HWND      hwnd = nullptr;
        bool      closing = false;
        bool      playerClosed = false;
        ChildData data;
    };


    std::vector<Child*> g_children;





    int g_playerClosedCount = 0;




    void OnChildCloseClick(HWND         , void* user)
    {
        Child* c = (Child*)user;
        if (!c) return;
        c->playerClosed = true;
        elog::Write(L"[popup] 玩家点了子窗口的关闭按钮（将扣 10 秒）");
    }


    int   g_gold = 0;
    int   g_goal = 500;
    DWORD g_remain = 0;
    DWORD g_totalMs = 0;



    double Pressure()
    {
        if (g_totalMs == 0) return 0.0;
        double p = 1.0 - (double)g_remain / (double)g_totalMs;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        return p;
    }


    DWORD NextWaveGap()
    {
        const double p = Pressure();




        const double lo = (double)kGapMinMs + ((double)kGapMinEndMs - (double)kGapMinMs) * p;
        const double hi = (double)kGapMaxMs + ((double)kGapMaxEndMs - (double)kGapMaxMs) * p;

        if (hi <= lo) return (DWORD)lo;
        return (DWORD)(lo + (double)(rand() % (int)(hi - lo)));
    }


    int NextWaveCount()
    {
        const int span = (Pressure() > 0.66) ? 3 : 2;
        return 1 + (rand() % span);
    }

    DWORD g_nextWave = 0;
    int   g_burstPending = 0;
    DWORD g_nextChildAt = 0;
    bool  g_burstDone = false;
    DWORD g_nextMainMove = 0;



    void MoveMainRandomly()
    {
        if (!g_main || !aero::IsAlive(g_main)) return;

        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

        const RECT mr = aero::RectOf(g_main);
        const int w = mr.right - mr.left;
        const int h = mr.bottom - mr.top;


        const int x0 = wa.left + kMainMoveMargin;
        const int x1 = wa.right - w - kMainMoveMargin;
        const int y0 = wa.top + kMainMoveMargin;
        const int y1 = wa.bottom - h - kMainMoveMargin;
        if (x1 < x0 || y1 < y0) return;

        const int nx = x0 + rand() % (x1 - x0 + 1);
        const int ny = y0 + rand() % (y1 - y0 + 1);

        aero::AnimateMoveTo(g_main, nx, ny, kMainMoveDurMs);
        elog::Write(L"[popup] 主窗口换位置 -> (%d,%d)，%lums 弧线动画",
            nx, ny, (unsigned long)kMainMoveDurMs);
    }



    void SprinkleNoise(Graphics& g, const RectF& rc, int count)
    {
        if (rc.Width < 4.0f || rc.Height < 4.0f) return;
        for (int i = 0; i < count; ++i)
        {
            const REAL x = rc.X + (REAL)(rand() % (int)rc.Width);
            const REAL y = rc.Y + (REAL)(rand() % (int)rc.Height);
            const BYTE v = (BYTE)(rand() & 0xFF);
            SolidBrush b(Color(190, v, v, v));
            g.FillRectangle(&b, x, y, 2.0f, 2.0f);
        }
    }

    void PaintChild(Graphics& g, const RectF& rc, DWORD frame, void* user)
    {
        const ChildData* d = (const ChildData*)user;
        if (!d) return;


        SolidBrush bg(Color(150, 10, 12, 18));
        g.FillRectangle(&bg, rc);

        const RectF inner(rc.X + 6.0f, rc.Y + 6.0f, rc.Width - 12.0f, rc.Height - 12.0f);
        if (inner.Width < 16.0f || inner.Height < 16.0f) return;



        {
            HDC hdc = g.GetHDC();
            RECT r;
            r.left = (LONG)inner.X;
            r.top = (LONG)inner.Y;
            r.right = (LONG)(inner.X + inner.Width);
            r.bottom = (LONG)(inner.Y + inner.Height);


            const int n = face::PopupImageCount();
            bool drew = (n > 0) && face::BlitPopupImage(hdc, r, d->variant % n);


            if (!drew)
            {
                if (d->variant % 3 == 0) face::BlitFace(hdc, r, false, frame);
                else if (d->variant % 3 == 1) face::BlitFace(hdc, r, true, frame);
                else                          face::BlitCrucified(hdc, r, frame);
            }

            g.ReleaseHDC(hdc);
        }



        SprinkleNoise(g, inner, 70);
    }

    void PaintMain(Graphics& g, const RectF& rc, DWORD          , void*         )
    {



        ui_layout::Status st;
        st.gold = g_gold;
        st.goal = (g_goal > 0) ? g_goal : 1;
        st.remainMs = g_remain;

        ui_layout::Render(g, rc, st);
    }



    void SpawnChild(DWORD lifeMs)
    {
        if ((int)g_children.size() >= kMaxChildren) return;

        Child* c = new Child();
        c->data.variant = rand() % 5;
        c->data.born = GetTickCount();
        c->data.life = lifeMs;




        const int w = 170 + rand() % 250;
        const int h = 140 + rand() % 190;

        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int aw = wa.right - wa.left;
        const int ah = wa.bottom - wa.top;

        aero::Options opt;
        opt.title = kTitles[rand() % kTitleCount];
        opt.width = w;
        opt.height = h;
        opt.x = wa.left + rand() % (aw > w + 28 ? aw - w - 28 : 1);
        opt.y = wa.top + rand() % (ah > h + 28 ? ah - h - 28 : 1);
        opt.buttons = true;
        opt.topmost = true;
        opt.resizable = false;
        opt.animate = true;




        opt.onUserClose = &OnChildCloseClick;
        opt.onUserCloseUser = c;



        opt.tickMs = 130;

        c->hwnd = aero::Create(g_hInst, opt, PaintChild, &c->data);
        if (!c->hwnd) { delete c; return; }

        aero::SetWobble(c->hwnd, true);

        elog::Write(L"[popup] 子窗口 %dx%d（比例 %.2f）图=RansomPopup%d",
            w, h, (double)w / (double)h, c->data.variant + 1);

        g_children.push_back(c);
        ++g_total;
    }



    void CloseAllChildren(bool animate)
    {
        if (g_children.empty()) return;

        if (animate)
        {
            int n = 0;
            for (size_t i = 0; i < g_children.size(); ++i)
            {
                Child* c = g_children[i];
                if (c->closing) continue;
                c->closing = true;
                c->data.life = 0;
                aero::AnimateClose(c->hwnd);
                ++n;
            }
            if (n > 0) elog::Write(L"[popup] %d 个子窗口开始淡出", n);
            return;
        }

        std::vector<Child*> old = g_children;
        g_children.clear();

        for (size_t i = 0; i < old.size(); ++i)
        {
            if (old[i]->playerClosed)
                ++g_playerClosedCount;
            aero::Destroy(old[i]->hwnd);
            delete old[i];
        }
        elog::Write(L"[popup] 立即关闭 %d 个子窗口", (int)old.size());
    }

    void ReapDeadChildren()
    {
        for (size_t i = 0; i < g_children.size(); )
        {
            if (!aero::IsAlive(g_children[i]->hwnd))
            {


                if (g_children[i]->playerClosed)
                    ++g_playerClosedCount;

                delete g_children[i];
                g_children.erase(g_children.begin() + i);
                continue;
            }
            ++i;
        }
    }

    void Tick()
    {
        const DWORD now = GetTickCount();



        if (g_payStage == PAY_TRAVEL && now - g_payStageAt >= kPayTravelMs)
        {

            ui_layout::SetActive(1);
            audio::PlaySuccess();






            if (aero::IsAlive(g_main))
                aero::AnimateJolt(g_main, 9.0f, 380);

            g_payStage = PAY_SHOW;
            g_payStageAt = now;
            elog::Write(L"[popup] 付钱演出：切到付钱排版，Accepta90 / Thankyou_sign 依次出场（窗口抖动）");
        }
        else if (g_payStage == PAY_SHOW && now - g_payStageAt >= kPayCloseAtMs)
        {

            HWND w = g_main;
            g_main = nullptr;
            aero::AnimateClose(w);

            g_payStage = PAY_CLOSING;
            g_payStageAt = now;
            elog::Write(L"[popup] 付钱演出：播放关闭动画收场");
        }




        if (g_payStage != PAY_NONE)
        {
            ReapDeadChildren();
            return;
        }

        if (!g_main)
        {

            ReapDeadChildren();
            return;
        }

        ReapDeadChildren();


        SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);


        if (now >= g_nextMainMove)
        {
            g_nextMainMove = now + kMainMoveMinMs +
                (DWORD)(rand() % (kMainMoveMaxMs - kMainMoveMinMs));
            MoveMainRandomly();
        }


        if (!g_burstDone && g_burstPending > 0 && now >= g_nextChildAt)
        {
            SpawnChild(0);
            --g_burstPending;
            g_nextChildAt = now + 70;
        }


        if (!g_burstDone && now >= g_nextWave)
        {
            CloseAllChildren(true);
            g_burstDone = true;
            g_nextWave = now + NextWaveGap();
            elog::Write(L"[popup] 开局批开始关闭，%.1f 秒后开始零星冒窗口",
                (g_nextWave - now) / 1000.0);
            return;
        }


        if (g_burstDone && now >= g_nextWave)
        {
            const int n = NextWaveCount();
            for (int i = 0; i < n; ++i)
            {
                const DWORD life = kChildLifeMin + (DWORD)(rand() % (kChildLifeMax - kChildLifeMin));
                SpawnChild(life);
            }
            elog::Write(L"[popup] 零星冒出 %d 个子窗口（存活 %lu-%lums）",
                n, (unsigned long)kChildLifeMin, (unsigned long)kChildLifeMax);

            g_nextWave = now + NextWaveGap();
        }


        for (size_t i = 0; i < g_children.size(); ++i)
        {
            Child* c = g_children[i];
            if (c->closing) continue;
            if (aero::IsAlive(c->hwnd) && c->data.life > 0 &&
                now - c->data.born >= c->data.life)
            {
                c->closing = true;
                c->data.life = 0;
                aero::AnimateClose(c->hwnd);
            }
        }
    }

    LRESULT CALLBACK DriverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_TIMER:
            if (wp == kTickId) { Tick(); return 0; }
            break;
        case WM_ERASEBKGND: return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

}

namespace popup {

    bool Start(HINSTANCE hInst)
    {
        if (g_driver) return true;
        g_hInst = hInst;




        ui_layout::Load(L"main_window.ini", L"payup.ini");

        WNDCLASSEXW dc = { sizeof(WNDCLASSEXW) };
        dc.lpfnWndProc = DriverProc;
        dc.hInstance = hInst;
        dc.hCursor = nullptr;
        dc.hbrBackground = nullptr;
        dc.lpszClassName = kDriverClass;
        RegisterClassExW(&dc);

        g_driver = CreateWindowExW(0, kDriverClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, hInst, nullptr);
        if (!g_driver)
        {
            elog::Write(L"[popup] 驱动窗口创建失败, err=%lu", GetLastError());
            return false;
        }

        SetTimer(g_driver, kTickId, kTickMs, nullptr);
        elog::Write(L"[popup] 已就绪（%d 个标题，子窗口上限 %d）", kTitleCount, kMaxChildren);
        return true;
    }

    void Stop()
    {
        CloseAllChildren(false);
        if (g_main) { aero::Destroy(g_main); g_main = nullptr; }

        if (g_driver)
        {
            KillTimer(g_driver, kTickId);
            DestroyWindow(g_driver);
            g_driver = nullptr;
        }
        elog::Write(L"[popup] 已停止");
    }

    void BeginRansom(int childBurst)
    {
        if (!g_driver) return;

        EndRansom();





        ui_layout::SetActive(0);



        aero::Options opt;
        opt.title = L"RANSOM.exe";



        opt.width = aero::OptionsWidthForContent(ui_layout::Width());
        opt.height = aero::OptionsHeightForContent(ui_layout::Height());
        opt.buttons = false;
        opt.topmost = true;

        g_main = aero::Create(g_hInst, opt, PaintMain, nullptr);
        if (!g_main) elog::Write(L"[popup] 主窗口创建失败");
        else         aero::SetWobble(g_main, true);


        if (childBurst < 1)            childBurst = 1;
        if (childBurst > kMaxChildren) childBurst = kMaxChildren;

        g_total = 0;
        g_children.reserve(kMaxChildren);




        g_burstPending = childBurst;
        g_nextChildAt = GetTickCount();

        g_burstDone = false;
        g_nextWave = GetTickCount() + kBurstLifeMs;


        g_nextMainMove = GetTickCount() + kBurstLifeMs + 2000;

        elog::Write(L"[popup] 勒索开始：主窗口=%s，开局批 %d 个错落放出（%lu 秒后统一关闭）",
            g_main ? L"成功" : L"失败", childBurst,
            (unsigned long)(kBurstLifeMs / 1000));
    }

    void EndRansom()
    {
        CloseAllChildren(false);
        if (g_main)
        {

            if (g_payStage != PAY_CLOSING) aero::Destroy(g_main);
            g_main = nullptr;
        }
        g_payStage = PAY_NONE;
        g_burstDone = false;
        g_nextWave = 0;


        g_remain = 0;
        g_totalMs = 0;
    }


    void BeginPayup()
    {
        if (!g_main || !aero::IsAlive(g_main)) return;



        if (!ui_layout::PayupReady())
        {
            elog::Write(L"[popup] 付钱排版不可用，跳过付钱演出");
            return;
        }

        CloseAllChildren(false);
        g_burstDone = true;
        g_nextWave = 0;




        aero::SetWobble(g_main, false);


        ui_layout::SetTravel(true);
        aero::Repaint(g_main);




        const int sw = aero::ShadowSize() * 2;
        const int w = aero::OptionsWidthForContent(ui_layout::PayupWidth()) + sw;
        const int h = aero::OptionsHeightForContent(ui_layout::PayupHeight()) + sw;

        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int x = wa.left + ((wa.right - wa.left) - w) / 2;
        const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;


        aero::AnimateMoveToStepped(g_main, x, y, kPayTravelMs, w, h,
            kPayTravelFrames, true, false);

        g_payStage = PAY_TRAVEL;
        g_payStageAt = GetTickCount();

        elog::Write(L"[popup] 付钱演出开始：飞向屏幕中心 (%d,%d) 并缩放到 %dx%d"
            L"（%lums / %d 帧 / 过冲）",
            x, y, w, h, (unsigned long)kPayTravelMs, kPayTravelFrames);
    }

    void SetStatus(int gold, int goal, DWORD remainMs, DWORD totalMs)
    {
        g_gold = gold;
        g_goal = (goal > 0) ? goal : 1;
        g_remain = remainMs;
        g_totalMs = totalMs;
    }
    bool MainAlive() { return aero::IsAlive(g_main); }
    int  Children() { return (int)g_children.size(); }
    int  TotalSpawned() { return g_total; }

    int ConsumePlayerClosedCount()
    {
        const int n = g_playerClosedCount;
        g_playerClosedCount = 0;
        return n;
    }
}