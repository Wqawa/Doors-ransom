

































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
#include <shobjidl.h>
#include <shlguid.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlwapi.h>
#include <oleauto.h>
#include <gdiplus.h>

#include "desktop_overlay.h"
#include "entity_log.h"
#include "face.h"
#include "lockdown.h"

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
using std::min;
using std::max;

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif


static const wchar_t* kAppName        = L"DesktopShortcutFrame";
static const wchar_t* kOverlayClass   = L"DesktopOverlayWnd";
static const UINT_PTR kOverlayTimerId = 1;


struct Options {

    BYTE     alpha     = 220;
    COLORREF color     = RGB(0x2E, 0xB8, 0xFF);
    float    thickness = 2.0f;
    float    radius    = 10.0f;
    float    pad       = 4.0f;
    bool     glow      = true;


    int  interval = 300;
    bool topmost  = false;
    bool hotkeys  = true;
    bool quiet    = false;
    bool verbose  = false;
    bool listOnly = false;
    int  boxMode  = 2;
    overlay::Look look = overlay::LOOK_FRAME;
};

static Options g_opt;



static BYTE     g_veilAlpha    = 0;
static COLORREF g_veilColor    = RGB(200, 20, 20);


static bool     g_forceTopmost = false;



#define DLog elog::Write







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
        out.insert(ToLower(fileName));
        size_t dot = fileName.find_last_of(L'.');
        if (dot != std::wstring::npos)
            out.insert(ToLower(fileName.substr(0, dot)));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void CollectShortcutNames(std::set<std::wstring>& out,
                                std::vector<std::wstring>& folders) {
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path))) {
        folders.push_back(path);
        ScanFolderForShortcuts(path, out);
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, path))) {
        folders.push_back(path);
        ScanFolderForShortcuts(path, out);
    }
}


