


#include "image_blob.h"

#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace image_blob {

Bitmap* Decode(const unsigned char* data, size_t size)
{
    if (!data || size == 0) return nullptr;




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



    Bitmap* copy = src->Clone(0, 0, w, h, PixelFormat32bppARGB);

    delete src;
    st->Release();

    if (!copy || copy->GetLastStatus() != Ok) { delete copy; return nullptr; }
    return copy;
}

}
