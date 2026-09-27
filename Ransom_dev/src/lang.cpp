// ============================================================================
//  lang.cpp
//
//  语言包的加载与查词。格式、扩展方式见 lang.h 顶部。
//
//  ---- 为什么语言包走 assets 那条路 ----
//
//  用户在设置界面里选的"语言"必须跟着 exe 走，不能依赖外部目录 —— 这个
//  程序本来就是单文件分发的。assets 那套（gen_assets.ps1 -> RCDATA 资源 ->
//  assets.cpp 按名字取字节）正好干这个，所以只是多开一个 KIND_LANG 分类，
//  没有另造一套资源加载。
//
//  ---- 解析 ----
//
//  行式文本，`键 = 值`，`#` / `;` 开头是注释，空行忽略。**值不做转义处理**：
//  这样用户在包里写 `C:\ D:\` 或 `50%` 都是字面意思，不用记 C++ 那套
//  `\\` / `%%`（这两条正是设置界面老文案表里最容易写错的地方）。
//  唯一还是 printf 语义的是 **格式串**：那种键的值要带 %d / %.1f 这些占位符，
//  占位符的个数和顺序不能动（这条写进包文件的开头了）。
// ============================================================================
#include "lang.h"

#include "assets.h"
#include "entity_log.h"

#include <windows.h>

#include <algorithm>
#include <map>

namespace {

struct Pack
{
    std::wstring code;                            // "zh-CN"
    std::wstring name;                            // "简体中文"（包里 meta.name）
    std::map<std::wstring, std::wstring> kv;      // 键 -> 值
};

// 只在 Init() 里填，之后**只读不增删**：T() 返回的 c_str() 依赖这一点
//（vector 不再扩容、map 节点地址稳定）。
std::vector<Pack> g_packs;

int  g_current = 0;
int  g_default = 0;
bool g_init    = false;

std::wstring Trim(std::wstring s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r')) --b;
    return s.substr(a, b - a);
}

// UTF-8 -> UTF-16，顺带吃掉 BOM（记事本另存会加）。
std::wstring Widen(const unsigned char* p, size_t n)
{
    if (!p || n == 0) return L"";

    size_t off = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) off = 3;

    const int src = (int)(n - off);
    if (src <= 0) return L"";

    const int len = MultiByteToWideChar(CP_UTF8, 0, (const char*)p + off, src, nullptr, 0);
    if (len <= 0) return L"";

    std::wstring s((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, (const char*)p + off, src, s.data(), len);
    return s;
}

// "zh-CN.lang" -> "zh-CN"（没有扩展名就整名返回）
std::wstring Stem(const std::wstring& filename)
{
    const size_t dot = filename.find_last_of(L'.');
    return (dot == std::wstring::npos) ? filename : filename.substr(0, dot);
}

bool EndsWithLang(const std::wstring& filename)
{
    const size_t dot = filename.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    return _wcsicmp(filename.c_str() + dot, L".lang") == 0;
}

void ParsePack(const std::wstring& text, Pack& out)
{
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();

        const std::wstring line = Trim(text.substr(pos, nl - pos));
        pos = nl + 1;

        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;          // 没有 '=' 的行当注释处理

        const std::wstring key = Trim(line.substr(0, eq));
        if (key.empty()) continue;

        out.kv[key] = Trim(line.substr(eq + 1));
    }

    // 代号 / 显示名：包里的 meta.* 优先，缺了就退回文件名（代号先定，名字用它兜底）
    std::map<std::wstring, std::wstring>::const_iterator it = out.kv.find(L"meta.code");
    if (it != out.kv.end() && !it->second.empty()) out.code = it->second;

    it = out.kv.find(L"meta.name");
    if (it != out.kv.end() && !it->second.empty()) out.name = it->second;
    if (out.name.empty()) out.name = out.code;
}

} // namespace

namespace lang {

void Init()
{
    if (g_init) return;
    g_init = true;

    const std::vector<std::wstring> files = assets::List(assets::KIND_LANG);

    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!EndsWithLang(files[i])) continue;

        assets::Blob blob;
        if (!assets::Get(assets::KIND_LANG, files[i].c_str(), blob) || !blob.Ok())
        {
            elog::Write(L"[lang] 语言包读不出来，跳过: %s", files[i].c_str());
            continue;
        }

