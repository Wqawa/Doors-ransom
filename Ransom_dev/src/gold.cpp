


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


#include <objbase.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {




const int kDesktopRootPercent = 25;


struct Coin {
    std::wstring path;
    int          amount;
    int          token;
};

std::vector<Coin> g_coins;
int g_total    = 0;
int g_sum      = 0;
int g_nextTok  = 1;
std::wstring g_exePath;
bool g_started = false;











std::wstring g_iconPath;
std::wstring g_honeyIconPath;



const int kHoneyIconThreshold = 499;


std::vector<std::wstring> g_stale;


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


bool IsSkippableDir(DWORD attrs)
{
    if (attrs & FILE_ATTRIBUTE_READONLY) return true;
    if (attrs & FILE_ATTRIBUTE_HIDDEN)   return true;
    if (attrs & FILE_ATTRIBUTE_SYSTEM)   return true;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    return false;
}


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


bool PickFreeName(const std::wstring& dir, int amount, std::wstring& outPath)
{
    for (int suffix = 0; suffix < 40; ++suffix)
    {
        wchar_t name[128];
        if (suffix == 0) swprintf_s(name, L"Gold_%d.lnk", amount);
        else             swprintf_s(name, L"Gold_%d_%d.lnk", amount, suffix);

        const std::wstring full = dir + L"\\" + name;


        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

        outPath = full;
        return true;
    }
    return false;
}



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





unsigned long long HashBlob(const assets::Blob& b)
{
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < b.Size(); ++i)
    {
        h ^= b.Data()[i];
        h *= 1099511628211ULL;
    }
    return h;
}




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



bool WriteIcoWithPng(const std::wstring& icoPath, const std::vector<BYTE>& png)
{
    if (png.empty() || png.size() > 0xFFFFFFFFu) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, icoPath.c_str(), L"wb") != 0 || !f) return false;

    const unsigned len = (unsigned)png.size();
    BYTE hdr[22] = {};
    hdr[2]  = 1;
    hdr[4]  = 1;
    hdr[6]  = 0;
    hdr[7]  = 0;
    hdr[8]  = 0;
    hdr[9]  = 0;
    hdr[10] = 1;
    hdr[12] = 32;
    hdr[14] = (BYTE)( len        & 0xFF);
    hdr[15] = (BYTE)((len >>  8) & 0xFF);
    hdr[16] = (BYTE)((len >> 16) & 0xFF);
    hdr[17] = (BYTE)((len >> 24) & 0xFF);
    hdr[18] = 22;

    const bool ok = (fwrite(hdr, 1, 22, f) == 22) &&
                    (fwrite(png.data(), 1, len, f) == len);
    fclose(f);

    if (!ok) DeleteFileW(icoPath.c_str());
    return ok;
}












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




    WIN32_FILE_ATTRIBUTE_DATA icoA = {};
    if (GetFileAttributesExW(ico.c_str(), GetFileExInfoStandard, &icoA) &&
        icoA.nFileSizeHigh == 0 &&
        icoA.nFileSizeLow > 0)
    {

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




bool IsOurCoin(const std::wstring& lnkPath)
{
    std::wstring target, args;
    if (!ReadLnk(lnkPath, target, args)) return false;

    if (_wcsicmp(target.c_str(), g_exePath.c_str()) != 0) return false;
    if (args.find(L"--pay ")   == std::wstring::npos) return false;
    if (args.find(L"--token ") == std::wstring::npos) return false;

    return true;
}




int RemoveOrphans()
{
    std::vector<std::wstring> dirs;
    CollectTargets(dirs);


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

















int PickCoinAmount(int goal)
{
    const std::vector<int>& all = settings::CoinAmounts();
    if (all.empty()) return 10;

    int cap = goal * 2;
    if (cap < 10)  cap = 10;
    if (cap > 500) cap = 500;


    int pool[32];
    int n = 0;
    for (size_t i = 0; i < all.size() && n < 32; ++i)
        if (all[i] <= cap) pool[n++] = all[i];

    if (n == 0)
    {

        int mn = all[0];
        for (size_t i = 1; i < all.size(); ++i)
            if (all[i] < mn) mn = all[i];
        return mn;
    }
    return pool[rand() % n];
}

}

namespace gold {

bool Start(HINSTANCE          )
{
    if (g_started) return true;

    wchar_t buf[MAX_PATH] = { 0 };
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return false;
    g_exePath = buf;

    g_coins.clear();
    g_total = g_sum = 0;
    g_nextTok = 1;
    g_started = true;





    g_iconPath = EnsureIcon(L"Gold_icon.png", L"gold_coin.ico");
    g_honeyIconPath = EnsureIcon(L"Honey_Pot_icon.png", L"honey_pot.ico");

    elog::Write(L"[gold] 已就绪（本体 %s，金币图标 %s，蜂蜜罐图标 %s）",
        g_exePath.c_str(),
        g_iconPath.empty() ? L"无" : g_iconPath.c_str(),
        g_honeyIconPath.empty() ? L"无" : g_honeyIconPath.c_str());
    return true;
}

int CleanupStale()
{
    int n = 0;


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


    const int target = goal + goal * 2 / 5;

    int made = 0;
    int failed = 0;
    int guard = 0;



    int roundSum = 0;



    int maxCoins = 14 + goal / 100;
    if (maxCoins > 30) maxCoins = 30;

    while (roundSum < target && made < maxCoins && guard < 300)
    {
        ++guard;



        const bool onDesktop =
            !roots.empty() && (dirs.empty() || (rand() % 100) < kDesktopRootPercent);

        const std::wstring& dir = onDesktop ? roots[rand() % roots.size()]
            : dirs[rand() % dirs.size()];


        const int amount = PickCoinAmount(goal);
        const int token = g_nextTok++;

        std::wstring lnk;
        if (!PickFreeName(dir, amount, lnk)) continue;

        wchar_t args[128];
        swprintf_s(args, L"--pay %d --token %d", amount, token);





        const std::wstring& iconPath =
            (amount > kHoneyIconThreshold && !g_honeyIconPath.empty())
            ? g_honeyIconPath
            : g_iconPath;

        if (!WriteLnk(lnk, args, iconPath))
        {
            ++failed;

            if (failed <= 3)
                elog::Write(L"[gold] 写快捷方式失败: %s", lnk.c_str());
            continue;
        }

        Coin c;
        c.path   = lnk;
        c.amount = amount;
        c.token  = token;
        g_coins.push_back(c);

        g_sum += amount;
        roundSum += amount;
        ++made;
        ++g_total;
    }

    SaveManifest();

    if (failed > 3)
        elog::Write(L"[gold] ……另有 %d 次写入失败（多为权限不足）", failed - 3);

    elog::Write(L"[gold] 生成 %d 个金币，本轮总额 %d（目标 %d），"
        L"候选 %d 个文件夹 + %d 个桌面根，上限 %d 颗",
        made, roundSum, target, (int)dirs.size(), (int)roots.size(),
        maxCoins);
    for (size_t i = 0; i < g_coins.size() && i < 20; ++i)
        elog::Write(L"[gold]    %d  %s  [%s]",
            g_coins[i].amount, g_coins[i].path.c_str(),
            (g_coins[i].amount > kHoneyIconThreshold && !g_honeyIconPath.empty())
            ? L"蜂蜜罐" : L"金币");

    return made;
}

int Consume(int token)
{
    for (size_t i = 0; i < g_coins.size(); ++i)
    {
        if (g_coins[i].token != token) continue;

        const int amount = g_coins[i].amount;


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
    g_iconPath.clear();
    g_honeyIconPath.clear();
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

}
