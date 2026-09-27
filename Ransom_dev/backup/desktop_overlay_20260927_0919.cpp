// ============================================================================
//  desktop_shortcut_frame.cpp
//
//  作用：检查 Windows 桌面上的所有快捷方式(.lnk / .url)，为每一个生成一个
//        “尺寸自适应 + 圆角 + 透明 + 只有轮廓”的方框，盖在图标上面。
//
//  原理：
//   1. 找到桌面图标所在的 SysListView32
//      （Progman / WorkerW -> SHELLDLL_DefView -> SysListView32）
//   2. 跨进程读取每个图标的文字和矩形：
//      LVM_GETITEMTEXT / LVM_GETITEMRECT / LVM_GETITEMPOSITION 的参数指针
//      **不会被系统自动封送**，必须用 VirtualAllocEx 分配在 explorer.exe
//      的地址空间里，否则拿到的是垃圾数据。
//   3. 用文件系统(当前用户桌面 + 公共桌面)里的 .lnk/.url 名单过滤出快捷方式，
//      这样即使系统设置成“隐藏已知文件扩展名”也能对上。
//   4. 建一个分层窗口(WS_EX_LAYERED)做逐像素透明，GDI+ 画圆角描边，
//      手动预乘 alpha 后 UpdateLayeredWindow 呈现；
//      WS_EX_TRANSPARENT 让鼠标穿透，不会挡住正常点击。
//
//  编译(MinGW-w64 / TDM-GCC，Dev-Cpp 自带的就是)：
//     g++ -std=c++11 -O2 -mwindows desktop_shortcut_frame.cpp -o desktop_shortcut_frame.exe ^
//         -static -lgdiplus -lcomctl32 -luser32 -lgdi32 -lshell32 -lole32
//     （加 -mwindows 就没有控制台窗口；不加则保留日志输出）
//
//  编译(MSVC)：
//     cl /std:c++14 /EHsc /O2 desktop_shortcut_frame.cpp /link gdiplus.lib comctl32.lib user32.lib shell32.lib
//
//  运行：
//     desktop_shortcut_frame.exe              # 画框；Ctrl+Alt+Q 退出
//     desktop_shortcut_frame.exe --list       # 只列出检测结果，不画框
//     desktop_shortcut_frame.exe --topmost    # 置顶显示(默认贴在桌面层)
//     desktop_shortcut_frame.exe --help
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>      // IFolderView / IShellView / SVGIO_ALLVIEW
#include <shlguid.h>
#include <exdisp.h>        // IShellWindows / CLSID_ShellWindows / SWC_DESKTOP
#include <servprov.h>      // IServiceProvider
#include <shlwapi.h>       // StrRetToBufW
#include <oleauto.h>
#include <gdiplus.h>

#include "desktop_overlay.h"
#include "entity_log.h"
#include "face.h"
#include "lockdown.h"
#include "settings.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Gdiplus;
using std::min;   // MinGW / NOMINMAX 环境下 windows.h 不提供 min/max 宏
using std::max;

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

// ============================================ 模块标识
static const wchar_t* kAppName        = L"DesktopShortcutFrame";
static const wchar_t* kOverlayClass   = L"DesktopOverlayWnd";
static const UINT_PTR kOverlayTimerId = 1;

// ------------------------------------------------------------------ 配置 ----
struct Options {
    // 外观
    BYTE     alpha     = 220;                   // 轮廓不透明度 0-255
    COLORREF color     = RGB(0x2E, 0xB8, 0xFF); // 轮廓颜色
    float    thickness = 2.0f;                  // 线宽(px)
    float    radius    = 10.0f;                 // 圆角半径(px)
    float    pad       = 4.0f;                  // 方框比图标外扩多少(px)
    bool     glow      = true;                  // 外发光，深浅壁纸都看得见

    // 行为
    int  interval = 300;                        // 重扫间隔(ms)
    bool topmost  = false;                      // true=最顶层；false=贴桌面层
    bool hotkeys  = true;
    bool quiet    = false;                      // 不打印每次扫描结果
    bool verbose  = false;                      // 打印原始矩形/格子尺寸，便于调参
    bool listOnly = false;                      // 只列结果不画框
    int  boxMode  = 2;                          // 0=只框图标 1=只框文字 2=图标+文字
    overlay::Look look = overlay::LOOK_FRAME;   // 普通标记 / 已加密
};

static Options g_opt;

// 全屏红底（锁定态用）。它画在方框**之前**，
// 所以停牌标识天然压在它上面，不会被盖住。
static BYTE     g_veilAlpha    = 0;
static COLORREF g_veilColor    = RGB(200, 20, 20);

// 锁定态强制置顶：红底要盖住普通窗口。
static bool     g_forceTopmost = false;

// ------------------------------------------------------------ 诊断日志 ----
// 统一走 elog 模块，这里只是个短别名，避免全文件改调用点。
#define DLog elog::Write

// 重复性日志的限流闸。
// 锁定态刷新间隔是 70ms，而下面几条日志**每次重扫都会写**，
// 一次完整演出下来能刷到一万行，把真正有用的行全淹掉
// （实测：10264 行里 2550 行是「[VM] 命中」，2392 行是「Overlay::Render」）。
// 所以同一类消息只在「结果变了」或者超过 minGapMs 时才算通过。
// 注意：只限流日志，**不影响重绘**——停牌图标每帧抖动是必须的。
struct LogGate {
    DWORD lastMs  = 0;
    int   lastSig = -1;

    bool Pass(int sig, DWORD minGapMs = 5000) {
        const DWORD now = GetTickCount();
        if (sig == lastSig && (now - lastMs) < minGapMs) return false;
        lastSig = sig;
        lastMs  = now;
        return true;
    }
};

// ------------------------------------------------------------ 小工具函数 ----
static std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    for (size_t i = 0; i < r.size(); ++i) r[i] = (wchar_t)towlower(r[i]);
    return r;
}

static void PrintUtf8(const wchar_t* s) {
    if (!s) return;
    char buf[8192];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (n > 0) fputs(buf, stdout);
    fflush(stdout);
}

static void PrintLine(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, _countof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[_countof(buf) - 1] = 0;
    PrintUtf8(buf);
    fputc('\n', stdout);
    fflush(stdout);
}

static bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    std::wstring a = ToLower(s), b = ToLower(suffix);
    return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0;
}

static bool IsShortcutFileName(const std::wstring& name) {
    return EndsWithNoCase(name, L".lnk") || EndsWithNoCase(name, L".url");
}

// ============================================ 第 1 步：桌面上有哪些快捷方式
static void ScanFolderForShortcuts(const wchar_t* dir, std::set<std::wstring>& out) {
    if (!dir || !*dir) return;
    std::wstring pattern = std::wstring(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fileName = fd.cFileName;
        if (!IsShortcutFileName(fileName)) continue;
        out.insert(ToLower(fileName));                       // 带扩展名
        size_t dot = fileName.find_last_of(L'.');
        if (dot != std::wstring::npos)
            out.insert(ToLower(fileName.substr(0, dot)));    // 隐藏扩展名时的显示名
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 扫一个目录里的**全部**条目（不只是快捷方式）。
// 写法规则和 ScanFolderForShortcuts 完全一致：带扩展名 + 去扩展名的都塞进去，
// 这样系统设成"隐藏已知文件扩展名"时也能对上。
static void ScanFolderForAll(const wchar_t* dir, std::set<std::wstring>& out) {
    if (!dir || !*dir) return;
    std::wstring pattern = std::wstring(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring fileName = fd.cFileName;
        if (fileName == L"." || fileName == L"..") continue;
        out.insert(ToLower(fileName));                       // 带扩展名
        size_t dot = fileName.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0)
            out.insert(ToLower(fileName.substr(0, dot)));    // 隐藏扩展名时的显示名
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void CollectShortcutNames(std::set<std::wstring>& out,
                                std::vector<std::wstring>& folders,
                                std::set<std::wstring>& allOut) {
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path))) {
        folders.push_back(path);
        ScanFolderForShortcuts(path, out);
        ScanFolderForAll(path, allOut);
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, path))) {
        folders.push_back(path);
        ScanFolderForShortcuts(path, out);
        ScanFolderForAll(path, allOut);
    }
}

// ============================================ 第 2 步：找到桌面列表视图
struct DesktopRef {
    HWND hProgman  = NULL;
    HWND hDefView  = NULL;
    HWND hHost     = NULL;   // 覆盖窗口的父窗口(Progman 或某个 WorkerW)
    HWND hListView = NULL;

    bool Valid() const { return hListView && IsWindow(hListView) && IsWindow(hHost); }
};

static BOOL CALLBACK FindDefViewProc(HWND hwnd, LPARAM lp) {
    HWND hDef = FindWindowExW(hwnd, NULL, L"SHELLDLL_DefView", NULL);
    if (hDef) {
        *(HWND*)lp = hDef;
        return FALSE;
    }
    return TRUE;
}

static bool DiscoverDesktop(DesktopRef& d) {
    d = DesktopRef();

    HWND hProgman = FindWindowW(L"Progman", NULL);
    HWND hDef = hProgman ? FindWindowExW(hProgman, NULL, L"SHELLDLL_DefView", NULL) : NULL;
    if (!hDef) {
        // 桌面图标也可能挂在顶层 WorkerW 下
        HWND found = NULL;
        EnumWindows(FindDefViewProc, (LPARAM)&found);
        hDef = found;
    }
    if (!hDef) return false;

    d.hProgman = hProgman;
    d.hDefView = hDef;
    d.hHost    = GetParent(hDef);
    if (!d.hHost) d.hHost = hProgman;
    d.hListView = FindWindowExW(hDef, NULL, L"SysListView32", NULL);
    return d.Valid();
}

// ============================================ 第 3 步：跨进程读列表项
class ListViewReader {
public:
    explicit ListViewReader(HWND hLV) : m_lv(hLV), m_proc(NULL), m_remote(NULL) {}
    ~ListViewReader() { Close(); }

    bool Open() {
        DWORD pid = 0;
        GetWindowThreadProcessId(m_lv, &pid);
        if (!pid) return false;
        m_proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                             FALSE, pid);
        if (!m_proc) return false;
        m_remote = VirtualAllocEx(m_proc, NULL, kRemoteSize, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
        if (!m_remote) { Close(); return false; }
        return true;
    }

    void Close() {
        if (m_remote && m_proc) VirtualFreeEx(m_proc, m_remote, 0, MEM_RELEASE);
        if (m_proc) CloseHandle(m_proc);
        m_remote = NULL;
        m_proc = NULL;
    }

    int Count() const { return (int)SendMessageW(m_lv, LVM_GETITEMCOUNT, 0, 0); }

    bool Text(int i, std::wstring& out) {
        if (!m_proc) return false;
        BYTE* textRemote = (BYTE*)m_remote + sizeof(LVITEMW);

        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask       = LVIF_TEXT;
        item.iItem      = i;
        item.iSubItem   = 0;
        item.pszText    = (LPWSTR)textRemote;
        item.cchTextMax = kMaxChars;

        SIZE_T written = 0;
        if (!WriteProcessMemory(m_proc, m_remote, &item, sizeof(item), &written)) return false;
        SendMessageW(m_lv, LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)m_remote);

