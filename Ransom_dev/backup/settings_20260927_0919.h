// ============================================================================
//  settings.h
//
//  启动设置。**一个纯数据模块**：不建窗口、不碰设备，只负责
//
//    * 定义「一次运行的全部可调项」（结构体 Set）
//    * 从 `%LOCALAPPDATA%\Ransom_dev\settings.ini` 读回来 / 写回去
//    * 把 Set 里的值推给各个子系统（audio / fx）
//
//  界面部分在 setup_ui.cpp —— 那边只管画和收输入，改的是同一个 Set。
//  这么切是为了让「谁都要读的参数」有唯一来源：director 要读随机间隔、
//  audio 要读音量、fx 要读光敏安全，全都只认这里的一份。
//
//  ---- 关于音量三条线 ----
//
//  信号链是  sample × 通道增益 × 主增益，最后统一软削波：
//
//    * 主题曲 / 故障底噪 -> 通道 = 背景音乐（0-200%）
//    * 跳杀 / 抓人 / 金币 / 报错 -> 通道 = 音效（0-200%）
//    * 主增益是**上限**（0-100%），默认 100%。它没有对应的滑条，
//      只是给「整机太吵」留的一道闸，手改 ini 的 master= 就能压。
//
//  之所以让主增益只当上限、默认 100：否则用户把背景音乐拉到 200% 时，
//  还要再乘一次主增益，实际听感和滑条读数对不上。
//
//  历史坑：director 以前在 PHASE_IDLE 里硬写 `audio::SetMaster(60)`，
//  每一轮遭遇战都把音量压回 60。现在那条改成了 `settings::ApplyAudio()`。
// ============================================================================
#pragma once

// std::wstring 是 Set::coinDrives 的元素类型（盘符清单）。
// 这个头文件以前只用 std::vector<int>，所以一直没引 <string> —— 现在必须引，
// 否则在只包含 settings.h 的翻译单元里 std::wstring 是不认识的类型。
#include <string>
#include <vector>

namespace settings {

	// ---- 取值范围（界面滑条和 ini 校验共用同一套）----
	const int kVolMin = 0;
	const int kVolMax = 200;      // 背景音乐 / 音效的最大值，按需求定在 200%

	const int kIntervalMinMs = 20;      // 两次遭遇战之间最短 20ms（几乎立刻）
	const int kIntervalMaxMs = 90000;   // 最长 90s

	// ---- 游戏模式 ----
	//
	// 原来是「硬核模式」一个布尔开关，现在升级成三态下拉框（界面在 setup_ui）。
	//   普通 kModeNormal   —— 原版演出
	//   硬核 kModeHardcore —— 困难版（3 分钟 / 假币 / 锁桌面 / 撒磁盘……）
	//   挂机 kModeIdle     —— **还没实装**，先占位：能选、能存，
	//                         真跑起来时按普通模式走，并在日志里记一行。
	//
	// 下游（popup / director / gold / overlay）读的仍然是 settings::Hardcore()，
	// 它现在等价于 mode == kModeHardcore —— 语义没变，那些调用一行都不用改。
	const int kModeNormal = 0;
	const int kModeHardcore = 1;
	const int kModeIdle = 2;
	const int kModeCount = 3;

	// ---- 赎金目标金币数 ----
	// 这个值同时决定两件事：
	//   * 付清赎金的判定阈值（director）
	//   * 桌面散布金币的总额与面额分布（gold）
	//
	// **普通和硬核各自独立存一份**（界面上也拆成了两条独立的滑条，
	// 各自绑自己的取值函数，互不影响）：
	//   普通：10 ~ 1000（原作 500）
	//   硬核：1000 ~ 9999（硬核要付得多）
	// 超过各自上限的部分一律砍掉 —— 夹在 GoldGoal() 里（唯一出口）。
	const int kGoldMin = 10;
	const int kGoldMax = 1000;
	const int kGoldHardMin = 1000;
	const int kGoldHardMax = 9999;

