// ============================================================================
//  entity_log.cpp
// ============================================================================
#include "entity_log.h"

#include <cstdio>
#include <cstdlib>      // _countof
#include <share.h>      // _SH_DENYNO

namespace {

FILE* g_log = nullptr;
DWORD g_startTick = 0;

} // namespace

namespace elog {

void Open(const wchar_t* path)
{
    Close();
    if (!path || !*path) return;
    // _SH_DENYNO：允许别的进程同时读。程序运行期间也能 tail 日志，
    // 排查长时演出问题时这一点很重要（_wfopen_s 默认会挡住读者）。
    // ccs=UTF-8：让 CRT 直接把宽字符写成 UTF-8，不用手动转换。
    g_log = _wfsopen(path, L"w, ccs=UTF-8", _SH_DENYNO);
    g_startTick = GetTickCount();
}

void Close()
{
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

bool Active() { return g_log != nullptr; }

void WriteV(const wchar_t* fmt, va_list ap)
{
    if (!g_log) return;

    wchar_t body[2048];
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);

    // 每行前面带相对启动的毫秒数 —— 排查时序问题（比如「隔了多久才开始」）时
    // 光有时间线看是不够的，得有数字。
    fwprintf(g_log, L"[%6lums] %s\n",
             (unsigned long)(GetTickCount() - g_startTick), body);
    fflush(g_log);
}

void Write(const wchar_t* fmt, ...)
{
    if (!g_log) return;

    va_list ap;
    va_start(ap, fmt);
    WriteV(fmt, ap);
    va_end(ap);
}

} // namespace elog