        wchar_t buf[kMaxChars];
        ZeroMemory(buf, sizeof(buf));
        if (!ReadProcessMemory(m_proc, textRemote, buf, (kMaxChars - 1) * sizeof(wchar_t), &written))
            return false;
        buf[kMaxChars - 1] = 0;
        out = buf;
        return !out.empty();
    }

    // type = LVIR_ICON / LVIR_LABEL / LVIR_BOUNDS；约定：结构体的 left 字段放类型
    bool Rect(int i, int type, RECT& out) {
        if (!m_proc) return false;
        RECT seed;
        seed.left = type;
        seed.top = seed.right = seed.bottom = 0;

        SIZE_T written = 0;
        if (!WriteProcessMemory(m_proc, m_remote, &seed, sizeof(seed), &written)) return false;
        if (!SendMessageW(m_lv, LVM_GETITEMRECT, (WPARAM)i, (LPARAM)m_remote)) return false;
        if (!ReadProcessMemory(m_proc, m_remote, &out, sizeof(RECT), &written)) return false;

        long w = out.right - out.left, h = out.bottom - out.top;
        const long kLimit = 100000;               // 挡住垃圾数据
        if (w <= 0 || h <= 0 || w > kLimit || h > kLimit) return false;
        if (out.left < -kLimit || out.top < -kLimit) return false;
        return true;
    }

    bool Position(int i, POINT& out) {
        if (!m_proc) return false;
        SIZE_T written = 0;
        POINT seed = {0, 0};
        if (!WriteProcessMemory(m_proc, m_remote, &seed, sizeof(seed), &written)) return false;
        if (!SendMessageW(m_lv, LVM_GETITEMPOSITION, (WPARAM)i, (LPARAM)m_remote)) return false;
        return ReadProcessMemory(m_proc, m_remote, &out, sizeof(POINT), &written) != 0;
    }

    DWORD Spacing() const { return (DWORD)SendMessageW(m_lv, LVM_GETITEMSPACING, FALSE, 0); }

private:
    static const int kMaxChars   = 512;
    static const SIZE_T kRemoteSize = 8192;

    HWND   m_lv;
    HANDLE m_proc;
    void*  m_remote;
};

// ============================================ 方框数据
struct Frame {
    std::wstring name;
    RECT         rc;    // 屏幕坐标，已含 pad
};

// ---- 锁定态的故障粒子 ----
// 每个锁定方框每隔几秒闪一下：1-4 个**很小**的 A-90_JUMPSCARE 图，
// 尺寸和宽扁比例都随机。纯粹是氛围——盯着"已加密"的方框看久了
// 会看到它们内部跳出几张一闪而过的脸。
//
// 生命周期按**帧**算（不是毫秒）：锁定态刷新间隔是 70ms，
// 3 帧 ≈ 210ms，够短促，也不至于一帧就没了看不出来。
//
// **定义位置**：必须在 Overlay 类之前。Overlay::Render 里要用到这两个
// 全局量，而模块状态那个匿名 namespace 在 Overlay 后面——放那里会编译
// 报"未声明的标识符"。
struct GlitchParticle
{
    float x, y;          // 屏幕坐标（中心点）
    float w, h;          // 绘制尺寸（宽高比随机）
    BYTE  startAlpha;    // 出生时的 alpha
    int   life;          // 剩余帧数
    int   maxLife;
};

std::vector<GlitchParticle> g_glitchParticles;

// 每个锁定方框一个**独立**的下次闪烁时刻。
//
// key 用方框的 (left, top)：这个位置在锁定期间基本不会变，
// 拿它当身份比用 rects 数组下标稳——数组每次重扫都可能重排，
// 用下标会把 A 的计时挪到 B 身上。
//
// 方框不在本轮 rects 里了（解锁、图标被拖走、桌面重排……）就在
// Render 里顺手把它删掉，免得 map 无限增长。
std::map<std::pair<LONG, LONG>, DWORD> g_frameNextBurst;

// ============================================ 硬核：随机锁定"非快捷方式"
//
// 除了快捷方式，硬核还会随机挑几个桌面上的**文件夹 / 文件**锁上一段
// 随机时长（设置里那条 0.9~18 秒的两珠滑条，默认 0.9~9 秒）；
// 到点后按"付清消散"那套**变绿 + 淡出**单独解锁。
//
// 身份键和上面一样用方框的 (left, top)：桌面重排时不能拿下标当身份，
// 数组每次重扫都可能重排，用下标会把 A 的计时挪到 B 身上。
//
// 每一项的走向（这三步合起来保证"同一个项不会被同时锁两次"）：
//   没有记录 -> 新锁一个（同时最多 settings::ExtraLockCount() 个）
//   到了 unlockAt -> 进入变绿淡出窗口（kExtraFadeMs）
//   淡出完 -> 进冷却（settings::kExtraLockCooldownMs），冷却期内不再被选中
//
// 生命周期：g_extraLockOn 为假、退出锁定态、付清消散开始时一律清空
// （见 Render 开头那段和 OverlayTick 的复位分支）。
bool g_extraLockOn = false;
std::map<std::pair<LONG, LONG>, DWORD> g_extraUnlockAt;
std::map<std::pair<LONG, LONG>, DWORD> g_extraFadeAt;
std::map<std::pair<LONG, LONG>, DWORD> g_extraCoolAt;

// 变绿 + 淡出的时长（观感和付清消散一致，只是进度按项算而不是全屏一波）。
const DWORD kExtraFadeMs = 600;

// 下面两个数组和 Render 收到的 rects **同索引**：
//   1 = 这一项正在变绿淡出，0 = 正常锁定。
// 之所以用同索引的全局数组而不是给 Render 加参数：Render 有 4 个调用点，
// 改签名要一起动；这两个数组只在 RefreshInternal 里填、填完立刻用，
// 索引对齐是唯一约定。
std::vector<BYTE>  g_rectExtraFade;
std::vector<DWORD> g_rectFadeElapsed;

// 桌面上的**全部**条目名（小写，带扩展名与去扩展名两种写法都塞进去）。
//
// 和 g_names（只有 .lnk/.url）的区别：这个是全部条目，用作"非快捷方式"的
// **白名单** —— 只有真在磁盘上存在的条目才会被锁。这一条同时解决了两件事：
//   * "此电脑""回收站"这类虚拟项根本不在磁盘上 -> 不在名单里，天然排除；
//   * 我们自己生成的金币 .lnk 是在 overlay::Start() **之后**才出现的
//     -> 也不在名单里，同样被排除。这正是要的：金币不该被当文件夹锁上。
// 名单只在 Start() 里建一次，所以"演出期间新建的桌面文件"不会被锁，可接受。
std::set<std::wstring> g_allNames;

// ---- 付清赎金后的桌面消散动画 ----
// 见 desktop_overlay.h 里 BeginPaidClear 的说明。
bool  g_paidClearActive = false;
DWORD g_paidClearStart = 0;

// 总时长。刻意略短于 popup 那边窗口飞回中心的 1000ms，
// 这样窗口起飞时桌面已经收拾干净了。
const DWORD kPaidClearTotalMs = 900;

static bool BuildFramesViaListView(const DesktopRef& d,
                                   const std::set<std::wstring>& shortcutNames,
                                   std::vector<Frame>& frames,
                                   std::vector<Frame>& others) {
    frames.clear();
    others.clear();
    if (!d.Valid()) { DLog(L"  [VM] 桌面引用无效"); return false; }

    ListViewReader reader(d.hListView);
    if (!reader.Open())
    {
        // 重扫间隔只有几百毫秒，这条每次都会失败。限流，别把日志刷爆。
        static DWORD s_lastVM = 0;
        const DWORD now = GetTickCount();
        if (now - s_lastVM > 10000)
        {
            s_lastVM = now;
            DLog(L"  [VM] reader.Open 失败 (OpenProcess/VirtualAllocEx), err=%lu", GetLastError());
        }
        return false;
    }

    const int count = reader.Count();
    DWORD spacing = reader.Spacing();
    long cellW = LOWORD(spacing), cellH = HIWORD(spacing);

    static LogGate gateFrames;
    if (gateFrames.Pass(count))
        DLog(L"  BuildFrames: 列表项=%d 候选名=%d 格子=%ldx%ld",
             count, (int)shortcutNames.size(), cellW, cellH);

    for (int i = 0; i < count; ++i) {
        std::wstring text;
        if (!reader.Text(i, text)) continue;

        const std::wstring lower = ToLower(text);
        const bool isShortcut = (shortcutNames.find(lower) != shortcutNames.end());

        // 不是快捷方式的那些：硬核要拿它们做"随机锁定"，所以另收一份。
        // 收的条件很严 —— 必须**确实在桌面上存在**（g_allNames 白名单）。
        // 虚拟项（此电脑/回收站）和我们自己生成的金币都不在名单里，
        // 于是天然被排除（理由见 g_allNames 的注释）。
        if (!isShortcut &&
            !(g_extraLockOn && !text.empty() && g_allNames.find(lower) != g_allNames.end()))
            continue;

        RECT rcIcon = {0}, rcLabel = {0};
        bool hasIcon  = reader.Rect(i, LVIR_ICON,  rcIcon);
        bool hasLabel = reader.Rect(i, LVIR_LABEL, rcLabel);

        if (!hasIcon && !hasLabel) {
            // 两个都拿不到：图标位置 + 格子大小兜底
            POINT pt;
            if (!reader.Position(i, pt) || cellW <= 0 || cellH <= 0) continue;
            SetRect(&rcIcon, pt.x, pt.y, pt.x + cellW, pt.y + (long)(cellH * 0.55));
            hasIcon = true;
        }

        // ---- 算出贴合这个快捷方式的方框(列表视图客户区坐标) ----
        // 纵向：从图标顶部到文字底部；横向：以图标中线为中心，宽度取
        //      max(图标区高度, 文字宽度)，并且**不超过格子宽度**，
        //      否则相邻两列的方框会互相压到一起。
        long pad = (long)(g_opt.pad + 0.5f);
        const long kGap = 2;   // 相邻方框之间至少留的空隙

        long top    = hasIcon ? rcIcon.top : rcLabel.top;
        long bottom = hasIcon ? rcIcon.bottom : rcLabel.bottom;
        if (hasIcon && hasLabel) bottom = max(bottom, rcLabel.bottom);
        long centerX = hasIcon ? (rcIcon.left + rcIcon.right) / 2
                               : (rcLabel.left + rcLabel.right) / 2;

        long iconSize = hasIcon ? (rcIcon.bottom - rcIcon.top) : 0;   // 图标区高度≈图标可视尺寸
        long labelW   = hasLabel ? (rcLabel.right - rcLabel.left) : 0;

        long halfW;
        if (g_opt.boxMode == 0)      halfW = iconSize / 2;            // 只框图标
        else if (g_opt.boxMode == 1) halfW = labelW / 2;              // 只框文字
        else                         halfW = max(iconSize, labelW) / 2; // 默认：图标+文字
        halfW += pad;

        if (cellW > 0) {                     // 格子宽度限制，避免和邻列方框重叠
            long limit = cellW / 2 - kGap;
            if (limit > 6 && halfW > limit) halfW = limit;
        }

        RECT rc;
        rc.left   = centerX - halfW;
        rc.right  = centerX + halfW;
        rc.top    = top - pad;
        rc.bottom = bottom + pad;

        if (g_opt.verbose)
            PrintLine(L"    %-28s icon=(%ld,%ld,%ld,%ld) label=(%ld,%ld,%ld,%ld) cell=%ldx%ld -> box=(%ld,%ld,%ld,%ld)",
                      text.c_str(),
                      rcIcon.left, rcIcon.top, rcIcon.right, rcIcon.bottom,
                      rcLabel.left, rcLabel.top, rcLabel.right, rcLabel.bottom,
                      cellW, cellH,
                      rc.left, rc.top, rc.right, rc.bottom);

        // 列表视图客户区坐标 -> 屏幕坐标
        MapWindowPoints(d.hListView, NULL, (LPPOINT)&rc, 2);

        Frame f;
        f.name = text;
        f.rc = rc;

        if (isShortcut) frames.push_back(f);
        else            others.push_back(f);   // 硬核的"非快捷方式"候选
    }
    static LogGate gateHit;
    if (gateHit.Pass((int)frames.size()))
        DLog(L"  [VM] 命中 %d 个", (int)frames.size());
    return true;
}

