// ============================================================================
//  recycle.h
//
//  回收站惩罚：没能在 90 秒内凑够赎金时，把「被锁定的图标」扔进回收站。
//
//  边界（这一条是硬规矩）：
//   * 只处理桌面（用户桌面 + 公共桌面）**顶层**的 .lnk / .url 快捷方式。
//     **绝不碰任何真实文件内容**——不读、不改、不删用户的数据文件。
//   * 排除本程序自己生成的金币 .lnk（目标是本程序、参数含 --pay/--token）。
//   * 用 SHFileOperation + FOF_ALLOWUNDO，是**真进回收站**，可手动还原。
//   * 移走前写一份还原清单（原路径），`--restore` 按清单把条目放回原位。
//   * 快捷方式都是可重建的（指向已安装的程序），丢了也能自己重建。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace recycle {

bool Start(HINSTANCE hInst);
void Stop();

// 会被「没收」的快捷方式有几个（桌面顶层 .lnk/.url，排除金币）。
int LockedCount();

// 把上述快捷方式全部移入回收站，并写还原清单。
// 返回实际移走个数；0 表示没有可移的或操作失败。
int SendToBin();

// 按清单从回收站还原。返回还原个数。
// 这个函数不依赖实体在运行，是 `--restore` 的实现。
int Restore();

// 清单里记了多少条。
int ManifestCount();

} // namespace recycle
