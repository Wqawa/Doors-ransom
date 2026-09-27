// ============================================================================
//  assets.h
//
//  素材访问的唯一入口。
//
//  素材**默认全部内嵌在 exe 的资源段里**（由 tools\gen_assets.ps1 生成的
//  assets_gen.rc 编译进去），所以产物是一个不依赖任何外部文件的单文件 exe。
//
//  想改素材又不想重编，可以给 --image-dir / --audio-dir 指定一个真实目录，
//  那一类素材就改成读盘。两种来源对调用方是透明的：都走 Get()。
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace assets {

// 素材分三类，决定去哪个子目录找。
// 注意注释末尾**不能留反斜杠**——行尾的 '\' 在 C++ 里是续行符，
// 会把后面一整行吞进注释里（这里踩过一次）。
enum Kind
{
    KIND_ROOT = 0,   // assets 根目录：main_window.ini / payup.ini / *.ico / *.ttf
    KIND_IMAGE,      // assets/image/
    KIND_AUDIO,      // assets/audio/
    KIND_COUNT
};

// 一份素材的字节。内嵌和读盘都统一成这个，调用方不用关心来源。
struct Blob
{
    std::vector<unsigned char> bytes;

    bool   Ok()   const { return !bytes.empty(); }
    const unsigned char* Data() const { return bytes.empty() ? nullptr : bytes.data(); }
    size_t Size() const { return bytes.size(); }
};

// ---- 来源切换 ----
// dir 传 nullptr 或空串表示回到内嵌资源。
void         UseDiskDir(Kind k, const wchar_t* dir);
bool         DiskMode(Kind k);
std::wstring DiskDir(Kind k);            // 读盘模式下的目录；内嵌模式返回空

// ---- 取素材 ----
// name 是**文件名**（含扩展名，不带目录）。找不到返回 false，out 被清空。
bool Get(Kind k, const wchar_t* name, Blob& out);

// 按通配符找第一个文件名（只支持 "*.ext" 和精确文件名）。
// 找不到返回空串。图标、字体都用它找——不写死文件名，改名也不会失效。
std::wstring FindFirst(Kind k, const wchar_t* pattern);

// 日志用：这份素材从哪儿来（"内嵌资源" 或完整路径）
std::wstring Where(Kind k, const wchar_t* name);

// 内嵌资源条数（启动时打一行日志，确认打包成功）
int EmbeddedCount();

// exe 所在目录（带尾部反斜杠）。--face-dump 之类要写文件的功能用。
std::wstring ExeDir();

} // namespace assets
