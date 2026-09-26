// ============================================================================
//  gold.cpp
// ============================================================================
#include "gold.h"

#include "assets.h"
#include "image_blob.h"
#include "entity_log.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <shlobj.h>
#include <shlguid.h>
#include <shobjidl.h>

// GDI+ 前置依赖（WIN32_LEAN_AND_MEAN 不会带进来）
#include <objbase.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

// 直接落在桌面根目录（而不是文件夹里）的金币比例，百分比。
// 桌面是「明面上」的位置，玩家一低头就能看到几枚；
// 其余仍然散进各顶层文件夹，保持翻找的节奏。
// 硬核下候选位置多了各磁盘**根目录**，桌面更需要"露面"，所以比例调高。
const int kDesktopRootPercentNormal = 25;
const int kDesktopRootPercentHard = 60;

// 本次运行生成的金币
struct Coin {
    std::wstring path;      // .lnk 的完整路径
    int          amount;
    int          token;

    // 假金币的污染掩码：
    //   0 = 真金币
    //   1 = 前缀 "Gold" 被污染  -> 双击时扣计时器（面额 × 0.1 秒）
    //   2 = 面额数字被污染      -> 双击时未付赎金 += 面额
    //   3 = 两个都污染          -> 两种效果同时触发
    // 判据是文件名，玩家眼睛看得出来；掩码只存在内存里，不进文件。
    int          fakeMask = 0;
};

std::vector<Coin> g_coins;
int g_total    = 0;         // 累计生成过几个
int g_sum      = 0;         // 累计生成金额
int g_nextTok  = 1;
std::wstring g_exePath;
bool g_started = false;

// 金币快捷方式用的 .ico（由 Gold_icon.png / Honey_Pot_icon.png 生成）。
// 空 = 没有，快捷方式不设图标。
// 声明放在这里是因为 WriteLnk() 在这行下面就要用它。
//
// 两个图标的分工（见 Spawn 里挑图标的逻辑）：
//   g_iconPath      面额 <= kHoneyIconThreshold 用（金币）
//   g_honeyIconPath 面额 >  kHoneyIconThreshold 用（蜂蜜罐）
//
// 素材缺 Honey_Pot_icon.png 时 g_honeyIconPath 留空，
// 那部分金币会自动退回金币图标，不影响生成。
std::wstring g_iconPath;
std::wstring g_honeyIconPath;

// ---- 假金币专用图标 ----
//
// 三种污染形态**各一套 5 张**图，名字里的 Left / Right / All 就是形态：
//   Left  = 前缀 "Gold" 被改坏      （kFakePrefix）
//   Right = 面额数字被改坏          （kFakeSuffix）
//   All   = 两个都改坏              （kFakePrefix | kFakeSuffix）
//
// 每套 5 张、生成时随机挑一张：否则一屏假币长得一模一样，玩家扫一眼就知道
// "这几张是一伙的"。三套分开则是让图**和名字对得上** —— 前缀坏掉的那张，
// 图上坏的就是左边那截。
//
// 转换（PNG -> 缓存 .ico）是**懒加载**：第一次真的要摆出某一形态的假币时，
// 才把那套图里的某一张转出来。硬核没开、或者假币比例是 0 的时候，
// 这 15 张图一点开销都不产生，启动也不会因此变慢。
const int kFakeIconKinds    = 3;    // Left / Right / All
const int kFakeIconVariants = 5;    // 每套几张

const wchar_t* kFakeIconPng[kFakeIconKinds][kFakeIconVariants] = {
    {
        L"Gold_icon_Left_Glich1.png", L"Gold_icon_Left_Glich2.png",
        L"Gold_icon_Left_Glich3.png", L"Gold_icon_Left_Glich4.png",
        L"Gold_icon_Left_Glich5.png",
    },
    {
        L"Gold_icon_Right_Glich1.png", L"Gold_icon_Right_Glich2.png",
        L"Gold_icon_Right_Glich3.png", L"Gold_icon_Right_Glich4.png",
        L"Gold_icon_Right_Glich5.png",
    },
    {
        L"Gold_icon_All_Glich1.png", L"Gold_icon_All_Glich2.png",
        L"Gold_icon_All_Glich3.png", L"Gold_icon_All_Glich4.png",
        L"Gold_icon_All_Glich5.png",
    },
};

// 缓存到 %LOCALAPPDATA%\Ransom_dev\ 下的 .ico 名。和真币那两个（gold_coin.ico /
// honey_pot.ico）分开，互不覆盖。
const wchar_t* kFakeIconIco[kFakeIconKinds][kFakeIconVariants] = {
    {
        L"gold_fake_left1.ico", L"gold_fake_left2.ico", L"gold_fake_left3.ico",
        L"gold_fake_left4.ico", L"gold_fake_left5.ico",
    },
    {
        L"gold_fake_right1.ico", L"gold_fake_right2.ico", L"gold_fake_right3.ico",
        L"gold_fake_right4.ico", L"gold_fake_right5.ico",
    },
    {
        L"gold_fake_all1.ico", L"gold_fake_all2.ico", L"gold_fake_all3.ico",
        L"gold_fake_all4.ico", L"gold_fake_all5.ico",
    },
};

// 生成结果（空串 = 这一张用不了）。tried 用来"只试一次"：
// 素材缺了就是一直缺，不必每枚假币都去撞一遍文件系统。
std::wstring g_fakeIcon[kFakeIconKinds][kFakeIconVariants];
bool g_fakeIconTried[kFakeIconKinds][kFakeIconVariants] = {};

// 面额**超过**这个值就换成蜂蜜罐图标。500 本身还是金币。
// 想改分界线改这里：想让 500 也变蜂蜜罐就写 499；想更宽松写 999。
const int kHoneyIconThreshold = 499;

// 上一次运行遗留的清单
std::vector<std::wstring> g_stale;

