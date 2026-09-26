// The ride screen's one exit: a small "‹ BACK" pill in the top-LEFT corner, pure-C++ Slate.
// See specs/two-screen-navigation.md FR2.
//
// It replaced a UMG bar of five buttons (Restart, Replay, Choose board, Camera, Back) that testers
// never found and that competed with the wave. Everything that bar did between waves now lives on
// the hub; the ride keeps only the way out. Top-left because reading order runs left to right, so
// the way out sits where the eye starts - the score gave up that corner for it. Inside the top 18 %
// of the screen that the touch zones leave free, so a thumb reaching for the stick cannot land on
// it. No confirmation: a ride is cheap, and a modal is one more thing that pauses the world.
//
// Also the exit from a replay: the replay overlay is HitTestInvisible, so the two coexist.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;

namespace RideHud
{
	/** Add the pill to the world's game viewport. OnBack runs on the game thread when it is
	 *  tapped. No-op if already installed. */
	void Install(UWorld* World, TFunction<void()> OnBack);

	/** Remove it. Safe when not installed. */
	void Uninstall(UWorld* World);

	bool IsInstalled(UWorld* World);
}
