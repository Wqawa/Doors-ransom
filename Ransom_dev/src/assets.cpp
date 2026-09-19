












#include "assets.h"

#include "entity_log.h"

#include <windows.h>

#include <map>

namespace {


const int kManifestResId = 1000;

std::wstring g_diskDir[assets::KIND_COUNT];


std::map<std::wstring, int> g_index[assets::KIND_COUNT];
bool                        g_indexBuilt = false;
int                         g_indexCount = 0;


bool IsDir(const std::wstring& p)
{
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring EnsureSlash(std::wstring d)
{
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d.push_back(L'\\');
    return d;
}


std::wstring ParentOf(std::wstring dir)
{
    dir = EnsureSlash(std::move(dir));
    if (dir.size() <= 1) return L"";
    dir.pop_back();
    const size_t s = dir.find_last_of(L"\\/");
    if (s == std::wstring::npos) return L"";
    return dir.substr(0, s + 1);
}


bool ReadResource(int id, assets::Blob& out)
{
    HMODULE self = GetModuleHandleW(nullptr);

    HRSRC h = FindResourceW(self, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!h) return false;

    const DWORD sz = SizeofResource(self, h);
    if (sz == 0) return false;

    HGLOBAL g = LoadResource(self, h);
    const void* p = g ? LockResource(g) : nullptr;
    if (!p) return false;

    out.bytes.assign((const unsigned char*)p, (const unsigned char*)p + sz);
    return true;
}



void BuildIndex()
{
    if (g_indexBuilt) return;
    g_indexBuilt = true;

    assets::Blob mani;
    if (!ReadResource(kManifestResId, mani))
    {
        elog::Write(L"[assets] 找不到素材清单资源 %d —— 这个 exe 没有打包素材？",
                    kManifestResId);
        return;
    }


    const char* p   = (const char*)mani.Data();
    const char* end = p + mani.Size();

    int bad = 0;
    while (p < end)
    {

        const char* nl = p;
        while (nl < end && *nl != '\n' && *nl != '\r') ++nl;

        const char* t1 = nullptr;
        const char* t2 = nullptr;
        for (const char* q = p; q < nl; ++q)
        {
            if (*q != '\t') continue;
            if (!t1) t1 = q;
            else if (!t2) { t2 = q; break; }
        }

        if (t1 && t2)
        {
            const std::string tag(p, t1);
            const std::string nm(t1 + 1, t2);

            int kind = -1;
            if      (tag == "ROOT") kind = assets::KIND_ROOT;
            else if (tag == "IMG")  kind = assets::KIND_IMAGE;
            else if (tag == "AUD")  kind = assets::KIND_AUDIO;

            const int id = atoi(t2 + 1);
            if (kind >= 0 && id > 0)
            {
                std::wstring wname(nm.begin(), nm.end());
                g_index[kind][wname] = id;
                ++g_indexCount;
            }
            else
            {
                ++bad;
            }
        }

        p = nl;
        while (p < end && (*p == '\n' || *p == '\r')) ++p;
    }

    elog::Write(L"[assets] 素材清单：%d 条%s", g_indexCount,
                bad ? L"（有无法解析的行）" : L"");
}

bool GetEmbedded(assets::Kind k, const wchar_t* name, assets::Blob& out)
{
    BuildIndex();

    std::map<std::wstring, int>::const_iterator it = g_index[k].find(name);
    if (it == g_index[k].end()) return false;

    return ReadResource(it->second, out);
}


bool MatchName(const wchar_t* base, const wchar_t* pattern)
{
    if (!pattern || !*pattern) return true;
    if (wcscmp(pattern, L"*") == 0) return true;

    if (pattern[0] == L'*' && pattern[1] == L'.')
    {
        const wchar_t* dot = wcsrchr(base, L'.');
        return dot && _wcsicmp(dot, pattern + 1) == 0;
    }
    return _wcsicmp(base, pattern) == 0;
}

std::wstring FindFirstEmbedded(assets::Kind k, const wchar_t* pattern)
{
    BuildIndex();


    for (std::map<std::wstring, int>::const_iterator it = g_index[k].begin();
         it != g_index[k].end(); ++it)
        if (MatchName(it->first.c_str(), pattern)) return it->first;

    return L"";
}

bool ReadFileBytes(const std::wstring& full, assets::Blob& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, full.c_str(), L"rb") != 0 || !f) return false;

    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }

    out.bytes.resize((size_t)n);
    const size_t got = fread(out.bytes.data(), 1, (size_t)n, f);
    fclose(f);

    if (got != (size_t)n) { out.bytes.clear(); return false; }
    return true;
}

}

namespace assets {


void UseDiskDir(Kind k, const wchar_t* dir)
{
    if (k < 0 || k >= KIND_COUNT) return;

    if (!dir || !*dir)
    {
        g_diskDir[k].clear();
        return;
    }

    std::wstring d = EnsureSlash(dir);
    if (!IsDir(d))
    {
        elog::Write(L"[assets] 指定的目录不存在，忽略: %s", d.c_str());
        return;
    }
    g_diskDir[k] = d;
}

bool DiskMode(Kind k)
{
    if (k < 0 || k >= KIND_COUNT) return false;
    return !g_diskDir[k].empty();
}

std::wstring DiskDir(Kind k)
{
    if (k < 0 || k >= KIND_COUNT) return L"";
    return g_diskDir[k];
}


bool Get(Kind k, const wchar_t* name, Blob& out)
{
    out.bytes.clear();
    if (k < 0 || k >= KIND_COUNT) return false;
    if (!name || !*name) return false;

    if (!g_diskDir[k].empty())
        return ReadFileBytes(g_diskDir[k] + name, out);

    return GetEmbedded(k, name, out);
}

std::wstring FindFirst(Kind k, const wchar_t* pattern)
{
    if (k < 0 || k >= KIND_COUNT) return L"";

    if (g_diskDir[k].empty()) return FindFirstEmbedded(k, pattern);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_diskDir[k] + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";

    std::wstring found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        found = fd.cFileName;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

std::wstring Where(Kind k, const wchar_t* name)
{
    if (k < 0 || k >= KIND_COUNT || !name) return L"";

    if (g_diskDir[k].empty())
    {
        const wchar_t* sub = (k == KIND_IMAGE) ? L"image/"
                           : (k == KIND_AUDIO) ? L"audio/" : L"";
        std::wstring s = L"exe 内嵌资源 assets/";
        s += sub;
        s += name;
        return s;
    }
    return g_diskDir[k] + name;
}

int EmbeddedCount()
{
    BuildIndex();

    int n = 0;
    for (int k = 0; k < KIND_COUNT; ++k)
        if (g_diskDir[k].empty()) n += (int)g_index[k].size();
    return n;
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return ParentOf(buf);
}

}