// 本地数据目录（清单、金币图标缓存都放这儿）。取不到返回空串。
std::wstring DataDir()
{
    wchar_t buf[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
        return L"";

    std::wstring dir = buf;
    dir += L"\\Ransom_dev";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// ------------------------------------------------------------ 清单路径 ----
std::wstring ManifestPath()
{
    const std::wstring dir = DataDir();
    if (dir.empty()) return L"";
    return dir + L"\\gold_manifest.txt";
}

void SaveManifest()
{
    const std::wstring p = ManifestPath();
    if (p.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;

    for (size_t i = 0; i < g_coins.size(); ++i)
        fwprintf(f, L"%s\n", g_coins[i].path.c_str());

    fclose(f);
}

void RemoveManifest()
{
    const std::wstring p = ManifestPath();
    if (!p.empty()) DeleteFileW(p.c_str());
}

void LoadStaleManifest()
{
    g_stale.clear();

    const std::wstring p = ManifestPath();
    if (p.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"r, ccs=UTF-8") != 0 || !f) return;

    wchar_t line[MAX_PATH * 2];
    while (fgetws(line, _countof(line), f))
    {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r'))
            s.pop_back();
        if (!s.empty()) g_stale.push_back(s);
    }
    fclose(f);
}

// ------------------------------------------------------------ 目标文件夹 ----
bool IsSkippableDir(DWORD attrs)
{
    if (attrs & FILE_ATTRIBUTE_READONLY) return true;
    if (attrs & FILE_ATTRIBUTE_HIDDEN)   return true;
    if (attrs & FILE_ATTRIBUTE_SYSTEM)   return true;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return true;   // 符号链接/junction 不碰
    return false;
}

// 只收桌面上的**顶层**文件夹
void ScanOneDesktop(int csidl, std::vector<std::wstring>& out)
{
    wchar_t desk[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, desk))) return;
    if (!*desk) return;

    std::wstring pattern = std::wstring(desk) + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (IsSkippableDir(fd.dwFileAttributes)) continue;

        out.push_back(std::wstring(desk) + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

// ---- 硬核：把候选位置扩到各固定磁盘的**顶层**目录 ----
//
// 深度只到 1 层，不递归。这样"翻盘找金币"的范围变大了，但不会
// 深到用户自己的项目目录里去。
//
// 系统目录一律排除：这是个整蛊程序，不该往 C:\Windows 这类地方写东西。
// 表里的名字全部小写比较（Windows 文件名不区分大小写）。
bool IsSystemDirName(const std::wstring& name)
{
    static const wchar_t* kBad[] = {
        L"windows", L"program files", L"program files (x86)", L"programdata",
        L"$recycle.bin", L"system volume information", L"perflogs",
        L"recovery", L"msocache", L"$windows.~bt", L"$windows.~ws",
        L"documents and settings", L"config.msi", L"intel", L"amd",
    };
    const int n = (int)(sizeof(kBad) / sizeof(kBad[0]));

    // 自己写小写化，不引 <cwctype>：这张表全是 ASCII
    std::wstring low = name;
    for (size_t i = 0; i < low.size(); ++i)
        if (low[i] >= L'A' && low[i] <= L'Z')
            low[i] = (wchar_t)(low[i] - L'A' + L'a');

    for (int i = 0; i < n; ++i)
        if (low == kBad[i]) return true;
    return false;
}

// 这个盘符在不在「允许撒金币」的范围内？（设置界面那个盘符页勾出来的）
//
// 三态（见 settings.h 里 kCoinDrivesNoneToken 的说明）：
//   CoinDrivesNone()   -> 谁都不算（金币只落桌面）
//   列表为空（没配过） -> 全部固定盘
//   否则               -> 只认列表里那几个
bool DriveAllowed(wchar_t letter)
{
    if (settings::CoinDrivesNone()) return false;

    const std::vector<std::wstring>& allow = settings::CoinDrives();
    if (allow.empty()) return true;

    const wchar_t up = (wchar_t)towupper(letter);
    for (size_t i = 0; i < allow.size(); ++i)
        if (allow[i].size() == 1 && allow[i][0] == up) return true;

    return false;
}

// 这个目录能不能写？（**不建测试文件**：直接以 GENERIC_WRITE 打开目录本身）
//
// 为什么必须探一下：金币现在直接落在**盘根**上，而 C:\ 的 ACL 默认不给
// 普通用户建文件 —— 不探的话那一整轮的写入会全失败，而 dirs 又不是空的，
// Spawn 的"没有可写位置"兜底不会触发，结果是整场一枚金币都生不出来。
// 打开目录需要 FILE_FLAG_BACKUP_SEMANTICS，权限不够时返回 ACCESS_DENIED。
bool CanWriteDir(const std::wstring& dir)
{
    HANDLE h = CreateFileW(dir.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// 硬核的额外落点：**各固定盘的根目录本身**（D:\ 这种），不再往下面的
// 文件夹里塞。用户明确要求过："金币直接在磁盘顶目录（比如 D:/）下面直接生成，
// 不要去摸下面的文件夹了"。
//
// forSpawn=true  —— 生成路径：只扫盘符页上勾了的盘，而且**只要盘根**。
// forSpawn=false —— 清理路径：扫全部固定盘，而且**盘根 + 顶层文件夹都扫**。
//   两个理由：
//     * 清理必须全盘扫，否则用户把某个盘的勾去掉之后，撒在那儿的金币收不回来；
//     * 早期版本的金币是撒在 D:\某文件夹 里的，光扫盘根收不回它们（旧残留）。
void ScanFixedDriveRoots(std::vector<std::wstring>& out, int limit, bool forSpawn)
{
    const DWORD mask = GetLogicalDrives();

    for (int d = 0; d < 26 && (int)out.size() < limit; ++d)
    {
        if (!(mask & (1u << d))) continue;

        const wchar_t letter = (wchar_t)(L'A' + d);
        wchar_t root[8] = { letter, L':', L'\\', 0 };

        // 只碰固定盘：U 盘 / 光驱 / 网络盘不动（拔掉就没了，还会误伤别人）
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;

        // 用户在盘符页上没勾这个盘 -> 生成时不往这儿撒
        if (forSpawn && !DriveAllowed(letter)) continue;

        // ---- 盘根本身 ----
        // 注意这里**不走** IsSkippableDir / IsSystemDirName 那两道过滤：
        // 那两个是给"顶层文件夹"用的，盘根的属性位不代表它不能写。
        {
            const std::wstring dir = root;
            if (!CanWriteDir(dir))
            {
                elog::Write(L"[gold] %s 根目录写不进去（权限/只读），跳过这个盘",
                    dir.c_str());
            }
            else
            {
                out.push_back(dir);
            }
        }

        // ---- 生成路径到此为止：不摸下面的文件夹 ----
        if (forSpawn) continue;

        // ---- 清理路径：把顶层文件夹也扫一遍（回收旧版本撒在里面的金币）----
        const std::wstring pattern = std::wstring(root) + L"*";

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (IsSkippableDir(fd.dwFileAttributes)) continue;    // 只读/隐藏/系统/软链
            if (IsSystemDirName(fd.cFileName)) continue;

            out.push_back(std::wstring(root) + fd.cFileName);
        } while (FindNextFileW(h, &fd) && (int)out.size() < limit);

        FindClose(h);
    }
}

void CollectTargets(std::vector<std::wstring>& out, bool forSpawn = true)
{
    out.clear();
    ScanOneDesktop(CSIDL_DESKTOPDIRECTORY,       out);
    ScanOneDesktop(CSIDL_COMMON_DESKTOPDIRECTORY, out);

    // 硬核：再加上各固定盘的**根目录**（普通模式一行不多扫）。
    //
    // forSpawn=false（清理路径）时不看盘符页的勾选、并额外扫顶层文件夹 ——
    // 理由见 ScanFixedDriveRoots 的注释。
    if (settings::Hardcore()) ScanFixedDriveRoots(out, 200, forSpawn);
}

// 桌面根目录本身。金币会**一部分**直接撒在桌面上（见 Spawn 里的比例），
// 主体散在桌面各顶层文件夹与（硬核下）各磁盘根目录里 ——
// 外面随手就能捡到两枚，剩下的还是得翻。
void CollectDesktopRoots(std::vector<std::wstring>& out)
{
    out.clear();

    wchar_t buf[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf)) && *buf)
        out.push_back(buf);

    buf[0] = 0;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, 0, buf)) && *buf)
        out.push_back(buf);
}

