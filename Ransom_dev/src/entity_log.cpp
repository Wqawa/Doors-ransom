


#include "entity_log.h"

#include <cstdio>
#include <cstdlib>
#include <share.h>

namespace {

FILE* g_log = nullptr;
DWORD g_startTick = 0;

}

namespace elog {

void Open(const wchar_t* path)
{
    Close();
    if (!path || !*path) return;



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

}
