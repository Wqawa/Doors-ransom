


#include "audio_clip.h"

#include "entity_log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>




#define MINIMP3_IMPLEMENTATION

#pragma warning(push)
#pragma warning(disable: 4244 4267 4456 4457 4701 4702 4703 4996)
#include "third_party/minimp3_ex.h"
#include "third_party/stb_vorbis.c"
#pragma warning(pop)

#pragma comment(lib, "winmm.lib")

namespace {

const int kOutRate = 44100;



void Normalize(const short* src, size_t frames, int channels, int rate,
               audio_clip::Clip& out)
{
    out.pcm.clear();
    out.srcRate     = rate;
    out.srcChannels = channels;

    if (!src || frames == 0 || channels < 1 || rate < 1) return;

    if (rate == kOutRate && channels == 1)
    {
        out.pcm.assign(src, src + frames);
        return;
    }

    const double ratio  = (double)rate / (double)kOutRate;
    const size_t outN   = (size_t)((double)frames / ratio);
    out.pcm.resize(outN);

    for (size_t i = 0; i < outN; ++i)
    {
        const double pos = i * ratio;
        const size_t i0  = (size_t)pos;
        const size_t i1  = (i0 + 1 < frames) ? i0 + 1 : i0;
        const double fr  = pos - (double)i0;

        double a = 0.0, b = 0.0;
        for (int c = 0; c < channels; ++c)
        {
            a += src[i0 * channels + c];
            b += src[i1 * channels + c];
        }
        a /= channels;
        b /= channels;

        const double v = a + (b - a) * fr;
        out.pcm[i] = (short)(v < -32768.0 ? -32768.0 : (v > 32767.0 ? 32767.0 : v));
    }
}



bool LoadWav(const unsigned char* data, size_t size, audio_clip::Clip& out)
{
    if (!data || size < 12) return false;

    const unsigned char* end = data + size;
    const unsigned char* p   = data;

    if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return false;
    p += 12;

    int   channels = 0, rate = 0, bits = 0;
    bool  isFloat  = false;
    std::vector<short> pcm;
    long  frames   = 0;

    while (p + 8 <= end)
    {
        char id[4];
        memcpy(id, p, 4);
        unsigned int sz = 0;
        memcpy(&sz, p + 4, 4);
        p += 8;

        const size_t avail = (size_t)(end - p);
        if (sz > avail) sz = (unsigned int)avail;

        if (memcmp(id, "fmt ", 4) == 0)
        {
            unsigned char fmt[40] = { 0 };
            const unsigned int take = (sz < sizeof(fmt)) ? sz : (unsigned int)sizeof(fmt);
            memcpy(fmt, p, take);

            const unsigned short tag = *(unsigned short*)(fmt + 0);
            channels = *(unsigned short*)(fmt + 2);
            rate     = *(unsigned int*) (fmt + 4);
            bits     = *(unsigned short*)(fmt + 14);
            isFloat  = (tag == 3);
            if (tag == 0xFFFE && take >= 26)
            {
                const unsigned short sub = *(unsigned short*)(fmt + 24);
                isFloat = (sub == 3);
            }
        }
        else if (memcmp(id, "data", 4) == 0)
        {
            if (channels < 1 || rate < 1 || bits < 8) {                   }
            else
            {
                const long bytesPerSample = bits / 8;
                frames = (long)(sz / (bytesPerSample * channels));
                if (frames <= 0) break;

                const size_t need = (size_t)frames * channels;
                pcm.resize(need);

                if (bits == 16 && !isFloat)
                {
                    if (sz >= need * 2) memcpy(pcm.data(), p, need * 2);
                    else                pcm.clear();
                }
                else if (bits == 8 && !isFloat)
                {
                    if (sz >= need)
                        for (size_t i = 0; i < need; ++i)
                            pcm[i] = (short)(((int)p[i] - 128) * 256);
                    else pcm.clear();
                }
                else if (bits == 24 && !isFloat)
                {
                    if (sz >= need * 3)
                        for (size_t i = 0; i < need; ++i)
                        {
                            const unsigned char* q = p + i * 3;
                            int v = (q[0] | (q[1] << 8) | (q[2] << 16));
                            if (v & 0x800000) v |= ~0xFFFFFF;
                            pcm[i] = (short)(v >> 8);
                        }
                    else pcm.clear();
                }
                else if (bits == 32 && isFloat)
                {
                    if (sz >= need * 4)
                        for (size_t i = 0; i < need; ++i)
                        {
                            float fv = 0.0f;
                            memcpy(&fv, p + i * 4, 4);
                            double v = fv * 32767.0;
                            if (v >  32767.0) v =  32767.0;
                            if (v < -32768.0) v = -32768.0;
                            pcm[i] = (short)v;
                        }
                    else pcm.clear();
                }
                else if (bits == 32 && !isFloat)
                {
                    if (sz >= need * 4)
                        for (size_t i = 0; i < need; ++i)
                        {
                            int v = 0;
                            memcpy(&v, p + i * 4, 4);
                            pcm[i] = (short)(v >> 16);
                        }
                    else pcm.clear();
                }
                else
                {
                    pcm.clear();
                }
                break;
            }
        }

        p += sz;
        if (sz & 1) ++p;
    }

    if (pcm.empty()) return false;
    Normalize(pcm.data(), (size_t)frames, channels, rate, out);
    return out.Ok();
}


bool LoadOgg(const unsigned char* data, size_t size, audio_clip::Clip& out)
{
    int   channels = 0, rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(data, (int)size, &channels, &rate, &pcm);

    if (frames <= 0 || !pcm)
    {
        if (pcm) free(pcm);
        return false;
    }

    Normalize(pcm, (size_t)frames, channels, rate, out);
    free(pcm);
    return out.Ok();
}


bool LoadMp3(const unsigned char* data, size_t size, audio_clip::Clip& out)
{
    mp3dec_t dec;
    mp3dec_init(&dec);

    mp3dec_file_info_t info = {};
    if (mp3dec_load_buf(&dec, data, size, &info, nullptr, nullptr) != 0 ||
        !info.buffer || info.samples == 0)
    {
        if (info.buffer) free(info.buffer);
        return false;
    }

    const int channels = info.channels;
    const size_t frames = info.samples / (channels > 0 ? channels : 1);

    Normalize(info.buffer, frames, channels, info.hz, out);
    free(info.buffer);
    return out.Ok();
}

}