// ---------------------------------------------------------------------------
//  路径 B：IFolderView COM
//
//  路径 A 要 OpenProcess(explorer) + VirtualAllocEx + WriteProcessMemory，
//  在下列环境会拿到 ERROR_ACCESS_DENIED(5)：
//    * 受限令牌 / 沙箱 / 低完整性进程
//    * 装了外壳钩子的安全软件
//  这里改用 Shell 自己暴露的 IFolderView 接口拿「条目数 + 位置 + 名字」，
//  COM 由系统负责跨进程封送，完全不需要碰 explorer 的内存。
//
//  代价：拿不到精确的图标/文字矩形，只能用格子尺寸近似，
//  所以方框会比路径 A 略大略方——但整齐，而且到哪儿都能用。
// ---------------------------------------------------------------------------
static bool BuildFramesViaFolderView(const DesktopRef& d,
                                     const std::set<std::wstring>& shortcutNames,
                                     std::vector<Frame>& frames)
{
    frames.clear();

    IShellWindows* psw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&psw))) || !psw)
    {
        DLog(L"  [COM] CoCreateInstance(ShellWindows) 失败");
        return false;
    }

    bool              ok    = false;
    IDispatch*        pdisp = nullptr;
    IServiceProvider* psp   = nullptr;
    IShellBrowser*    psb   = nullptr;
    IShellView*       psv   = nullptr;
    IFolderView*      pfv   = nullptr;
    IShellFolder*     psf   = nullptr;

    do
    {
        VARIANT vEmpty;
        VariantInit(&vEmpty);
        long lhwnd = 0;

        if (FAILED(psw->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, &lhwnd,
                                     SWFO_NEEDDISPATCH, &pdisp)) || !pdisp)
        {
            DLog(L"  [COM] FindWindowSW(SWC_DESKTOP) 失败");
            break;
        }
        if (FAILED(pdisp->QueryInterface(IID_PPV_ARGS(&psp))) || !psp)
        {
            DLog(L"  [COM] QueryInterface(IServiceProvider) 失败");
            break;
        }
        if (FAILED(psp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&psb))) || !psb)
        {
            DLog(L"  [COM] QueryService(SID_STopLevelBrowser) 失败");
            break;
        }
        if (FAILED(psb->QueryActiveShellView(&psv)) || !psv)
        {
            DLog(L"  [COM] QueryActiveShellView 失败");
            break;
        }
        if (FAILED(psv->QueryInterface(IID_PPV_ARGS(&pfv))) || !pfv)
        {
            DLog(L"  [COM] QueryInterface(IFolderView) 失败");
            break;
        }

        int count = 0;
        if (FAILED(pfv->ItemCount(SVGIO_ALLVIEW, &count)))
        {
            DLog(L"  [COM] ItemCount 失败");
            break;
        }

        pfv->GetFolder(IID_PPV_ARGS(&psf));   // 失败不致命，只是拿不到名字

        // 格子尺寸：LVM_GETITEMSPACING 直接返回一个 DWORD，不需要跨进程封送
        DWORD spacing = (DWORD)SendMessageW(d.hListView, LVM_GETITEMSPACING, FALSE, 0);
        long cellW = LOWORD(spacing);
        long cellH = HIWORD(spacing);
        if (cellW <= 0 || cellH <= 0) { cellW = 76; cellH = 76; }   // 兜底

        for (int i = 0; i < count; ++i)
        {
            PITEMID_CHILD pidl = nullptr;
            if (FAILED(pfv->Item(i, &pidl)) || !pidl) continue;

            // 注意：IFolderView::GetItemPosition 收的是 PIDL，不是索引
            POINT pt = { 0, 0 };
            const HRESULT hrPos = pfv->GetItemPosition(pidl, &pt);

            std::wstring display, parsing;
            if (psf)
            {
                wchar_t buf[MAX_PATH] = { 0 };
                STRRET sr;

                if (SUCCEEDED(psf->GetDisplayNameOf(pidl, SHGDN_INFOLDER, &sr)) &&
                    SUCCEEDED(StrRetToBufW(&sr, pidl, buf, MAX_PATH)))
                    display = buf;

                if (SUCCEEDED(psf->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &sr)) &&
                    SUCCEEDED(StrRetToBufW(&sr, pidl, buf, MAX_PATH)))
                    parsing = buf;
            }
            CoTaskMemFree(pidl);

            if (FAILED(hrPos)) continue;

            // 用「解析路径」判断是不是快捷方式（虚拟项如「此电脑」会返回 ::{GUID}）
            if (parsing.empty() || !IsShortcutFileName(parsing)) continue;
            // 用「显示名」跟文件系统名单对上
            if (shortcutNames.find(ToLower(display)) == shortcutNames.end()) continue;

            RECT rc;
            rc.left   = pt.x + 2;
            rc.top    = pt.y + 2;
            rc.right  = pt.x + cellW - 2;
            rc.bottom = pt.y + cellH - 2;

            if (rc.right - rc.left < 8 || rc.bottom - rc.top < 8) continue;

            // 列表视图客户区坐标 -> 屏幕坐标
            MapWindowPoints(d.hListView, NULL, (LPPOINT)&rc, 2);

            Frame f;
            f.name = display;
            f.rc   = rc;
            frames.push_back(f);
        }

        DLog(L"  [COM] 列表项=%d 命中=%d 格子=%ldx%ld",
             count, (int)frames.size(), cellW, cellH);
        ok = true;
    } while (false);

    if (pfv)   pfv->Release();
    if (psf)   psf->Release();
    if (psv)   psv->Release();
    if (psb)   psb->Release();
    if (psp)   psp->Release();
    if (pdisp) pdisp->Release();
    psw->Release();

    return ok;
}

// 先试精确的路径 A，不行再退到路径 B
static bool BuildFrames(const DesktopRef& d, const std::set<std::wstring>& shortcutNames,
                        std::vector<Frame>& frames, std::vector<Frame>& others)
{
    frames.clear();
    others.clear();
    if (!d.Valid()) { DLog(L"  BuildFrames: 桌面引用无效"); return false; }

    if (BuildFramesViaListView(d, shortcutNames, frames, others))
        return true;

    // 同上，限流
    {
        static DWORD s_lastFallback = 0;
        const DWORD now = GetTickCount();
        if (now - s_lastFallback > 10000)
        {
            s_lastFallback = now;
            DLog(L"  BuildFrames: 路径 A 不可用，改走 IFolderView COM");
        }
    }
    frames.clear();
    others.clear();   // 兜底路径不收"非快捷方式"：COM 那边拿不到可靠的显示名 -> 磁盘名的对应
    return BuildFramesViaFolderView(d, shortcutNames, frames);
}