	// ---- 关掉一个勒索子窗口的惩罚时长（毫秒）----
	// 也是共用一条滑条、范围按模式分：
	//   普通：0 ~ 18000（18 秒）
	//   硬核：0 ~ 30000（30 秒）
	// 0 = 关窗口完全不扣时间。夹在 ChildCloseMs() 里。
	const int kCloseMin = 0;
	const int kCloseNormalMax = 18000;
	const int kCloseHardMax = 30000;
	// 两种模式各自的默认值：切换开关时如果这一项还是"另一套的默认"，
	// 就跟着换成这一套的（用户手调过的不动，见 setup_ui 的 ApplyModeDefaults）。
	const int kCloseNormalDefault = 10000;
	const int kCloseHardDefault = 30000;

	// ---- 假金币 ----
	// fakePercent：每个金币生成时"是假币"的概率，0-100（%）。
	// 后面三个是**假币内部**的形态权重（前缀 / 后缀 / 两个都改），各自 0-100，
	// 按权重比例分配 —— **不要求加起来等于 100**，三个都是 0 就干脆不出假币。
	// 这四项都只在硬核模式下生效（界面上的滑条也只在硬核下能拖）。
	const int kFakePctMin = 0;
	const int kFakePctMax = 100;
	const int kDefaultFakePercent = 35;
	const int kDefaultFakePrefixPct = 40;
	const int kDefaultFakeSuffixPct = 40;
	const int kDefaultFakeBothPct = 20;

	// ---- 金币面额池 ----
	// 桌面散布的每一个金币，面额都从这个池里随机挑一个。
	// 想改默认值就改下面这个数组（顺序无所谓，读取时会排序去重）；
	// 想临时改一次运行的面额，去 settings.ini 里改 [game] coin_amounts=。
	const int kDefaultCoinAmounts[] = { 10, 50, 75, 100, 125, 150, 325, 500 };
	const int kDefaultCoinAmountCount = 8;

	// 面额池的项数上限。超过就截断 —— 防止 ini 里塞进来几百个值。
	const int kCoinAmountMax = 16;

	// ---- 硬核模式 ----
	//
	// 硬核下赎金滑条的默认值（不是"强制值"）。
	// 打开硬核开关时，如果用户原来的赎金还在普通区间里（<=1000），
	// 就把它顶到这个数；之后用户想拖到 1000~9999 之间的任何值都行。
	const int kHardcoreGoldGoal = 5000;

	// 硬核的面额池：全部 <= 100（"金币面额减少"）。
	// 池子收窄到 50/75/100，是为了让凑 5000 大约需要 74 枚金币
	// （面额再小就得往桌面上撒一百多枚 .lnk，翻找和清理都受不了）。
	const int kHardcoreCoinAmounts[] = { 50, 75, 100 };
	const int kHardcoreCoinAmountCount = 3;

	// 硬核的勒索倒计时（3 分钟），单位毫秒。
	// 必须和硬核主题曲处理后的长度严格一致，见 audio.cpp 的 EditTheme。
	const int kHardcoreRansomMs = 180000;

	// ---- 硬核：同时最多几个勒索子窗口 ----
	// 普通模式固定 14（popup.cpp 的 kMaxChildren，不走设置），只有硬核这一项
	// 交给用户调。上限 30 是"再加下去窗口动画会被消息循环饿死"的界限：
	// 每个子窗口都是一个独立的分层窗口 + 自己的定时器。
	const int kHardPopupMin = 10;
	const int kHardPopupMax = 30;
	const int kDefaultHardPopupMax = 22;        // = 原来的 kHardMaxChildren

	// ---- 硬核：贴着鼠标生成的"阻挡弹窗"的间隔（随机区间）----
	// 只有硬核有这一路弹窗，所以这两个值也只在硬核下用。
	// 界面上是一条两珠滑条：两珠落在同一个值上 = 固定间隔。
	const int kCursorGapFloorMs = 900;          // 滑条左端 0.9 秒
	const int kCursorGapCeilMs  = 18000;        // 滑条右端 18 秒
	const int kDefaultCursorGapMinMs = 900;
	const int kDefaultCursorGapMaxMs = 9000;