namespace audio_clip {

bool Load(const wchar_t* name, const unsigned char* data, size_t size, Clip& out)
{
    out = Clip();
    if (!name || !*name || !data || size == 0) return false;

    const std::wstring p = name;
    const size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;

    std::wstring ext = p.substr(dot);
    for (size_t i = 0; i < ext.size(); ++i) ext[i] = (wchar_t)towlower(ext[i]);

    bool ok = false;
    if      (ext == L".wav") ok = LoadWav(data, size, out);
    else if (ext == L".ogg") ok = LoadOgg(data, size, out);
    else if (ext == L".mp3") ok = LoadMp3(data, size, out);

    if (ok)
    {
        elog::Write(L"[clip] 载入 %s  ->  %d 声道 %dHz  %.2f 秒",
                    name, out.srcChannels, out.srcRate, out.Seconds());
    }
    else
    {
        elog::Write(L"[clip] 载入失败: %s", name);
    }
    return ok;
}













namespace {

const int kSF    = 2048;
const int kSHop  = 512;
const int kSSrch = 80;
const int kSCorr = 384;
const int kSStep = 6;

}

bool StretchPitchPreserving(const Clip& src, size_t inFrames,
                            double factor, Clip& out)
{
    out = Clip();

    if (!src.Ok() || factor <= 0.0) return false;
    if (inFrames > src.Frames()) inFrames = src.Frames();
    if (inFrames < (size_t)(kSF * 4)) return false;

    const size_t outFrames = (size_t)((double)inFrames * factor);
    if (outFrames < (size_t)kSF) return false;

    std::vector<double> win(kSF);
    for (int i = 0; i < kSF; ++i)
        win[i] = 0.5 * (1.0 - cos(6.283185307179586 * (double)i / (double)(kSF - 1)));

    std::vector<double> acc(outFrames + kSF, 0.0);
    std::vector<double> wsum(outFrames + kSF, 0.0);

    const short* s = src.pcm.data();
    const double analysisHop = (double)kSHop / factor;

    size_t prevSeg  = 0;
    bool   havePrev = false;
    int    k        = 0;

    for (;;)
    {
        const size_t synPos = (size_t)k * (size_t)kSHop;
        if (synPos + kSF > outFrames) break;

        const double idealD = (double)k * analysisHop;
        if (idealD + kSF + kSSrch >= (double)inFrames) break;

        const size_t ideal = (size_t)(idealD + 0.5);
        size_t seg = ideal;


        if (havePrev && prevSeg + kSHop + kSCorr <= inFrames)
        {
            const short* ref = s + prevSeg + kSHop;

            size_t lo = (ideal > (size_t)kSSrch) ? ideal - kSSrch : 0;
            size_t hi = ideal + kSSrch;
            if (hi + kSF > inFrames) hi = inFrames - kSF;

            double best = -1.0;
            for (size_t cand = lo; cand <= hi; ++cand)
            {
                const short* c = s + cand;
                long long dot = 0, energy = 0;
                for (int i = 0; i < kSCorr; i += kSStep)
                {
                    const long long a = c[i];
                    dot    += a * (long long)ref[i];
                    energy += a * a;
                }
                if (energy <= 0) continue;

                const double score = (double)dot * (double)dot / (double)energy;
                if (score > best) { best = score; seg = cand; }
            }
        }

        const short* p = s + seg;
        for (int i = 0; i < kSF; ++i)
        {
            acc[synPos + i]  += (double)p[i] * win[i];
            wsum[synPos + i] += win[i];
        }

        prevSeg  = seg;
        havePrev = true;
        ++k;
    }

    if (k == 0) return false;

    out.pcm.resize(outFrames);
    for (size_t i = 0; i < outFrames; ++i)
    {
        const double w = wsum[i];
        double v = (w > 1e-6) ? acc[i] / w : 0.0;
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        out.pcm[i] = (short)(v >= 0.0 ? v + 0.5 : v - 0.5);
    }

    out.srcRate     = src.srcRate;
    out.srcChannels = src.srcChannels;
    return true;
}

}
