





#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>

namespace elog {


void Open(const wchar_t* path);


void Close();


bool Active();


void Write(const wchar_t* fmt, ...);


void WriteV(const wchar_t* fmt, va_list ap);

}
