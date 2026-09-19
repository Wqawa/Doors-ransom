


#include "settings.h"

#include "audio.h"
#include "entity_log.h"
#include "fx.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>


#pragma comment(lib, "shell32.lib")

namespace {

    settings::Set g_set;


    int g_idleMs = settings::kDefaultMinMs;





    std::wstring ResolveFilePath()
    {
        wchar_t base[MAX_PATH] = {};


        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base)))
            return L"";

        std::wstring p = base;
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        p += L"Ransom_dev";
        CreateDirectoryW(p.c_str(), nullptr);
        p += L"\\settings.ini";
        return p;
    }

    int ClampInt(int v, int lo, int hi)
    {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }



    void Sanitize(settings::Set& s)
    {
        s.bgmVol    = ClampInt(s.bgmVol, settings::kVolMin, settings::kVolMax);
        s.sfxVol    = ClampInt(s.sfxVol, settings::kVolMin, settings::kVolMax);
        s.masterVol = ClampInt(s.masterVol, 0, 100);

        s.minMs = ClampInt(s.minMs, settings::kIntervalMinMs, settings::kIntervalMaxMs);
        s.maxMs = ClampInt(s.maxMs, settings::kIntervalMinMs, settings::kIntervalMaxMs);



        if (s.minMs > s.maxMs)
        {
            const int t = s.minMs;
            s.minMs = s.maxMs;
            s.maxMs = t;
        }


        s.goldGoal = ClampInt(s.goldGoal, settings::kGoldMin, settings::kGoldMax);




        if (s.coinAmounts.empty())
        {
            for (int i = 0; i < settings::kDefaultCoinAmountCount; ++i)
                s.coinAmounts.push_back(settings::kDefaultCoinAmounts[i]);
        }
        else
        {
            for (size_t i = 0; i < s.coinAmounts.size(); ++i)
            {
                int v = s.coinAmounts[i];
                if (v < 1)      v = 1;
                if (v > 100000) v = 100000;
                s.coinAmounts[i] = v;
            }
            std::sort(s.coinAmounts.begin(), s.coinAmounts.end());
            s.coinAmounts.erase(
                std::unique(s.coinAmounts.begin(), s.coinAmounts.end()),
                s.coinAmounts.end());

            if ((int)s.coinAmounts.size() > settings::kCoinAmountMax)
                s.coinAmounts.resize(settings::kCoinAmountMax);
        }
    }


    int ReadInt(const std::wstring& path, const wchar_t* sec, const wchar_t* key, int fallback)
    {
        if (path.empty()) return fallback;
        return (int)GetPrivateProfileIntW(sec, key, fallback, path.c_str());
    }






    bool WriteIni(const std::wstring& path, const settings::Set& s)
    {
        if (path.empty()) return false;


        std::wstring pool;
        for (size_t i = 0; i < s.coinAmounts.size(); ++i)
        {
            if (i) pool += L',';
            wchar_t n[16];
            swprintf_s(n, L"%d", s.coinAmounts[i]);
            pool += n;
        }

        wchar_t buf[1024];
        swprintf_s(buf,
            L"; Ransom_dev startup settings\n"
            L"; Written by the program itself; hand-editing works too (restart to apply).\n"
            L"; Delete this file to get all defaults back.\n"
            L"\n"
            L"[audio]\n"
            L"; background music volume, 0-200 (%%). 100 = original level.\n"
            L"bgm=%d\n"
            L"; sound effect volume, 0-200 (%%).\n"
            L"sfx=%d\n"
            L"; master volume ceiling, 0-100 (%%). --volume also caps this.\n"
            L"master=%d\n"
            L"\n"
            L"[show]\n"
            L"; photosensitivity-safe mode: 1 = damp full-screen brightness swings.\n"
            L"safe=%d\n"
            L"; idle time between two encounters, min-max (milliseconds).\n"
            L"; min == max means a fixed delay. Max 90000 (= 90 seconds).\n"
            L"idle_min=%d\n"
            L"idle_max=%d\n"
            L"\n"
            L"[game]\n"
            L"; ransom goal in gold, 10-1000. Affects both the win condition\n"
            L"; and how much gold gets scattered across the desktop.\n"
            L"gold_goal=%d\n"
            L"; gold face values, comma-separated. Any positive integers.\n"
            L"; Duplicates and order are normalized on load (sorted, deduped).\n"
            L"; e.g. 10,50,75,100,125,150,325,500\n"
            L"coin_amounts=%s\n",
            s.bgmVol, s.sfxVol, s.masterVol,
            s.photosensitiveSafe ? 1 : 0,
            s.minMs, s.maxMs,
            s.goldGoal,
            pool.c_str());

        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;



        const unsigned char bom[2] = { 0xFF, 0xFE };
        fwrite(bom, 1, 2, f);
        fwrite(buf, sizeof(wchar_t), wcslen(buf), f);
        fclose(f);
        return true;
    }

}

namespace settings {

