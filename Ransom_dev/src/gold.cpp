// ============================================================================
//  gold.cpp
// ============================================================================
#include "gold.h"

#include "assets.h"
#include "image_blob.h"
#include "entity_log.h"

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
const int kDesktopRootPercent = 25;

// 本次运行生成的金币
struct Coin {
    std::wstring path;      // .lnk 的完整路径
    int          amount;
    int          token;
};

std::vector<Coin> g_coins;
int g_total    = 0;         // 累计生成过几个
int g_sum      = 0;         // 累计生成金额
int g_nextTok  = 1;
std::wstring g_exePath;
bool g_started = false;

// 金币快捷方式用的 .ico（由 Gold_icon.png 生成）。空 = 没有，快捷方式不设图标。
// 声明放在这里是因为 WriteLnk() 在这行下面就要用它。
std::wstring g_iconPath;

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

void CollectTargets(std::vector<std::wstring>& out)
{
    out.clear();
    ScanOneDesktop(CSIDL_DESKTOPDIRECTORY,       out);
    ScanOneDesktop(CSIDL_COMMON_DESKTOPDIRECTORY, out);
}

// 桌面根目录本身。金币会**少量**直接撒在桌面上（见 Spawn 里的比例），
// 主体仍然散在各顶层文件夹里——外面随手就能捡到两枚，
// 剩下的还是得一个个文件夹翻。
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
bool WriteLnk(const std::wstring& lnkPath, const std::wstring& args)
{
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) return false;

    link->SetPath(g_exePath.c_str());
    link->SetArguments(args.c_str());
    link->SetDescription(L"Gold");
    link->SetWorkingDirectory(g_exePath.substr(0, g_exePath.find_last_of(L'\\')).c_str());

    // 金币自己的图标。生成失败（源图缺失/格式不支持）就不设，
    // 快捷方式退回默认图标——不影响金币能不能点。
    if (!g_iconPath.empty())
        link->SetIconLocation(g_iconPath.c_str(), 0);

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