	// ---- 硬核：随机锁定桌面上的**非快捷方式**项 ----
	// 数量上限的硬顶是 90；但滑条实际能拖到的上界 = min(90, 桌面上真实存在
	// 的非快捷方式条目数) —— 桌面上一共没那么多东西时，拖到 90 毫无意义。
	// 见 ExtraLockCapacity()。
	//
	// 时长同样是"随机区间"，滑条两端 0.9~18 秒，两珠重合 = 固定时长。
	const int kExtraLockCountCeil = 90;
	const int kDefaultExtraLockCount = 9;       // = 原来的 kExtraLockMax
	const int kExtraLockFloorMs = 900;          // 滑条左端 0.9 秒
	const int kExtraLockCeilMs  = 18000;        // 滑条右端 18 秒
	const int kDefaultExtraLockMinMs = 900;
	const int kDefaultExtraLockMaxMs = 9000;
	const int kExtraLockCooldownMs = 9000;

	// 注：曾经打算让硬核下的安全阀"连按两次才退出"（T13），
	// 后来放弃了这个设计 —— 安全阀在两种模式下都是**按一次就停**。
	// 相关的常量和提示文案都已经删掉了，别再按那条路改。

	// ---- 硬核：金币撒哪些盘（设置界面那个 2D 盘符页勾的）----
	//
	// 硬核会把金币撒到各**固定盘**的顶层目录里。「撒哪几个盘」由用户在
	// 设置界面的盘符页上勾（见 setup_ui 的 drive 页）。
	//
	// 三态语义（缺省 = 老行为，一行都不改）：
	//   ini 没这个键 / 空串  -> 全部固定盘（老行为）
	//   "C,D"                -> 只撒 C 和 D 这两个固定盘
	//   "-"                  -> 一个盘都不撒（**显式的空集**）
	//
	// 为什么空串不能兼职表示"空集"：空串的含义是"没配过"，必须和"我就是要
	// 一个都不撒"分得开 —— 否则用户把勾全去掉、重启后又变回全部盘，等于骗人。
	const wchar_t* const kCoinDrivesNoneToken = L"-";

	// ---- 默认值 ----
	const int kDefaultBgmVol = 100;
	const int kDefaultSfxVol = 100;
	const int kDefaultMasterVol = 100;
	// 4000ms = 原来的固定 kIdleMs（见 director.cpp），默认行为保持不变。
	// 注意别写成 400：那会让默认的伺候时间缩短到原来的十分之一。
	const int kDefaultMinMs = 4000;
	const int kDefaultMaxMs = 4000;
	// 原作就是 500 Gold。想改默认值改这里，ini 里没写 gold_goal= 时会用它。
	const int kDefaultGoldGoal = 500;

	struct Set {
		// 音量（%）。0-200，其中背景音乐 / 音效最大到 200。
		int  bgmVol = kDefaultBgmVol;
		int  sfxVol = kDefaultSfxVol;
		int  masterVol = kDefaultMasterVol;   // 上限，不出现在界面滑条上

		// 光敏安全模式（「癫痫模式」）：
		//   false（默认）= 原版演出，全强度噪点 / 亮红幕 / 四角红光
		//   true         = 压低整屏亮度跳变与高频噪点，对光敏人群友好
		bool photosensitiveSafe = false;

		// 两次遭遇战之间的**潜伏**时长，在 [minMs, maxMs] 之间随机。
		// 两者相等 = 固定时长（默认 400ms 就是原来的行为）。
		int  minMs = kDefaultMinMs;
		int  maxMs = kDefaultMaxMs;

		// 赎金目标金币数：**普通 / 硬核各存一份**（滑条也是拆开的两条，
		// 各自绑自己的取值函数）。生效值由 GoldGoal() 按当前模式选。
		int  goldGoalNormal = kDefaultGoldGoal;
		int  goldGoalHardcore = kHardcoreGoldGoal;

		// 关掉一个勒索子窗口时倒计时往前扣多少毫秒（0 = 不扣）。
		// 同样按模式夹上限：普通 <=18000，硬核 <=30000，见 ChildCloseMs()。
		int  childCloseMs = kCloseNormalDefault;