// ------------------------------------------------------------ 写快捷方式 ----
// iconPath：这个快捷方式要用的 .ico。空串 = 不设图标（用目标程序自己的）。
// 由调用方按面额挑好传进来，这里不再自己判断。
bool WriteLnk(const std::wstring& lnkPath, const std::wstring& args,
    const std::wstring& iconPath)
{
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) return false;

    link->SetPath(g_exePath.c_str());
    link->SetArguments(args.c_str());
    link->SetDescription(L"Gold");
    link->SetWorkingDirectory(g_exePath.substr(0, g_exePath.find_last_of(L'\\')).c_str());

    // 图标由调用方按面额挑好（金币 / 蜂蜜罐）。生成失败（源图缺失 /
    // 格式不支持）就不设，快捷方式退回目标程序自己的图标——
    // 不影响金币能不能点。
    if (!iconPath.empty())
        link->SetIconLocation(iconPath.c_str(), 0);

    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf))) && pf)
    {
        ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

// ------------------------------------------------------------ 假金币 ----
// 硬核模式会混进一批"假金币"：文件名被同形字替换过（Gold_50 -> G01d_5o），
// 双击它**一分钱都不进账**，而是按污染的位置吃惩罚：
//   前缀 "Gold" 被污染 -> 倒计时扣 面额 × 0.1 秒
//   面额数字被污染     -> 未付的赎金 += 面额
//   两个都被污染       -> 两种效果同时触发
// 判据全在文件名的样子上 —— 玩家得自己看名字辨真假。
const int kFakePrefix = 1;   // bit0
const int kFakeSuffix = 2;   // bit1

// 同形字替换表：字形接近的"字母 / 数字"配对，双向可用。
// 玩家盯着看能看出来，扫一眼看不出来 —— 这正是要的效果。
const wchar_t kFakePairs[][2] = {
    { L'o', L'0' }, { L'l', L'1' }, { L'i', L'1' }, { L's', L'5' },
    { L'g', L'6' }, { L'b', L'8' }, { L'z', L'2' }, { L'e', L'3' },
    { L't', L'7' }, { L'a', L'4' },
};
const int kFakePairCount = (int)(sizeof(kFakePairs) / sizeof(kFakePairs[0]));

// 把 s 里的一处字符换成同形字。返回是否真换掉了一处。
bool FakeSwapOne(std::wstring& s)
{
    int pos[64];
    int n = 0;
    for (size_t i = 0; i < s.size() && n < 64; ++i)
    {
        for (int k = 0; k < kFakePairCount; ++k)
        {
            if (s[i] == kFakePairs[k][0] || s[i] == kFakePairs[k][1])
            {
                pos[n++] = (int)i;
                break;
            }
        }
    }
    if (n == 0) return false;

    const size_t i = (size_t)pos[rand() % n];
    for (int k = 0; k < kFakePairCount; ++k)
    {
        if (s[i] == kFakePairs[k][0]) { s[i] = kFakePairs[k][1]; return true; }
        if (s[i] == kFakePairs[k][1]) { s[i] = kFakePairs[k][0]; return true; }
    }
    return false;
}

// 造假金币的显示名（**不带扩展名**）。
// mask 决定污染哪一段；双污染时两段各至少换一处，
// 这样"两种效果都看得出来"这件事在名字上是自洽的。
void MakeFakeStem(int amount, int mask, std::wstring& out)
{
    wchar_t digits[24];
    swprintf_s(digits, L"%d", amount);

    std::wstring pre = L"Gold";
    std::wstring num = digits;

    if (mask & kFakePrefix) FakeSwapOne(pre);
    if (mask & kFakeSuffix) FakeSwapOne(num);

    // 兜底：万一两段都没换成（正常不会发生），至少动一处，
    // 否则这个名字是"真金币的名字"，却带着假金币的效果，谁也看不出来。
    if (pre == L"Gold" && num == digits) FakeSwapOne(pre);

    out = pre + L"_" + num;
}

// 挑一个不冲突的文件名。**绝不覆盖已存在的文件。**
//
// 拼 "目录 + 文件名"。**目录末尾可能已经带了反斜杠**（固定盘的根目录就是
// "D:\"），再无条件加一个会拼出 "D:\\Gold_50.lnk" —— Windows 自己认这种
// 双斜杠，但这条路径要写进清单、还要跟后面对回来比字符串，脏着不划算。
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    std::wstring s = dir;
    if (!s.empty() && s.back() != L'\\' && s.back() != L'/') s += L'\\';
    s += name;
    return s;
}

