// Pure-C++ Slate "wave radar": a small top-down schematic HUD showing the wave face and break line
// around the surfer, for rear/peripheral break awareness. No scene capture / render target — it paints
// 2D shapes from a grid snapshot the pawn pushes each update. See specs/wave-radar.md.
#pragma once

#include "CoreMinimal.h"

class UWorld;

/** One frame of radar data, sampled in BOARD-LOCAL space (heading-up): index = gy*GridN + gx,
 *  gx 0..N-1 = left..right, gy 0..N-1 = behind..ahead. The widget just paints it. */
struct FWaveRadarSnapshot
{
	int32 GridN = 0;
	TArray<float> HeightT;    // GridN*GridN, 0 (trough) .. 1 (crest) for face shading
	TArray<float> Foam;       // GridN*GridN, 0..1 foam intensity (temporally smoothed white-water density)
	float SurferAngleDeg = 0.0f; // surfer heading on the radar; 0 = up, 90 = pointing right
	float BoardHalfLenFrac = 0.0f; // board half-length as a fraction of radar size (world-scaled; 0 = fallback)
	float BoardHalfWidFrac = 0.0f; // board half-width as a fraction of radar size
	bool bValid = false;
};

namespace WaveRadar
{
	/** Add the radar widget to the world's game viewport (bottom-left). No-op if already installed. */
	GONESURFING_API void Install(UWorld* World, float RadarSizePx);

	/** Remove the radar widget. */
	GONESURFING_API void Uninstall(UWorld* World);

	/** Push the latest sampled grid to the installed widget (game thread). */
	GONESURFING_API void UpdateData(UWorld* World, const FWaveRadarSnapshot& Snapshot);
}
