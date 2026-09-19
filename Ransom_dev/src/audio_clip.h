











#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>

namespace audio_clip {

struct Clip {
    std::vector<short> pcm;
    int  srcRate     = 0;
    int  srcChannels = 0;

    bool   Ok() const     { return !pcm.empty(); }
    size_t Frames() const { return pcm.size(); }
    double Seconds() const { return (double)pcm.size() / 44100.0; }
};







bool Load(const wchar_t* name, const unsigned char* data, size_t size, Clip& out);









bool StretchPitchPreserving(const Clip& src, size_t inFrames,
                            double factor, Clip& out);

}