// 挑一个不冲突的文件名。**绝不覆盖已存在的文件。**
bool PickFreeName(const std::wstring& dir, int amount, std::wstring& outPath)
{
    for (int suffix = 0; suffix < 40; ++suffix)
    {
        wchar_t name[128];
        if (suffix == 0) swprintf_s(name, L"Gold_%d.lnk", amount);
        else             swprintf_s(name, L"Gold_%d_%d.lnk", amount, suffix);

        const std::wstring full = dir + L"\\" + name;

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
// 「把内嵌的 Gold_icon.png 转成缓存文件」，不是「素材没打包进去」。
std::wstring EnsureCoinIcon()
{
    assets::Blob png;
    if (!assets::Get(assets::KIND_IMAGE, L"Gold_icon.png", png))
    {
        elog::Write(L"[gold] 素材包里没有 Gold_icon.png，快捷方式将用默认图标");
        return L"";
    }

    const std::wstring dir = DataDir();
    if (dir.empty()) return L"";
    const std::wstring ico = dir + L"\\gold_coin.ico";

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
            if (got == sizeof(was) && was == (unsigned long long)png.Size()) return ico;
        }
    }

    std::vector<BYTE> pngBytes;
    if (!EncodeScaledPng(png, pngBytes)) return L"";
    if (!WriteIcoWithPng(ico, pngBytes))
    {
        elog::Write(L"[gold] 金币图标写入失败: %s", ico.c_str());
        return L"";
    }

    // 记下这次的源图大小，下次直接命中缓存
    FILE* sf = nullptr;
    if (_wfopen_s(&sf, (ico + L".src").c_str(), L"wb") == 0 && sf)
    {
        const unsigned long long n = (unsigned long long)png.Size();
        fwrite(&n, 1, sizeof(n), sf);
        fclose(sf);
    }

    elog::Write(L"[gold] 金币图标已生成: %s（%u 字节，源自内嵌 Gold_icon.png %zu 字节）",
                ico.c_str(), (unsigned)pngBytes.size(), png.Size());
    return ico;
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
// 不依赖任何清单文件：直接扫桌面顶层文件夹，找出「目标指向本程序
// 且参数像金币」的快捷方式删掉。清单写不出去时（受限环境）这条路径仍然有效。
int RemoveOrphans()
{
    std::vector<std::wstring> dirs;
    CollectTargets(dirs);

    // 桌面根目录也要扫：金币现在也可能直接落在那里
    std::vector<std::wstring> roots;
    CollectDesktopRoots(roots);
    for (size_t i = 0; i < roots.size(); ++i) dirs.push_back(roots[i]);

    int n = 0;
    for (size_t d = 0; d < dirs.size(); ++d)
    {
        const std::wstring pattern = dirs[d] + L"\\*.lnk";

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            const std::wstring full = dirs[d] + L"\\" + fd.cFileName;
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

    // 金币快捷方式的图标：把 assets\image\Gold_icon.png 转成 .ico 并缓存。
    // 失败不致命，只是金币用默认图标。
    g_iconPath = EnsureCoinIcon();

    elog::Write(L"[gold] 已就绪（本体 %s，图标 %s）",
                g_exePath.c_str(), g_iconPath.empty() ? L"无" : g_iconPath.c_str());
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

    // 目标：总额达到 goal 再加约 40% 余量，让玩家有余地
    const int target = goal + goal * 2 / 5;

    int made = 0;
    int failed = 0;
    int guard = 0;
    // 本轮生成的金额。**不能拿 g_sum 当停止条件**：它是进程累计值，
    // 第一轮就已经越过 target，于是第二轮回合循环体一次都不执行，
    // 结果是「被抓住的第二轮一个金币都没有」——桌面被锁死却无从付款。
    int roundSum = 0;

    while (roundSum < target && made < 14 && guard < 200)
    {
        ++guard;

        // 大多数金币散进桌面上的顶层文件夹，约四分之一直接落在桌面上。
        // 文件夹那边优先，桌面只是「顺手能捡到几枚」。
        const bool onDesktop =
            !roots.empty() && (dirs.empty() || (rand() % 100) < kDesktopRootPercent);

        const std::wstring& dir = onDesktop ? roots[rand() % roots.size()]
                                            : dirs[rand() % dirs.size()];

        // 金额：50 / 75 / 100 / 125 / 150
        static const int kAmounts[5] = { 50, 75, 100, 125, 150 };
        const int amount = kAmounts[rand() % 5];
        const int token  = g_nextTok++;

        std::wstring lnk;
        if (!PickFreeName(dir, amount, lnk)) continue;

        wchar_t args[128];
        swprintf_s(args, L"--pay %d --token %d", amount, token);

        if (!WriteLnk(lnk, args))
        {
            ++failed;
            // 权限受限时可能整片失败，别把日志刷爆
            if (failed <= 3)
                elog::Write(L"[gold] 写快捷方式失败: %s", lnk.c_str());
            continue;
        }

        Coin c;
        c.path   = lnk;
        c.amount = amount;
        c.token  = token;
        g_coins.push_back(c);

        g_sum += amount;          // 进程累计，只作统计用
        roundSum += amount;       // 本轮累计，循环的停止条件
        ++made;
        ++g_total;
    }

    SaveManifest();

    if (failed > 3)
        elog::Write(L"[gold] ……另有 %d 次写入失败（多为权限不足）", failed - 3);

    elog::Write(L"[gold] 生成 %d 个金币，本轮总额 %d（目标 %d），"
                L"候选 %d 个文件夹 + %d 个桌面根",
                made, roundSum, target, (int)dirs.size(), (int)roots.size());
    for (size_t i = 0; i < g_coins.size() && i < 20; ++i)
        elog::Write(L"[gold]    %d  %s", g_coins[i].amount, g_coins[i].path.c_str());

    return made;
}

int Consume(int token)
{
    for (size_t i = 0; i < g_coins.size(); ++i)
    {
        if (g_coins[i].token != token) continue;

        const int amount = g_coins[i].amount;

        // 只删自己登记过的那个路径
        DeleteFileW(g_coins[i].path.c_str());

        elog::Write(L"[gold] 收下 %d Gold（%s）", amount, g_coins[i].path.c_str());
        g_coins.erase(g_coins.begin() + i);
        SaveManifest();
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
