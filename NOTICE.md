# 第三方内容与素材来源

这个仓库分三类内容，授权情况各不相同。

---

## 1. 源代码 —— MIT

`Ransom_dev/src/` 下的自有代码（除 `third_party/` 外）采用 MIT 许可，见 [LICENSE](LICENSE)。

---

## 2. 第三方库 —— 各自原始许可

### stb_vorbis

- 文件：`Ransom_dev/src/third_party/stb_vorbis.c`
- 用途：解码 Ogg Vorbis 音频
- 作者：Sean Barrett 及贡献者
- 来源：https://github.com/nothings/stb
- 许可：**MIT / 公有领域双许可**（unlicense 或 MIT，二选一）

### minimp3

- 文件：`Ransom_dev/src/third_party/minimp3.h`、`minimp3_ex.h`
- 用途：解码 MP3 音频
- 作者：Lion (lieff) 及贡献者
- 来源：https://github.com/lieff/minimp3
- 许可：**CC0 1.0 通用（公有领域奉献）**

两个库都是**源码内联**（`audio_clip.cpp` 直接 `#include` 进来），不需要额外下载。

### Roboto Mono

- 文件：`assets/RobotoMono-VariableFont_wght.ttf`
- 用途：窗口标题栏字体（字体会在运行时从内嵌资源注册）
- 作者：Christian Robertson / Google
- 来源：https://fonts.google.com/specimen/Roboto+Mono
- 许可：**SIL Open Font License 1.1** —— 允许自由使用、修改、再分发

---

## 3. 游戏素材 —— ⚠️ 无授权，属于原作版权方

以下内容来自 Roblox 游戏 **《DOORS》**（作者 LSPLASH 等），版权归原作方所有。
**本项目是个人学习性质的同人作品，对这些素材没有任何授权。**

| 文件 | 内容 |
|---|---|
| `assets/image/A-90_IDLE.png`、`A-90_JUMPSCARE.png`、`A90Crucifixion.png`、`Accepta90.png`、`Blocka90.png` | A-90 的形象与停牌 |
| `assets/image/RansomPopup1..5.png` | 勒索弹窗素材 |
| `assets/image/Gold_icon.png`、`Gold_icon.webp` | 金币图标 |
| `assets/image/loading_1..10.png`、`loadingBG.png`、`payup_bg.png`、`Thankyou_sign.png` | 加载条与致谢画面 |
| `assets/audio/Ransom_full_theme.mp3`、`Ransom_encounter.wav`、`Ransom_start_*.ogg`、`ransom_success.ogg`、`Ransom_UI_-_Error_*.ogg`、`Ransomgold_increase_*.ogg`、`GEN_GLITCH_LOOP_*.ogg`、`Glitchyjustfaster.ogg`、`jumpscare2.mp3` | 主题曲与全部音效 |
| `assets/ransom.ico` | 应用图标（用 A-90 形象做的） |

### 如果你要公开托管这个仓库

建议二选一：

1. **只留代码**：把 `assets/image/` 和 `assets/audio/` 从仓库里删掉。
   程序仍然能编译、能跑，只是画不出脸、没有声音（`assets.cpp` 找不到素材时
   会退化成内置兜底图形，不会崩）。
2. **换自制素材**：自己做一套图和一个音效，文件名保持一样，`tools/gen_assets.ps1`
   会自动把新素材打包进去。

另外，脸部立绘其实是**程序化生成**的（`face.cpp`），不是直接贴原作图 ——
所以即使把 `assets/image/` 全删掉，A-90 的脸和停牌仍然画得出来。
真正离不开原作素材的是主题曲和那几个音效。
