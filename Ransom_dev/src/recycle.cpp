// ============================================================================
//  recycle.cpp
// ============================================================================
#include "recycle.h"

#include "entity_log.h"
#include "gold.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <shlobj.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <shellapi.h>      // SHFILEOPSTRUCTW / FO_DELETE / FOF_ALLOWUNDO / SEE_MASK_ASYNCOK
#include <shlwapi.h>       // StrRetToBufW

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

// ------------------------------------------------------------ 清单 ----
// 优先 %LOCALAPPDATA%，写不进去就退回本体所在目录。
std::wstring ManifestPath()
{
    wchar_t buf[MAX_PATH] = { 0 };

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
    {
        std::wstring dir = buf;
        dir += L"\\Ransom_dev";
        if (CreateDirectoryW(dir.c_str(), nullptr) ||
            GetLastError() == ERROR_ALREADY_EXISTS)
        {
            return dir + L"\\recycle_manifest.txt";
        }
    }

    wchar_t self[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, self, MAX_PATH))
    {
        std::wstring p = self;
        const size_t slash = p.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            return p.substr(0, slash) + L"\\recycle_manifest.txt";
    }
    return L"";
}

void SaveManifest(const std::vector<std::wstring>& items)
{
    const std::wstring p = ManifestPath();
    if (p.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    for (size_t i = 0; i < items.size(); ++i)
        fwprintf(f, L"%s\n", items[i].c_str());
    fclose(f);
}

void LoadManifest(std::vector<std::wstring>& out)
{
    out.clear();
    const std::wstring p = ManifestPath();
    if (p.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"r, ccs=UTF-8") != 0 || !f) return;

    wchar_t line[MAX_PATH * 2];
    while (fgetws(line, _countof(line), f))
    {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        if (!s.empty()) out.push_back(s);
    }
    fclose(f);
}

// ------------------------------------------------------------ 收集 ----
// 只扫桌面（用户 + 公共）**顶层**的 .lnk / .url
void ScanDesktopShortcuts(int csidl, std::vector<std::wstring>& out)
{
    wchar_t desk[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, desk))) return;
    if (!*desk) return;

    const wchar_t* kPatterns[2] = { L"\\*.lnk", L"\\*.url" };

    for (int p = 0; p < 2; ++p)
    {
        const std::wstring pat = std::wstring(desk) + kPatterns[p];

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            out.push_back(std::wstring(desk) + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    }
}

void CollectLocked(std::vector<std::wstring>& out)
{
    out.clear();
    ScanDesktopShortcuts(CSIDL_DESKTOPDIRECTORY,       out);
    ScanDesktopShortcuts(CSIDL_COMMON_DESKTOPDIRECTORY, out);

    // 排除本程序自己生成的金币——这是我们造的东西，不该被当成「被锁定的图标」。
    // 判据在 gold 模块里：目标是本程序 + 参数含 --pay/--token。
    std::vector<std::wstring> keep;
    keep.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        const std::wstring& p = out[i];
        if (p.size() >= 4 && _wcsicmp(p.c_str() + p.size() - 4, L".lnk") == 0)
        {
            if (gold::IsOurCoinFile(p.c_str())) continue;
        }
        keep.push_back(p);
    }
    out.swap(keep);
}

