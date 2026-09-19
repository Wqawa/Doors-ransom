


#include "audio.h"

#include "assets.h"
#include "audio_clip.h"
#include "entity_log.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace {

    const double kSampleRate = 44100.0;
    const double kTwoPi = 6.283185307179586;










    enum ClipId {
        C_SPAWN = 0,
        C_CAUGHT,
        C_HIT,
        C_RISER,
        C_SUCCESS,
        C_ERROR,
        C_COIN,
        C_THEME,
        C_GLITCHLOOP,
        C_COUNT
    };

    const wchar_t* kFileNames[C_COUNT] = {
        L"jumpscare2.mp3",
        L"Ransom_start_(132683318015866).ogg",
        L"Glitchyhitfaster.ogg",
        L"Ransom_encounter.wav",
        L"ransom_success.ogg",
        L"Ransom_UI_-_Error_(80099403859001).ogg",
        L"Ransomgold_increase_(97004792231127).ogg",
        L"Ransom_full_theme.mp3",
        L"GEN_GLITCH_LOOP_(135402425939071).ogg",
    };








    const double kOneShotStartSec[C_COUNT] = {
        0.4,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
    };

    audio_clip::Clip g_clips[C_COUNT];
    int              g_loaded = 0;






    const double kThemeKeepSec = 80.0;
    const double kThemeOutSec = 90.0;


    const double kThemeFullGain = 0.55;


    const int kMaxOneShots = 8;




    const int kThemeSeekFadeFrames = 1500;




    std::atomic<int>  g_themeSeekReq{ 0 };
    int               g_themeSeekSeen = 0;
    std::atomic<long> g_themeSeekDeltaFrames{ 0 };


    struct LoopLayer {
        int    clipId = -1;
        size_t pos = 0;
        double gain = 0.0;
        double target = 0.0;





        bool loop = true;




        int  fadeInLeft = 0;

        void Tick()
        {
            if (fadeInLeft > 0) return;
            gain += (target - gain) * 0.00025;
            if (gain < 0.0001 && target == 0.0) gain = 0.0;
        }

        double Sample()
        {
            if (clipId < 0) return 0.0;



            if (fadeInLeft > 0)
            {
                --fadeInLeft;
                gain = target * (1.0 - (double)fadeInLeft / (double)kThemeSeekFadeFrames);
                if (gain < 0.0) gain = 0.0;
            }

            if (gain <= 0.0) return 0.0;
            const audio_clip::Clip& c = g_clips[clipId];
            if (!c.Ok()) return 0.0;


            if (pos >= c.Frames())
            {
                if (!loop) return 0.0;
                pos = 0;
            }

            const double v = (c.pcm[pos] / 32768.0) * gain;
            ++pos;
            if (loop && pos >= c.Frames()) pos = 0;
            return v;
        }
    };

    struct OneShot {
        int    clipId = -1;
        size_t pos = 0;
        double gain = 1.0;
        bool   active = false;
    };

    LoopLayer g_theme;
    LoopLayer g_bed;
    OneShot   g_shots[kMaxOneShots];


    std::atomic<int> g_req[C_COUNT];
    int              g_seen[C_COUNT];




    std::atomic<int> g_themeRestart{ 0 };
    int              g_themeRestartSeen = 0;

    std::atomic<int>  g_master{ 100 };
    std::atomic<int>  g_bgm{ 100 };
    std::atomic<int>  g_sfx{ 100 };



    std::atomic<DWORD> g_previewUntil{ 0 };
    HWAVEOUT          g_hwo = nullptr;
    HANDLE            g_thread = nullptr;
    std::atomic<bool> g_stop{ false };

    unsigned int g_rng = 0x12345678u;
    inline double RndBi()
    {
        g_rng = g_rng * 1664525u + 1013904223u;
        return ((double)((g_rng >> 8) & 0xFFFFFF) / 8388607.5) - 1.0;
    }


    void SpawnOneShot(int clipId)
    {
        if (clipId < 0 || clipId >= C_COUNT) return;



        size_t startPos = (size_t)(kOneShotStartSec[clipId] * kSampleRate);
        const audio_clip::Clip& c = g_clips[clipId];
        if (!c.Ok() || startPos >= c.Frames()) startPos = 0;

        for (int i = 0; i < kMaxOneShots; ++i)
        {
            if (g_shots[i].active) continue;
            g_shots[i].clipId = clipId;
            g_shots[i].pos = startPos;
            g_shots[i].gain = 1.0;
            g_shots[i].active = true;
            return;
        }

        g_shots[0].clipId = clipId;
        g_shots[0].pos = startPos;
        g_shots[0].gain = 1.0;
        g_shots[0].active = true;
    }


    void Fill(short* out, int n)
    {




        const double mGain   = g_master.load() / 100.0;
        const double bgmGain = g_bgm.load() / 100.0;
        const double sfxGain = g_sfx.load() / 100.0;
        const double themeGain = mGain * bgmGain;
        const double bedGain   = mGain * bgmGain;
        const double oneGain   = mGain * sfxGain;




        {
            const DWORD until = g_previewUntil.load();
            if (until != 0 && GetTickCount() >= until)
            {
                g_previewUntil.store(0);
                g_theme.target = 0.0;
                g_bed.target = 0.0;
                elog::Write(L"[audio] 试听结束");
            }
        }


        for (int t = 0; t < C_COUNT; ++t)
        {
            const int cur = g_req[t].load();
            while (g_seen[t] < cur) { SpawnOneShot(t); ++g_seen[t]; }
        }


        {
            const int cur = g_themeRestart.load();
            if (g_themeRestartSeen != cur)
            {
                g_themeRestartSeen = cur;
                g_theme.pos = 0;
                g_theme.loop = true;
                g_theme.fadeInLeft = 0;
            }
        }


        {
            const int cur = g_themeSeekReq.load();
            if (g_themeSeekSeen != cur)
            {
                g_themeSeekSeen = cur;

                const long delta = g_themeSeekDeltaFrames.load();
                const audio_clip::Clip& c = g_clips[g_theme.clipId];
                if (c.Ok())
                {
                    long long p = (long long)g_theme.pos + (long long)delta;
                    const long long len = (long long)c.Frames();





                    if (p >= len)
                    {
                        p = len;
                        g_theme.loop = false;
                    }
                    if (p < 0) p = 0;

                    g_theme.pos = (size_t)p;



                    g_theme.gain = 0.0;
                    g_theme.fadeInLeft = kThemeSeekFadeFrames;
                }
            }
        }

        for (int i = 0; i < n; ++i)
        {
            double s = 0.0;

            g_theme.Tick();
            g_bed.Tick();


            s += g_theme.Sample() * themeGain;
            s += g_bed.Sample() * bedGain;


            for (int v = 0; v < kMaxOneShots; ++v)
            {
                OneShot& o = g_shots[v];
                if (!o.active) continue;

                const audio_clip::Clip& c = g_clips[o.clipId];
                if (!c.Ok() || o.pos >= c.Frames()) { o.active = false; continue; }

                s += (c.pcm[o.pos] / 32768.0) * o.gain * oneGain;
                ++o.pos;
            }





            if (s > 1.0) s = 1.0;
            if (s < -1.0) s = -1.0;

            out[i] = (short)(s * 32000.0);
        }
    }

    DWORD WINAPI AudioThread(LPVOID)
    {
        const int kBufs = 4;
        const int kSamples = 4096;

        std::vector<short>   data((size_t)kBufs * kSamples, 0);
        std::vector<WAVEHDR> hdr(kBufs);

        for (int i = 0; i < kBufs; ++i)
        {
            ZeroMemory(&hdr[i], sizeof(WAVEHDR));
            hdr[i].lpData = (LPSTR)&data[(size_t)i * kSamples];
            hdr[i].dwBufferLength = kSamples * sizeof(short);

            if (waveOutPrepareHeader(g_hwo, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            {
                elog::Write(L"[audio] waveOutPrepareHeader 失败 (buf %d)", i);
                return 0;
            }
            Fill(&data[(size_t)i * kSamples], kSamples);
            waveOutWrite(g_hwo, &hdr[i], sizeof(WAVEHDR));
        }

        while (!g_stop.load())
        {
            for (int i = 0; i < kBufs && !g_stop.load(); ++i)
            {
                while (!(hdr[i].dwFlags & WHDR_DONE))
                {
                    if (g_stop.load()) break;
                    Sleep(5);
                }
                if (g_stop.load()) break;

                Fill(&data[(size_t)i * kSamples], kSamples);
                hdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_hwo, &hdr[i], sizeof(WAVEHDR));
            }
        }

        waveOutReset(g_hwo);
        for (int i = 0; i < kBufs; ++i)
            waveOutUnprepareHeader(g_hwo, &hdr[i], sizeof(WAVEHDR));

        return 0;
    }



    void EditTheme()
    {
        audio_clip::Clip& c = g_clips[C_THEME];
        if (!c.Ok()) return;

        const size_t keep = (size_t)(kThemeKeepSec * kSampleRate);
        if (c.Frames() <= keep)
        {
            elog::Write(L"[audio] 主题曲 %.2f 秒，不长于 %.0f 秒，跳过裁剪与慢放",
                c.Seconds(), kThemeKeepSec);
            return;
        }

        const double factor = kThemeOutSec / kThemeKeepSec;
        const double before = c.Seconds();

        audio_clip::Clip out;
        const DWORD t0 = GetTickCount();
        if (!audio_clip::StretchPitchPreserving(c, keep, factor, out))
        {
            elog::Write(L"[audio] 主题曲慢放失败，退回未处理版本（%.2f 秒）", before);
            return;
        }
        const DWORD ms = GetTickCount() - t0;

        elog::Write(L"[audio] 主题曲已处理：原 %.2f 秒 -> 保留前 %.1f 秒 -> 慢放 x%.4f -> %.2f 秒（耗时 %lums）",
            before, kThemeKeepSec, factor, out.Seconds(), (unsigned long)ms);

        c = out;
    }

}