// ============================================ 第 4 步：圆角透明覆盖窗口
class Overlay {
public:
    // hDesktopHint: 桌面(Progman/WorkerW)窗口，用来确定 z 序插入点
    bool Create(bool topmost, HWND hDesktopHint) {
        m_topmost = topmost;
        m_hint    = hDesktopHint;

        DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        // 注意：不能用 WS_CHILD 挂到 explorer 的 WorkerW 下面——
        // 跨进程创建子窗口会失败(CreateWindowEx 返回 NULL，SetParent 也会失败)。
        // 所以这里用普通弹出式窗口，靠 z 序插到 "壁纸之上、普通窗口之下"。
        //
        // 另外：建窗口时**直接带上最终位置和尺寸**，不要随后再用
        // SetWindowPos(SWP_NOZORDER) 调整——在某些环境下(装了外壳钩子的机器)
        // 那会让紧接着的置顶调用失效。
        RECT want = VirtualScreenRect();
        m_hwnd = CreateWindowExW(exStyle, kOverlayClass, kAppName, WS_POPUP,
                                 want.left, want.top,
                                 want.right - want.left, want.bottom - want.top,
                                 NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (!m_hwnd) return false;

        DLog(L"Overlay::Create: 覆盖窗口已建 hwnd=0x%p 尺寸=%ldx%ld err=%lu",
             (void*)m_hwnd, (long)(want.right - want.left), (long)(want.bottom - want.top),
             GetLastError());

        m_screenDC = GetDC(NULL);
        if (!m_screenDC) return false;

        ApplyZOrder(true);                  // 第一件 z 序操作就是定好层级
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    void Destroy() {
        DestroyBuffers();
        if (m_screenDC) { ReleaseDC(NULL, m_screenDC); m_screenDC = NULL; }
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = NULL; }
    }

    HWND Handle() const { return m_hwnd; }

    static RECT VirtualScreenRect() {
        RECT r;
        r.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
        r.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
        r.right  = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = r.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return r;
    }

    // 分辨率/多屏变化时同步窗口覆盖范围
    void SyncGeometry() {
        if (!m_hwnd) return;
        RECT want = VirtualScreenRect();
        RECT cur;
        GetWindowRect(m_hwnd, &cur);
        if (cur.left == want.left && cur.top == want.top &&
            cur.right == want.right && cur.bottom == want.bottom)
            return;
        SetWindowPos(m_hwnd, NULL, want.left, want.top,
                     want.right - want.left, want.bottom - want.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyZOrder(true);   // 调整过尺寸后重新确认一次层级
    }

    // 重新摆正 z 序：默认贴在桌面之上、其它窗口之下
    void ApplyZOrder(bool force) {
        if (!m_hwnd) return;

        if (m_topmost || g_forceTopmost) {
            // 有些机器(装了外壳钩子/管控软件)第一次 SetWindowPos(HWND_TOPMOST)
            // 会“返回成功但没真正置顶”，所以这里校验一下再补几次。
            for (int i = 0; i < 3; ++i) {
                LONG_PTR es = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
                if (es & WS_EX_TOPMOST) break;
                SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return;
        }

        HWND anchor = FindDesktopAnchor(m_hwnd, m_hint);
        if (!anchor) {   // 找不到桌面窗口，退化为“同级最上”
            SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }
        if (!force && GetWindow(anchor, GW_HWNDPREV) == m_hwnd)
            return;      // 已经就贴在桌面正上方了

        // 关键：SetWindowPos 的 hWndInsertAfter 是“排在这个窗口**后面(下面)**”，
        // 所以要排到 anchor 上面，得插到 anchor 上面那个窗口的后面。
        HWND above = GetWindow(anchor, GW_HWNDPREV);
        SetWindowPos(m_hwnd, above ? above : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void Render(const std::vector<RECT>& rects) {
        if (!m_hwnd || !m_screenDC) return;

        SyncGeometry();
        RECT wr;
        GetWindowRect(m_hwnd, &wr);
        int w = wr.right - wr.left;
        int h = wr.bottom - wr.top;
        if (w <= 0 || h <= 0) return;
        if (!EnsureBuffers(w, h)) return;

        // 已经退出锁定态、或者正在播放付清消散动画：把上一次攒下的粒子和
        // 每个方框的计时一并清掉，免得下次再进锁定态时它们从旧位置继续跳。
        // 付清消散期间粒子会一直保持为空（动画开始时清过一次，之后
        // 生成分支也被禁止了），所以这里只是再兜一道底。
        if (g_opt.look != overlay::LOOK_LOCKED || g_paidClearActive)
        {
            if (!g_glitchParticles.empty()) g_glitchParticles.clear();
            if (!g_frameNextBurst.empty())  g_frameNextBurst.clear();

            // 硬核的"额外锁定"也一并清掉：退出锁定态之后不该留任何记录，
            // 否则下一轮再进锁定态时，这些项会拿着上一轮的解锁时刻直接变绿。
            if (!g_extraUnlockAt.empty()) g_extraUnlockAt.clear();
            if (!g_extraFadeAt.empty())   g_extraFadeAt.clear();
            if (!g_extraCoolAt.empty())   g_extraCoolAt.clear();
        }

        // 本帧需要做预乘 alpha 的区域（只要轮廓覆盖到的范围，省时间）
        std::vector<RECT> dirty;
        dirty.reserve(rects.size());

        {
            Graphics g(m_memDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.Clear(Color(0, 0, 0, 0));   // 全透明底

            // 付清消散期间仍然按锁定外观渲染（只是逐帧变形）：
            // 一旦动画开始，g_opt.look 就不再是 LOOK_LOCKED 的唯一判据。
            const bool paidActive = g_paidClearActive;
            const bool locked = (g_opt.look == overlay::LOOK_LOCKED) || paidActive;
            const BYTE a = locked ? 255 : g_opt.alpha;

            // 全屏红底：画在方框**之前**，所以停牌一定压在它上面
            if (locked && g_veilAlpha > 0)
            {
                SolidBrush veil(Color(g_veilAlpha,
                    GetRValue(g_veilColor),
                    GetGValue(g_veilColor),
                    GetBValue(g_veilColor)));
                // 参数全用 REAL，否则 FillRectangle 在 INT/REAL 重载之间有歧义
                g.FillRectangle(&veil, 0.0f, 0.0f, (REAL)w, (REAL)h);
            }

            // ---- 付清消散：算出每个方框的进度 ----
            // 「从左往右、从上往下」的排序方式：先按 top（行），同行的再按 left（列）。
            // 每个方框有它自己的开始时刻，所以它们会依次动起来；每个方框自身的
            // 动画时长相同，所以看起来像一波从左上扫到右下。
            //
            // 注意用 rects 而不是 g_hitRects：两者尺寸相同，但 rects 是这一帧
            // 真正要画的那一份（含抖动）。抖动幅度只有 ±g_jitter px，
            // 排序结果在帧间是稳定的，不会来回翻。
            std::vector<float> paidP;
            if (paidActive && !rects.empty())
            {
                paidP.assign(rects.size(), 0.0f);

                const size_t N = rects.size();
                std::vector<size_t> order(N);
                for (size_t i = 0; i < N; ++i) order[i] = i;
                std::sort(order.begin(), order.end(),
                    [&rects](size_t p, size_t q) {
                        if (rects[p].top != rects[q].top)  return rects[p].top < rects[q].top;
                        return rects[p].left < rects[q].left;
                    });

                const DWORD elapsed = GetTickCount() - g_paidClearStart;
                const DWORD perBoxMs = kPaidClearTotalMs * 5 / 10;   // 每个方框自身的动画时长
                const DWORD staggerMs = kPaidClearTotalMs - perBoxMs; // 首尾错开的总时长
                const double denom = (N > 1) ? (double)(N - 1) : 1.0;

                for (size_t rank = 0; rank < N; ++rank)
                {
                    const DWORD startAt = (N > 1)
                        ? (DWORD)((double)staggerMs * (double)rank / denom)
                        : 0;
                    long long t = (long long)elapsed - (long long)startAt;
                    if (t < 0) t = 0;
                    if (t > (long long)perBoxMs) t = perBoxMs;
                    paidP[order[rank]] = (float)t / (float)perBoxMs;
                }

                // 消散动画期间整窗都在变（每个方框的位置/颜色/大小都不一样），
                // 直接丢一个覆盖全窗的脏区进去，省去逐方框算脏区的麻烦。
                RECT full = { 0, 0, w, h };
                dirty.push_back(full);
            }

            for (size_t i = 0; i < rects.size(); ++i) {
                const RECT& r = rects[i];
                float x = (float)(r.left - wr.left);
                float y = (float)(r.top - wr.top);
                float cx = (float)(r.right - r.left);
                float cy = (float)(r.bottom - r.top);
                if (cx < 1.0f || cy < 1.0f) continue;

                // ---- 付清消散：本帧该方框的进度（0..1） ----
                const float pp = paidActive ? paidP[i] : 0.0f;
                float boxScale = 1.0f;    // 方框整体放大系数
                float boxAlpha = 1.0f;    // 方框（边框 + 底衬）的整体不透明度
                float stopAlpha = 1.0f;    // 停牌自身的 alpha
                float greenMix = 0.0f;    // 边框颜色：0=红 1=绿

                if (paidActive) {
                    // 颜色**立即**变绿，不做红→绿过渡。
                    // 轮到某个方框时它是"啪"一下跳成纯绿，然后才开始消失——
                    // 把"已解锁"这一下强调出来。greenMix 直接给 1.0，
                    // 后面那两处 红→绿 插值公式自然算出 (0,255,0)。
                    greenMix = 1.0f;

                    // 前半段：停牌淡出
                    const float aPhase = (pp < 0.55f) ? (pp / 0.55f) : 1.0f;
                    stopAlpha = 1.0f - aPhase;

                    // 后半段：方框整体淡出（**不放大**）。
                    // 和停牌淡出有 0.10 的重叠，整个消失过程是连贯的一波。
                    if (pp > 0.45f) {
                        const float bPhase = (pp - 0.45f) / 0.55f;
                        boxAlpha = 1.0f - bPhase;
                    }

                    // boxScale 保持 1.0 —— 下面的放大变换会自动跳过。
                }

                // ---- 硬核：额外锁的桌面项自己的"变绿 + 淡出" ----
                //
                // 和付清消散同一套观感，区别只是**进度按项算**（付清那边是
                // 全屏从左到右一波）。你要的"锁定时间一到就让框变绿渐变消失"
                // 就是这一段。
                //
                // 注意这里做的是**真正的红→绿插值**（前 40% 变色），
                // 不是付清那边的"啪一下跳纯绿" —— greenMix 喂给下面
                // outline / 底衬那两处插值公式即可。
                // 付清消散优先：它一旦开始就所有框都走那边，这里不插手。
                if (!paidActive && i < g_rectExtraFade.size() && g_rectExtraFade[i])
                {
                    float e = (float)g_rectFadeElapsed[i] / (float)kExtraFadeMs;
                    if (e < 0.0f) e = 0.0f;
                    if (e > 1.0f) e = 1.0f;

                    greenMix = (e < 0.40f) ? (e / 0.40f) : 1.0f;

                    // 前半段停牌淡出，后半段方框整体淡出（和付清那边同样的比例）
                    stopAlpha = 1.0f - ((e < 0.55f) ? (e / 0.55f) : 1.0f);
                    if (e > 0.45f) boxAlpha = 1.0f - (e - 0.45f) / 0.55f;
                }

                // 放大是绕方框中心做的，不然会从左上角往外长
                if (boxScale != 1.0f) {
                    const float ccx = x + cx * 0.5f;
                    const float ccy = y + cy * 0.5f;
                    x = ccx - (cx * boxScale) * 0.5f;
                    y = ccy - (cy * boxScale) * 0.5f;
                    cx *= boxScale;
                    cy *= boxScale;
                }

                float rad = g_opt.radius;
                float maxRad = min(cx, cy) / 2.0f - 0.5f;
                if (rad > maxRad) rad = maxRad;
                if (rad < 0.0f) rad = 0.0f;

                GraphicsPath path;
                AddRoundRect(path, RectF(x, y, cx, cy), rad);

                // 这一帧这个方框的轮廓颜色（锁定态红→绿渐变，普通态用 g_opt.color）。
                // 颜色和 alpha 都在循环内算，因为付清消散期间每个方框的进度不同。
                Color outline;
                if (locked) {
                    BYTE rr = 235, gg = 24, bb = 24;
                    if (greenMix > 0.0f) {
                        rr = (BYTE)(235.0f * (1.0f - greenMix) + 0.0f * greenMix);
                        gg = (BYTE)(24.0f * (1.0f - greenMix) + 255.0f * greenMix);
                        bb = (BYTE)(24.0f * (1.0f - greenMix) + 0.0f * greenMix);
                    }
                    outline = Color((BYTE)(255.0f * boxAlpha), rr, gg, bb);
                }
                else {
                    outline = Color(a,
                        GetRValue(g_opt.color),
                        GetGValue(g_opt.color),
                        GetBValue(g_opt.color));
                }

                // 外发光：只在非锁定态画。锁定态不要阴影
                if (!locked && g_opt.glow) {
                    const float gw[3] = {9.0f, 6.0f, 4.0f};
                    const int   ga[3] = {26, 46, 70};
                    for (int k = 0; k < 3; ++k) {
                        Pen gp(Color((BYTE)(ga[k] * a / 255),
                                     outline.GetR(), outline.GetG(), outline.GetB()),
                               g_opt.thickness + gw[k]);
                        gp.SetLineJoin(LineJoinRound);
                        g.DrawPath(&gp, &path);
                    }
                }

                // 锁定态：底下垫一层很淡的底衬。
                // 付清消散期间底衬也跟着边框一起红→绿、一起淡出。
                if (locked) {
                    const BYTE fillA = (BYTE)(38.0f * boxAlpha);
                    if (fillA > 0) {
                        const BYTE fr = (BYTE)(235.0f * (1.0f - greenMix) + 0.0f * greenMix);
                        const BYTE fg = (BYTE)(24.0f * (1.0f - greenMix) + 255.0f * greenMix);
                        const BYTE fb = (BYTE)(24.0f * (1.0f - greenMix) + 0.0f * greenMix);
                        SolidBrush fill(Color(fillA, fr, fg, fb));
                        g.FillPath(&fill, &path);
                    }
                }

                Pen pen(outline, locked ? g_opt.thickness + 0.8f : g_opt.thickness);
                pen.SetLineJoin(LineJoinRound);
                g.DrawPath(&pen, &path);

                // 锁定态：方框正中一个小停牌，做小幅度**角度抖动**
                if (locked) {
                    // 图标要够大才看得清（原来 0.42 太小）
                    const float side = min(cx, cy) * 0.78f;
                    if (side > 10.0f) {
                        // 每帧随机一个角度，范围 ±kStopJitterDeg 度。
                        // 想调抖动大小就改 kStopJitterDeg：
                        //   0    = 不抖（静止的停牌）
                        //   3    = 轻微晃
                        //   7    = 原值，看得出在抖
                        //   20   = 明显摇晃，接近"摇摇欲坠"
                        //   45+  = 剧烈乱转，像坏了
                        const float kStopJitterDeg = 3.0f;
                        const float ang = ((float)(rand() % 2001) / 1000.0f - 1.0f)
                            * kStopJitterDeg;

                        RECT sr;
                        sr.left   = (LONG)(x + cx / 2.0f - side / 2.0f);
                        sr.top    = (LONG)(y + cy / 2.0f - side / 2.0f);
                        sr.right  = (LONG)(sr.left + side);
                        sr.bottom = (LONG)(sr.top  + side);

                        HDC hdc = g.GetHDC();
                        face::BlitStopSign(hdc, sr, ang, stopAlpha);
                        g.ReleaseHDC(hdc);
                    }
                }

                // 付清消散期间已经整窗标脏了，跳过逐方框脏区（不重复加）。
                if (!paidActive) {
                    long m = (long)(g_opt.thickness + 12.0f);
                    RECT dirtyRect = { r.left - m, r.top - m, r.right + m, r.bottom + m };
                    MapWindowPoints(NULL, m_hwnd, (LPPOINT)&dirtyRect, 2);
                    dirty.push_back(dirtyRect);
                }
            }

            // ---- 锁定态的故障粒子 ----
            // 每个锁定方框**各自**每隔 4-8 秒闪出 1-4 个缩小的
            // A-90_JUMPSCARE，尺寸、宽高比、透明度、下一次的时刻都随机。
            //
            // 计时是 per-frame 的：g_frameNextBurst 里每个方框一条独立
            // 记录。以前这里是一个全局时刻，所有框一起闪，看起来像整屏
            // 一起掉帧；改成 per-frame 之后它们会各自错开、此起彼伏。
            //
            // 这套逻辑放在方框循环**之后**：粒子是覆盖在停牌图标之上的，
            // 先画方框和停牌再叠粒子，粒子落上去就像方框里冒出来的噪点。
            //
            // paidActive 期间不生成 —— 消散动画开始时粒子已经被清过一次，
            // 这段时间再冒新的会跟"桌面正在收拾干净"的观感打架。
            if (locked && !rects.empty() && !paidActive)
            {
                const DWORD nowTick = GetTickCount();

                // ---- 1. 老化 ----
                // 每帧统一减一。寿命到 0 就删——用 erase 逐个来，
                // 粒子数最多几十个，不值得换成"标记-压缩"两趟。
                for (size_t i = 0; i < g_glitchParticles.size(); )
                {
                    if (--g_glitchParticles[i].life <= 0)
                        g_glitchParticles.erase(g_glitchParticles.begin() + i);
                    else
                        ++i;
                }

                // ---- 2. 每个方框独立推进自己的计时 ----
                //
                // alive 收集本轮还在场上的方框 key，末尾用来清理
                // g_frameNextBurst 里已经消失的条目（解锁、图标被拖走、
                // 桌面重排……），否则 map 会一直涨。
                std::set<std::pair<LONG, LONG> > alive;

                for (size_t ri = 0; ri < rects.size(); ++ri)
                {
                    const RECT& r = rects[ri];
                    const LONG bw = r.right - r.left;
                    const LONG bh = r.bottom - r.top;

                    // 太小放不下粒子的方框直接跳过（正常桌面图标不会这么小，
                    // 但超多列、高 DPI 混排时可能出现）。
                    if (bw < 16 || bh < 16) continue;

                    const std::pair<LONG, LONG> key(r.left, r.top);
                    alive.insert(key);

                    std::map<std::pair<LONG, LONG>, DWORD>::iterator it =
                        g_frameNextBurst.find(key);

                    if (it == g_frameNextBurst.end())
                    {
                        // 第一次见到这个方框：给它排一个 4-8 秒后的
                        // **随机**首闪时刻。每个框抽的时间都不同，
                        // 所以一进锁定态它们就是错开的，不会齐闪。
                        g_frameNextBurst[key] =
                            nowTick + 4000 + (DWORD)(rand() % 4001);
                        continue;
                    }

                    if (nowTick < it->second) continue;   // 这个框还没到点

                    // ---- 到点了：生成本轮粒子 ----
                    const int n = 1 + (rand() % 4);     // 1-4 个
                    int made = 0;

                    for (int k = 0; k < n; ++k)
                    {
                        GlitchParticle p;

                        // 尺寸：**很小**。10-20px 高，这样即使铺满一屏方框
                        // 也不会喧宾夺主 —— 它只是方框里的"杂点"。
                        p.h = 10.0f + (float)(rand() % 11);

                        // 宽扁随机：宽高比 0.4 - 2.5。
                        // 偏扁的多，偏瘦的少；比例再离谱就成了一条线或者
                        // 一个方块，失去"A-90 的脸"的可辨识度。
                        p.w = p.h * (0.4f + (float)(rand() % 211) / 100.0f);

                        // 落点：把粒子整体塞进方框里（含 2px 内边距），
                        // 免得它的边缘越过方框线，看着像是"漏出去了"。
                        const float marginX = p.w * 0.5f + 2.0f;
                        const float marginY = p.h * 0.5f + 2.0f;

                        const LONG xMin = r.left + (LONG)marginX;
                        const LONG xMax = r.right - (LONG)marginX;
                        const LONG yMin = r.top + (LONG)marginY;
                        const LONG yMax = r.bottom - (LONG)marginY;

                        if (xMin >= xMax || yMin >= yMax) continue;

                        p.x = (float)(xMin + (rand() % (xMax - xMin + 1)));
                        p.y = (float)(yMin + (rand() % (yMax - yMin + 1)));

                        // 透明度随机：90-255。压低下限是刻意的 ——
                        // 全不透明会让小图很"实"，和噪点氛围不搭。
                        p.startAlpha = (BYTE)(90 + (rand() % 166));

                        p.maxLife = 3;
                        p.life = p.maxLife;

                        g_glitchParticles.push_back(p);
                        ++made;
                    }

                    // 排下一次：又是 4-8 秒后的独立随机。
                    // 每个框各排各的，所以之后它们的节奏也是错开的。
                    g_frameNextBurst[key] =
                        nowTick + 4000 + (DWORD)(rand() % 4001);

                    if (made > 0)
                        DLog(L"[overlay] 故障粒子：%d 个 @(%ld,%ld)（下次 %.1f 秒后）",
                            made, r.left, r.top,
                            (g_frameNextBurst[key] - nowTick) / 1000.0);
                }

                // ---- 2b. 清掉本轮不在场上的方框 ----
                // 它们在锁定期间消失了（解锁、被拖走、桌面重排），
                // 留着只会让 map 越滚越大，而且以后要是同样的
                // (left, top) 又被占上，会拿一份陈旧的计时用。
                for (std::map<std::pair<LONG, LONG>, DWORD>::iterator
                    it = g_frameNextBurst.begin();
                    it != g_frameNextBurst.end(); )
                {
                    if (alive.find(it->first) == alive.end())
                        it = g_frameNextBurst.erase(it);
                    else
                        ++it;
                }

                // ---- 3. 绘制 ----
                if (!g_glitchParticles.empty())
                {
                    HDC hdc = g.GetHDC();
                    for (size_t i = 0; i < g_glitchParticles.size(); ++i)
                    {
                        const GlitchParticle& p = g_glitchParticles[i];
                        if (p.life <= 0) continue;

                        // 寿命线性衰减：出生那帧最亮，最后一帧最暗。
                        const float fade = (float)p.life / (float)p.maxLife;
                        const float alpha = ((float)p.startAlpha / 255.0f) * fade;
                        if (alpha < 0.02f) continue;

                        RECT pr;
                        pr.left = (LONG)(p.x - p.w * 0.5f - wr.left);
                        pr.top = (LONG)(p.y - p.h * 0.5f - wr.top);
                        pr.right = (LONG)(p.x + p.w * 0.5f - wr.left);
                        pr.bottom = (LONG)(p.y + p.h * 0.5f - wr.top);

                        face::BlitFaceAlpha(hdc, pr, true /* 张口脸 */, alpha);
                    }
                    g.ReleaseHDC(hdc);
                }

                // ---- 4. 粒子的脏区 ----
                // 算一个包围盒整体塞进 dirty 就行。粒子数最多几十个、
                // 每个又小，一个并集矩形足够了；分成一个个小矩形反而会
                // 因为预乘的固定开销变慢。
                if (!g_glitchParticles.empty())
                {
                    RECT pb = { LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };

                    for (size_t i = 0; i < g_glitchParticles.size(); ++i)
                    {
                        const GlitchParticle& p = g_glitchParticles[i];
                        if (p.life <= 0) continue;

                        const LONG l = (LONG)(p.x - p.w * 0.5f);
                        const LONG t = (LONG)(p.y - p.h * 0.5f);
                        const LONG r2 = (LONG)(p.x + p.w * 0.5f);
                        const LONG b2 = (LONG)(p.y + p.h * 0.5f);

                        if (l < pb.left)   pb.left = l;
                        if (t < pb.top)    pb.top = t;
                        if (r2 > pb.right)  pb.right = r2;
                        if (b2 > pb.bottom) pb.bottom = b2;
                    }

                    if (pb.left < pb.right && pb.top < pb.bottom)
                    {
                        MapWindowPoints(NULL, m_hwnd, (LPPOINT)&pb, 2);
                        dirty.push_back(pb);
                    }
                }
            }
        }   // Graphics 析构时会把所有绘制刷到 DIB 上

        // 红底铺满整屏，预乘要覆盖全窗
        if (g_opt.look == overlay::LOOK_LOCKED && g_veilAlpha > 0)
        {
            RECT full = { 0, 0, w, h };
            dirty.push_back(full);
        }

        // 关键：UpdateLayeredWindow 要的是**预乘 alpha**，GDI+ 写的是直通 alpha
        for (size_t i = 0; i < dirty.size(); ++i)
            Premultiply(m_bits, w, h, dirty[i]);

        BLENDFUNCTION bf;
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;

        SIZE  size = {w, h};
        POINT src  = {0, 0};
        // pptDst 传 NULL：位置由 SetWindowPos 决定，这里只更新内容
        BOOL ok = UpdateLayeredWindow(m_hwnd, m_screenDC, NULL, &size, m_memDC, &src, 0, &bf, ULW_ALPHA);

        static LogGate gateRender;
        if (gateRender.Pass((int)rects.size() * 2 + (ok ? 1 : 0)))
            DLog(L"Overlay::Render: %dx%d rects=%d UpdateLayeredWindow=%d err=%lu",
                 w, h, (int)rects.size(), (int)ok, GetLastError());
    }

private:
    // 按 z 序(从上往下)找到最靠上的**真正的桌面层**，把我们的窗口插到它上面：
    // 盖住壁纸和桌面图标，但仍然在普通窗口之下。
    // 注意：系统里有一堆不可见的小 WorkerW 辅助窗口，必须用“可见 + 覆盖大半屏”
    // 把它们排除掉，否则会把方框糊到别的程序窗口上面去。
    static HWND FindDesktopAnchor(HWND self, HWND hint) {
        long vw = (long)GetSystemMetrics(SM_CXVIRTUALSCREEN);
        long vh = (long)GetSystemMetrics(SM_CYVIRTUALSCREEN);
        long screenArea = (vw > 0 && vh > 0) ? vw * vh : 0;

        for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (h == self) continue;
            wchar_t cls[64] = {0};
            if (!GetClassNameW(h, cls, 64)) continue;
            if (wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0) continue;
            if (!IsWindowVisible(h)) continue;
            RECT r;
            if (!GetWindowRect(h, &r)) continue;
            long area = (long)(r.right - r.left) * (long)(r.bottom - r.top);
            if (screenArea > 0 && area < screenArea / 4) continue;   // 小辅助窗口，跳过
            return h;
        }
        if (hint && IsWindow(hint) && IsWindowVisible(hint)) return hint;
        return NULL;
    }

    static void AddRoundRect(GraphicsPath& path, const RectF& r, float rad) {
        if (rad <= 0.01f) { path.AddRectangle(r); return; }
        float d = rad * 2.0f;
        path.AddArc(r.X,               r.Y,                d, d, 180.0f, 90.0f);
        path.AddArc(r.GetRight() - d,  r.Y,                d, d, 270.0f, 90.0f);
        path.AddArc(r.GetRight() - d,  r.GetBottom() - d,  d, d,   0.0f, 90.0f);
        path.AddArc(r.X,               r.GetBottom() - d,  d, d,  90.0f, 90.0f);
        path.CloseFigure();
    }

    // 把一块区域里的颜色乘以 alpha（BGRA 排列）
    static void Premultiply(void* bits, int w, int h, const RECT& area) {
        if (!bits) return;
        long l = max(0L, (long)area.left), t = max(0L, (long)area.top);
        long r = min((long)w, (long)area.right), b = min((long)h, (long)area.bottom);
        if (l >= r || t >= b) return;

        BYTE* base = (BYTE*)bits;
        for (long y = t; y < b; ++y) {
            BYTE* p = base + ((size_t)y * w + l) * 4;
            for (long x = l; x < r; ++x, p += 4) {
                BYTE al = p[3];
                if (al == 255) continue;
                if (al == 0) { p[0] = p[1] = p[2] = 0; continue; }
                p[0] = (BYTE)(p[0] * al / 255);
                p[1] = (BYTE)(p[1] * al / 255);
                p[2] = (BYTE)(p[2] * al / 255);
            }
        }
    }

    bool EnsureBuffers(int w, int h) {
        if (m_memDC && m_bits && w == m_w && h == m_h) return true;
        DestroyBuffers();

        m_memDC = CreateCompatibleDC(m_screenDC);
        if (!m_memDC) return false;

        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;      // 自上而下
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = NULL;
        m_bmp = CreateDIBSection(m_screenDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!m_bmp || !bits) { DestroyBuffers(); return false; }

        ZeroMemory(bits, (size_t)w * h * 4);
        m_oldBmp = SelectObject(m_memDC, m_bmp);
        m_bits = bits;
        m_w = w;
        m_h = h;
        return true;
    }

    void DestroyBuffers() {
        if (m_memDC && m_oldBmp) { SelectObject(m_memDC, m_oldBmp); m_oldBmp = NULL; }
        if (m_bmp) { DeleteObject(m_bmp); m_bmp = NULL; }
        if (m_memDC) { DeleteDC(m_memDC); m_memDC = NULL; }
        m_bits = NULL;
        m_w = m_h = 0;
    }

    HWND    m_hwnd     = NULL;
    HDC     m_screenDC = NULL;
    HDC     m_memDC    = NULL;
    HBITMAP m_bmp      = NULL;
    HGDIOBJ m_oldBmp   = NULL;
    void*   m_bits     = NULL;
    int     m_w        = 0;
    int     m_h        = 0;
    HWND    m_hint     = NULL;
    bool    m_topmost  = false;
};

// ============================================ 模块状态
namespace {

HINSTANCE              g_hInst          = NULL;
HWND                   g_hwnd           = NULL;
bool                   g_topmost        = false;
bool                   g_visible        = true;

DesktopRef             g_desktop;
std::set<std::wstring> g_names;
std::vector<RECT>      g_lastRects;     // 实际绘制的方框（含错位/抖动）
std::vector<RECT>      g_hitRects;      // 命中判定用的方框（**不含**抖动）
int                    g_shortcutCount  = 0;

int                    g_jitter = 0;   // 抖动幅度(px)
int                    g_offsetX = 0;   // 整体错位
int                    g_offsetY = 0;

Overlay* g_overlay = NULL;

// ---- 拦截鼠标/键盘：已加密的图标打不开、弹不出右键菜单、也拖不走 ----


// ---- 拦截鼠标/键盘：已加密的图标打不开、弹不出右键菜单、也拖不走 ----
// 用低层鼠标钩子吞掉落在「已加密」方框里的鼠标消息：
//   * 双击的第二次 WM_LBUTTONDOWN —— 系统就不会生成 WM_LBUTTONDBLCLK，
//     explorer 也就打不开那个图标。
//   * WM_RBUTTONDOWN / WM_RBUTTONUP —— 桌面右键菜单是 explorer 在
//     ListView 收到「按下+抬起」后弹的，两下都吞掉它就无从弹起；
//     顺带也不会选中那个图标（不会出现「选中的却是没锁的图标」的错位感）。
//   * 普通左键的 WM_LBUTTONDOWN —— 见下面「拖不动」那段，这是阻止
//     拖动发生的**唯一可靠时机**。
// 只吞落在已加密方框里的那几下，其它点击一概放行。
HHOOK                  g_mouseHook      = nullptr;
std::atomic<int>       g_blockedRights{ 0 };   // 累计拦了多少次右键
std::atomic<int>       g_blockedClicks{ 0 };   // 累计吞了多少次普通左键（防选中/防拖动）
std::atomic<int>       g_blockedDrags { 0 };   // 累计拦了多少次拖动
std::atomic<bool>      g_menuKeyDown  { false };
DWORD                  g_lastClickLogTick = 0; // 「吞左键」日志限流
POINT                  g_lastClickLogPt   = { 0, 0 };

// 拖动兜底用的按下状态。见下面 LongDrag：
// **只有「按下放行给了 explorer」的那一次才 arm**（也就是按住 Ctrl/Shift
// 的那条路），因为只有那条路才可能真的开始拖放。普通左键按下就被吞了，
// 系统根本没拿到按下，也就不必去动抬起。
struct DragLatch {
    bool  armed  = false;
    POINT origin = { 0, 0 };
};
DragLatch              g_dragLatch;             // 钩子回调里访问，单线程，不用锁
// 右键菜单拦截的总开关。正常演出时一直开着，
// 留 --no-block-menu 是为了排查「按键没反应」这类问题时能一键排除本模块。
bool                   g_blockContextMenu = true;

// 光标下面到底是不是桌面图标区？
//
// 低层鼠标钩子是**全局**的：用户可能在别的窗口（打开的文件夹、
// 浏览器、终端……）里操作，那个窗口如果正好和某个「已加密」方框
// 在屏幕坐标上重叠，光看坐标是分不出来的 —— 不查一下就会把
// 别人窗口里的右键菜单、左键点击也一并吞掉。
//
// 判断方式：取光标下最顶层的窗口，看它是不是桌面列表视图
// （SysListView32）本身或它的后代。是才算「在桌面上」。
//
// 注意 WindowFromPoint 会跳过 WS_EX_TRANSPARENT 的窗口，
// 所以我们的覆盖层、fx 层、face 层都不会出现在返回结果里 ——
// 光标在桌面上时拿到的就是 SysListView32 或 Progman/WorkerW。
bool PointOnDesktop(POINT pt)
{
    HWND under = WindowFromPoint(pt);
    if (!under) return false;

    // 兜底：万一某些环境下 WindowFromPoint 没跳过我们的覆盖层
    // （它是 layered + transparent，按文档应被跳过），直接认它。
    // 覆盖层贴在桌面之上、普通窗口之下，它可见时能拿到，
    // 说明光标确实在桌面这一层。
    if (under == g_hwnd) return true;

    // 桌面图标都挂在 SysListView32 上：往上找它。
    // 最多走 4 级（SysListView32 -> SHELLDLL_DefView -> Progman/WorkerW），
    // 足够覆盖所有已知的桌面结构。
    if (g_desktop.hListView)
    {
        HWND h = under;
        for (int i = 0; i < 4 && h; ++i)
        {
            if (h == g_desktop.hListView) return true;
            h = GetParent(h);
        }
    }
    return false;
}

// 命中判定：点是不是落在某个「已加密」图标上。
//
// 这里**刻意用 g_hitRects 而不是 g_lastRects**。后者是实际画出来的方框，
// 含整体错位和逐帧随机抖动 —— 拿它做判定的话，图标抖动时它的判定区
// 会跟着漂走，明明点在图标上却漏判。绘制要抖，判定要稳，两套矩形分开存。
bool PointInLockedFrame(POINT pt)
{
    // 先确认光标确实落在桌面上 —— 否则别的窗口上随便一个点，
    // 只要屏幕坐标碰巧和某个锁定方框重叠，就会被误拦。
    // 这是「在打开的文件夹里点右键时被吞掉」的根因。
    if (!PointOnDesktop(pt)) return false;

    for (size_t i = 0; i < g_hitRects.size(); ++i)
        if (PtInRect(&g_hitRects[i], pt)) return true;
    return false;
}
// 「现在该不该拦」。只有已加密(LOOK_LOCKED) + 开关打开时才拦。
inline bool BlockingNow()
{
    return g_blockContextMenu && g_opt.look == overlay::LOOK_LOCKED;
}

// WM_RBUTTONDOWN / WM_RBUTTONUP / WM_RBUTTONDBLCLK 都走这一个判定
inline bool IsRightButtonMsg(WPARAM wp)
{
    return wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP || wp == WM_RBUTTONDBLCLK;
}

// ---- 拖不动：为什么必须拦「按下」，而不是拦「抬起」 ----
//
// 桌面上按住图标拖走，走的是 OLE 拖放：系统在**按下并移动超过拖动阈值**
// 之后才进入拖放循环（IDropSource::QueryContinueDrag 判定），松手时由
// 目标窗口的 IDropTarget::Drop 真正把位置落下去。所以从原理上说，
// 「吞掉抬起」也能阻止落下。
//
// 但这里选的是**吞掉按下**，因为它是唯一不依赖时序的一条路：
//   * 吞掉按下 → ListView 收不到按钮按下 → 既不选中、也不进入拖放循环，
//     图标根本没有被「拿起来」，拖的动作在发生之前就没了。
//   * 吞掉抬起 → 得指望钩子在拖放循环**跑完之后**还能收到那一下抬起。
//     而低层钩子的回调跑在本进程的 UI 线程上，拖放循环恰恰是本线程在跑，
//     超过系统给的超时（约 1 秒）钩子就会被摘掉 —— 拖到一半拦截就没了。
//     也就是说这条路上「拖得慢一点」就能把拦截绕过去。
//
// 代价：普通左键点一下也不再选中那个图标（选中是按下时发生的）。
// 想让用户还能并选，按住 Ctrl / Shift 点就放行（多选本来就靠它）。
inline bool ModifierHeld()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
}

// 光标按下之后移动超过这个距离就当「在拖」。
inline int DragThreshold()
{
    return max(4, GetSystemMetrics(SM_CXDRAG) / 2);
}

// 「吞掉普通左键」要不要记一行日志。
//
// 这一下是**拖动拦截真正的着力点**，所以必须能在日志里看到它发生了。
// 但它是最高频的操作（用户会反复点），不能每下都写：
// 限流规则是「同一个点、0.5 秒内只记一次」，够排查用，也不刷屏。
bool ShouldLogClick(const POINT& pt)
{
    const DWORD now = GetTickCount();
    if (now - g_lastClickLogTick < 500 &&
        pt.x == g_lastClickLogPt.x && pt.y == g_lastClickLogPt.y)
        return false;

    g_lastClickLogTick = now;
    g_lastClickLogPt   = pt;
    return true;
}

// 抬起时判断这次是不是一次拖动，是就把累计数加上、必要时记日志。
//
// 这条路是**兜底**：正常情况按下就被吞了，根本走不到这里。
// 它真正的作用是「万一漏了」：既拦住落下，又在日志里留下痕迹 ——
// 日志里出现这行，就说明「吞按下」那条路有漏，得去查。
bool LongDrag(const POINT& pt)
{
    const int dx = abs(pt.x - g_dragLatch.origin.x);
    const int dy = abs(pt.y - g_dragLatch.origin.y);

    if (dx >= DragThreshold() || dy >= DragThreshold())
    {
        const int n = ++g_blockedDrags;
        DLog(L"[overlay] 拦截拖动：松开时已走了 (%d,%d) 像素，抬起被吞掉，图标落不下去", dx, dy);
        if (n == 1 || n % 20 == 0)
            DLog(L"[overlay] 拖动拦截累计 #%d", n);
        return true;
    }
    return false;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && BlockingNow())
    {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;
        const POINT pt = { (LONG)ms->pt.x, (LONG)ms->pt.y };
        const bool inFrame = PointInLockedFrame(pt);

        // ---- 左键抬起：给「拖走」兜最后一道 ----
        // 能走到这里而且闩锁还 armed，说明按下那一下是**放行**给 explorer 的
        // （Ctrl/Shift 那条路）。只有确实拖动超过阈值才吞掉抬起。
        if (wParam == WM_LBUTTONUP)
        {
            if (g_dragLatch.armed)
            {
                g_dragLatch.armed = false;
                if (LongDrag(pt))
                    return 1;      // 吞掉抬起 = 这次拖放不会完成
            }
        }

        // ---- 右键落在已加密图标上：吞掉按下 + 抬起，右键菜单弹不出来 ----
        if (IsRightButtonMsg(wParam) && inFrame)
        {
            // 只在「按下」那一下记日志：一次右键会进来俩消息（按下+抬起），
            // 两个都记会翻倍，看着像拦了两次。
            if (wParam != WM_RBUTTONUP)
            {
                const int n = ++g_blockedRights;
                if (n == 1 || n % 20 == 0)
                    DLog(L"[overlay] 拦截右键菜单 #%d: (%ld,%ld) 落在已加密图标上",
                         n, pt.x, pt.y);
            }
            return 1;
        }

        // ---- 左键落在已加密图标上 ----
        if (wParam == WM_LBUTTONDOWN && inFrame)
        {
            // 按住 Ctrl / Shift：多选、加选、Ctrl+双击打开这类操作还给它，
            // 别拦。同时记下按下点，万一它后面真拖动了，抬起时兜底。
            if (ModifierHeld())
            {
                g_dragLatch.armed  = true;
                g_dragLatch.origin = pt;
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // 无修饰键的普通左键：直接吞掉。图标拿不起来 → 不会选中，
            // 也进不了拖放循环，位置动不了。
            g_dragLatch.armed = false;   // 系统没拿到按下，抬起不必管

            const int n = ++g_blockedClicks;
            if (ShouldLogClick(pt))
                DLog(L"[overlay] 吞掉普通左键 #%d: (%ld,%ld) 落在已加密图标上（防选中/防拖动）",
                     n, pt.x, pt.y);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// 光标下面那个图标是不是「当前被选中的那个」。
// 桌面 ListView 用 LVM_GETNEXTITEM(-1, LVNI_SELECTED) 能拿到选中项索引，
// 再取它的矩形和光标比一下就知道了。
// 取不到（reader 打不开 / 压根没选中）一律返回 false —— 保持拦截行为。
bool SelectedAnyFrame()
{
    if (!g_desktop.Valid() || g_lastRects.empty()) return false;

    ListViewReader reader(g_desktop.hListView);
    if (!reader.Open()) return false;

    const int sel = (int)SendMessageW(g_desktop.hListView, LVM_GETNEXTITEM,
                                      (WPARAM)-1, MAKELPARAM(LVNI_SELECTED, 0));
    if (sel < 0) return false;      // 没有选中项

    RECT rcIcon = {0}, rcLabel = {0};
    const bool hasIcon  = reader.Rect(sel, LVIR_ICON,  rcIcon);
    const bool hasLabel = reader.Rect(sel, LVIR_LABEL, rcLabel);
    if (!hasIcon && !hasLabel) return false;

    RECT rc = hasIcon ? rcIcon : rcLabel;
    if (hasIcon && hasLabel) UnionRect(&rc, &rcIcon, &rcLabel);
    MapWindowPoints(g_desktop.hListView, NULL, (LPPOINT)&rc, 2);

    // 稍微放宽一点，避免像素级误差把「其实选中了」判成没选中
    InflateRect(&rc, 4, 4);

    POINT pt;
    GetCursorPos(&pt);
    return PtInRect(&rc, pt) != FALSE;
}

// ---- 键盘这一路：右键菜单不只有鼠标能叫出来 ----
// 菜单键（VK_APPS）和 Shift+F10 都是 explorer 认的「弹出上下文菜单」键，
// 它们不经过鼠标钩子。不一起吞掉的话，鼠标点不出来、键盘却弹得出来，
// 这个「加密」就漏气了。
HHOOK                  g_kbHook    = nullptr;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;
        const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        // ---- A. 勒索清场期间：Win / Alt+Tab / Ctrl+Esc 一律吞掉 ----
        // 这是「阻止被清场的窗口被叫回来」的输入层。
        // 放在最前面、而且不受 BlockingNow() 影响 —— 清场是独立于
        // 「已加密鼠标拦截」的另一件事，两者开关不共用。
        if (lockdown::ShouldSwallowKey(kb->vkCode))
            return 1;

        // ---- B. 菜单键 / Shift+F10：只在已加密 + 开关打开时拦 ----
        if (BlockingNow())
        {
            bool menuKey = (kb->vkCode == VK_APPS);
            if (!menuKey && kb->vkCode == VK_F10)
                menuKey = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            if (menuKey)
            {
                POINT pt;
                GetCursorPos(&pt);

                // 按下时：光标在被锁图标上，而且这个图标没被选中。
                // （桌面本来就没选中项时，explorer 的上下文菜单是给光标
                //   底下那个图标用的，所以这种情况也该拦。）
                if (isDown && !g_menuKeyDown.load() &&
                    PointInLockedFrame(pt) && !SelectedAnyFrame())
                {
                    g_menuKeyDown = true;
                    DLog(L"[overlay] 拦截键盘唤出右键菜单: vk=0x%02X @(%ld,%ld)",
                         (unsigned)kb->vkCode, pt.x, pt.y);
                }
                // 抬起也要吞：否则会留一个「按下被吃、抬起放行」的残局，
                // explorer 反而可能借那一下把菜单弹出来。
                if ((isDown || isUp) && g_menuKeyDown.load())
                {
                    if (isUp) g_menuKeyDown = false;
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ---- 低层钩子会被系统悄悄摘掉，得看着点 ----
// 低层钩子的回调跑在**本进程的 UI 线程**上，系统对它有超时限制（约 1 秒没
// 响应就摘钩）。我们这个进程里恰恰有会阻塞主消息循环的东西：模态弹窗、
// 动画等待、对话框。一旦被摘钩，右键菜单拦截会**静默失效** —— 演出还在
// 跑，但桌面又能右键了。
//
// 没有「查询钩子是否还活着」的 API，所以干脆定期重装：Unhook + Hook。
// 开销是两次内核调用，锁定态 70ms 一次也无所谓；换来的是「被摘掉最多
// 1 个 tick 内自动恢复」。
void ReHookInput()
{
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }

    const bool menuBlock = BlockingNow();

    // 键盘钩子有两种用途：已加密图标的菜单键拦截，以及清场期间的
    // Win / Alt+Tab 拦截。两者任一需要，钩子就得在。
    const bool wantKbHook = menuBlock || lockdown::Active();

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                    GetModuleHandleW(NULL), 0);
    if (!g_mouseHook)
        DLog(L"[overlay] 鼠标钩子重装失败, err=%lu（双击/右键拦截会失效）", GetLastError());

    if (wantKbHook)
    {
        if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                     GetModuleHandleW(NULL), 0);
        if (!g_kbHook)
            DLog(L"[overlay] 键盘钩子重装失败, err=%lu（菜单键/Win/Alt+Tab 拦不住）", GetLastError());
    }
    else if (g_kbHook)
    {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
}

} // namespace

// 扫描一次：变化时才重画
//
// 命中矩形（g_hitRects）和绘制矩形（g_lastRects）是**两套**：
// 绘制的那套要抖（看起来图标在躁动），判定的那套不能抖（否则点不准）。
// 所以这里每次都把 base 那套抽出来存一遍，而且**在「无变化就返回」之前**
// 就存好 —— 抖动幅度为 0 时锁定态下会频繁走到那个提前返回，
// 放在后面会出现「窗口在跳但 hitRects 空着」的漏判。
static void RefreshInternal(bool force)
{
    if (!g_overlay) return;

    std::vector<Frame> fresh;
    std::vector<Frame> others;      // 硬核：桌面上的"非快捷方式"候选
    if (!BuildFrames(g_desktop, g_names, fresh, others))
    {
        DLog(L"RefreshInternal: BuildFrames 返回 false，本帧不渲染");
        g_hitRects.clear();
        g_lastRects.clear();
        g_shortcutCount = 0;
        if (g_overlay && g_visible)
        {
            std::vector<RECT> none;
            g_overlay->Render(none);
        }
        return;
    }

    g_shortcutCount = (int)fresh.size();

    // 判定的那套：只跟整体错位走，不带逐帧抖动
    g_hitRects.clear();
    g_hitRects.reserve(fresh.size());
    for (size_t i = 0; i < fresh.size(); ++i)
    {
        RECT r = fresh[i].rc;
        r.left   += g_offsetX;
        r.right  += g_offsetX;
        r.top    += g_offsetY;
        r.bottom += g_offsetY;
        g_hitRects.push_back(r);
    }

    std::vector<RECT> rects;
    rects.reserve(fresh.size());
    for (size_t i = 0; i < fresh.size(); ++i)
    {
        RECT r = fresh[i].rc;

        // ---- 图标操控：整体错位 + 逐帧随机抖动 ----
        r.left   += g_offsetX;
        r.right  += g_offsetX;
        r.top    += g_offsetY;
        r.bottom += g_offsetY;

        if (g_jitter > 0)
        {
            const int dx = (rand() % (g_jitter * 2 + 1)) - g_jitter;
            const int dy = (rand() % (g_jitter * 2 + 1)) - g_jitter;
            r.left   += dx;  r.right  += dx;
            r.top    += dy;  r.bottom += dy;
        }

        rects.push_back(r);
    }

    // ---- 硬核：随机锁定"非快捷方式"（0.9~9 秒，到点变绿淡出）----
    //
    // 三项的走向见 g_extraUnlockAt 那一带的注释。这里只强调一件事：
    // **放掉的项这一轮既不进 rects、也不进 g_hitRects**。
    // 后者是命中判定的唯一依据（PointInLockedFrame），所以
    // "框已经变绿消失了、点击还被吞"这种不一致从结构上就不会发生。
    std::vector<BYTE>  extraFade;
    std::vector<DWORD> fadeElapsed;

    if (g_extraLockOn && g_opt.look == overlay::LOOK_LOCKED && !g_paidClearActive)
    {
        const DWORD now = GetTickCount();

        // 前 rects.size() 个都是快捷方式，不属于"额外锁定"
        extraFade.assign(rects.size(), 0);
        fadeElapsed.assign(rects.size(), 0);

        // 本轮的数量上限与时长区间**在这里读一次**（而不是在下面的循环里
        // 逐项读）：这几个是 accessor，里面还要跟桌面实际条目数夹一道，
        // 循环里每项调一次没必要。数量上限可能为 0（桌面上一个能锁的
        // 非快捷方式项都没有），那种情况下整个循环等于空转。
        const int  lockCount = settings::ExtraLockCount();
        const int  lockMinMs = settings::ExtraLockMinMs();
        const int  lockMaxMs = settings::ExtraLockMaxMs();
        const int  durSpan = lockMaxMs - lockMinMs;

        for (size_t i = 0; i < others.size(); ++i)
        {
            RECT r = others[i].rc;
            r.left   += g_offsetX;  r.right  += g_offsetX;
            r.top    += g_offsetY;  r.bottom += g_offsetY;

            const std::pair<LONG, LONG> key(r.left, r.top);

            // 冷却中：什么都不做（既不复锁，也不画框）
            std::map<std::pair<LONG, LONG>, DWORD>::iterator cool = g_extraCoolAt.find(key);
            if (cool != g_extraCoolAt.end())
            {
                if (now < cool->second) continue;
                g_extraCoolAt.erase(cool);
            }

            std::map<std::pair<LONG, LONG>, DWORD>::iterator it = g_extraUnlockAt.find(key);
            BYTE  fading  = 0;
            DWORD elapsed = 0;

            if (it == g_extraUnlockAt.end())
            {
                // 新名额：满了就不再锁新的（map 的键天然保证"同一项不会被锁两次"）
                if ((int)g_extraUnlockAt.size() >= lockCount) continue;

                const DWORD life = (DWORD)lockMinMs +
                    ((durSpan > 0) ? (DWORD)(rand() % (durSpan + 1)) : 0);
                g_extraUnlockAt[key] = now + life;

                DLog(L"[overlay] 硬核：锁定桌面项 <%s> %.1f 秒",
                     others[i].name.c_str(), life / 1000.0);
            }
            else if (now >= it->second)
            {
                // 到点：进入"变绿 + 淡出"窗口
                std::map<std::pair<LONG, LONG>, DWORD>::iterator fit = g_extraFadeAt.find(key);
                if (fit == g_extraFadeAt.end())
                {
                    g_extraFadeAt[key] = now;
                    fit = g_extraFadeAt.find(key);
                }

                elapsed = now - fit->second;
                if (elapsed >= kExtraFadeMs)
                {
                    // 彻底放掉：进冷却，冷却完才可能被重新选中
                    g_extraUnlockAt.erase(key);
                    g_extraFadeAt.erase(key);
                    g_extraCoolAt[key] = now + (DWORD)settings::kExtraLockCooldownMs;

                    DLog(L"[overlay] 硬核：解锁桌面项 <%s>（进入 %d 秒冷却）",
                         others[i].name.c_str(), settings::kExtraLockCooldownMs / 1000);
                    continue;
                }
                fading = 1;
            }

            rects.push_back(r);
            g_hitRects.push_back(r);      // 锁定 / 淡出期间照样拦右键和拖动
            extraFade.push_back(fading);
            fadeElapsed.push_back(elapsed);
        }
    }

    g_rectExtraFade   = extraFade;
    g_rectFadeElapsed = fadeElapsed;

    // 锁定态下停牌图标每帧都要重新抖动，所以强制重绘
    bool changed = force || (g_opt.look == overlay::LOOK_LOCKED) ||
                   rects.size() != g_lastRects.size();
    if (!changed)
    {
        for (size_t i = 0; i < rects.size(); ++i)
        {
            const RECT& a = rects[i];
            const RECT& b = g_lastRects[i];
            if (a.left != b.left || a.top != b.top ||
                a.right != b.right || a.bottom != b.bottom)
            {
                changed = true;
                break;
            }
        }
    }
    if (!changed)
    {
        DLog(L"RefreshInternal: 无变化，跳过重绘 (rects=%d)", (int)rects.size());
        return;
    }

    g_lastRects = rects;

    static LogGate gateRedraw;
    if (gateRedraw.Pass((int)rects.size() * 2 + ((int)g_visible ? 1 : 0)))
        DLog(L"RefreshInternal: 重绘 rects=%d visible=%d", (int)rects.size(), (int)g_visible);

    if (g_visible)
    {
        g_overlay->ApplyZOrder(false);
        g_overlay->Render(rects);
    }
}

// 定时器回调：重扫 + 应对 explorer 重启
static void OverlayTick()
{
    // 先把钩子重新装上。低层钩子会被系统静默摘掉（主线程阻塞超过约 1 秒
    // 就会发生），锁定/清场期间必须持续看着，否则拦截会无声失效。
    ReHookInput();

    // 清场守望：把被叫回来的窗口重新收回去。内部自己按 150ms 限流，
    // 不用在这里判断间隔。
    lockdown::Tick();

    // ---- 付清消散动画播完：复位外观并隐藏覆盖层 ----
    // 这一刻桌面已经没有任何标记了（停牌、方框、粒子都不在了），
    // 直接切回普通外观并藏起来 —— 不走 SetLook / SetVisible，
    // 那条路会顺带触发一次重绘，把"最后一帧"再画一遍。
    if (g_paidClearActive)
    {
        const DWORD el = GetTickCount() - g_paidClearStart;
        if (el >= kPaidClearTotalMs)
        {
            g_paidClearActive = false;
            g_glitchParticles.clear();
            g_frameNextBurst.clear();

            // 硬核的额外锁定状态也要在这里清干净（同 Render 开头那道兜底）
            g_extraUnlockAt.clear();
            g_extraFadeAt.clear();
            g_extraCoolAt.clear();
            g_rectExtraFade.clear();
            g_rectFadeElapsed.clear();

            g_opt.look = overlay::LOOK_FRAME;
            g_opt.interval = 300;
            if (g_hwnd) SetTimer(g_hwnd, kOverlayTimerId, (UINT)g_opt.interval, NULL);

            g_visible = false;
            if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);

            elog::Write(L"[overlay] 付清消散动画完成，覆盖层已复位并隐藏");
        }
    }

    if (!g_desktop.Valid() && !DiscoverDesktop(g_desktop))
    {
        g_hitRects.clear();
        g_lastRects.clear();
        g_shortcutCount = 0;
        std::vector<RECT> none;
        if (g_overlay) g_overlay->Render(none);
        return;
    }
    RefreshInternal(false);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == kOverlayTimerId) { OverlayTick(); return 0; }
        break;

    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================ 模块 API
namespace overlay {

bool Start(HINSTANCE hInst, bool topmost)
{
    if (g_hwnd) return true;

    g_hInst   = hInst;
    g_topmost = topmost;

    // ---- 先把「自己人」登记给清场模块 ----
    // 勒索阶段会把屏幕上能最小化的窗口全收走，本程序自己的窗口当然不能收。
    // 主力防线是 lockdown 里的进程号比对，这里登记类名是第二道：
    // 万一将来有窗口跑到别的进程去（像沙箱化外壳那样），也还能兜住。
    lockdown::AddOwnClass(kOverlayClass);
    lockdown::AddOwnClass(L"RansomFaceWnd");
    lockdown::AddOwnClass(L"RansomFxOverlay");
    lockdown::AddOwnClass(L"RansomAeroWnd");
    lockdown::AddOwnClass(L"RansomDirectorWnd");
    lockdown::AddOwnClass(L"RansomDevIpcWnd");

    DLog(L"overlay::Start(topmost=%d)", (int)topmost);

    {
        std::vector<std::wstring> folders;
        CollectShortcutNames(g_names, folders, g_allNames);
        DLog(L"  桌面目录 %d 个, 快捷方式名候选 %d 个, 全部条目名 %d 个",
             (int)folders.size(), (int)g_names.size(), (int)g_allNames.size());
        for (size_t i = 0; i < folders.size() && i < 4; ++i)
            DLog(L"    dir: %s", folders[i].c_str());
        if (g_names.empty()) { DLog(L"  桌面上没有快捷方式，放弃"); return false; }
    }

    if (!DiscoverDesktop(g_desktop))
    {
        DLog(L"  DiscoverDesktop 失败：找不到 SysListView32");
        return false;
    }
    DLog(L"  桌面列表 OK: defView=0x%p listView=0x%p host=0x%p",
         (void*)g_desktop.hDefView, (void*)g_desktop.hListView, (void*)g_desktop.hHost);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.style         = 0;
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = NULL;
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);

    g_overlay = new Overlay();
    if (!g_overlay->Create(topmost, g_desktop.hHost))
    {
        delete g_overlay;
        g_overlay = NULL;
        return false;
    }

    g_hwnd    = g_overlay->Handle();
    g_visible = true;

    // 装输入钩子：用于吞掉落在地图标上的双击（阻止打开被锁定的图标），
    // 以及落在已加密图标上的右键（阻止弹出右键菜单）。
    // 键盘那一路只在锁定态才需要（菜单键 / Shift+F10），ReHookInput 里
    // 按当前外观决定要不要挂。
    ReHookInput();

    RefreshInternal(true);
    SetTimer(g_hwnd, kOverlayTimerId, (UINT)g_opt.interval, NULL);
    return true;
}

void Stop()
{
    if (g_mouseHook)
    {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_kbHook)
    {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
    g_menuKeyDown = false;
    g_dragLatch.armed = false;
    g_lastClickLogTick = 0;
    g_lastClickLogPt   = { 0, 0 };

    if (g_hwnd)
    {
        KillTimer(g_hwnd, kOverlayTimerId);
        g_hwnd = NULL;
    }
    if (g_overlay)
    {
        g_overlay->Destroy();
        delete g_overlay;
        g_overlay = NULL;
    }
    g_lastRects.clear();
    g_hitRects.clear();
    g_shortcutCount = 0;
}

void SetStyle(const Style& s)
{
    g_opt.alpha     = s.alpha;
    g_opt.color     = s.color;
    g_opt.thickness = s.thickness;
    g_opt.radius    = s.radius;
    g_opt.pad       = s.pad;
    g_opt.glow      = s.glow;
    g_opt.boxMode   = s.boxMode;
    g_opt.interval  = s.interval;
    g_opt.look      = s.look;

    if (g_hwnd) SetTimer(g_hwnd, kOverlayTimerId, (UINT)g_opt.interval, NULL);
    RefreshInternal(true);
}

Style GetStyle()
{
    Style s;
    s.alpha     = g_opt.alpha;
    s.color     = g_opt.color;
    s.thickness = g_opt.thickness;
    s.radius    = g_opt.radius;
    s.pad       = g_opt.pad;
    s.glow      = g_opt.glow;
    s.boxMode   = g_opt.boxMode;
    s.interval  = g_opt.interval;
    s.look      = g_opt.look;
    return s;
}

void SetLook(Look l)
{
    if (g_opt.look == l) return;
    g_opt.look = l;

    // 锁定态要画抖动的停牌，刷新率得提上去
    g_opt.interval = (l == LOOK_LOCKED) ? 70 : 300;
    if (g_hwnd) SetTimer(g_hwnd, kOverlayTimerId, (UINT)g_opt.interval, NULL);

    // 锁定态**保持贴桌面层**：边框不能被别的应用窗口透出来。
    // 全屏红底改由 fx 模块做（它才是置顶的），覆盖层只负责方框和停牌图标。
    const bool locked = (l == LOOK_LOCKED);

    g_forceTopmost = false;
    g_veilAlpha    = 0;

    // 注意最后一个字段打印的是 locked 标志，不是 g_forceTopmost。
    // 锁定态下 SetLook 明确把 g_forceTopmost 归零（红幕改由 fx 模块的
    // 边缘红光负责），所以这里标成「置顶」会误导排查。
    elog::Write(L"[overlay] 外观切换为 %s（刷新 %dms，红底 alpha=%d，锁定=%d）",
                locked ? L"已加密" : L"普通标记", g_opt.interval,
                (int)g_veilAlpha, (int)locked);

    // 锁定的那一刻就把钩子按新状态重挂（锁定态多挂一个键盘钩子）。
    // 不等定时器，免得开头那几十毫秒里右键还是能弹菜单。
    ReHookInput();

    RefreshInternal(true);
}

Look GetLook() { return g_opt.look; }

// ---- 硬核：随机锁定"非快捷方式"的开关 ----
//
// 打开之后，每次重扫都会在桌面上挑几个**真实存在**的非快捷方式项
// （文件夹 / 文件）锁上 0.9~9 秒；到点后单独播"变绿 + 淡出"再放掉，
// 放掉之后进冷却，冷却期内不会被重新选中。
//
// 只在 LOOK_LOCKED 期间真的生效；退出锁定态、付清消散开始时状态一律清空
// （见 Render 开头和 OverlayTick 的复位分支），所以关掉它不需要额外收尾。
void SetExtraLockEnabled(bool on)
{
    if (g_extraLockOn == on) return;
    g_extraLockOn = on;

    if (!on)
    {
        // 关掉时立刻清干净，不留半锁状态给下一轮
        g_extraUnlockAt.clear();
        g_extraFadeAt.clear();
        g_extraCoolAt.clear();
        g_rectExtraFade.clear();
        g_rectFadeElapsed.clear();
    }

    elog::Write(L"[overlay] 硬核的「随机锁非快捷方式」：%s（同时最多 %d 个，%d-%dms，释放后冷却 %dms）",
        on ? L"开" : L"关",
        (int)settings::ExtraLockCount(),
        (int)settings::ExtraLockMinMs(), (int)settings::ExtraLockMaxMs(),
        (int)settings::kExtraLockCooldownMs);

    if (g_hwnd) RefreshInternal(true);
}

bool ExtraLockEnabled() { return g_extraLockOn; }

void SetVeil(BYTE alpha, COLORREF color)
{
    g_veilAlpha = alpha;
    g_veilColor = color;
    if (g_opt.look == LOOK_LOCKED) RefreshInternal(true);
}

BYTE VeilAlpha() { return g_veilAlpha; }

void SetJitter(int amplitudePx){
    g_jitter = (amplitudePx < 0) ? 0 : amplitudePx;
}

int Jitter() { return g_jitter; }

void SetOffset(int dx, int dy) { g_offsetX = dx; g_offsetY = dy; }

void GetOffset(int* dx, int* dy)
{
    if (dx) *dx = g_offsetX;
    if (dy) *dy = g_offsetY;
}

// ---- 付清赎金：桌面消散动画 ----
void BeginPaidClear()
{
    // 只有真正处于锁定态才有意义。重复调用是空操作 —— 一场演出里
    // 只会被调一次，但万一将来被别处误触发也不会出事。
    if (g_opt.look != overlay::LOOK_LOCKED) return;
    if (g_paidClearActive) return;

    g_paidClearActive = true;
    g_paidClearStart = GetTickCount();

    // 立刻清空故障粒子和它们的计时。动画期间 Render 里的生成分支
    // 也会被 paidActive 挡住，不会再冒新粒子。
    g_glitchParticles.clear();
    g_frameNextBurst.clear();

    // 消散期间把刷新率提上去：锁定态默认是 70ms 一帧，900ms 的动画
    // 只有 13 帧，淡出和放大都会显得一跳一跳的。33ms 差不多翻倍。
    g_opt.interval = 33;
    if (g_hwnd) SetTimer(g_hwnd, kOverlayTimerId, (UINT)g_opt.interval, NULL);

    elog::Write(L"[overlay] 付清：开始桌面消散动画（%d 个方框，从左往右、从上往下，%lums）",
        (int)g_hitRects.size(), (unsigned long)kPaidClearTotalMs);

    // 立刻重扫一次：强制重绘，动画第一帧就能画出来，
    // 不用等下一个 33ms 的 tick。
    RefreshInternal(true);
}

void SetVisible(bool visible)
{
    g_visible = visible;
    if (!g_hwnd) return;

    ShowWindow(g_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);

    if (visible)
    {
        RefreshInternal(true);
    }
    else if (g_overlay)
    {
        std::vector<RECT> none;      // 渲染空集 = 整层透明
        g_overlay->Render(none);
    }
}

bool Visible() { return g_visible; }

// ---- 右键菜单拦截 ----
// 开关只影响「拦截」这一件事，不影响画框。
// 注意它管的是**整组输入拦截**（右键菜单 + 拖动 + 吞左键），因为这几件
// 事都挂在同一个低层钩子的回调里。关掉时键盘钩子一并摘掉，鼠标钩子留着。
void SetBlockContextMenu(bool on)
{
    if (g_blockContextMenu == on) return;
    g_blockContextMenu = on;

    // 关掉时清掉闩锁，防止残留一个「按下已吞、抬起待吞」的状态
    g_menuKeyDown     = false;
    g_dragLatch.armed = false;

    ReHookInput();
    elog::Write(L"[overlay] 右键菜单拦截：%s", on ? L"开" : L"关");
}

bool BlockContextMenu() { return g_blockContextMenu; }
int  BlockedRightClicks() { return g_blockedRights.load(); }
int  BlockedClicks() { return g_blockedClicks.load(); }
int  BlockedDrags() { return g_blockedDrags.load(); }

void Refresh(bool force) { RefreshInternal(force); }

HWND Handle() { return g_hwnd; }

int ShortcutCount() { return g_shortcutCount; }

void SetLogFile(const wchar_t* path) { elog::Open(path); }

} // namespace overlay
