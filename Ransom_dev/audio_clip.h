// ============================================================================
//  audio_clip.h
//
//  音频文件解码前端：把 .wav / .ogg / .mp3 解码成混音器能直接吃的 PCM。
//
//  为什么要有这一层：
//   * Windows 自带 WAV 和 MP3 解码，但**不带 Vorbis(OGG) 解码器**，
//     而这套素材里 5 个音效都是 OGG。所以这里内嵌两个单文件公有领域解码器：
//       - stb_vorbis.c  (public domain)  -> OGG
//       - minimp3       (CC0)            -> MP3
//   * 统一重采样成 44100Hz 单声道，混音器里就不用再做采样率转换了。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>

namespace audio_clip {

struct Clip {
    std::vector<short> pcm;          // 44100Hz 单声道
    int  srcRate     = 0;            // 源采样率（记录用）
    int  srcChannels = 0;            // 源声道数（记录用）

    bool   Ok() const     { return !pcm.empty(); }
    size_t Frames() const { return pcm.size(); }
    double Seconds() const { return (double)pcm.size() / 44100.0; }
};

// 从**内存**解码。素材全部内嵌在 exe 里，磁盘上根本没有文件，
// 所以三个解码器都走各自的内存入口：
//     WAV  -> 自己按 RIFF 块扫（跟以前一样，只是数据源换成内存指针）
//     OGG  -> stb_vorbis_decode_memory
//     MP3  -> mp3dec_load_buf
// name 只用来判断格式（看扩展名）和写日志。
bool Load(const wchar_t* name, const unsigned char* data, size_t size, Clip& out);

// 保持音高的时间拉伸（WSOLA：波形相似度重叠相加）。
// 只取 src 的**前 inFrames 帧**，输出 inFrames*factor 帧（用于「裁到 1:20
// 再慢放到 1:30」这种处理）。factor > 1 = 变慢变长。
//
// 为什么不用重采样：改变读取速度会连带改变音高，那是「慢放 + 降调」。
// 这里要的是**慢放不变调**，所以必须走重叠相加，而不是重新采样。
//
// 素材太短或参数不合法时返回 false，out 保持为空。
bool StretchPitchPreserving(const Clip& src, size_t inFrames,
                            double factor, Clip& out);

} // namespace audio_clip
