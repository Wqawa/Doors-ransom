



























#pragma once

#include <vector>

namespace settings {


	const int kVolMin = 0;
	const int kVolMax = 200;

	const int kIntervalMinMs = 20;
	const int kIntervalMaxMs = 90000;





	const int kGoldMin = 10;
	const int kGoldMax = 1000;





	const int kDefaultCoinAmounts[] = { 10, 50, 75, 100, 125, 150, 325, 500 };
	const int kDefaultCoinAmountCount = 8;


	const int kCoinAmountMax = 16;


	const int kDefaultBgmVol = 100;
	const int kDefaultSfxVol = 100;
	const int kDefaultMasterVol = 100;


	const int kDefaultMinMs = 4000;
	const int kDefaultMaxMs = 4000;

	const int kDefaultGoldGoal = 500;

	struct Set {

		int  bgmVol = kDefaultBgmVol;
		int  sfxVol = kDefaultSfxVol;
		int  masterVol = kDefaultMasterVol;




		bool photosensitiveSafe = false;



		int  minMs = kDefaultMinMs;
		int  maxMs = kDefaultMaxMs;


		int  goldGoal = kDefaultGoldGoal;




		std::vector<int> coinAmounts;



		bool save = false;
	};





	void Load();


	const Set& Current();


	void SetCurrent(const Set& s);


	void ResetToDefault();



	bool Save();




	void Apply();



	void ApplyAudio();



	int  BgmVol();
	int  SfxVol();
	int  MasterVol();
	bool PhotosensitiveSafe();
	int  MinMs();
	int  MaxMs();
	int  GoldGoal();


	const std::vector<int>& CoinAmounts();




	int  StartupIdleMs();


	int  PickIdleMs();


	const wchar_t* FilePath();

}
