# 自测报告 —— 全部命令行参数 × 产物

- 生成时间 (UTC): `2026-10-14 23:02:11`
- 提交: `a11b00121e21d0e43c0490f0146efee05f7efc20`
- 工作流运行: #8 (id 38651121727)
- 用例总数: **62** ｜ PASS: **58** ｜ FAIL: **0** ｜ SKIP: **4**
- 音频设备: **无**（--audio-dump 期望退出码 1）
- 桌面截屏: **88** 帧（每个 interactive 用例连拍 4 帧：1200 / 2000 / 3000 / 5000 ms）
- 截图目录: `screenshots\<类别>\<架构>\<用例>\` —— 50 个用例目录 / 122 张 PNG / 27,551,097 B
- 总耗时: 190 秒

> 判定说明：[A] headless 类用例断言退出码等于期望值；
> [B] interactive 类用例期望"启动后一直跑不掉"，由脚本连拍数帧后强杀，
> 能撑过观察期即 PASS。``--pay`` 在无实体运行时故意返回 3，属正常。
> **最大变化帧** = 与首帧的 8x8 分块亮度签名平均差最大的那一帧（Δ）。
> Δ 接近 0 说明这一例的桌面从头到尾没变过 —— 要么该阶段本来就不画东西
> （PHASE_IDLE 会把 face/fx/overlay 全关掉），要么演出窗口错过了连拍时段。
> **SKIP** = 该用例依赖的环境在这台机器上不存在（这里是声卡），
> 程序按设计返回了正确的非零退出码，但功能本身没被真正跑到 —— 不计入 FAIL。

## x32  (29 PASS / 2 SKIP / 31 用例)

| 用例 | 类别 | 期望 | 实际 | 判定 | 耗时 | 产出文件 | 首帧亮度 | 最大变化帧 | 备注 |
|---|---|---|---|---|---|---|---|---|---|
| `face-dump` | headless | 0 | 0 | PASS | 4.6s | 4 (106,134 B) |  |  |  |
| `audio-dump` | headless | 1 (无声卡) | 1 | SKIP | 2.4s | 0 (2,843 B) |  |  |  |
| `theme-dump` | headless | 0 | 0 | PASS | 2.4s | 1 (7,940,954 B) |  |  |  |
| `ui-preview` | headless | 0 | 0 | PASS | 0.1s | 1 (26,392 B) |  |  |  |
| `ui-preview-grid` | headless | 0 | 0 | PASS | 0.1s | 1 (33,853 B) |  |  |  |
| `ui-preview-payup` | headless | 0 | 0 | PASS | 0s | 1 (39,823 B) |  |  |  |
| `setup-ui` | headless | 0 | 0 | PASS | 0.4s | 1 (29,603 B) |  |  |  |
| `setup-ui-grid` | headless | 0 | 0 | PASS | 0.1s | 1 (49,763 B) |  |  |  |
| `setup-ui-scroll` | headless | 0 | 0 | PASS | 0.2s | 1 (32,449 B) |  |  |  |
| `notice-ui` | headless | 0 | 0 | PASS | 0.1s | 1 (21,943 B) |  |  |  |
| `notice-ui-grid` | headless | 0 | 0 | PASS | 0s | 1 (34,519 B) |  |  |  |
| `fx-glow` | headless | 0 | 0 | PASS | 7.6s | 1 (35,209 B) |  |  |  |
| `fx-black` | headless | 0 | 0 | PASS | 5s | 1 (571,403 B) |  |  |  |
| `fx-stop` | headless | 0 | 0 | PASS | 4.9s | 1 (11,252 B) |  |  |  |
| `image-dir` | headless | 0 | 0 | PASS | 0s | 1 (26,400 B) |  |  |  |
| `audio-dir` | headless | 1 (无声卡) | 1 | SKIP | 2.3s | 0 (2,826 B) |  |  |  |
| `tolerance` | headless | 0 | 0 | PASS | 0s | 1 (26,395 B) |  |  |  |
| `clean-gold` | headless | 0 | 0 | PASS | 0s | 0 (1,737 B) |  |  | 没有产出任何文件 |
| `restore` | headless | 0 | 0 | PASS | 0.1s | 0 (1,744 B) |  |  | 没有产出任何文件 |
| `pay-no-entity` | headless | 3 | 3 | PASS | 0s | 0 (0 B) |  |  | 没有产出任何文件 |
| `face-demo-idle` | interactive | timeout | still-running | PASS | 6.8s | 9 (1,066,032 B) | 71.53 | 1200ms Δ0 | 按预期仍在运行，已强杀 |
| `face-demo-stop` | interactive | timeout | still-running | PASS | 6.6s | 9 (1,976,125 B) | 72.43 | 5000ms Δ3.4 | 按预期仍在运行，已强杀 |
| `face-demo-attack` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,923,724 B) | 62.61 | 3000ms Δ12.7 | 按预期仍在运行，已强杀 运行中点金币 -> 退出码 0 |
| `face-demo-loading` | interactive | timeout | still-running | PASS | 6.5s | 9 (315,622 B) | 28.55 | 5000ms Δ52.4 | 按预期仍在运行，已强杀 |
| `phase-0-idle` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,022,860 B) | 71.75 | 1200ms Δ0 | 按预期仍在运行，已强杀 |
| `phase-1-face` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,033,616 B) | 71.53 | 2000ms Δ0.3 | 按预期仍在运行，已强杀 |
| `phase-2-center` | interactive | timeout | still-running | PASS | 6.6s | 9 (2,259,300 B) | 38.8 | 3000ms Δ21.5 | 按预期仍在运行，已强杀 |
| `phase-3-stop` | interactive | timeout | still-running | PASS | 6.5s | 9 (20,652 B) | 56.67 | 1200ms Δ0 | 按预期仍在运行，已强杀 |
| `phase-5-caught` | interactive | timeout | still-running | PASS | 6.6s | 9 (1,036,503 B) | 27.64 | 5000ms Δ57.6 | 按预期仍在运行，已强杀 |
| `phase-6-paid` | interactive | timeout | still-running | PASS | 6.6s | 9 (1,034,569 B) | 71.79 | 2000ms Δ0.1 | 按预期仍在运行，已强杀 |
| `phase-7-punish` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,061,901 B) | 28.9 | 2000ms Δ54.5 | 按预期仍在运行，已强杀 |

<details><summary>完整参数</summary>

```
face-dump: --face-dump D:\a\Doors-ransom\Doors-ransom\selftest\x32\face-dump\faces --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\face-dump\diag.log
audio-dump: --audio-dump D:\a\Doors-ransom\Doors-ransom\selftest\x32\audio-dump\audio-all.wav --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\audio-dump\diag.log
theme-dump: --theme-dump D:\a\Doors-ransom\Doors-ransom\selftest\x32\theme-dump\theme.wav --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\theme-dump\diag.log
ui-preview: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x32\ui-preview\ui.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\ui-preview\diag.log
ui-preview-grid: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x32\ui-preview-grid\ui-grid.png --ui-grid --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\ui-preview-grid\diag.log
ui-preview-payup: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x32\ui-preview-payup\ui-payup.png --payup --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\ui-preview-payup\diag.log
setup-ui: --setup-ui D:\a\Doors-ransom\Doors-ransom\selftest\x32\setup-ui\setup.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\setup-ui\diag.log
setup-ui-grid: --setup-ui D:\a\Doors-ransom\Doors-ransom\selftest\x32\setup-ui-grid\setup-grid.png --setup-grid --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\setup-ui-grid\diag.log
setup-ui-scroll: --setup-ui D:\a\Doors-ransom\Doors-ransom\selftest\x32\setup-ui-scroll\setup-scroll.png --setup-scroll 240 --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\setup-ui-scroll\diag.log
notice-ui: --notice-ui D:\a\Doors-ransom\Doors-ransom\selftest\x32\notice-ui\notice.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\notice-ui\diag.log
notice-ui-grid: --notice-ui D:\a\Doors-ransom\Doors-ransom\selftest\x32\notice-ui-grid\notice-grid.png --setup-grid --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\notice-ui-grid\diag.log
fx-glow: --fx-demo glow --fx-dump D:\a\Doors-ransom\Doors-ransom\selftest\x32\fx-glow\fx-glow.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\fx-glow\diag.log
fx-black: --fx-demo black --fx-dump D:\a\Doors-ransom\Doors-ransom\selftest\x32\fx-black\fx-black.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\fx-black\diag.log
fx-stop: --fx-demo stop --fx-dump D:\a\Doors-ransom\Doors-ransom\selftest\x32\fx-stop\fx-stop.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\fx-stop\diag.log
image-dir: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x32\image-dir\ui-imagedir.png --image-dir assets\image --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\image-dir\diag.log
audio-dir: --audio-dump D:\a\Doors-ransom\Doors-ransom\selftest\x32\audio-dir\audio-dir.wav --audio-dir assets\audio --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\audio-dir\diag.log
tolerance: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x32\tolerance\ui-tol.png --tolerance 42 --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\tolerance\diag.log
clean-gold: --clean-gold --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\clean-gold\diag.log
restore: --restore --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\restore\diag.log
pay-no-entity: --pay 100 --token 1 --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\pay-no-entity\diag.log
face-demo-idle: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo idle --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-face-demo-idle\diag.log
face-demo-stop: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo stop --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-face-demo-stop\diag.log
face-demo-attack: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo attack --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-face-demo-attack\diag.log
face-demo-loading: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo loading --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-face-demo-loading\diag.log
phase-0-idle: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 0 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-phase-0-idle\diag.log
phase-1-face: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 1 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-phase-1-face\diag.log
phase-2-center: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 2 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-phase-2-center\diag.log
phase-3-stop: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 3 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-phase-3-stop\diag.log
phase-5-caught: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 5 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-phase-5-caught\diag.log
phase-6-paid: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 6 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-phase-6-paid\diag.log
phase-7-punish: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 7 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x32\screen-phase-7-punish\diag.log
```

</details>

## x64  (29 PASS / 2 SKIP / 31 用例)

| 用例 | 类别 | 期望 | 实际 | 判定 | 耗时 | 产出文件 | 首帧亮度 | 最大变化帧 | 备注 |
|---|---|---|---|---|---|---|---|---|---|
| `face-dump` | headless | 0 | 0 | PASS | 0.6s | 4 (106,132 B) |  |  |  |
| `audio-dump` | headless | 1 (无声卡) | 1 | SKIP | 0.5s | 0 (2,841 B) |  |  |  |
| `theme-dump` | headless | 0 | 0 | PASS | 0.5s | 1 (7,940,952 B) |  |  |  |
| `ui-preview` | headless | 0 | 0 | PASS | 0s | 1 (26,330 B) |  |  |  |
| `ui-preview-grid` | headless | 0 | 0 | PASS | 0s | 1 (33,784 B) |  |  |  |
| `ui-preview-payup` | headless | 0 | 0 | PASS | 0s | 1 (39,819 B) |  |  |  |
| `setup-ui` | headless | 0 | 0 | PASS | 0s | 1 (29,603 B) |  |  |  |
| `setup-ui-grid` | headless | 0 | 0 | PASS | 0s | 1 (49,763 B) |  |  |  |
| `setup-ui-scroll` | headless | 0 | 0 | PASS | 0s | 1 (32,449 B) |  |  |  |
| `notice-ui` | headless | 0 | 0 | PASS | 0s | 1 (21,943 B) |  |  |  |
| `notice-ui-grid` | headless | 0 | 0 | PASS | 0s | 1 (34,519 B) |  |  |  |
| `fx-glow` | headless | 0 | 0 | PASS | 1.2s | 1 (34,537 B) |  |  |  |
| `fx-black` | headless | 0 | 0 | PASS | 1.2s | 1 (571,048 B) |  |  |  |
| `fx-stop` | headless | 0 | 0 | PASS | 1.2s | 1 (10,897 B) |  |  |  |
| `image-dir` | headless | 0 | 0 | PASS | 0s | 1 (26,338 B) |  |  |  |
| `audio-dir` | headless | 1 (无声卡) | 1 | SKIP | 0.5s | 0 (2,824 B) |  |  |  |
| `tolerance` | headless | 0 | 0 | PASS | 0s | 1 (26,333 B) |  |  |  |
| `clean-gold` | headless | 0 | 0 | PASS | 0s | 0 (1,735 B) |  |  | 没有产出任何文件 |
| `restore` | headless | 0 | 0 | PASS | 0.5s | 0 (2,431 B) |  |  | 没有产出任何文件 |
| `pay-no-entity` | headless | 3 | 3 | PASS | 0s | 0 (0 B) |  |  | 没有产出任何文件 |
| `face-demo-idle` | interactive | timeout | still-running | PASS | 6.7s | 9 (1,065,933 B) | 71.62 | 1200ms Δ0 | 按预期仍在运行，已强杀 |
| `face-demo-stop` | interactive | timeout | still-running | PASS | 6.6s | 9 (1,974,628 B) | 72.52 | 5000ms Δ3.9 | 按预期仍在运行，已强杀 |
| `face-demo-attack` | interactive | timeout | still-running | PASS | 6.7s | 9 (1,926,500 B) | 62.32 | 3000ms Δ12 | 按预期仍在运行，已强杀 运行中点金币 -> 退出码 0 |
| `face-demo-loading` | interactive | timeout | still-running | PASS | 6.7s | 9 (314,314 B) | 28.11 | 5000ms Δ53 | 按预期仍在运行，已强杀 |
| `phase-0-idle` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,022,641 B) | 71.84 | 1200ms Δ0 | 按预期仍在运行，已强杀 |
| `phase-1-face` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,033,449 B) | 71.62 | 2000ms Δ0.3 | 按预期仍在运行，已强杀 |
| `phase-2-center` | interactive | timeout | still-running | PASS | 6.8s | 9 (2,253,598 B) | 38.8 | 5000ms Δ20.3 | 按预期仍在运行，已强杀 |
| `phase-3-stop` | interactive | timeout | still-running | PASS | 6.6s | 9 (20,652 B) | 56.67 | 1200ms Δ0 | 按预期仍在运行，已强杀 |
| `phase-5-caught` | interactive | timeout | still-running | PASS | 6.7s | 9 (1,136,726 B) | 28.68 | 3000ms Δ54.1 | 按预期仍在运行，已强杀 |
| `phase-6-paid` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,028,345 B) | 71.84 | 1200ms Δ0 | 按预期仍在运行，已强杀 |
| `phase-7-punish` | interactive | timeout | still-running | PASS | 6.5s | 9 (1,065,296 B) | 34.29 | 2000ms Δ54.2 | 按预期仍在运行，已强杀 |

<details><summary>完整参数</summary>

```
face-dump: --face-dump D:\a\Doors-ransom\Doors-ransom\selftest\x64\face-dump\faces --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\face-dump\diag.log
audio-dump: --audio-dump D:\a\Doors-ransom\Doors-ransom\selftest\x64\audio-dump\audio-all.wav --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\audio-dump\diag.log
theme-dump: --theme-dump D:\a\Doors-ransom\Doors-ransom\selftest\x64\theme-dump\theme.wav --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\theme-dump\diag.log
ui-preview: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x64\ui-preview\ui.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\ui-preview\diag.log
ui-preview-grid: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x64\ui-preview-grid\ui-grid.png --ui-grid --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\ui-preview-grid\diag.log
ui-preview-payup: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x64\ui-preview-payup\ui-payup.png --payup --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\ui-preview-payup\diag.log
setup-ui: --setup-ui D:\a\Doors-ransom\Doors-ransom\selftest\x64\setup-ui\setup.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\setup-ui\diag.log
setup-ui-grid: --setup-ui D:\a\Doors-ransom\Doors-ransom\selftest\x64\setup-ui-grid\setup-grid.png --setup-grid --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\setup-ui-grid\diag.log
setup-ui-scroll: --setup-ui D:\a\Doors-ransom\Doors-ransom\selftest\x64\setup-ui-scroll\setup-scroll.png --setup-scroll 240 --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\setup-ui-scroll\diag.log
notice-ui: --notice-ui D:\a\Doors-ransom\Doors-ransom\selftest\x64\notice-ui\notice.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\notice-ui\diag.log
notice-ui-grid: --notice-ui D:\a\Doors-ransom\Doors-ransom\selftest\x64\notice-ui-grid\notice-grid.png --setup-grid --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\notice-ui-grid\diag.log
fx-glow: --fx-demo glow --fx-dump D:\a\Doors-ransom\Doors-ransom\selftest\x64\fx-glow\fx-glow.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\fx-glow\diag.log
fx-black: --fx-demo black --fx-dump D:\a\Doors-ransom\Doors-ransom\selftest\x64\fx-black\fx-black.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\fx-black\diag.log
fx-stop: --fx-demo stop --fx-dump D:\a\Doors-ransom\Doors-ransom\selftest\x64\fx-stop\fx-stop.png --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\fx-stop\diag.log
image-dir: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x64\image-dir\ui-imagedir.png --image-dir assets\image --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\image-dir\diag.log
audio-dir: --audio-dump D:\a\Doors-ransom\Doors-ransom\selftest\x64\audio-dir\audio-dir.wav --audio-dir assets\audio --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\audio-dir\diag.log
tolerance: --ui-preview D:\a\Doors-ransom\Doors-ransom\selftest\x64\tolerance\ui-tol.png --tolerance 42 --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\tolerance\diag.log
clean-gold: --clean-gold --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\clean-gold\diag.log
restore: --restore --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\restore\diag.log
pay-no-entity: --pay 100 --token 1 --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\pay-no-entity\diag.log
face-demo-idle: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo idle --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-face-demo-idle\diag.log
face-demo-stop: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo stop --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-face-demo-stop\diag.log
face-demo-attack: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo attack --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-face-demo-attack\diag.log
face-demo-loading: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --face-demo loading --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-face-demo-loading\diag.log
phase-0-idle: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 0 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-phase-0-idle\diag.log
phase-1-face: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 1 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-phase-1-face\diag.log
phase-2-center: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 2 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-phase-2-center\diag.log
phase-3-stop: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 3 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-phase-3-stop\diag.log
phase-5-caught: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 5 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-phase-5-caught\diag.log
phase-6-paid: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 6 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-phase-6-paid\diag.log
phase-7-punish: --no-guardian --no-lockdown --no-block-menu --no-setup --no-audio --phase 7 --no-auto --diag D:\a\Doors-ransom\Doors-ransom\selftest\x64\screen-phase-7-punish\diag.log
```

</details>

