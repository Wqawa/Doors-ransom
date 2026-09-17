// ============================================================================
//  settings.cpp
// ============================================================================
#include "settings.h"

#include "audio.h"
#include "entity_log.h"
#include "fx.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

// SHGetFolderPathW 在 shell32 里；CoTaskMemFree 之类走 ole32（已由别处引入）。
#pragma comment(lib, "shell32.lib")

namespace {

    settings::Set g_set;

    // 本次运行的随机潜伏时长。Load() 时抽一次，之后每轮遭遇战结束再抽。
    int g_idleMs = settings::kDefaultMinMs;

    // 配置文件路径。拼不出来时返回空串（此时 Load/Save 都直接放弃）。
    //
    // 名字刻意不叫 FilePath：settings::FilePath()（公开的那个）要复用它，
    // 同名的话在参数列表解析阶段会先命中公开那个的声明，容易看岔。
    std::wstring ResolveFilePath()
    {
        wchar_t base[MAX_PATH] = {};
        // CSIDL_LOCAL_APPDATA：不要漫游配置——这是个整蛊程序，
        // 设置跟着机器走就行，别同步到域账户里去。
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base)))
            return L"";

        std::wstring p = base;
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        p += L"Ransom_dev";
        CreateDirectoryW(p.c_str(), nullptr);      // 已存在会返回 FALSE，无所谓
        p += L"\\settings.ini";
        return p;
    }

    int ClampInt(int v, int lo, int hi)
    {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    // 把 Set 夹到合法范围。**所有**入口（ini、界面、命令行）都过这一关，
    // 免得下游还要各自防一手。
    void Sanitize(settings::Set& s)
    {
        s.bgmVol    = ClampInt(s.bgmVol, settings::kVolMin, settings::kVolMax);
        s.sfxVol    = ClampInt(s.sfxVol, settings::kVolMin, settings::kVolMax);
        s.masterVol = ClampInt(s.masterVol, 0, 100);

        s.minMs = ClampInt(s.minMs, settings::kIntervalMinMs, settings::kIntervalMaxMs);
        s.maxMs = ClampInt(s.maxMs, settings::kIntervalMinMs, settings::kIntervalMaxMs);

        // 上下限写反了就交换，而不是拒绝——用户把两个滑块拖过去时，
        // 他想要的是「那一段区间」，不是一句报错。
        if (s.minMs > s.maxMs)
        {
            const int t = s.minMs;
            s.minMs = s.maxMs;
            s.maxMs = t;
        }

        // 金币目标：10-1000
        s.goldGoal = ClampInt(s.goldGoal, settings::kGoldMin, settings::kGoldMax);
    }

    // 从 ini 读一个整数。读不到（键不存在 / 文件没有）时返回 fallback。
    int ReadInt(const std::wstring& path, const wchar_t* sec, const wchar_t* key, int fallback)
    {
        if (path.empty()) return fallback;
        return (int)GetPrivateProfileIntW(sec, key, fallback, path.c_str());
    }

    // 写一整个 ini。
    //
    // 这里**不用** WritePrivateProfileStringW：它每写一个键就重排一次文件，
    // 顺序会变成字母序，注释也保不住。自己拼一份纯文本更好读——
    // 这个文件是给人看、给人手改的。
    bool WriteIni(const std::wstring& path, const settings::Set& s)
    {
        if (path.empty()) return false;

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
            L"gold_goal=%d\n",
            s.bgmVol, s.sfxVol, s.masterVol,
            s.photosensitiveSafe ? 1 : 0,
            s.minMs, s.maxMs,
            s.goldGoal);

        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;

        // 写 UTF-16LE + BOM：GetPrivateProfileIntW 认，记事本也能正确显示
        // 别的程序写进来的注释（纯 UTF-8 的中文会被记事本按 ANSI 读成乱码）。
        const unsigned char bom[2] = { 0xFF, 0xFE };
        fwrite(bom, 1, 2, f);
        fwrite(buf, sizeof(wchar_t), wcslen(buf), f);
        fclose(f);
        return true;
    }

} // namespace

namespace settings {

    void Load()
    {
        const std::wstring path = ResolveFilePath();
        g_set = Set();          // 先摆上默认值，缺项自然落到默认

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

            // 新增：赎金目标金币
            g_set.goldGoal = ReadInt(path, L"game", L"gold_goal", kDefaultGoldGoal);

            elog::Write(L"[settings] loaded %s", path.c_str());
        }

        Sanitize(g_set);
        g_idleMs = PickIdleMs();

        elog::Write(L"[settings] bgm %d%% / sfx %d%% / master %d%% / safe %s / idle %d-%dms / gold %d",
            g_set.bgmVol, g_set.sfxVol, g_set.masterVol,
            g_set.photosensitiveSafe ? L"on" : L"off",
            g_set.minMs, g_set.maxMs,
            g_set.goldGoal);
    }

    const Set& Current() { return g_set; }

    void SetCurrent(const Set& s)
    {
        // save 标志由调用方负责，这里**不**悄悄改它：
        // 界面收起滑条之后才决定要不要落盘。
        const bool save = s.save;
        g_set = s;
        Sanitize(g_set);
        g_set.save = save;
    }

    void ResetToDefault()
    {
        g_set = Set();          // Set 的成员默认值就是「默认设置」
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

        // 光敏安全模式只在**状态变化**时写日志（fx 内部也做了同样的去重），
        // 免得 director 每轮回 IDLE 都刷一行。
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

    int PickIdleMs()
    {
        const int lo = g_set.minMs;
        const int hi = g_set.maxMs;
        if (hi <= lo) return lo;

        // rand() 够用：这里的随机只决定「玩家等多久才被吓」，
        // 不需要密码学强度，而且 director / popup 已经在用同一套。
        const int span = hi - lo + 1;
        return lo + (rand() % span);
    }

    int StartupIdleMs() { return g_idleMs; }

    const wchar_t* FilePath()
    {
        // 每次调都重新解析一遍：SHGetFolderPathW 不算便宜，但这个函数
        // 一局只被调几次（日志 + 读写），不值得为它维护一份可能过期的缓存。
        static std::wstring cached;
        cached = ResolveFilePath();
        return cached.c_str();
    }

} // namespace settings