// stem 由调用方给（真金币是 "Gold_50"，假金币是同形替换过的变体）。
// fake = true 时**不加数字后缀**：加了后缀会把"看名字辨真假"这件事糊掉，
// 撞名就干脆跳过这一次生成（调用方 continue）。反正少一颗不影响大局。
bool PickFreeName(const std::wstring& dir, const std::wstring& stem,
                  bool fake, std::wstring& outPath)
{
    // 假金币只试一次（不加数字后缀，撞名就换地方生成），真金币最多试 40 个后缀。
    const int tries = fake ? 1 : 40;

    for (int suffix = 0; suffix < tries; ++suffix)
    {
        wchar_t name[160];
        if (suffix == 0) swprintf_s(name, L"%s.lnk", stem.c_str());
        else             swprintf_s(name, L"%s_%d.lnk", stem.c_str(), suffix);

        const std::wstring full = JoinPath(dir, name);

        // 已存在就换下一个候选名——**不覆盖**
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

        outPath = full;
        return true;
    }
    return false;
}

// ------------------------------------------------------------ 读快捷方式 ----
// 读出 .lnk 的目标与参数，用来判断「这是不是我造的金币」。
bool ReadLnk(const std::wstring& lnkPath, std::wstring& target, std::wstring& args)
{
    target.clear();
    args.clear();

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) || !link)
        return false;

    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf))) && pf)
    {
        if (SUCCEEDED(pf->Load(lnkPath.c_str(), STGM_READ)))
        {
            wchar_t buf[MAX_PATH * 2] = { 0 };
            if (SUCCEEDED(link->GetPath(buf, _countof(buf), nullptr, SLGP_UNCPRIORITY)))
                target = buf;
            if (SUCCEEDED(link->GetArguments(buf, _countof(buf))))
                args = buf;
            ok = true;
        }
        pf->Release();
    }
    link->Release();
    return ok;
}

// ------------------------------------------------------------ 金币图标 ----
// 快捷方式的图标**只能指向 .ico / .exe / .dll**——指向 .png 会被 shell 直接
// 忽略、退回目标程序自己的图标（实测过：指向 PNG 时桌面显示的是本程序的
// exe 图标，不是金币图）。所以这里把 assets\image\ 里那张金币图转成 .ico，
// 缓存在 %LOCALAPPDATA%，所有金币快捷方式都指向它。
//
// 缓存策略：ico 不存在、或者比源图**旧**，就重新生成一次。
// 这样你换掉 Gold_icon.png 之后下次运行会自动跟上，不用手动转换。

int GetPngEncoderClsid(CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)malloc(size);
    if (!info) return -1;
    Gdiplus::GetImageEncoders(num, size, info);

    int found = -1;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(info[i].MimeType, L"image/png") == 0)
        { *clsid = info[i].Clsid; found = (int)i; break; }

    free(info);
    return found;
}

// 计算一段素材字节的 FNV-1a 哈希。
// 用途：EnsureIcon 的缓存版本戳。以前用源图字节数当版本戳，
// 如果换了一张内容不同但字节数恰好相同的图标，缓存不会重建。
// 改用哈希之后，只要内容变了，缓存就一定会失效重建。
unsigned long long HashBlob(const assets::Blob& b)
{
    unsigned long long h = 1469598103934665603ULL; // FNV offset basis
    for (size_t i = 0; i < b.Size(); ++i)
    {
        h ^= b.Data()[i];
        h *= 1099511628211ULL;                     // FNV prime
    }
    return h;
}

// 把源图缩到 256x256、编成 PNG 存内存。
// 为什么是 256：ICO 的宽高字段只有 1 字节，0 表示 256，所以 256 就是上限。
// 源图来自内嵌资源（磁盘上没有 PNG 可读），所以走内存解码。
bool EncodeScaledPng(const assets::Blob& png, std::vector<BYTE>& out)
{
    using namespace Gdiplus;

    Bitmap* src = image_blob::Decode(png.Data(), png.Size());
    if (!src)
    {
        elog::Write(L"[gold] 金币图标源图解不开（%zu 字节）", png.Size());
        return false;
    }

    const int S = 256;
    Bitmap scaled(S, S, PixelFormat32bppARGB);
    {
        Graphics g(&scaled);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));
        g.DrawImage(src, Rect(0, 0, S, S),
                    0, 0, (INT)src->GetWidth(), (INT)src->GetHeight(), UnitPixel);
    }
    delete src;

    CLSID clsid;
    if (GetPngEncoderClsid(&clsid) < 0) { elog::Write(L"[gold] 找不到 PNG 编码器"); return false; }

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK) return false;
    if (scaled.Save(stream, &clsid, nullptr) != Ok) { stream->Release(); return false; }

    STATSTG st = {};
    stream->Stat(&st, STATFLAG_NONAME);
    const ULONG len = (ULONG)st.cbSize.LowPart;

    out.resize(len);
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    stream->Read(out.data(), len, &got);
    stream->Release();

    return (got == len && len > 0);
}