    void Load()
    {
        const std::wstring path = ResolveFilePath();
        g_set = Set();

        if (path.empty())
        {
            elog::Write(L"[settings] LOCALAPPDATA unavailable, using defaults");
        }
        else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            elog::Write(L"[settings] no settings file (%s), using defaults", path.c_str());
        }
        else
        {
            g_set.bgmVol    = ReadInt(path, L"audio", L"bgm", kDefaultBgmVol);
            g_set.sfxVol    = ReadInt(path, L"audio", L"sfx", kDefaultSfxVol);
            g_set.masterVol = ReadInt(path, L"audio", L"master", kDefaultMasterVol);

            g_set.photosensitiveSafe = ReadInt(path, L"show", L"safe", 0) != 0;

            g_set.minMs = ReadInt(path, L"show", L"idle_min", kDefaultMinMs);
            g_set.maxMs = ReadInt(path, L"show", L"idle_max", kDefaultMaxMs);


            g_set.goldGoal = ReadInt(path, L"game", L"gold_goal", kDefaultGoldGoal);


            {
                wchar_t raw[512] = { 0 };
                GetPrivateProfileStringW(L"game", L"coin_amounts", L"",
                    raw, _countof(raw), path.c_str());
                g_set.coinAmounts.clear();

                const std::wstring str = raw;
                size_t pos = 0;
                while (pos <= str.size())
                {
                    const size_t comma = str.find(L',', pos);
                    std::wstring tok = (comma == std::wstring::npos)
                        ? str.substr(pos)
                        : str.substr(pos, comma - pos);


                    while (!tok.empty() &&
                        (tok.front() == L' ' || tok.front() == L'\t')) tok.erase(tok.begin());
                    while (!tok.empty() &&
                        (tok.back() == L' ' || tok.back() == L'\t' ||
                            tok.back() == L'\r' || tok.back() == L'\n')) tok.pop_back();

                    if (!tok.empty())
                    {
                        const int v = _wtoi(tok.c_str());
                        if (v > 0) g_set.coinAmounts.push_back(v);
                    }

                    if (comma == std::wstring::npos) break;
                    pos = comma + 1;
                }
            }

            elog::Write(L"[settings] loaded %s", path.c_str());
        }

        Sanitize(g_set);
        g_idleMs = PickIdleMs();

        elog::Write(L"[settings] bgm %d%% / sfx %d%% / master %d%% / safe %s / idle %d-%dms / gold %d",
            g_set.bgmVol, g_set.sfxVol, g_set.masterVol,
            g_set.photosensitiveSafe ? L"on" : L"off",
            g_set.minMs, g_set.maxMs,
            g_set.goldGoal);


        {
            std::wstring pool;
            for (size_t i = 0; i < g_set.coinAmounts.size(); ++i)
            {
                if (i) pool += L',';
                wchar_t n[16];
                swprintf_s(n, L"%d", g_set.coinAmounts[i]);
                pool += n;
            }
            elog::Write(L"[settings] 金币面额池（%d 项）: %s",
                (int)g_set.coinAmounts.size(), pool.c_str());
        }
    }

    const Set& Current() { return g_set; }

    void SetCurrent(const Set& s)
    {


        const bool save = s.save;
        g_set = s;
        Sanitize(g_set);
        g_set.save = save;
    }

    void ResetToDefault()
    {
        g_set = Set();
        Sanitize(g_set);
    }

    bool Save()
    {
        const std::wstring path = ResolveFilePath();
        const bool ok = WriteIni(path, g_set);
        if (ok) elog::Write(L"[settings] wrote %s", path.c_str());
        else    elog::Write(L"[settings] write failed (%s)", path.c_str());
        return ok;
    }

    void ApplyAudio()
    {
        audio::SetMaster(g_set.masterVol);
        audio::SetBgmLevel(g_set.bgmVol);
        audio::SetSfxLevel(g_set.sfxVol);
    }

    void Apply()
    {
        ApplyAudio();



        static int lastSafe = -1;
        const int safe = g_set.photosensitiveSafe ? 1 : 0;
        if (safe != lastSafe)
        {
            lastSafe = safe;
            elog::Write(L"[settings] photosensitive-safe mode %s",
                safe ? L"ON (flashes and noise damped)" : L"OFF (original show)");
        }
        fx::SetPhotosensitiveSafe(g_set.photosensitiveSafe);
    }

    int BgmVol() { return g_set.bgmVol; }
    int SfxVol() { return g_set.sfxVol; }
    int MasterVol() { return g_set.masterVol; }
    bool PhotosensitiveSafe() { return g_set.photosensitiveSafe; }
    int MinMs() { return g_set.minMs; }
    int MaxMs() { return g_set.maxMs; }
    int GoldGoal() { return g_set.goldGoal; }

    const std::vector<int>& CoinAmounts() { return g_set.coinAmounts; }

    int PickIdleMs()
    {
        const int lo = g_set.minMs;
        const int hi = g_set.maxMs;
        if (hi <= lo) return lo;



        const int span = hi - lo + 1;
        return lo + (rand() % span);
    }

    int StartupIdleMs() { return g_idleMs; }

    const wchar_t* FilePath()
    {


        static std::wstring cached;
        cached = ResolveFilePath();
        return cached.c_str();
    }

}
