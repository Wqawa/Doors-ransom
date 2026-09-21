// ============================================================================
//  director.h
//
//  遭遇战调度器：把 DOORS 的 Ransom 机制串成一条时间线。
//
//  剧本（对照原作）：
//    脸在屏幕随机位置浮现  →  你必须停下不动  →  红色八角停牌闪现（判定时刻）
//      ├─ 没动  →  它退走
//      └─ 动了  →  满屏弹窗 + 桌面快捷方式被标记为已加密
//                 →  90 秒内点够 500 Gold 赎回
//                     ├─ 付清  →  致谢画面
//                     └─ 超时  →  jumpscare + 被锁定的快捷方式进回收站
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace director {

enum Phase {
    PHASE_IDLE = 0,      // 潜伏：什么都没发生
    PHASE_FACE,          // 头在屏幕任意位置浮现，此刻开始「不许动」
    PHASE_CENTER,        // 瞬移到屏幕正中央停留（抽动）
    PHASE_STOP,          // 停牌闪现、头隐藏 —— 判定时刻
    PHASE_ESCAPED,       // 没动：撤停牌，头再露几帧后消失
    PHASE_CAUGHT,        // 动了：张口脸抖动 + 尖叫，约 2 秒后进入勒索
    PHASE_PAID,          // 付清，致谢
    PHASE_PUNISH,        // 超时，jumpscare + 回收站
    PHASE_COUNT
};

bool Start(HINSTANCE hInst);
void Stop();

void JumpTo(Phase p);
Phase Current();
const wchar_t* PhaseName(Phase p);
Phase PhaseFromName(const wchar_t* name);

void SetAutoAdvance(bool on);
bool AutoAdvance();

DWORD PhaseElapsedMs();
DWORD TotalElapsedMs();

// ---- 赎金 ----
// 累加已收集的金币。累计到目标值会立即触发付款成功。
// 真金币会**先抵掉假金币顶上去的那部分赎金**（"会回落"），抵完的余量才进 g_gold。
void CreditGold(int amount);
int  Gold();
int  GoldGoal();
DWORD RansomRemainMs();     // 剩余毫秒；不在付款阶段返回 0

// ---- 假金币的两种惩罚（见 gold.h 的 fakeMask）----
// 前缀 "Gold" 被污染 -> 倒计时往前扣 ms。复用"玩家关子窗口"那套提前量，
//                     所以主题曲会同步跳，渐隐 / riser / 超时判定自动跟着走。
void PenalizeTime(DWORD ms);
// 面额数字被污染 -> 未付的赎金 += amount（把目标顶上去，之后会被真金币抵回来）。
void PenalizeGoal(int amount);

// 本轮遭遇是否触发过（抓没抓到）。
bool CaughtThisRound();

} // namespace director
