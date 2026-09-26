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
#include <cwctype>
#include <set>
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

    // ---- 桌面上的"真实条目"计数 ----
    //
    // 「桌面锁定非快捷方式文件的数量上限」这条滑条的右端 = min(90, 这个数)：
    // 桌面上一共就 N 个能锁的东西，滑条拖到 90 也锁不满。
    //
    // 口径尽量和 overlay 的候选名单对齐（见 desktop_overlay.cpp 的 g_allNames）：
    //   * 只数**磁盘上真实存在**的条目 —— "此电脑""回收站"这类虚拟项本来就
    //     不在磁盘上，天然不参与；
    //   * 文件夹照数（它们同样是"非快捷方式"，同样会被锁）；
    //   * .lnk / .url 不算 —— 那些走的是"已加密"那一套，不属于额外锁定；
    //   * 隐藏项跳过（桌面列表视图默认也不显示，典型的就是 desktop.ini）；
    //   * 用户桌面 + 公共桌面都数，重名只算一个。
    //
    // 结果是**一次扫描缓存到底**：它会被绘制循环读到，不能每帧翻目录。
    int ScanDesktopItems()
    {
        const int kIds[2] = { CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY };
        std::set<std::wstring> seen;

        for (int d = 0; d < 2; ++d)
        {
            wchar_t dir[MAX_PATH] = {};
            if (FAILED(SHGetFolderPathW(nullptr, kIds[d], nullptr, 0, dir))) continue;
            if (!dir[0]) continue;

            std::wstring pattern = dir;
            if (pattern.back() != L'\\') pattern += L'\\';
            pattern += L'*';

            WIN32_FIND_DATAW fd = {};
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;

            do
            {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;

                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    // 快捷方式（.lnk/.url）不算"非快捷方式"
                    const size_t dot = name.find_last_of(L'.');
                    if (dot != std::wstring::npos)
                    {
                        std::wstring ext = name.substr(dot);
                        for (size_t i = 0; i < ext.size(); ++i)
                            ext[i] = (wchar_t)towlower(ext[i]);
                        if (ext == L".lnk" || ext == L".url") continue;
                    }
                }

                std::wstring lower = name;
                for (size_t i = 0; i < lower.size(); ++i)
                    lower[i] = (wchar_t)towlower(lower[i]);
                seen.insert(lower);

                // 上限就 90，数够了没必要继续翻（桌面被塞满几千个文件时省点事）
                if (seen.size() >= 400) break;
            } while (FindNextFileW(h, &fd));

            FindClose(h);
        }

        return (int)seen.size();
    }

    int g_desktopItems = -1;        // -1 = 还没数过

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

        // 赎金目标：**两条滑条各存一份**，各自夹到自己的段里。
        // （升级成三态模式时从"一条共用滑条"拆出来的 —— 普通调 500、
        //   硬核调 5000，互不干扰。）
        s.goldGoalNormal   = ClampInt(s.goldGoalNormal,
            settings::kGoldMin, settings::kGoldMax);
        s.goldGoalHardcore = ClampInt(s.goldGoalHardcore,
            settings::kGoldHardMin, settings::kGoldHardMax);

        // 游戏模式：夹到合法枚举。认不出的值一律退回普通。
        if (s.mode < 0 || s.mode >= settings::kModeCount) s.mode = settings::kModeNormal;

        // 关窗惩罚：同样存在并集 [0, 30000] 里，模式上限在 ChildCloseMs() 里夹。
        s.childCloseMs = ClampInt(s.childCloseMs, settings::kCloseMin, settings::kCloseHardMax);

        // 假金币四项都是百分比，各自夹到 0-100。
        // 注意三个形态权重**不要求加起来等于 100**：生成时按权重比例分配，
        // 三个全是 0 就不出假币（见 gold.cpp 的 Spawn）。
        s.fakePercent   = ClampInt(s.fakePercent,   settings::kFakePctMin, settings::kFakePctMax);
        s.fakePrefixPct = ClampInt(s.fakePrefixPct, settings::kFakePctMin, settings::kFakePctMax);
        s.fakeSuffixPct = ClampInt(s.fakeSuffixPct, settings::kFakePctMin, settings::kFakePctMax);
        s.fakeBothPct   = ClampInt(s.fakeBothPct,   settings::kFakePctMin, settings::kFakePctMax);

        // ---- 硬核那四项 ----
        // 数量上限这里只夹到"硬顶 90"：真正的动态上界要看桌面上有几个能锁的
        // 东西，那一步在 ExtraLockCount() 里做（Sanitize 会被界面每帧调到，
        // 不能在这里翻目录）。
        s.hardPopupMax = ClampInt(s.hardPopupMax,
            settings::kHardPopupMin, settings::kHardPopupMax);

        s.cursorGapMinMs = ClampInt(s.cursorGapMinMs,
            settings::kCursorGapFloorMs, settings::kCursorGapCeilMs);
        s.cursorGapMaxMs = ClampInt(s.cursorGapMaxMs,
            settings::kCursorGapFloorMs, settings::kCursorGapCeilMs);

        s.extraLockCount = ClampInt(s.extraLockCount, 0, settings::kExtraLockCountCeil);
        s.extraLockMinMs = ClampInt(s.extraLockMinMs,
            settings::kExtraLockFloorMs, settings::kExtraLockCeilMs);
        s.extraLockMaxMs = ClampInt(s.extraLockMaxMs,
            settings::kExtraLockFloorMs, settings::kExtraLockCeilMs);

        // 同 idle：上下限写反了就交换，而不是拒绝 —— 用户拖过去时想要的是
        // 「那一段区间」，不是一句报错。
        if (s.cursorGapMinMs > s.cursorGapMaxMs)
        {
            const int t = s.cursorGapMinMs;
            s.cursorGapMinMs = s.cursorGapMaxMs;
            s.cursorGapMaxMs = t;
        }
        if (s.extraLockMinMs > s.extraLockMaxMs)
        {
            const int t = s.extraLockMinMs;
            s.extraLockMinMs = s.extraLockMaxMs;
            s.extraLockMaxMs = t;
        }

        // ---- 金币撒哪些盘 ----
        // 只留 "A".."Z" 的单字母（转大写），外加那个特殊的 "-"（空集标记）。
        // 去重、排序；乱写的一律丢掉（"CC" / "1" / "C:\\" 都会被扔）。
        {
            std::vector<std::wstring> clean;
            for (size_t i = 0; i < s.coinDrives.size(); ++i)
            {
                std::wstring t = s.coinDrives[i];

                while (!t.empty() && (t.front() == L' ' || t.front() == L'\t'))
                    t.erase(t.begin());
                while (!t.empty() && (t.back() == L' ' || t.back() == L'\t' ||
                    t.back() == L'\r' || t.back() == L'\n')) t.pop_back();

                if (t == settings::kCoinDrivesNoneToken)
                {
                    // "-" = 一个都不撒：它单独成立，不跟别的项共存
                    clean.clear();
                    clean.push_back(t);
                    break;
                }

                if (t.size() != 1) continue;
                const wchar_t c = (wchar_t)towupper(t[0]);
                if (c < L'A' || c > L'Z') continue;

                t.assign(1, c);
                bool dup = false;
                for (size_t k = 0; k < clean.size(); ++k)
                    if (clean[k] == t) { dup = true; break; }
                if (!dup) clean.push_back(t);
            }

            if (clean.size() > 1) std::sort(clean.begin(), clean.end());
            s.coinDrives.swap(clean);
        }

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

        // 金币盘符清单同理："C,D" / "-" / 空串
        std::wstring drives;
        for (size_t i = 0; i < s.coinDrives.size(); ++i)
        {
            if (i) drives += L',';
            drives += s.coinDrives[i];
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
            L"; game mode: 0 = normal, 1 = hardcore, 2 = idle (NOT IMPLEMENTED YET;\n"
            L"; picking it runs the normal show and only writes a log line).\n"
            L"; 'hardcore=' below is kept in sync for older builds.\n"
            L"mode=%d\n"
            L"hardcore=%d\n"
            L"; ransom goal in gold. Normal mode: 10-1000, hardcore: 1000-9999.\n"
            L"; The two sliders in the UI are independent, so both are stored.\n"
            L"; gold_goal is the effective value, written for older builds.\n"
            L"gold_goal_normal=%d\n"
            L"gold_goal_hardcore=%d\n"
            L"gold_goal=%d\n"
            L"; penalty for closing one ransom child window, in milliseconds.\n"
            L"; 0 = closing windows costs no time. Normal caps at 18000,\n"
            L"; hardcore at 30000. Shared by both modes (one slider).\n"
            L"close_penalty_ms=%d\n"
            L"; fake coins (hardcore only). percent = chance a generated coin is\n"
            L"; fake, 0-100. The three weights below decide what gets corrupted:\n"
            L"; prefix \"Gold\", the amount digits, or both. They are weights, not\n"
            L"; percentages that must sum to 100 - they are normalized together,\n"
            L"; and all three at 0 means no fake coins at all.\n"
            L"fake_percent=%d\n"
            L"fake_prefix_pct=%d\n"
            L"fake_suffix_pct=%d\n"
            L"fake_both_pct=%d\n"
            L"; gold face values, comma-separated. Any positive integers.\n"
            L"; Duplicates and order are normalized on load (sorted, deduped).\n"
            L"; e.g. 10,50,75,100,125,150,325,500\n"
            L"coin_amounts=%s\n"
            L"\n"
            L"[hardcore]\n"
            L"; Everything below only matters while hardcore=1 in [game].\n"
            L"; Max ransom child windows alive at once, 10-30 (normal mode is fixed\n"
            L"; at 14 and does not read this).\n"
            L"popup_max=%d\n"
            L"; Interval between the popups that spawn right on your mouse cursor\n"
            L"; (they exist to block clicking), min-max in milliseconds, 900-18000.\n"
            L"; min == max means a fixed interval.\n"
            L"cursor_gap_min=%d\n"
            L"cursor_gap_max=%d\n"
            L"; Random locks on non-shortcut desktop items: how many may be locked at\n"
            L"; once (0-90; the UI cap is also limited by how many lockable items the\n"
            L"; desktop really has), and how long each one stays locked, min-max in\n"
            L"; milliseconds, 900-18000. min == max means a fixed duration.\n"
            L"desk_lock_count=%d\n"
            L"desk_lock_min_ms=%d\n"
            L"desk_lock_max_ms=%d\n"
            L"; which fixed drives gold gets scattered onto (hardcore only).\n"
            L"; empty  = every fixed drive (the old behaviour);\n"
            L"; \"C,D\"  = only those two;  \"-\" = no drive at all (desktop only).\n"
            L"coin_drives=%s\n",
            s.bgmVol, s.sfxVol, s.masterVol,
            s.photosensitiveSafe ? 1 : 0,
            s.minMs, s.maxMs,
            s.mode,
            (s.mode == settings::kModeHardcore) ? 1 : 0,
            s.goldGoalNormal, s.goldGoalHardcore,
            (s.mode == settings::kModeHardcore) ? s.goldGoalHardcore : s.goldGoalNormal,
            s.childCloseMs,
            s.fakePercent, s.fakePrefixPct, s.fakeSuffixPct, s.fakeBothPct,
            pool.c_str(),
            s.hardPopupMax,
            s.cursorGapMinMs, s.cursorGapMaxMs,
            s.extraLockCount, s.extraLockMinMs, s.extraLockMaxMs,
            drives.c_str());

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

            // ---- 游戏模式（三态）----
            // 新键 mode= 优先；没有这个键的旧 ini 回落看 hardcore=1/0。
            // 用 GetPrivateProfileStringW 判"键存不存在"—— GetPrivateProfileInt
            // 分不清"没写"和"写了 0"。
            {
                wchar_t raw[32] = { 0 };
                GetPrivateProfileStringW(L"game", L"mode", L"", raw, _countof(raw),
                    path.c_str());

                if (raw[0] != 0)
                {
                    g_set.mode = _wtoi(raw);
                }
                else
                {
                    const bool hc = ReadInt(path, L"game", L"hardcore", 0) != 0;
                    g_set.mode = hc ? kModeHardcore : kModeNormal;
                }
            }

            // ---- 赎金目标：两条滑条各一份 ----
            // 新键优先；只有旧 gold_goal= 时，按它落在哪一段塞进对应那条。
            {
                wchar_t raw[32] = { 0 };
                GetPrivateProfileStringW(L"game", L"gold_goal_normal", L"", raw,
                    _countof(raw), path.c_str());
                const bool hasNormal = (raw[0] != 0);
                if (hasNormal) g_set.goldGoalNormal = _wtoi(raw);

                raw[0] = 0;
                GetPrivateProfileStringW(L"game", L"gold_goal_hardcore", L"", raw,
                    _countof(raw), path.c_str());
                const bool hasHard = (raw[0] != 0);
                if (hasHard) g_set.goldGoalHardcore = _wtoi(raw);

                // 旧键兜底：>=1000 的算硬核那份，否则算普通那份
                if (!hasNormal && !hasHard)
                {
                    const int legacy = ReadInt(path, L"game", L"gold_goal",
                        kDefaultGoldGoal);
                    if (legacy >= kGoldHardMin) g_set.goldGoalHardcore = legacy;
                    else                        g_set.goldGoalNormal = legacy;
                }
            }

            // 新增：关窗惩罚时长（毫秒）—— 两模式共用一条滑条，只有一个值
            g_set.childCloseMs = ReadInt(path, L"game", L"close_penalty_ms", kCloseNormalDefault);

            // 新增：假金币比例与三种形态权重
            g_set.fakePercent   = ReadInt(path, L"game", L"fake_percent",    kDefaultFakePercent);
            g_set.fakePrefixPct = ReadInt(path, L"game", L"fake_prefix_pct", kDefaultFakePrefixPct);
            g_set.fakeSuffixPct = ReadInt(path, L"game", L"fake_suffix_pct", kDefaultFakeSuffixPct);
            g_set.fakeBothPct   = ReadInt(path, L"game", L"fake_both_pct",   kDefaultFakeBothPct);

            // 新增：[hardcore] 硬核专属四项（旧 ini 里没有这一段，落到默认值）
            g_set.hardPopupMax  = ReadInt(path, L"hardcore", L"popup_max",
                kDefaultHardPopupMax);
            g_set.cursorGapMinMs = ReadInt(path, L"hardcore", L"cursor_gap_min",
                kDefaultCursorGapMinMs);
            g_set.cursorGapMaxMs = ReadInt(path, L"hardcore", L"cursor_gap_max",
                kDefaultCursorGapMaxMs);
            g_set.extraLockCount = ReadInt(path, L"hardcore", L"desk_lock_count",
                kDefaultExtraLockCount);
            g_set.extraLockMinMs = ReadInt(path, L"hardcore", L"desk_lock_min_ms",
                kDefaultExtraLockMinMs);
            g_set.extraLockMaxMs = ReadInt(path, L"hardcore", L"desk_lock_max_ms",
                kDefaultExtraLockMaxMs);

            // 新增：[hardcore] coin_drives= 金币撒哪几个固定盘（逗号分隔的盘符）
            {
                wchar_t raw[128] = { 0 };
                GetPrivateProfileStringW(L"hardcore", L"coin_drives", L"",
                    raw, _countof(raw), path.c_str());

                g_set.coinDrives.clear();

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

                    if (!tok.empty()) g_set.coinDrives.push_back(tok);

                    if (comma == std::wstring::npos) break;
                    pos = comma + 1;
                }
            }

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

        // 模式名字（日志用）：三态上线后这里统一打中文名
        static const wchar_t* const kModeName[kModeCount] = {
            L"普通", L"硬核", L"挂机（未实装，先按普通跑）"
        };
        const wchar_t* modeName = kModeName[(Mode() >= 0 && Mode() < kModeCount)
            ? Mode() : 0];

        elog::Write(L"[settings] bgm %d%% / sfx %d%% / master %d%% / safe %s / idle %d-%dms / 模式 %s（mode=%d）",
            g_set.bgmVol, g_set.sfxVol, g_set.masterVol,
            g_set.photosensitiveSafe ? L"on" : L"off",
            g_set.minMs, g_set.maxMs,
            modeName, g_set.mode);

        // 赎金 / 关窗惩罚 / 假币那几项按**实际生效值**打（也就是夹过之后的），
        // 手改 ini 写了个越界的数时，一眼就能看出程序实际用的是多少。
        elog::Write(L"[settings] 赎金 %d（普通 %d / 硬核 %d）/ 关窗惩罚 %dms（共用滑条值 %d）/ 假币 %d%%（前缀 %d / 后缀 %d / 都改 %d）",
            GoldGoal(), GoldGoalNormal(), GoldGoalHardcore(),
            ChildCloseMs(), g_set.childCloseMs,
            FakePercent(), FakePrefixPct(), FakeSuffixPct(), FakeBothPct());

        elog::Write(L"[settings] 硬核专属：弹窗上限 %d（滑条值 %d）/ 阻挡弹窗间隔 %d-%dms / 桌面锁定 %d 个（滑条值 %d，桌面上限 %d）%d-%dms",
            HardPopupMax(), g_set.hardPopupMax,
            CursorGapMinMs(), CursorGapMaxMs(),
            ExtraLockCount(), g_set.extraLockCount, ExtraLockCapacity(),
            ExtraLockMinMs(), ExtraLockMaxMs());

        // 金币盘符：打的是**生效口径**（"全部固定盘" / 具体几个 / 一个都不撒）
        {
            if (CoinDrivesNone())
            {
                elog::Write(L"[settings] 金币只落桌面：用户在盘符页上勾了「一个盘都不撒」");
            }
            else
            {
                const std::vector<std::wstring>& dv = CoinDrives();
                if (dv.empty())
                {
                    elog::Write(L"[settings] 金币盘符：未配置 -> 全部固定盘");
                }
                else
                {
                    std::wstring s2;
                    for (size_t i = 0; i < dv.size(); ++i)
                    {
                        if (i) s2 += L',';
                        s2 += dv[i];
                    }
                    elog::Write(L"[settings] 金币盘符：%s（共 %d 个）",
                        s2.c_str(), (int)dv.size());
                }
            }
        }

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
        // 赎金目标**按当前模式选那一条**，而且这是唯一出口：
        //   普通（含挂机）-> goldGoalNormal  夹到 [10, 1000]
        //   硬核          -> goldGoalHardcore 夹到 [1000, 9999]
        //
        // 三态模式升级时把原来"一条共用滑条"拆成了两条独立的滑条，
        // 两份值互不影响 —— 普通调 500、硬核调 5000，各存各的。
        return g_set.IsHardcore()
            ? ClampInt(g_set.goldGoalHardcore, kGoldHardMin, kGoldHardMax)
            : ClampInt(g_set.goldGoalNormal, kGoldMin, kGoldMax);
    }

    int GoldGoalNormal()
    {
        return ClampInt(g_set.goldGoalNormal, kGoldMin, kGoldMax);
    }

    int GoldGoalHardcore()
    {
        return ClampInt(g_set.goldGoalHardcore, kGoldHardMin, kGoldHardMax);
    }

    int Mode()
    {
        if (g_set.mode < 0 || g_set.mode >= kModeCount) return kModeNormal;
        return g_set.mode;
    }

    int ChildCloseMs()
    {
        // 同上，按模式夹上限：普通最多 18 秒，硬核最多 30 秒。0 是合法的
        //（关窗口完全不扣时间）。**两模式共用一条滑条**（用户定的），
        // 所以只有一个值，只是上限跟着模式变。
        return g_set.IsHardcore()
            ? ClampInt(g_set.childCloseMs, kCloseMin, kCloseHardMax)
            : ClampInt(g_set.childCloseMs, kCloseMin, kCloseNormalMax);
    }

    int FakePercent()   { return ClampInt(g_set.fakePercent,   kFakePctMin, kFakePctMax); }
    int FakePrefixPct() { return ClampInt(g_set.fakePrefixPct, kFakePctMin, kFakePctMax); }
    int FakeSuffixPct() { return ClampInt(g_set.fakeSuffixPct, kFakePctMin, kFakePctMax); }
    int FakeBothPct()   { return ClampInt(g_set.fakeBothPct,   kFakePctMin, kFakePctMax); }

    // ---- 硬核专属四项 ----
    // 这四个都**不做模式判断**：读它们的只有硬核那几条路径（popup 的
    // 鼠标位弹窗 / 子窗口上限、overlay 的额外锁定），普通模式下没人调。
    int HardPopupMax()
    {
        return ClampInt(g_set.hardPopupMax, kHardPopupMin, kHardPopupMax);
    }

    int CursorGapMinMs()
    {
        const int a = ClampInt(g_set.cursorGapMinMs, kCursorGapFloorMs, kCursorGapCeilMs);
        const int b = ClampInt(g_set.cursorGapMaxMs, kCursorGapFloorMs, kCursorGapCeilMs);
        return (a < b) ? a : b;
    }

    int CursorGapMaxMs()
    {
        const int a = ClampInt(g_set.cursorGapMinMs, kCursorGapFloorMs, kCursorGapCeilMs);
        const int b = ClampInt(g_set.cursorGapMaxMs, kCursorGapFloorMs, kCursorGapCeilMs);
        return (a > b) ? a : b;
    }

    int DesktopItemCount()
    {
        // 只扫一次：这个数被界面绘制循环读，不能每帧翻目录。
        // 演出期间桌面多出几个文件不会重扫 —— 它只影响"滑条能拖多高"，
        // 不影响演出本身（真正锁多少由 overlay 按候选名单自己收口）。
        if (g_desktopItems < 0)
        {
            g_desktopItems = ScanDesktopItems();
            elog::Write(L"[settings] 桌面上可锁的非快捷方式条目：%d 项（数量上限滑条右端 = %d）",
                g_desktopItems,
                (g_desktopItems > kExtraLockCountCeil) ? kExtraLockCountCeil : g_desktopItems);
        }
        return g_desktopItems;
    }

    int ExtraLockCapacity()
    {
        const int n = DesktopItemCount();
        return (n > kExtraLockCountCeil) ? kExtraLockCountCeil : n;
    }

    int ExtraLockCount()
    {
        // 夹到"桌面上真有几个能锁的"为止：手改 ini 写了 90，而桌面上只有
        // 8 个非快捷方式项时，实际同时最多只能锁 8 个。
        return ClampInt(g_set.extraLockCount, 0, ExtraLockCapacity());
    }

    int ExtraLockMinMs()
    {
        const int a = ClampInt(g_set.extraLockMinMs, kExtraLockFloorMs, kExtraLockCeilMs);
        const int b = ClampInt(g_set.extraLockMaxMs, kExtraLockFloorMs, kExtraLockCeilMs);
        return (a < b) ? a : b;
    }

    int ExtraLockMaxMs()
    {
        const int a = ClampInt(g_set.extraLockMinMs, kExtraLockFloorMs, kExtraLockCeilMs);
        const int b = ClampInt(g_set.extraLockMaxMs, kExtraLockFloorMs, kExtraLockCeilMs);
        return (a > b) ? a : b;
    }

    // ---- 金币撒哪些盘 ----
    const std::vector<std::wstring>& CoinDrives()
    {
        // Sanitize 已经把这份洗成"大写单字母、去重、有序"了；
        // 但"-"（空集标记）不算盘符，从这里看就是空的。
        static const std::vector<std::wstring> kEmpty;

        if (CoinDrivesNone()) return kEmpty;
        return g_set.coinDrives;
    }

    bool CoinDrivesNone()
    {
        return g_set.coinDrives.size() == 1 &&
            g_set.coinDrives[0] == kCoinDrivesNoneToken;
    }

    bool Hardcore() { return g_set.IsHardcore(); }

    const std::vector<int>& CoinAmounts()
    {
        // 硬核换成一套 <=100 的窄池（"金币面额减少"）。
        // 用函数内静态量：只在第一次调用时构造一次，之后返回同一份引用，
        // 和普通模式返回 g_set.coinAmounts 的行为一致（调用方只读）。
        static const std::vector<int> hcPool(
            kHardcoreCoinAmounts,
            kHardcoreCoinAmounts + kHardcoreCoinAmountCount);

        return g_set.IsHardcore() ? hcPool : g_set.coinAmounts;
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