        Pack p;
        p.code = Stem(files[i]);
        ParsePack(Widen(blob.Data(), blob.Size()), p);

        if (p.kv.empty())
        {
            elog::Write(L"[lang] 语言包是空的，跳过: %s", files[i].c_str());
            continue;
        }

        elog::Write(L"[lang] 语言包 %s「%s」：%d 条（%s）",
            p.code.c_str(), p.name.c_str(), (int)p.kv.size(),
            assets::Where(assets::KIND_LANG, files[i].c_str()).c_str());

        g_packs.push_back(p);
    }

    // 顺序按代号排，保证任何机器上跑出来的下拉框顺序都一样
    std::sort(g_packs.begin(), g_packs.end(),
        [](const Pack& a, const Pack& b) { return _wcsicmp(a.code.c_str(), b.code.c_str()) < 0; });

    // 默认语言 = zh-CN（就是改造前那一份文案）；没有它就取第 0 个。
    g_default = 0;
    for (size_t i = 0; i < g_packs.size(); ++i)
        if (_wcsicmp(g_packs[i].code.c_str(), L"zh-CN") == 0) { g_default = (int)i; break; }

    g_current = g_default;

    if (g_packs.empty())
    {
        elog::Write(L"[lang] 一个语言包都没有 —— 界面会直接显示键名，检查 assets\\lang 是否打包进去了");
    }
    else
    {
        elog::Write(L"[lang] 共 %d 种语言，当前 %s（默认 %s）",
            (int)g_packs.size(), CurrentCode(), g_packs[(size_t)g_default].code.c_str());
    }
}

int Count() { return (int)g_packs.size(); }

const wchar_t* Code(int i)
{
    if (i < 0 || i >= (int)g_packs.size()) return L"";
    return g_packs[(size_t)i].code.c_str();
}

const wchar_t* Name(int i)
{
    if (i < 0 || i >= (int)g_packs.size()) return L"";
    return g_packs[(size_t)i].name.c_str();
}

int IndexOfCode(const wchar_t* code)
{
    if (!code || !*code) return -1;
    for (size_t i = 0; i < g_packs.size(); ++i)
        if (_wcsicmp(g_packs[i].code.c_str(), code) == 0) return (int)i;
    return -1;
}

int CurrentIndex() { return (g_packs.empty() ? -1 : g_current); }

const wchar_t* CurrentCode()
{
    if (g_packs.empty()) return L"";
    return g_packs[(size_t)g_current].code.c_str();
}

void SetCurrent(int i)
{
    if (i < 0 || i >= (int)g_packs.size()) return;
    if (g_current == i) return;

    g_current = i;
    elog::Write(L"[lang] 界面语言切换为 %s「%s」",
        g_packs[(size_t)i].code.c_str(), g_packs[(size_t)i].name.c_str());
}

bool SetCurrentCode(const wchar_t* code)
{
    const int i = IndexOfCode(code);
    if (i < 0) return false;
    SetCurrent(i);
    return true;
}

int DefaultIndex() { return (g_packs.empty() ? -1 : g_default); }

int FallbackIndex()
{
    if (!g_init) Init();
    if (g_packs.empty()) return -1;

    // 英文优先：选了的那种包没了，摆一屏英文比摆一屏看不懂的字强
    const int en = IndexOfCode(L"en-US");
    if (en >= 0) return en;

    return g_default;
}

const wchar_t* T(const wchar_t* key)
{
    if (!key || !*key) return L"";
    if (!g_init) Init();

    if (g_packs.empty()) return key;

    const std::map<std::wstring, std::wstring>::const_iterator it =
        g_packs[(size_t)g_current].kv.find(key);
    if (it != g_packs[(size_t)g_current].kv.end()) return it->second.c_str();

    if (g_default != g_current)
    {
        const std::map<std::wstring, std::wstring>::const_iterator d =
            g_packs[(size_t)g_default].kv.find(key);
        if (d != g_packs[(size_t)g_default].kv.end()) return d->second.c_str();
    }

    // 缺词：把键名原样返回 —— 界面上直接看到 ui.xxx.yyy，一眼就知道少了哪条
    return key;
}

} // namespace lang