struct DesktopRef {
    HWND hProgman  = NULL;
    HWND hDefView  = NULL;
    HWND hHost     = NULL;
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
        const long kLimit = 100000;
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


struct Frame {
    std::wstring name;
    RECT         rc;
};












struct GlitchParticle
{
    float x, y;
    float w, h;
    BYTE  startAlpha;
    int   life;
    int   maxLife;
};

std::vector<GlitchParticle> g_glitchParticles;









std::map<std::pair<LONG, LONG>, DWORD> g_frameNextBurst;



bool  g_paidClearActive = false;
DWORD g_paidClearStart = 0;



const DWORD kPaidClearTotalMs = 900;

static bool BuildFramesViaListView(const DesktopRef& d,
                                   const std::set<std::wstring>& shortcutNames,
                                   std::vector<Frame>& frames) {
    frames.clear();
    if (!d.Valid()) { DLog(L"  [VM] 桌面引用无效"); return false; }

    ListViewReader reader(d.hListView);
    if (!reader.Open())
    {

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
        if (shortcutNames.find(ToLower(text)) == shortcutNames.end()) continue;

        RECT rcIcon = {0}, rcLabel = {0};
        bool hasIcon  = reader.Rect(i, LVIR_ICON,  rcIcon);
        bool hasLabel = reader.Rect(i, LVIR_LABEL, rcLabel);

        if (!hasIcon && !hasLabel) {

            POINT pt;
            if (!reader.Position(i, pt) || cellW <= 0 || cellH <= 0) continue;
            SetRect(&rcIcon, pt.x, pt.y, pt.x + cellW, pt.y + (long)(cellH * 0.55));
            hasIcon = true;
        }





        long pad = (long)(g_opt.pad + 0.5f);
        const long kGap = 2;

        long top    = hasIcon ? rcIcon.top : rcLabel.top;
        long bottom = hasIcon ? rcIcon.bottom : rcLabel.bottom;
        if (hasIcon && hasLabel) bottom = max(bottom, rcLabel.bottom);
        long centerX = hasIcon ? (rcIcon.left + rcIcon.right) / 2
                               : (rcLabel.left + rcLabel.right) / 2;

        long iconSize = hasIcon ? (rcIcon.bottom - rcIcon.top) : 0;
        long labelW   = hasLabel ? (rcLabel.right - rcLabel.left) : 0;

        long halfW;
        if (g_opt.boxMode == 0)      halfW = iconSize / 2;
        else if (g_opt.boxMode == 1) halfW = labelW / 2;
        else                         halfW = max(iconSize, labelW) / 2;
        halfW += pad;

        if (cellW > 0) {
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


        MapWindowPoints(d.hListView, NULL, (LPPOINT)&rc, 2);

        Frame f;
        f.name = text;
        f.rc = rc;
        frames.push_back(f);
    }
    static LogGate gateHit;
    if (gateHit.Pass((int)frames.size()))
        DLog(L"  [VM] 命中 %d 个", (int)frames.size());
    return true;
}














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

        pfv->GetFolder(IID_PPV_ARGS(&psf));


        DWORD spacing = (DWORD)SendMessageW(d.hListView, LVM_GETITEMSPACING, FALSE, 0);
        long cellW = LOWORD(spacing);
        long cellH = HIWORD(spacing);
        if (cellW <= 0 || cellH <= 0) { cellW = 76; cellH = 76; }

        for (int i = 0; i < count; ++i)
        {
            PITEMID_CHILD pidl = nullptr;
            if (FAILED(pfv->Item(i, &pidl)) || !pidl) continue;


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


            if (parsing.empty() || !IsShortcutFileName(parsing)) continue;

            if (shortcutNames.find(ToLower(display)) == shortcutNames.end()) continue;

            RECT rc;
            rc.left   = pt.x + 2;
            rc.top    = pt.y + 2;
            rc.right  = pt.x + cellW - 2;
            rc.bottom = pt.y + cellH - 2;

            if (rc.right - rc.left < 8 || rc.bottom - rc.top < 8) continue;


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


static bool BuildFrames(const DesktopRef& d, const std::set<std::wstring>& shortcutNames,
                        std::vector<Frame>& frames)
{
    frames.clear();
    if (!d.Valid()) { DLog(L"  BuildFrames: 桌面引用无效"); return false; }

    if (BuildFramesViaListView(d, shortcutNames, frames))
        return true;


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
    return BuildFramesViaFolderView(d, shortcutNames, frames);
}


class Overlay {
public:

    bool Create(bool topmost, HWND hDesktopHint) {
        m_topmost = topmost;
        m_hint    = hDesktopHint;

        DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;







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

        ApplyZOrder(true);
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
        ApplyZOrder(true);
    }


    void ApplyZOrder(bool force) {
        if (!m_hwnd) return;

        if (m_topmost || g_forceTopmost) {


            for (int i = 0; i < 3; ++i) {
                LONG_PTR es = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
                if (es & WS_EX_TOPMOST) break;
                SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return;
        }

        HWND anchor = FindDesktopAnchor(m_hwnd, m_hint);
        if (!anchor) {
            SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }
        if (!force && GetWindow(anchor, GW_HWNDPREV) == m_hwnd)
            return;



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





        if (g_opt.look != overlay::LOOK_LOCKED || g_paidClearActive)
        {
            if (!g_glitchParticles.empty()) g_glitchParticles.clear();
            if (!g_frameNextBurst.empty())  g_frameNextBurst.clear();
        }


        std::vector<RECT> dirty;
        dirty.reserve(rects.size());

        {
            Graphics g(m_memDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.Clear(Color(0, 0, 0, 0));



            const bool paidActive = g_paidClearActive;
            const bool locked = (g_opt.look == overlay::LOOK_LOCKED) || paidActive;
            const BYTE a = locked ? 255 : g_opt.alpha;


            if (locked && g_veilAlpha > 0)
            {
                SolidBrush veil(Color(g_veilAlpha,
                    GetRValue(g_veilColor),
                    GetGValue(g_veilColor),
                    GetBValue(g_veilColor)));

                g.FillRectangle(&veil, 0.0f, 0.0f, (REAL)w, (REAL)h);
            }









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
                const DWORD perBoxMs = kPaidClearTotalMs * 5 / 10;
                const DWORD staggerMs = kPaidClearTotalMs - perBoxMs;
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


                const float pp = paidActive ? paidP[i] : 0.0f;
                float boxScale = 1.0f;
                float boxAlpha = 1.0f;
                float stopAlpha = 1.0f;
                float greenMix = 0.0f;

                if (paidActive) {




                    greenMix = 1.0f;


                    const float aPhase = (pp < 0.55f) ? (pp / 0.55f) : 1.0f;
                    stopAlpha = 1.0f - aPhase;



                    if (pp > 0.45f) {
                        const float bPhase = (pp - 0.45f) / 0.55f;
                        boxAlpha = 1.0f - bPhase;
                    }


                }


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


                if (locked) {

                    const float side = min(cx, cy) * 0.78f;
                    if (side > 10.0f) {







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


                if (!paidActive) {
                    long m = (long)(g_opt.thickness + 12.0f);
                    RECT dirtyRect = { r.left - m, r.top - m, r.right + m, r.bottom + m };
                    MapWindowPoints(NULL, m_hwnd, (LPPOINT)&dirtyRect, 2);
                    dirty.push_back(dirtyRect);
                }
            }














            if (locked && !rects.empty() && !paidActive)
            {
                const DWORD nowTick = GetTickCount();




                for (size_t i = 0; i < g_glitchParticles.size(); )
                {
                    if (--g_glitchParticles[i].life <= 0)
                        g_glitchParticles.erase(g_glitchParticles.begin() + i);
                    else
                        ++i;
                }






                std::set<std::pair<LONG, LONG> > alive;

                for (size_t ri = 0; ri < rects.size(); ++ri)
                {
                    const RECT& r = rects[ri];
                    const LONG bw = r.right - r.left;
                    const LONG bh = r.bottom - r.top;



                    if (bw < 16 || bh < 16) continue;

                    const std::pair<LONG, LONG> key(r.left, r.top);
                    alive.insert(key);

                    std::map<std::pair<LONG, LONG>, DWORD>::iterator it =
                        g_frameNextBurst.find(key);

                    if (it == g_frameNextBurst.end())
                    {



                        g_frameNextBurst[key] =
                            nowTick + 4000 + (DWORD)(rand() % 4001);
                        continue;
                    }

                    if (nowTick < it->second) continue;


                    const int n = 1 + (rand() % 4);
                    int made = 0;

                    for (int k = 0; k < n; ++k)
                    {
                        GlitchParticle p;



                        p.h = 10.0f + (float)(rand() % 11);




                        p.w = p.h * (0.4f + (float)(rand() % 211) / 100.0f);



                        const float marginX = p.w * 0.5f + 2.0f;
                        const float marginY = p.h * 0.5f + 2.0f;

                        const LONG xMin = r.left + (LONG)marginX;
                        const LONG xMax = r.right - (LONG)marginX;
                        const LONG yMin = r.top + (LONG)marginY;
                        const LONG yMax = r.bottom - (LONG)marginY;

                        if (xMin >= xMax || yMin >= yMax) continue;

                        p.x = (float)(xMin + (rand() % (xMax - xMin + 1)));
                        p.y = (float)(yMin + (rand() % (yMax - yMin + 1)));



                        p.startAlpha = (BYTE)(90 + (rand() % 166));

                        p.maxLife = 3;
                        p.life = p.maxLife;

                        g_glitchParticles.push_back(p);
                        ++made;
                    }



                    g_frameNextBurst[key] =
                        nowTick + 4000 + (DWORD)(rand() % 4001);

                    if (made > 0)
                        DLog(L"[overlay] 故障粒子：%d 个 @(%ld,%ld)（下次 %.1f 秒后）",
                            made, r.left, r.top,
                            (g_frameNextBurst[key] - nowTick) / 1000.0);
                }





                for (std::map<std::pair<LONG, LONG>, DWORD>::iterator
                    it = g_frameNextBurst.begin();
                    it != g_frameNextBurst.end(); )
                {
                    if (alive.find(it->first) == alive.end())
                        it = g_frameNextBurst.erase(it);
                    else
                        ++it;
                }


                if (!g_glitchParticles.empty())
                {
                    HDC hdc = g.GetHDC();
                    for (size_t i = 0; i < g_glitchParticles.size(); ++i)
                    {
                        const GlitchParticle& p = g_glitchParticles[i];
                        if (p.life <= 0) continue;


                        const float fade = (float)p.life / (float)p.maxLife;
                        const float alpha = ((float)p.startAlpha / 255.0f) * fade;
                        if (alpha < 0.02f) continue;

                        RECT pr;
                        pr.left = (LONG)(p.x - p.w * 0.5f - wr.left);
                        pr.top = (LONG)(p.y - p.h * 0.5f - wr.top);
                        pr.right = (LONG)(p.x + p.w * 0.5f - wr.left);
                        pr.bottom = (LONG)(p.y + p.h * 0.5f - wr.top);

                        face::BlitFaceAlpha(hdc, pr, true          , alpha);
                    }
                    g.ReleaseHDC(hdc);
                }





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
        }


        if (g_opt.look == overlay::LOOK_LOCKED && g_veilAlpha > 0)
        {
            RECT full = { 0, 0, w, h };
            dirty.push_back(full);
        }


        for (size_t i = 0; i < dirty.size(); ++i)
            Premultiply(m_bits, w, h, dirty[i]);

        BLENDFUNCTION bf;
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;

        SIZE  size = {w, h};
        POINT src  = {0, 0};

        BOOL ok = UpdateLayeredWindow(m_hwnd, m_screenDC, NULL, &size, m_memDC, &src, 0, &bf, ULW_ALPHA);

        static LogGate gateRender;
        if (gateRender.Pass((int)rects.size() * 2 + (ok ? 1 : 0)))
            DLog(L"Overlay::Render: %dx%d rects=%d UpdateLayeredWindow=%d err=%lu",
                 w, h, (int)rects.size(), (int)ok, GetLastError());
    }

private:




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
            if (screenArea > 0 && area < screenArea / 4) continue;
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
        bi.bmiHeader.biHeight      = -h;
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


namespace {

HINSTANCE              g_hInst          = NULL;
HWND                   g_hwnd           = NULL;
bool                   g_topmost        = false;
bool                   g_visible        = true;

DesktopRef             g_desktop;
std::set<std::wstring> g_names;
std::vector<RECT>      g_lastRects;
std::vector<RECT>      g_hitRects;
int                    g_shortcutCount  = 0;

int                    g_jitter = 0;
int                    g_offsetX = 0;
int                    g_offsetY = 0;

Overlay* g_overlay = NULL;














HHOOK                  g_mouseHook      = nullptr;
std::atomic<int>       g_blockedRights{ 0 };
std::atomic<int>       g_blockedClicks{ 0 };
std::atomic<int>       g_blockedDrags { 0 };
std::atomic<bool>      g_menuKeyDown  { false };
DWORD                  g_lastClickLogTick = 0;
POINT                  g_lastClickLogPt   = { 0, 0 };





struct DragLatch {
    bool  armed  = false;
    POINT origin = { 0, 0 };
};
DragLatch              g_dragLatch;


bool                   g_blockContextMenu = true;














bool PointOnDesktop(POINT pt)
{
    HWND under = WindowFromPoint(pt);
    if (!under) return false;





    if (under == g_hwnd) return true;




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






bool PointInLockedFrame(POINT pt)
{



    if (!PointOnDesktop(pt)) return false;

    for (size_t i = 0; i < g_hitRects.size(); ++i)
        if (PtInRect(&g_hitRects[i], pt)) return true;
    return false;
}

inline bool BlockingNow()
{
    return g_blockContextMenu && g_opt.look == overlay::LOOK_LOCKED;
}


inline bool IsRightButtonMsg(WPARAM wp)
{
    return wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP || wp == WM_RBUTTONDBLCLK;
}


















inline bool ModifierHeld()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
}


inline int DragThreshold()
{
    return max(4, GetSystemMetrics(SM_CXDRAG) / 2);
}






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




        if (wParam == WM_LBUTTONUP)
        {
            if (g_dragLatch.armed)
            {
                g_dragLatch.armed = false;
                if (LongDrag(pt))
                    return 1;
            }
        }


        if (IsRightButtonMsg(wParam) && inFrame)
        {


            if (wParam != WM_RBUTTONUP)
            {
                const int n = ++g_blockedRights;
                if (n == 1 || n % 20 == 0)
                    DLog(L"[overlay] 拦截右键菜单 #%d: (%ld,%ld) 落在已加密图标上",
                         n, pt.x, pt.y);
            }
            return 1;
        }


        if (wParam == WM_LBUTTONDOWN && inFrame)
        {


            if (ModifierHeld())
            {
                g_dragLatch.armed  = true;
                g_dragLatch.origin = pt;
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }



            g_dragLatch.armed = false;

            const int n = ++g_blockedClicks;
            if (ShouldLogClick(pt))
                DLog(L"[overlay] 吞掉普通左键 #%d: (%ld,%ld) 落在已加密图标上（防选中/防拖动）",
                     n, pt.x, pt.y);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}





bool SelectedAnyFrame()
{
    if (!g_desktop.Valid() || g_lastRects.empty()) return false;

    ListViewReader reader(g_desktop.hListView);
    if (!reader.Open()) return false;

    const int sel = (int)SendMessageW(g_desktop.hListView, LVM_GETNEXTITEM,
                                      (WPARAM)-1, MAKELPARAM(LVNI_SELECTED, 0));
    if (sel < 0) return false;

    RECT rcIcon = {0}, rcLabel = {0};
    const bool hasIcon  = reader.Rect(sel, LVIR_ICON,  rcIcon);
    const bool hasLabel = reader.Rect(sel, LVIR_LABEL, rcLabel);
    if (!hasIcon && !hasLabel) return false;

    RECT rc = hasIcon ? rcIcon : rcLabel;
    if (hasIcon && hasLabel) UnionRect(&rc, &rcIcon, &rcLabel);
    MapWindowPoints(g_desktop.hListView, NULL, (LPPOINT)&rc, 2);


    InflateRect(&rc, 4, 4);

    POINT pt;
    GetCursorPos(&pt);
    return PtInRect(&rc, pt) != FALSE;
}





HHOOK                  g_kbHook    = nullptr;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;
        const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);





        if (lockdown::ShouldSwallowKey(kb->vkCode))
            return 1;


        if (BlockingNow())
        {
            bool menuKey = (kb->vkCode == VK_APPS);
            if (!menuKey && kb->vkCode == VK_F10)
                menuKey = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            if (menuKey)
            {
                POINT pt;
                GetCursorPos(&pt);




                if (isDown && !g_menuKeyDown.load() &&
                    PointInLockedFrame(pt) && !SelectedAnyFrame())
                {
                    g_menuKeyDown = true;
                    DLog(L"[overlay] 拦截键盘唤出右键菜单: vk=0x%02X @(%ld,%ld)",
                         (unsigned)kb->vkCode, pt.x, pt.y);
                }


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










void ReHookInput()
{
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }

    const bool menuBlock = BlockingNow();



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

}








static void RefreshInternal(bool force)
{
    if (!g_overlay) return;

    std::vector<Frame> fresh;
    if (!BuildFrames(g_desktop, g_names, fresh))
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


static void OverlayTick()
{


    ReHookInput();



    lockdown::Tick();





    if (g_paidClearActive)
    {
        const DWORD el = GetTickCount() - g_paidClearStart;
        if (el >= kPaidClearTotalMs)
        {
            g_paidClearActive = false;
            g_glitchParticles.clear();
            g_frameNextBurst.clear();

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


namespace overlay {

bool Start(HINSTANCE hInst, bool topmost)
{
    if (g_hwnd) return true;

    g_hInst   = hInst;
    g_topmost = topmost;





    lockdown::AddOwnClass(kOverlayClass);
    lockdown::AddOwnClass(L"RansomFaceWnd");
    lockdown::AddOwnClass(L"RansomFxOverlay");
    lockdown::AddOwnClass(L"RansomAeroWnd");
    lockdown::AddOwnClass(L"RansomDirectorWnd");
    lockdown::AddOwnClass(L"RansomDevIpcWnd");

    DLog(L"overlay::Start(topmost=%d)", (int)topmost);

    {
        std::vector<std::wstring> folders;
        CollectShortcutNames(g_names, folders);
        DLog(L"  桌面目录 %d 个, 快捷方式名候选 %d 个",
             (int)folders.size(), (int)g_names.size());
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


    g_opt.interval = (l == LOOK_LOCKED) ? 70 : 300;
    if (g_hwnd) SetTimer(g_hwnd, kOverlayTimerId, (UINT)g_opt.interval, NULL);



    const bool locked = (l == LOOK_LOCKED);

    g_forceTopmost = false;
    g_veilAlpha    = 0;




    elog::Write(L"[overlay] 外观切换为 %s（刷新 %dms，红底 alpha=%d，锁定=%d）",
                locked ? L"已加密" : L"普通标记", g_opt.interval,
                (int)g_veilAlpha, (int)locked);



    ReHookInput();

    RefreshInternal(true);
}

Look GetLook() { return g_opt.look; }

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


void BeginPaidClear()
{


    if (g_opt.look != overlay::LOOK_LOCKED) return;
    if (g_paidClearActive) return;

    g_paidClearActive = true;
    g_paidClearStart = GetTickCount();



    g_glitchParticles.clear();
    g_frameNextBurst.clear();



    g_opt.interval = 33;
    if (g_hwnd) SetTimer(g_hwnd, kOverlayTimerId, (UINT)g_opt.interval, NULL);

    elog::Write(L"[overlay] 付清：开始桌面消散动画（%d 个方框，从左往右、从上往下，%lums）",
        (int)g_hitRects.size(), (unsigned long)kPaidClearTotalMs);



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
        std::vector<RECT> none;
        g_overlay->Render(none);
    }
}

bool Visible() { return g_visible; }





void SetBlockContextMenu(bool on)
{
    if (g_blockContextMenu == on) return;
    g_blockContextMenu = on;


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

}