// 组装 ICO：6 字节 ICONDIR + 16 字节 ICONDIRENTRY + 一段 PNG 数据。
// Vista 以后 ICO 允许直接内嵌 PNG，省掉 BMP + AND 掩码那一整套。
bool WriteIcoWithPng(const std::wstring& icoPath, const std::vector<BYTE>& png)
{
    if (png.empty() || png.size() > 0xFFFFFFFFu) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, icoPath.c_str(), L"wb") != 0 || !f) return false;

    const unsigned len = (unsigned)png.size();
    BYTE hdr[22] = {};
    hdr[2]  = 1;                     // type = 1 (icon)
    hdr[4]  = 1;                     // count = 1
    hdr[6]  = 0;                     // width  = 0 -> 256
    hdr[7]  = 0;                     // height = 0 -> 256
    hdr[8]  = 0;                     // 调色板数
    hdr[9]  = 0;                     // reserved
    hdr[10] = 1;                     // planes
    hdr[12] = 32;                    // 位深
    hdr[14] = (BYTE)( len        & 0xFF);
    hdr[15] = (BYTE)((len >>  8) & 0xFF);
    hdr[16] = (BYTE)((len >> 16) & 0xFF);
    hdr[17] = (BYTE)((len >> 24) & 0xFF);
    hdr[18] = 22;                    // 数据偏移

    const bool ok = (fwrite(hdr, 1, 22, f) == 22) &&
                    (fwrite(png.data(), 1, len, f) == len);
    fclose(f);

    if (!ok) DeleteFileW(icoPath.c_str());
    return ok;
}

// 需要（重新）生成就生成。返回可用的 .ico 路径，失败返回空串。
//
// 注意这里**还是要在磁盘上落一个 .ico**：IShellLink::SetIconLocation 只认
// 文件路径（.ico/.exe/.dll），没法从内存里的字节取图标。所以这一步是
// 「把内嵌的 PNG 转成缓存文件」，不是「素材没打包进去」。
//
// 参数化之后金币图标和蜂蜜罐图标共用这一份逻辑：
//   pngName  assets\image\ 下那张源图（如 "Gold_icon.png"）
//   icoName  缓存到 %LOCALAPPDATA%\Ransom_dev\ 下的文件名（如 "gold_coin.ico"）
//
// 两份图标分开缓存、互不干扰 —— 素材缺一张不影响另一张。
std::wstring EnsureIcon(const wchar_t* pngName, const wchar_t* icoName)
{
    assets::Blob png;
    if (!assets::Get(assets::KIND_IMAGE, pngName, png))
    {
        elog::Write(L"[gold] 素材包里没有 %s，这一档快捷方式将用默认图标", pngName);
        return L"";
    }

    const std::wstring dir = DataDir();
    if (dir.empty()) return L"";
    const std::wstring ico = dir + L"\\" + icoName;

    // 缓存还有效？（存在、大小和源图当前字节数一致）
    // 内嵌素材没有修改时间可比，所以拿源图字节数当版本戳——换图标必然会
    // 改变 PNG 大小，改一次就够触发重建了。
    WIN32_FILE_ATTRIBUTE_DATA icoA = {};
    if (GetFileAttributesExW(ico.c_str(), GetFileExInfoStandard, &icoA) &&
        icoA.nFileSizeHigh == 0 &&
        icoA.nFileSizeLow > 0)
    {
        // 缓存里存过版本戳就直接比，省掉一次编码
        const std::wstring stamp = ico + L".src";
        FILE* f = nullptr;
        if (_wfopen_s(&f, stamp.c_str(), L"rb") == 0 && f)
        {
            unsigned long long was = 0;
            const size_t got = fread(&was, 1, sizeof(was), f);
            fclose(f);
            if (got == sizeof(was) && was == HashBlob(png)) return ico;
        }
    }

    std::vector<BYTE> pngBytes;
    if (!EncodeScaledPng(png, pngBytes)) return L"";
    if (!WriteIcoWithPng(ico, pngBytes))
    {
        elog::Write(L"[gold] 图标写入失败: %s", ico.c_str());
        return L"";
    }

    // 记下这次的源图哈希，下次直接命中缓存
    FILE* sf = nullptr;
    if (_wfopen_s(&sf, (ico + L".src").c_str(), L"wb") == 0 && sf)
    {
        const unsigned long long n = HashBlob(png);
        fwrite(&n, 1, sizeof(n), sf);
        fclose(sf);
    }

    elog::Write(L"[gold] 图标已生成: %s（%u 字节，源自内嵌 %s %zu 字节）",
        ico.c_str(), (unsigned)pngBytes.size(), pngName, png.Size());
    return ico;
}

// 假币的三种污染形态 -> 上面那三套图的下标。不是假币返回 -1。
int FakeIconSlot(int mask)
{
    if (mask == kFakePrefix)                  return 0;   // Left
    if (mask == kFakeSuffix)                  return 1;   // Right
    if (mask == (kFakePrefix | kFakeSuffix))  return 2;   // All
    return -1;
}

// 按污染形态随机挑一张假币图标（第一次用到时把那张 PNG 转成 .ico 缓存）。
//
// 返回空串 = 这一套用不了（素材包里没有 / ico 写不出去），调用方退回真币图标，
// 不影响金币生成。**注意返回的是常引用**：调用处要拷贝一份再改。
const std::wstring& PickFakeIcon(int mask)
{
    static const std::wstring kNone;

    const int slot = FakeIconSlot(mask);
    if (slot < 0) return kNone;

    const int v = rand() % kFakeIconVariants;
    if (!g_fakeIconTried[slot][v])
    {
        // 只试一次：素材缺了就一直缺，没必要每枚假币都撞一遍
        g_fakeIconTried[slot][v] = true;
        g_fakeIcon[slot][v] =
            EnsureIcon(kFakeIconPng[slot][v], kFakeIconIco[slot][v]);
    }
    return g_fakeIcon[slot][v];
}

