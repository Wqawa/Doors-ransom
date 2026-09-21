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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

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

        // ---- 面额池 ----
        // 空 -> 填默认。非空 -> 逐项夹到 [1, 100000]、排序、去重、截断。
        // 上限放到 100000 是为了给「自定义」留余地（默认池最大才 500）。
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

        // 面额池先拼成 "10,50,75,..." 这么一串，供下面 %s 用
        std::wstring pool;
        for (size_t i = 0; i < s.coinAmounts.size(); ++i)
        {
            if (i) pool += L',';
            wchar_t n[16];
            swprintf_s(n, L"%d", s.coinAmounts[i]);
            pool += n;
        }

        // ini 全文的缓冲区。
        //
        // 大小**必须留足**，而且不能靠"大概够了"：swprintf_s 的数组重载
        // 在放不下时会去调 invalid-parameter handler，默认 handler 在
        // Release 里是 fast-fail（0xC0000409，进程当场死），**不是**静默截断。
        // 表现就是：设置界面点「开始」的那一瞬整个程序消失/中断到调试器 ——
        // 因为 Save() 正是在 Commit() 里被调的。
        //
        // 实测踩过一次：给 [game] 加了一段 hardcore 的注释之后，文本从
        // 约 900 个字符涨到 1261，1024 就不够了，点「开始」当场挂。
        // 现在留 4096：面额池满 16 项时还要再多 100 来个字符，也够。
        // **改格式串的人注意：加完自己数一下长度。**
        wchar_t buf[4096];
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
            L"; hardcore mode: 1 = 3-minute timer, 5000 gold goal, fake coins,\n"
            L"; more and stickier popups, gold in disk folders, random locks on\n"
            L"; non-shortcut desktop items, panic hotkey needs two presses.\n"
            L"; 0 = the original show. While this is 1, gold_goal above is\n"
            L"; IGNORED (5000 is forced) but kept as-is for when you turn it off.\n"
            L"hardcore=%d\n"
            L"; gold face values, comma-separated. Any positive integers.\n"
            L"; Duplicates and order are normalized on load (sorted, deduped).\n"
            L"; e.g. 10,50,75,100,125,150,325,500\n"
            L"coin_amounts=%s\n",
            s.bgmVol, s.sfxVol, s.masterVol,
            s.photosensitiveSafe ? 1 : 0,
            s.minMs, s.maxMs,
            s.goldGoal,
            s.hardcore ? 1 : 0,
            pool.c_str());

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

            // 新增：硬核模式开关
            g_set.hardcore = ReadInt(path, L"game", L"hardcore", 0) != 0;

            // 新增：金币面额池（逗号分隔的整数串）
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

                    // 掐头去尾的空白
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

        elog::Write(L"[settings] bgm %d%% / sfx %d%% / master %d%% / safe %s / idle %d-%dms / gold %d / hardcore %s",
            g_set.bgmVol, g_set.sfxVol, g_set.masterVol,
            g_set.photosensitiveSafe ? L"on" : L"off",
            g_set.minMs, g_set.maxMs,
            g_set.goldGoal,
            g_set.hardcore ? L"ON" : L"off");

        // 面额池一行单独打：条数不定，拼成一个短串更直观
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
    int GoldGoal()
    {
        // 硬核直接把赎金顶到 5000，**但不改写** g_set.goldGoal ——
        // 用户自己调的那个值原样留着，关掉硬核就回到它。
        return g_set.hardcore ? kHardcoreGoldGoal : g_set.goldGoal;
    }

    bool Hardcore() { return g_set.hardcore; }

    const std::vector<int>& CoinAmounts()
    {
        // 硬核换成一套 <=100 的窄池（"金币面额减少"）。
        // 用函数内静态量：只在第一次调用时构造一次，之后返回同一份引用，
        // 和普通模式返回 g_set.coinAmounts 的行为一致（调用方只读）。
        static const std::vector<int> hcPool(
            kHardcoreCoinAmounts,
            kHardcoreCoinAmounts + kHardcoreCoinAmountCount);

        return g_set.hardcore ? hcPool : g_set.coinAmounts;
    }

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
