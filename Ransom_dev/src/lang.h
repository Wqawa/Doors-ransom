// ============================================================================
//  lang.h
//
//  多语言。**界面上的字全部从这里取**，代码里不再写死中文。
//
//  ---- 语言包住在哪儿 ----
//
//  `assets/lang/<代号>.lang`，纯 UTF-8 文本（见包文件开头的格式说明）。
//  这些文件跟图片、音频走**同一条路**：tools\gen_assets.ps1 扫描目录 ->
//  assets_gen.rc 生成 RCDATA 资源 -> 编译进 exe。所以产物仍然是单文件，
//  运行时不需要外部文件。
//
//  ---- 怎么加一种新语言 ----
//
//  照着 assets\lang\zh-CN.lang 拷一份、改名（比如 ja-JP.lang）、把值翻译掉，
//  丢回 assets\lang\ 重新编译即可 —— **一行 C++ 都不用改**：语言列表是
//  lang::Init() 扫资源清单得出来的，设置界面的下拉框自动多出一项。
//  某条键忘了翻也不怕：查不到会退回默认语言（zh-CN）那一份。
//
//  ---- 取词的两种姿势 ----
//
//    lang::T(L"ui.btn.start")                    -> 整句，直接拿去画
//    swprintf_s(buf, lang::T(L"ui.fmt.pct"), v)  -> 格式串，占位符照旧
//
//  T() 返回的指针**指向常驻内存**（语言包只在 Init 时解析一次，之后不再动），
//  可以安全地存下来、传给 GDI+。但切换语言之后就该重新取一次 —— 所以
//  界面代码一律**每帧现取**，不要把结果缓存成静态变量。
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace lang {

// 扫内嵌语言包并解析。要在 elog::Open 之后、任何界面画字之前调一次。
// 重复调用是安全的（第二次直接返回）。
void Init();

// 内建语言包数量（0 = 没打进任何包，这时候 T() 会把 key 原样返回）。
int  Count();

// 第 i 个语言包的代号（"zh-CN"）/ 显示名（包里的 meta.name）。
// 越界返回空串。指针常驻，可以存。
const wchar_t* Code(int i);
const wchar_t* Name(int i);

// 按代号找下标，找不到返回 -1。
int  IndexOfCode(const wchar_t* code);

// 当前语言下标 / 代号。
int  CurrentIndex();
const wchar_t* CurrentCode();

// 切到第 i 个语言。越界忽略。切完界面自重绘就会变。
void SetCurrent(int i);

// 按代号切（ini 里存的就是代号）。找不到就保持原样并返回 false。
bool SetCurrentCode(const wchar_t* code);

// 默认语言（zh-CN 在的话就是它，否则第 0 个）—— 缺词时的兜底。
int  DefaultIndex();

// 兜底语言：**用户选的那种整个包不存在**时改用它（比如他把 en-US.lang 之外的
// 包删了、或者 ini 里存了个拼错的代号）。规则：en-US 在就用 en-US
//（二十六個字母，谁都能连蒙带猜），没有才退回 DefaultIndex()。没有包时返回 -1。
//
// 注意和 DefaultIndex() 的分工：那个管"某一条键缺了"，这个管"整包缺了"。
int  FallbackIndex();

// 取词。key 形如 L"ui.lbl.safe"。
// 查找顺序：当前语言 -> 默认语言 -> 把 key 原样返回（缺词一眼能看出来）。
const wchar_t* T(const wchar_t* key);

} // namespace lang
