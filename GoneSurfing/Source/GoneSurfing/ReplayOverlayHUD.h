// Pure-C++ Slate "replay overlay": a full-screen cinematic HUD shown while a kinematic ride
// replay is playing. Letterbox bars + a "REPLAY" badge + a playback progress/timer bar make it
// unmistakable that the game is in REPLAY (not LIVE) mode; an end-card on the final-frame hold
// makes the stop read as intentional rather than a bug. No UMG asset, no scene capture — it just
// paints 2D shapes/text from a snapshot the pawn pushes each tick. Mirrors WaveRadarHUD. The
// widget is HitTestInvisible so it never blocks the on-screen touch controls beneath it.
// See specs/on-device-ride-replay.md and specs/replay-mode-clarity.md.
#pragma once

#include "CoreMinimal.h"

class UWorld;

/** One frame of replay-overlay data pushed by the pawn each replay tick. The widget just paints it. */
struct FReplayOverlaySnapshot
{
	float Progress = 0.0f;       // 0..1 through the trace
	float CurrentSeconds = 0.0f; // playback cursor, seconds
	float TotalSeconds = 0.0f;   // trace length, seconds
	bool bHolding = false;       // reached the end and holding on the last frame -> show end-card
};

namespace ReplayOverlay
{
	/** Add the replay overlay to the world's game viewport (full-screen, HitTestInvisible). No-op if already installed. */
	GONESURFING_API void Install(UWorld* World);

	/** Remove the replay overlay. */
	GONESURFING_API void Uninstall(UWorld* World);

	/** Push the latest playback state to the installed widget (game thread). */
	GONESURFING_API void UpdateData(UWorld* World, const FReplayOverlaySnapshot& Snapshot);
}