// 是不是我们造的金币？
// 判据很严：目标必须是本程序本体，参数必须同时含 --pay 和 --token。
// 你自己的快捷方式目标是别的程序，永远不会被误判。
bool IsOurCoin(const std::wstring& lnkPath)
{
    std::wstring target, args;
    if (!ReadLnk(lnkPath, target, args)) return false;

    if (_wcsicmp(target.c_str(), g_exePath.c_str()) != 0) return false;
    if (args.find(L"--pay ")   == std::wstring::npos) return false;
    if (args.find(L"--token ") == std::wstring::npos) return false;

    return true;
}

// ------------------------------------------------------------ 孤儿回收 ----
// 不依赖任何清单文件：直接扫候选目录，找出「目标指向本程序
// 且参数像金币」的快捷方式删掉。清单写不出去时（受限环境）这条路径仍然有效。
//
// 注意候选目录来自 CollectTargets(false) —— 传 false 是因为清理**不看**
// 盘符页的勾选，硬核下它覆盖**所有**固定盘的顶层目录：
//   * 被强杀之后残留的金币必须能全部收回来，用户后来把某个盘的勾去掉
//     也不能成为"那儿的金币收不回来"的理由；
//   * 判据始终是 IsOurCoin（目标 + --pay/--token），不会误删别人的东西。
int RemoveOrphans()
{
    std::vector<std::wstring> dirs;
    CollectTargets(dirs, false);

    // 桌面根目录也要扫：金币现在也可能直接落在那里
    std::vector<std::wstring> roots;
    CollectDesktopRoots(roots);
    for (size_t i = 0; i < roots.size(); ++i) dirs.push_back(roots[i]);

    int n = 0;
    for (size_t d = 0; d < dirs.size(); ++d)
    {
        const std::wstring pattern = JoinPath(dirs[d], L"*.lnk");

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            const std::wstring full = JoinPath(dirs[d], fd.cFileName);
            if (!IsOurCoin(full)) continue;

            if (DeleteFileW(full.c_str()))
            {
                ++n;
                elog::Write(L"[gold] 回收孤儿金币: %s", full.c_str());
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    }
    return n;
}

// ------------------------------------------------------------ 面额池 ----
// 从**可配置**的面额池里挑一个。池子来源：settings.ini 的
// [game] coin_amounts=（settings::CoinAmounts()），没配就用内置默认
// { 10, 50, 75, 100, 125, 150, 325, 500 }。
//
// 挑的时候会按目标做一层过滤：只挑 <= cap 的面额，
// 其中 cap = min(目标*2, 500)。这个上限是硬编码的，**不受自定义池影响**：
// 目标是 10 的时候如果掉出一颗 500，玩家一捡就通关，桌面散布的节奏感
// 就没了 —— 所以这里必须有个闸。
//
//   目标 10   -> 只会掉池里 <= 20 的那档
//   目标 200  -> 掉池里 <= 400 的
//   目标 500+ -> 池里所有面额都能掉
//
// 池里所有面额都超过 cap 时（例如池里只有 {500}，而目标是 10），
// 退回池里**最小**的那一个 —— 否则永远生成不出金币。
int PickCoinAmount(int goal)
{
    const std::vector<int>& all = settings::CoinAmounts();
    if (all.empty()) return 10;   // 理论上不会：Sanitize 保证非空

    int cap = goal * 2;
    if (cap < 10)  cap = 10;
    if (cap > 500) cap = 500;

    // 挑出所有 <= cap 的。池子最多 kCoinAmountMax（16）项，定长缓冲够用。
    int pool[32];
    int n = 0;
    for (size_t i = 0; i < all.size() && n < 32; ++i)
        if (all[i] <= cap) pool[n++] = all[i];

    if (n == 0)
    {
        // 全都超了：退回池里最小的那个
        int mn = all[0];
        for (size_t i = 1; i < all.size(); ++i)
            if (all[i] < mn) mn = all[i];
        return mn;
    }
    return pool[rand() % n];
}

} // namespace