		// 假金币比例（0-100，%），以及假币内部三种形态的权重。
		// **只有硬核模式会用到**；普通模式下这些值原样存着、不生效。
		int  fakePercent = kDefaultFakePercent;
		int  fakePrefixPct = kDefaultFakePrefixPct;
		int  fakeSuffixPct = kDefaultFakeSuffixPct;
		int  fakeBothPct = kDefaultFakeBothPct;

		// ---- 硬核专属的四项（普通模式下原样存着、不生效）----
		//
		// 同时最多几个勒索子窗口（普通模式固定 14，这一项只在硬核下被读）。
		int  hardPopupMax = kDefaultHardPopupMax;

		// 贴鼠标那路"阻挡弹窗"的间隔随机区间（毫秒）。
		int  cursorGapMinMs = kDefaultCursorGapMinMs;
		int  cursorGapMaxMs = kDefaultCursorGapMaxMs;

		// 同时最多锁几个桌面上的非快捷方式项（0 = 一个都不锁），
		// 以及每项锁多久的随机区间（毫秒）。
		int  extraLockCount = kDefaultExtraLockCount;
		int  extraLockMinMs = kDefaultExtraLockMinMs;
		int  extraLockMaxMs = kDefaultExtraLockMaxMs;

		// 金币撒哪些固定盘（在设置界面的盘符页上勾）。
		// **空 = 全部固定盘**（老行为）；里面只放盘符字母，如 "C"、"D"；
		// 只放一个 kCoinDrivesNoneToken 表示"一个盘都不撒"。
		// 从 ini 的 [hardcore] coin_drives= 读（逗号分隔），
		// 读取时会去重、转大写、排序。
		std::vector<std::wstring> coinDrives;

		// 游戏模式：kModeNormal / kModeHardcore / kModeIdle。
		// 开着硬核时下面这些全都换一套：
		//   3 分钟倒计时 / 赎金 1000-9999 / 面额 <=100 / 假金币 /
		//   弹窗更多更黏人 / 往磁盘根目录撒金币 / 随机锁非快捷方式。
		// 所有子系统都只读 settings::Hardcore() 这一个来源（见 settings.cpp）；
		// 「挂机」还没实装，选中后按普通模式跑。
		int  mode = kModeNormal;

		// 当前是不是硬核（= mode == kModeHardcore）。
		// 界面和 settings.cpp 内部都读这个，避免到处写 mode 比较。
		bool IsHardcore() const { return mode == kModeHardcore; }

		// 金币面额池。**空 = 用 kDefaultCoinAmounts 里的默认**。
		// 从 ini 的 [game] coin_amounts= 读（逗号分隔，如 "10,50,325,500"）。
		// 读取时会自动排序去重、夹到合法范围。
		std::vector<int> coinAmounts;

		// 是否让界面在关闭前把值写回 ini。
		// 「开始」= true；「恢复默认」只改内存不落盘；关窗口中止 = 不落盘。
		bool save = false;
	};

	// ---- 生命周期 ----

	// 读回设置。文件不存在 / 读不动就用默认值（不报错，日志里写一句）。
	// 必须在 elog::Open 之后调。
	void Load();

	// 当前设置（只读）。
	const Set& Current();

	// 直接改当前设置。界面走这条；会自动夹到合法范围。
	void SetCurrent(const Set& s);

	// 恢复默认值（不动文件，不推给子系统——由调用方决定要不要 Apply）。
	void ResetToDefault();

	// 写回 `%LOCALAPPDATA%\Ransom_dev\settings.ini`。
	// 目录不存在会自动建。失败只写日志，不弹窗。
	bool Save();

	// 把当前设置推给各个子系统（音量 + 光敏安全）。
	// 在「窗口建好之后、演出开始之前」调一次；
	// director 每轮回到 PHASE_IDLE 时也会再调一次（见 director.cpp）。
	void Apply();

	// 只推音量那部分。给 director 的 PHASE_IDLE 用——
	// 那里原来硬写 SetMaster(60)，现在改成读设置。
	void ApplyAudio();

	// ---- 查询 ----

	int  BgmVol();
	int  SfxVol();
	int  MasterVol();
	bool PhotosensitiveSafe();
	int  MinMs();
	int  MaxMs();