namespace audio {

    bool Start()
    {







        if (g_hwo) return true;



        g_loaded = 0;
        assets::Blob blob;
        for (int i = 0; i < C_COUNT; ++i)
        {
            if (assets::Get(assets::KIND_AUDIO, kFileNames[i], blob))
                if (audio_clip::Load(kFileNames[i], blob.Data(), blob.Size(), g_clips[i]))
                    ++g_loaded;
        }

        elog::Write(L"[audio] 素材来源 %s —— 成功载入 %d / %d 个",
                    assets::Where(assets::KIND_AUDIO, kFileNames[0]).c_str(),
                    g_loaded, C_COUNT);


        for (int i = 0; i < C_COUNT; ++i)
            if (kOneShotStartSec[i] > 0.0)
                elog::Write(L"[audio]   起播偏移: %s -> %.2f 秒",
                    kFileNames[i], kOneShotStartSec[i]);


        EditTheme();

        if (g_hwo) return true;

        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = (DWORD)kSampleRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

        const MMRESULT mr = waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
        if (mr != MMSYSERR_NOERROR)
        {
            elog::Write(L"[audio] waveOutOpen 失败 mr=%u（没有音频设备？静默降级）", (unsigned)mr);
            g_hwo = nullptr;
            return false;
        }

        for (int i = 0; i < C_COUNT; ++i) { g_req[i] = 0; g_seen[i] = 0; }

        g_theme.clipId = C_THEME;
        g_theme.pos = 0;
        g_theme.gain = 0.0;
        g_theme.target = 0.0;

        g_bed.clipId = C_GLITCHLOOP;
        g_bed.pos = 0;
        g_bed.gain = 0.0;
        g_bed.target = 0.0;

        g_stop = false;
        g_thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
        if (!g_thread)
        {
            elog::Write(L"[audio] 合成线程创建失败, err=%lu", GetLastError());
            waveOutClose(g_hwo);
            g_hwo = nullptr;
            return false;
        }

        elog::Write(L"[audio] 已启动 %dHz 16bit 单声道", (int)kSampleRate);
        return true;
    }

