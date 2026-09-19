





#pragma once

#include <cstddef>

namespace Gdiplus { class Bitmap; }

namespace image_blob {




Gdiplus::Bitmap* Decode(const unsigned char* data, size_t size);

}