namespace gold {

bool Start(HINSTANCE /*hInst*/)
{
    if (g_started) return true;

    wchar_t buf[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return false;
    g_exePath = buf;

    g_coins.clear();
    g_total = g_sum = 0;
    g_nextTok = 1;
    g_started = true;

    // 两种图标一起生成：
    //   面额 <= 500 -> 金币    (Gold_icon.png       -> gold_coin.ico)
    //   面额 >  500 -> 蜂蜜罐  (Honey_Pot_icon.png -> honey_pot.ico)
    // 各自缓存、互不影响。任一失败都不致命——那部分快捷方式退回默认图标。
    //
    // 假币那三套图（Left / Right / All 各 5 张）**不在这里生成**：它们是懒加载的
    // （见 PickFakeIcon），省掉"硬核没开也要转 15 张图"的启动开销。
    g_iconPath = EnsureIcon(L"Gold_icon.png", L"gold_coin.ico");
    g_honeyIconPath = EnsureIcon(L"Honey_Pot_icon.png", L"honey_pot.ico");

    elog::Write(L"[gold] 已就绪（本体 %s，金币图标 %s，蜂蜜罐图标 %s，假币三套图按需生成）",
        g_exePath.c_str(),
        g_iconPath.empty() ? L"无" : g_iconPath.c_str(),
        g_honeyIconPath.empty() ? L"无" : g_honeyIconPath.c_str());
    return true;
}

int CleanupStale()
{
    int n = 0;

    // 路径 1：清单（快，但依赖 %LOCALAPPDATA% 可写）
    LoadStaleManifest();
    for (size_t i = 0; i < g_stale.size(); ++i)
    {
        const std::wstring& p = g_stale[i];
        if (p.size() < 4) continue;
        if (_wcsicmp(p.c_str() + p.size() - 4, L".lnk") != 0) continue;
        if (DeleteFileW(p.c_str())) ++n;
    }
    if (!g_stale.empty())
    {
        RemoveManifest();
        elog::Write(L"[gold] 按清单清理 %d 条（删掉 %d 个）", (int)g_stale.size(), n);
        g_stale.clear();
    }

    // 路径 2：扫桌面找孤儿。
    // 这条不依赖任何可写位置，是真正兜底的那条——
    // 判据是「目标是本程序 + 参数含 --pay/--token」，不会误伤你的快捷方式。
    n += RemoveOrphans();

    if (n > 0) elog::Write(L"[gold] 上次遗留共清理 %d 个", n);
    return n;
}

int Spawn(int goal)
{
    if (!g_started) return 0;

    std::vector<std::wstring> dirs;
    CollectTargets(dirs);

    std::vector<std::wstring> roots;
    CollectDesktopRoots(roots);

    if (dirs.empty() && roots.empty())
    {
        elog::Write(L"[gold] 桌面上没有可写的位置，放弃生成");
        return 0;
    }

    // 目标总额。
    //   普通：goal 再加约 40% 余量，让玩家有余地。
    //   硬核：面额被压到 100 以内（池 50/75/100，均值 75），5000 要 74 枚左右;
    //         余量收到 10% 就够了，否则桌面上要撒一百多枚 .lnk。
    const bool hc = settings::Hardcore();
    const int target = hc ? (goal + goal / 10) : (goal + goal * 2 / 5);

    int made = 0;
    int failed = 0;
    int guard = 0;
    // 本轮生成的金额。**不能拿 g_sum 当停止条件**：它是进程累计值，
    // 第一轮就已经越过 target，于是第二轮回合循环体一次都不执行，
    // 结果是「被抓住的第二轮一个金币都没有」——桌面被锁死却无从付款。
    int roundSum = 0;

    // 生成上限跟着目标走：目标越大需要越多颗才能凑齐。
    // 原固定值 14 在目标 1000 时会偏紧，多给一点余量。
    //
    // 硬核必须另外给：面额压到 100 以内之后，30 枚的上限最多只能凑 3000，
    // 而赎金是 5000 —— 不改这里的话整场根本付不清。
    //   90 枚 × 均值 75 ≈ 6750 > 5500（target），留了余量。
    int maxCoins = 14 + goal / 100;   // 10..1000 -> 14..24
    if (hc) maxCoins = 90;
    const int hardCap = hc ? 120 : 30;
    if (maxCoins > hardCap) maxCoins = hardCap;

    // ---- 假金币的预算 ----
    // 关键：假币**不占真币的名额**。
    // 一开始我把两者算在同一个 made 上限里，结果是 90 个名额里被假币吃掉 31 个，
    // 真币只剩 59 枚 × 均值 75 ≈ 4425 < 5000 —— 整场根本付不清。
    // 假币是"混进来的陷阱"，不是"替代真币"，所以它另有预算、额外生成。
    //
    // 比例由设置里的「假金币比例」滑条控制：它**对标真金币**（maxCoins 的百分比），
    // 所以拉满就是"真币多少枚、假币也多少枚"。
    const int fakePct = settings::FakePercent();
    int fakeMade = 0;
    const int fakeBudget = hc ? (maxCoins * fakePct / 100) : 0;

    // 假币内部三种形态的**权重**（不是必须凑 100 的百分比）：
    // 按权重比例随机分配，三个全是 0 就干脆不出假币。
    const int wPre = settings::FakePrefixPct();
    const int wSuf = settings::FakeSuffixPct();
    const int wBoth = settings::FakeBothPct();
    const int wTotal = wPre + wSuf + wBoth;

    while (roundSum < target && made < maxCoins && guard < 900)
    {
        ++guard;

        // 落点在 dirs 里随机挑：普通模式是桌面上的顶层文件夹，
        // 硬核是「桌面顶层文件夹 + 各固定盘根目录」的合集；
        // 一定比例直接落在桌面上（roots）。假金币不占名额
        //（它不计入 roundSum，所以不会把循环提前喂饱）。
        const int rootPct = hc ? kDesktopRootPercentHard : kDesktopRootPercentNormal;
        const bool onDesktop =
            !roots.empty() && (dirs.empty() || (rand() % 100) < rootPct);

        const std::wstring& dir = onDesktop ? roots[rand() % roots.size()]
            : dirs[rand() % dirs.size()];

        // 面额从自适应池里挑（见 PickCoinAmount）
        const int amount = PickCoinAmount(goal);
        const int token = g_nextTok++;

        // ---- 是不是假金币 ----
        //   先按「假金币比例」掷一次；
        //   中了再按三种形态的**权重**分配（权重和可以是任意值，
        //   三个全是 0 就一枚假币都不出）。
        int fakeMask = 0;
        if (fakeBudget > 0 && fakeMade < fakeBudget && wTotal > 0 &&
            (rand() % 100) < fakePct)
        {
            const int r = rand() % wTotal;
            if (r < wPre)
                fakeMask = kFakePrefix;
            else if (r < wPre + wSuf)
                fakeMask = kFakeSuffix;
            else
                fakeMask = kFakePrefix | kFakeSuffix;   // 两个都污染，两种效果叠加
        }

        std::wstring stem;
        if (fakeMask)
        {
            MakeFakeStem(amount, fakeMask, stem);
        }
        else
        {
            wchar_t b[32];
            swprintf_s(b, L"Gold_%d", amount);
            stem = b;
        }

        std::wstring lnk;
        if (!PickFreeName(dir, stem, fakeMask != 0, lnk)) continue;

        wchar_t args[128];
        swprintf_s(args, L"--pay %d --token %d", amount, token);

        // ---- 挑图标 ----
        // 假币优先：按污染形态（Left / Right / All）从那套 5 张里随机挑一张。
        // 挑不出来（素材缺了）就往下走，退回真币那套图标——不写额外的分支，
        // 空串自然落到下面的判断上。
        //
        // 真币还是老规矩：面额 > kHoneyIconThreshold（默认 500）的用蜂蜜罐，
        // 其余用金币。蜂蜜罐素材缺失时 g_honeyIconPath 是空串，也自然退回金币。
        //
        // 刻意**不给假币留"面额大就用蜂蜜罐"的口子**：假币只在硬核下出现，
        // 而硬核的面额池是 50/75/100，本来就到不了那个分界线；退一步说，
        // 就算将来把假币放进普通模式，图上更该"看得出是假的"而不是"看得出很值钱"。
        std::wstring iconPath;
        if (fakeMask)
            iconPath = PickFakeIcon(fakeMask);

        if (iconPath.empty())
            iconPath = (amount > kHoneyIconThreshold && !g_honeyIconPath.empty())
            ? g_honeyIconPath
            : g_iconPath;

        if (!WriteLnk(lnk, args, iconPath))
        {
            ++failed;
            // 权限受限时可能整片失败，别把日志刷爆
            if (failed <= 3)
                elog::Write(L"[gold] 写快捷方式失败: %s", lnk.c_str());
            continue;
        }

        Coin c;
        c.path     = lnk;
        c.amount   = amount;
        c.token    = token;
        c.fakeMask = fakeMask;
        g_coins.push_back(c);

        ++g_total;

        if (fakeMask)
        {
            // 假金币**既不占真币的名额，也不计入 roundSum**：
            // 它一分钱都付不了，计进去的话循环会被提前喂饱、真金币变少，
            // 等于一份惩罚吃两遍（而且会让整场付不清）。
            ++fakeMade;
        }
        else
        {
            ++made;                   // 只有真金币占 maxCoins 的名额
            g_sum += amount;          // 进程累计，只作统计用
            roundSum += amount;       // 本轮累计，循环的停止条件
        }
    }

    SaveManifest();

    if (failed > 3)
        elog::Write(L"[gold] ……另有 %d 次写入失败（多为权限不足）", failed - 3);

    elog::Write(L"[gold] 生成真金币 %d 个 + 假币 %d 个，真币本轮总额 %d（目标 %d），"
        L"候选落点 %d 个（桌面顶层文件夹%s）+ %d 个桌面根，真币上限 %d 颗，%s模式",
        made, fakeMade, roundSum, target, (int)dirs.size(),
        hc ? L" + 各固定盘根目录" : L"",
        (int)roots.size(),
        maxCoins, hc ? L"硬核" : L"普通");
    for (size_t i = 0; i < g_coins.size() && i < 20; ++i)
        elog::Write(L"[gold]    %d  %s  [%s%s]",
            g_coins[i].amount, g_coins[i].path.c_str(),
            g_coins[i].fakeMask
            ? ((g_coins[i].fakeMask == (kFakePrefix | kFakeSuffix)) ? L"假币图(All)"
               : (g_coins[i].fakeMask == kFakePrefix ? L"假币图(Left)" : L"假币图(Right)"))
            : ((g_coins[i].amount > kHoneyIconThreshold && !g_honeyIconPath.empty())
                ? L"蜂蜜罐" : L"金币"),
            g_coins[i].fakeMask
            ? ((g_coins[i].fakeMask == (kFakePrefix | kFakeSuffix)) ? L"·假币(双重)"
               : (g_coins[i].fakeMask == kFakePrefix ? L"·假币(前缀)" : L"·假币(数字)"))
            : L"");

    return made;
}

int Consume(int token, int* fakeMask)
{
    for (size_t i = 0; i < g_coins.size(); ++i)
    {
        if (g_coins[i].token != token) continue;

        const int amount = g_coins[i].amount;
        const int mask = g_coins[i].fakeMask;

        // 只删自己登记过的那个路径
        DeleteFileW(g_coins[i].path.c_str());

        if (mask)
            elog::Write(L"[gold] 收下**假**金币 %d（掩码 %d：%s）（%s）", amount, mask,
                (mask == (kFakePrefix | kFakeSuffix)) ? L"前缀+数字都被污染"
                : (mask == kFakePrefix ? L"前缀被污染" : L"数字被污染"),
                g_coins[i].path.c_str());
        else
            elog::Write(L"[gold] 收下 %d Gold（%s）", amount, g_coins[i].path.c_str());

        g_coins.erase(g_coins.begin() + i);
        SaveManifest();

        if (fakeMask) *fakeMask = mask;
        return amount;
    }

    elog::Write(L"[gold] token=%d 找不到对应金币（可能已经收过了）", token);
    return 0;
}

void Cleanup()
{
    int n = 0;
    for (size_t i = 0; i < g_coins.size(); ++i)
    {
        if (DeleteFileW(g_coins[i].path.c_str())) ++n;
    }

    if (n > 0)
        elog::Write(L"[gold] 清空剩余 %d 个金币文件", n);

    g_coins.clear();
    RemoveManifest();
}

void Stop()
{
    Cleanup();
    g_sum = 0;
    g_started = false;
    g_iconPath.clear();
    g_honeyIconPath.clear();

    // 假币那 15 张的缓存路径也清掉（磁盘上的 .ico 留着，下次命中哈希直接复用）。
    // tried 一起复位：这一轮没转出来的图，下一轮还有机会再试一次。
    for (int k = 0; k < kFakeIconKinds; ++k)
    {
        for (int v = 0; v < kFakeIconVariants; ++v)
        {
            g_fakeIcon[k][v].clear();
            g_fakeIconTried[k][v] = false;
        }
    }
    elog::Write(L"[gold] 已停止");
}

int SpawnedTotal() { return g_sum; }
int Alive()        { return (int)g_coins.size(); }
int TotalCreated() { return g_total; }

bool IsOurCoinFile(const wchar_t* lnkPath)
{
    if (!lnkPath || !*lnkPath) return false;
    return IsOurCoin(lnkPath);
}

} // namespace gold