    void Stop()
    {
        if (!g_hwo) return;

        g_stop = true;
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }

        waveOutClose(g_hwo);
        g_hwo = nullptr;

        for (int i = 0; i < C_COUNT; ++i) g_clips[i] = audio_clip::Clip();
        g_loaded = 0;
        g_themeRestartSeen = g_themeRestart.load();

        elog::Write(L"[audio] 已停止");
    }

    bool Active() { return g_hwo != nullptr; }

    int LoadedClips() { return g_loaded; }
    int TotalClips() { return C_COUNT; }

    void SetMaster(int level)
    {
        if (level < 0)   level = 0;
        if (level > 100) level = 100;
        g_master = level;
    }

    int Master() { return g_master.load(); }



    void SetBgmLevel(int percent)
    {
        if (percent < 0)   percent = 0;
        if (percent > 200) percent = 200;
        g_bgm = percent;
    }

    int BgmLevel() { return g_bgm.load(); }

    void SetSfxLevel(int percent)
    {
        if (percent < 0)   percent = 0;
        if (percent > 200) percent = 200;
        g_sfx = percent;
    }

    int SfxLevel() { return g_sfx.load(); }

    void SetTheme(bool on)
    {
        g_theme.target = on ? kThemeFullGain : 0.0;




        if (on) ++g_themeRestart;



        const wchar_t* why = !g_hwo ? L"（音频未启动）"
            : (g_clips[C_THEME].Ok() ? L"" : L"（素材缺失）");
        elog::Write(L"[audio] 主题曲 %s%s（%.2f 秒，从头起播）",
            on ? L"开启" : L"关闭", why, g_clips[C_THEME].Seconds());
    }

    void SetThemeLevel(int percent)
    {
        if (percent < 0)   percent = 0;
        if (percent > 100) percent = 100;



        g_theme.target = (percent / 100.0) * kThemeFullGain;
    }

    void SetGlitchBed(int level)
    {
        if (level < 0)   level = 0;
        if (level > 100) level = 100;
        g_bed.target = (level / 100.0) * 0.42;
    }

    void PlayGlitch() { ++g_req[C_SPAWN];      elog::Write(L"[audio] → spawn (jumpscare2, 起播 0.4s)"); }
    void PlayCaught() { ++g_req[C_CAUGHT];     elog::Write(L"[audio] → 被抓 (Ransom_start)"); }
    void PlayHit() { ++g_req[C_HIT];        elog::Write(L"[audio] → 跳杀 (Glitchyhitfaster)"); }
    void PlayRiser() { ++g_req[C_RISER];      elog::Write(L"[audio] → riser (Ransom_encounter)"); }
    void PlayError() { ++g_req[C_ERROR];      elog::Write(L"[audio] → UI 错误"); }
    void PlayCoin() { ++g_req[C_COIN];       elog::Write(L"[audio] → 金币"); }
    void PlaySuccess() { ++g_req[C_SUCCESS];    elog::Write(L"[audio] → 赎回成功 (ransom_success)"); }

    void Silence()
    {
        g_theme.target = 0.0;
        g_bed.target = 0.0;
        g_previewUntil.store(0);
        for (int i = 0; i < kMaxOneShots; ++i) g_shots[i].active = false;
    }

    void PreviewBgm(DWORD previewMs)
    {

        if (!g_hwo) return;
        if (previewMs < 200) previewMs = 200;

        ++g_themeRestart;
        g_theme.target = kThemeFullGain;
        g_bed.target = (6 / 100.0) * 0.42;

        g_previewUntil.store(GetTickCount() + previewMs);
    }

    void PreviewSfx()
    {
        if (!g_hwo) return;
        ++g_req[C_SPAWN];
    }

    void SeekThemeBy(double seconds)
    {


        if (!g_hwo) return;

        const long frames = (long)(seconds * kSampleRate);
        if (frames == 0) return;

        g_themeSeekDeltaFrames.store(frames);
        ++g_themeSeekReq;

        elog::Write(L"[audio] 主题曲位置跳变 %+.2f 秒（约 %ld 帧）",
            seconds, frames);
    }


    namespace {

        bool WriteWav16(const wchar_t* path, const short* data, int count)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;

            const DWORD dataBytes = (DWORD)(count * sizeof(short));
            const DWORD rate = (DWORD)kSampleRate;
            const WORD  channels = 1, bits = 16;
            const WORD  align = (WORD)(channels * bits / 8);
            const DWORD byteRate = rate * align;

            auto w32 = [&](DWORD v) { fwrite(&v, 4, 1, f); };
            auto w16 = [&](WORD  v) { fwrite(&v, 2, 1, f); };
            auto tag = [&](const char* s) { fwrite(s, 1, 4, f); };

            tag("RIFF"); w32(36 + dataBytes); tag("WAVE");
            tag("fmt "); w32(16); w16(1); w16(channels);
            w32(rate); w32(byteRate); w16(align); w16(bits);
            tag("data"); w32(dataBytes);
            fwrite(data, 1, dataBytes, f);
            fclose(f);
            return true;
        }

    }

    bool DumpMix(const wchar_t* path, int seconds)
    {
        if (!path || !*path || seconds < 1) return false;

        const int n = (int)(kSampleRate * seconds);
        std::vector<short> buf((size_t)n, 0);


        for (int i = 0; i < kMaxOneShots; ++i) g_shots[i].active = false;
        for (int i = 0; i < C_COUNT; ++i) { g_req[i] = 0; g_seen[i] = 0; }

        const int savedMaster = g_master.load();
        const int savedBgm    = g_bgm.load();
        const int savedSfx    = g_sfx.load();
        g_master = 100;
        g_bgm    = 100;
        g_sfx    = 100;
        g_previewUntil.store(0);
        g_theme.target = kThemeFullGain;
        g_bed.target = 0.20;




        struct Cue { double at; int clip; const wchar_t* name; };
        const Cue cues[] = {
            { 0.30, C_SPAWN,  L"spawn / jumpscare2 (从 0.4s 起)" },
            { 1.20, C_ERROR,  L"UI error"                  },
            { 2.20, C_COIN,   L"gold increase"             },
            { 3.00, C_CAUGHT, L"caught / Ransom_start"     },
            { 4.00, C_HIT,    L"hit / Glitchyhitfaster"    },
            { 5.50, C_RISER,  L"riser / Ransom_encounter"  },
        };
        const int cueCount = (int)(sizeof(cues) / sizeof(cues[0]));

        int pos = 0, nextCue = 0;
        const int chunk = 1024;

        while (pos < n)
        {
            const double t = (double)pos / kSampleRate;
            while (nextCue < cueCount && cues[nextCue].at <= t)
            {
                ++g_req[cues[nextCue].clip];
                elog::Write(L"[audio] 导出时间表 %.2fs -> %s", cues[nextCue].at, cues[nextCue].name);
                ++nextCue;
            }

            const int len = (pos + chunk <= n) ? chunk : (n - pos);
            Fill(&buf[(size_t)pos], len);
            pos += len;
        }

        const bool ok = WriteWav16(path, buf.data(), n);

        Silence();
        g_master = savedMaster;
        g_bgm    = savedBgm;
        g_sfx    = savedSfx;
        for (int i = 0; i < kMaxOneShots; ++i) g_shots[i].active = false;

        elog::Write(L"[audio] 已导出 %d 秒混音到 %s（成功=%d）", seconds, path, (int)ok);
        return ok;
    }

    bool DumpTheme(const wchar_t* path)
    {
        const audio_clip::Clip& c = g_clips[C_THEME];
        if (!c.Ok())
        {
            elog::Write(L"[audio] 主题曲未载入，无法导出");
            return false;
        }



        const bool ok = WriteWav16(path, c.pcm.data(), (int)c.pcm.size());
        elog::Write(L"[audio] 主题曲导出%s（%.2f 秒，%zu 帧）",
            ok ? L"成功" : L"失败", c.Seconds(), c.Frames());
        return ok;
    }

}