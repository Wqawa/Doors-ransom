// ============================================================================
//  image_blob.cpp
// ============================================================================
#include "image_blob.h"

#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>      // SHCreateMemStream

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace image_blob {

Bitmap* Decode(const unsigned char* data, size_t size)
{
    if (!data || size == 0) return nullptr;

    // SHCreateMemStream 直接把一段内存包成 IStream，比手搓
    // CreateStreamOnHGlobal + GlobalAlloc + 拷贝短得多。
    // 它拷贝了一份数据，所以调用方的缓冲区可以立刻释放。
    IStream* st = SHCreateMemStream(data, (UINT)size);
    if (!st) return nullptr;

    Bitmap* src = Bitmap::FromStream(st);
    if (!src || src->GetLastStatus() != Ok)
    {
        delete src;
        st->Release();
        return nullptr;
    }

    const int w = (int)src->GetWidth();
    const int h = (int)src->GetHeight();
    if (w <= 0 || h <= 0)
    {
        delete src;
        st->Release();
        return nullptr;
    }

    // Clone 出一份自有的像素：这样解码结果和那个临时流彻底脱钩，
    // 后面可以放心 Release 掉流（GDI+ 的 Image 会一直引用它）。
    Bitmap* copy = src->Clone(0, 0, w, h, PixelFormat32bppARGB);

    delete src;
    st->Release();

    if (!copy || copy->GetLastStatus() != Ok) { delete copy; return nullptr; }
    return copy;
}

} // namespace image_blob