	// 当前游戏模式（kModeNormal / kModeHardcore / kModeIdle），已夹到合法范围。
	int  Mode();

	// 赎金目标。**按当前模式选那一条、并夹过的最终值，也是唯一出口**：
	//   普通 -> goldGoalNormal 夹到 [10, 1000]
	//   硬核 -> goldGoalHardcore 夹到 [1000, 9999]
	// 所有判定和显示都必须读这个，别直接读 Set 里那两个字段。
	int  GoldGoal();

	// 两条滑条各自的原始值（已夹过），给设置界面的两条独立滑条用。
	// 注意它们是**互不影响**的两份数据：普通调 500、硬核调 5000，各存各的。
	int  GoldGoalNormal();      // [kGoldMin, kGoldMax]
	int  GoldGoalHardcore();    // [kGoldHardMin, kGoldHardMax]

	// 关掉一个勒索子窗口的惩罚时长（毫秒），按模式夹过：
	// 普通 -> [0, 18000]，硬核 -> [0, 30000]。
	int  ChildCloseMs();

	// 假金币比例与三种形态权重（0-100）。只在硬核下有意义。
	int  FakePercent();
	int  FakePrefixPct();
	int  FakeSuffixPct();
	int  FakeBothPct();

	// ---- 硬核专属四项（夹过之后的值，下游只读这几个）----

	// 同时最多几个勒索子窗口，夹到 [kHardPopupMin, kHardPopupMax]。
	int  HardPopupMax();

	// 贴鼠标的阻挡弹窗的间隔区间（毫秒），夹到
	// [kCursorGapFloorMs, kCursorGapCeilMs]，并保证 min <= max。
	int  CursorGapMinMs();
	int  CursorGapMaxMs();

	// 同时最多锁几个桌面非快捷方式项。夹到 [0, ExtraLockCapacity()]。
	int  ExtraLockCount();

	// 桌面锁定时长区间（毫秒），夹到 [kExtraLockFloorMs, kExtraLockCeilMs]，
	// 并保证 min <= max。
	int  ExtraLockMinMs();
	int  ExtraLockMaxMs();

	// 数量上限的**动态上界** = min(90, 桌面上真实存在的非快捷方式条目数)。
	// 界面滑条拿它当右端；桌面上一个能锁的东西都没有时返回 0
	//（此时 ExtraLockCount() 也是 0，等于这个功能不生效）。
	int  ExtraLockCapacity();

	// 桌面上真实存在的非快捷方式条目数（用户桌面 + 公共桌面，去重，
	// 跳过 .lnk/.url 和隐藏项）。诊断和界面提示文案用。
	//
	// 注意：**只在第一次调用时扫一遍盘**，之后返回缓存值 ——
	// 它被绘制循环读到，不能每次重绘都去翻目录。
	int  DesktopItemCount();

	// 金币撒哪些固定盘。**大写字母、去重、已排序**；空 = 全部固定盘。
	// 含 kCoinDrivesNoneToken（"-"）时返回空（那种情况看 CoinDrivesNone()）。
	const std::vector<std::wstring>& CoinDrives();

	// 用户是不是**显式**勾了"一个盘都不撒"（ini 里存的是 "-"）。
	// 为真时金币只落在桌面上，一个固定盘都不碰。
	bool CoinDrivesNone();

	// 硬核模式开关。
	bool Hardcore();
	// 当前生效的面额池。**永远非空**：ini 没配、或配了空串时，
	// 里面就是 kDefaultCoinAmounts 那一份。已排序去重。
	const std::vector<int>& CoinAmounts();

	// 本次运行的起始阶段时长（两次遭遇战之间的随机间隔）。
	// 抽一次就固定下来，整个进程只用这一个值——同一个随机数被
	// director 和日志读到时不会各抽一次得到两个答案。
	int  StartupIdleMs();

	// 重新抽一次（每轮遭遇战结束回到 IDLE 时调）。
	int  PickIdleMs();

	// 设置文件路径（日志和「打开所在目录」用）。
	const wchar_t* FilePath();

} // namespace settings
