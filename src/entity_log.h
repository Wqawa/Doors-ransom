// ============================================================================
//  entity_log.h
//
//  公共诊断日志。GUI 子系统没有控制台，stdout 写出去没人看，
//  所以各个模块统一把过程写进同一个文件，方便排查。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdarg>

namespace elog {

// 打开（或重开）日志文件，UTF-8。传 nullptr / 空串表示只关闭不打开。
void Open(const wchar_t* path);

// 关闭日志文件。
void Close();

// 是否已经在写日志。
bool Active();

// 写一行。未打开日志时是空操作。
void Write(const wchar_t* fmt, ...);

// 供已有 va_list 的包装层使用。
void WriteV(const wchar_t* fmt, va_list ap);

} // namespace elog