// ------------------------------------------------------------ 还原 ----
// 对回收站里的一个条目调用它的「还原」动词，然后**核对文件是否真的回到原位**。
//
// 注意两点：
//  1. 不要用 CMIC_MASK_ASYNCOK。带上它 InvokeCommand 会立刻返回 S_OK，
//     但那只表示「已排队」，操作可能根本没执行——返回成功是假象。
//  2. 即使同步调用，Shell 有时也在后台线程里完成还原，
//     所以这里做一小段重试核对，而不是盲信返回值。
//
// 不解析回收站的内部格式（$I 文件那套是非公开的），走 Shell 的正规接口。
bool InvokeRestoreVerb(IShellFolder* bin, LPCITEMIDLIST pidl, const std::wstring& expectPath)
{
    IContextMenu* cm = nullptr;
    if (FAILED(bin->GetUIObjectOf(nullptr, 1, &pidl, IID_IContextMenu, nullptr,
                                  (void**)&cm)) || !cm)
        return false;

    bool invoked = false;
    HMENU hMenu = CreatePopupMenu();

    if (hMenu && SUCCEEDED(cm->QueryContextMenu(hMenu, 0, 1, 0x7FFF, CMF_NORMAL)))
    {
        const int cnt = GetMenuItemCount(hMenu);

        for (int i = 0; i < cnt; ++i)
        {
            MENUITEMINFOW mii = { sizeof(MENUITEMINFOW) };
            wchar_t text[256] = { 0 };
            mii.fMask      = MIIM_STRING | MIIM_ID;
            mii.dwTypeData = text;
            mii.cch        = 255;

            if (!GetMenuItemInfoW(hMenu, i, TRUE, &mii)) continue;

            // 中英文都认，另外兜一手 "estore"
            // （回收站还原动词的规范名历史上是 "ESTORE"，不能只认 "restore"）
            if (wcsstr(text, L"还原") || wcsstr(text, L"Restore") || wcsstr(text, L"estore"))
            {
                CMINVOKECOMMANDINFO ici = { sizeof(CMINVOKECOMMANDINFO) };
                ici.fMask  = 0;                    // 同步执行，不要 ASYNCOK
                ici.hwnd   = nullptr;
                ici.lpVerb = MAKEINTRESOURCEA(mii.wID - 1);
                ici.nShow  = SW_SHOWNORMAL;

                invoked = SUCCEEDED(cm->InvokeCommand(&ici));
                break;
            }
        }
    }

    if (hMenu) DestroyMenu(hMenu);
    cm->Release();

    if (!invoked) return false;

    // 核对：文件真的回到原路径了吗
    for (int t = 0; t < 20; ++t)
    {
        if (GetFileAttributesW(expectPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
        Sleep(50);
    }
    return false;
}

// 取回收站条目的「名称」列（0）和「原始位置」列（1）。
// 注意：名称列**不含扩展名**（显示为 "QQ" 而不是 "QQ.lnk"），
// 所以这里分开返回，匹配时再跟清单条目去掉扩展名后比较。
bool GetRecycleItemInfo(IShellFolder2* bin2, LPCITEMIDLIST pidl,
                        std::wstring& name, std::wstring& folder)
{
    name.clear();
    folder.clear();
    if (!bin2) return false;

    for (int col = 0; col <= 1; ++col)
    {
        SHELLDETAILS sd = {};
        if (FAILED(bin2->GetDetailsOf(pidl, col, &sd))) return false;

        wchar_t buf[MAX_PATH * 2] = { 0 };

        if (sd.str.uType == STRRET_WSTR && sd.str.pOleStr)
        {
            wcsncpy_s(buf, sd.str.pOleStr, _TRUNCATE);
            CoTaskMemFree(sd.str.pOleStr);
        }
        else if (sd.str.uType == STRRET_OFFSET)
        {
            // 偏移相对条目自己的短 pidl
            MultiByteToWideChar(CP_ACP, 0, (const char*)pidl + sd.str.uOffset,
                                -1, buf, _countof(buf));
        }
        else
        {
            return false;
        }

        if (col == 0) name   = buf;
        else          folder = buf;
    }

    return !name.empty() && !folder.empty();
}

// 把清单里的完整路径拆成「目录」和「不含扩展名的文件名」，
// 好跟回收站暴露的两列对上。
bool SplitStem(const std::wstring& full, std::wstring& dir, std::wstring& stem)
{
    const size_t slash = full.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;

    dir = full.substr(0, slash);

    std::wstring name = full.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);

    stem = name;
    return true;
}

// 回收站条目的物理文件保留了扩展名（形如 $R1K4LM3.lnk）。
// 清单缺失时靠它判断「这是不是快捷方式」，而不是普通文件。
bool GetPhysicalExt(IShellFolder2* bin2, LPCITEMIDLIST pidl, std::wstring& ext)
{
    ext.clear();
    if (!bin2) return false;

    IShellFolder* sf = nullptr;
    if (FAILED(bin2->QueryInterface(IID_PPV_ARGS(&sf))) || !sf) return false;

    bool ok = false;
    STRRET sr = {};
    if (SUCCEEDED(sf->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &sr)))
    {
        wchar_t buf[MAX_PATH * 2] = { 0 };
        if (SUCCEEDED(StrRetToBufW(&sr, pidl, buf, _countof(buf))))
        {
            const std::wstring p = buf;
            const size_t dot = p.find_last_of(L'.');
            if (dot != std::wstring::npos) { ext = p.substr(dot); ok = true; }
        }
    }
    sf->Release();
    return ok;
}

