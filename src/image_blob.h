// ============================================================================
//  image_blob.h
//
//  从**内存**里解一张图。素材现在内嵌在 exe 里，GDI+ 的 Bitmap::FromFile
//  用不上了，只能走 IStream。
// ============================================================================
#pragma once

#include <cstddef>

namespace Gdiplus { class Bitmap; }

namespace image_blob {

// 解出来的位图归调用方 delete。失败返回 nullptr。
// 返回的位图是**独立的一份**（内部已经 Clone 过），
// 不依赖传入的缓冲区，也不依赖内部那个临时流。
Gdiplus::Bitmap* Decode(const unsigned char* data, size_t size);

} // namespace image_blob