bool IsDesktopFolder(const std::wstring& folder)
{
    wchar_t buf[MAX_PATH] = { 0 };

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf)))
        if (_wcsicmp(buf, folder.c_str()) == 0) return true;

    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, 0, buf)))
        if (_wcsicmp(buf, folder.c_str()) == 0) return true;

    return false;
}

} // namespace

namespace recycle {

bool Start(HINSTANCE /*hInst*/)
{
    elog::Write(L"[recycle] 已就绪（清单 %s）", ManifestPath().c_str());
    return true;
}

void Stop()
{
    // 有意**不自动还原**：惩罚就得有代价，还原靠 `--restore`。
    elog::Write(L"[recycle] 已停止（还原请用 --restore）");
}

int LockedCount()
{
    std::vector<std::wstring> items;
    CollectLocked(items);
    return (int)items.size();
}

int ManifestCount()
{
    std::vector<std::wstring> items;
    LoadManifest(items);
    return (int)items.size();
}

int SendToBin()
{
    std::vector<std::wstring> items;
    CollectLocked(items);

    if (items.empty())
    {
        elog::Write(L"[recycle] 桌面顶层没有可没收的快捷方式");
        return 0;
    }

    // SHFileOperation 要的是「双空字符结尾」的多字符串
    std::wstring list;
    for (size_t i = 0; i < items.size(); ++i)
    {
        list += items[i];
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    SHFILEOPSTRUCTW op = {};
    op.hwnd   = nullptr;
    op.wFunc  = FO_DELETE;
    op.pFrom  = list.c_str();
    // FOF_ALLOWUNDO = 进回收站（可还原），这是本模块唯一的删改动作
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int rc = SHFileOperationW(&op);

    if (rc != 0 || op.fAnyOperationsAborted)
    {
        elog::Write(L"[recycle] SHFileOperation 失败 rc=%d aborted=%d", rc, (int)op.fAnyOperationsAborted);
        return 0;
    }

    SaveManifest(items);

    elog::Write(L"[recycle] 已把 %d 个快捷方式移入回收站（清单已写，可用 --restore 还原）",
                (int)items.size());
    for (size_t i = 0; i < items.size() && i < 30; ++i)
        elog::Write(L"[recycle]    %s", items[i].c_str());

    return (int)items.size();
}

int Restore()
{
    std::vector<std::wstring> want;
    LoadManifest(want);

    const bool haveManifest = !want.empty();
    if (!haveManifest)
    {
        // 清单可能因为各种原因没写成（受限环境）或已被消费掉。
        // 兜底：还原回收站里「原始目录是桌面 + 物理文件是 .lnk/.url」的条目。
        // 这个判据足够窄，不会误碰用户自己删的普通文件。
        elog::Write(L"[recycle] 清单为空 —— 启用兜底模式（还原回收站里位于桌面的快捷方式）");
    }

    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) return 0;

    LPITEMIDLIST pidlBin = nullptr;
    if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &pidlBin)) || !pidlBin)
    {
        desktop->Release();
        return 0;
    }

    IShellFolder* bin = nullptr;
    if (FAILED(desktop->BindToObject(pidlBin, nullptr, IID_PPV_ARGS(&bin))) || !bin)
    {
        CoTaskMemFree(pidlBin);
        desktop->Release();
        return 0;
    }

    IShellFolder2* bin2 = nullptr;
    bin->QueryInterface(IID_PPV_ARGS(&bin2));

    IEnumIDList* en = nullptr;
    int restored = 0;
    int seen = 0, matched = 0, verbFail = 0, infoFail = 0;

    if (SUCCEEDED(bin->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &en)) && en)
    {
        LPITEMIDLIST pidl = nullptr;
        ULONG got = 0;

        while (en->Next(1, &pidl, &got) == S_OK)
        {
            ++seen;

            std::wstring rname, rfolder;
            if (!GetRecycleItemInfo(bin2, pidl, rname, rfolder))
            {
                ++infoFail;
                if (infoFail <= 3)
                    elog::Write(L"[recycle] 条目 %d 取列失败（bin2=%d）", seen, (int)(bin2 != nullptr));
                CoTaskMemFree(pidl);
                continue;
            }

            if (seen <= 5)
                elog::Write(L"[recycle] 条目 name='%s' folder='%s'", rname.c_str(), rfolder.c_str());

            bool hit = false;
            std::wstring expect;

            if (haveManifest)
            {
                // 指令清单：回收站给的是「不含扩展名的名字 + 原始目录」，
                // 所以拿清单条目去掉扩展名后再比。
                for (size_t i = 0; i < want.size(); ++i)
                {
                    std::wstring wdir, wstem;
                    if (!SplitStem(want[i], wdir, wstem)) continue;

                    if (_wcsicmp(wdir.c_str(),  rfolder.c_str()) == 0 &&
                        _wcsicmp(wstem.c_str(), rname.c_str())   == 0)
                    {
                        hit = true;
                        expect = want[i];      // 用清单里的完整路径（带扩展名）去核对
                        break;
                    }
                }
            }
            else
            {
                // 兜底：原始目录是桌面，且物理文件是 .lnk/.url
                std::wstring ext;
                if (IsDesktopFolder(rfolder) &&
                    GetPhysicalExt(bin2, pidl, ext) &&
                    (_wcsicmp(ext.c_str(), L".lnk") == 0 ||
                     _wcsicmp(ext.c_str(), L".url") == 0))
                {
                    hit = true;
                    expect = rfolder;
                    if (!expect.empty() && expect.back() != L'\\') expect.push_back(L'\\');
                    expect += rname + ext;
                }
            }

            if (hit)
            {
                ++matched;
                if (InvokeRestoreVerb(bin, pidl, expect))
                {
                    ++restored;
                    elog::Write(L"[recycle] 还原 %s", expect.c_str());
                }
                else
                {
                    ++verbFail;
                    elog::Write(L"[recycle] 还原失败 %s", expect.c_str());
                }
            }

            CoTaskMemFree(pidl);
        }
        en->Release();
    }

    if (bin2) bin2->Release();
    bin->Release();
    CoTaskMemFree(pidlBin);
    desktop->Release();

    elog::Write(L"[recycle] 枚举 %d 项，取列失败 %d，命中 %d，动词失败 %d，成功还原 %d（清单 %d 条）",
                seen, infoFail, matched, verbFail, restored, (int)want.size());

    // 只有「清单条数 == 成功还原数」才消费清单。
    // 早先按 restored>0 就删，结果一次假成功把清单误删了，剩下 27 个文件困在回收站里。
    if (haveManifest && restored >= (int)want.size() && !want.empty())
    {
        const std::wstring p = ManifestPath();
        if (!p.empty()) DeleteFileW(p.c_str());
        elog::Write(L"[recycle] 全部还原完毕，清单已消费");
    }

    return restored;
}

} // namespace recycle
