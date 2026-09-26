#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WaveGeoDebugSubsystem.generated.h"

class AWaveHeight;
class AParticleSystemsController;
class ACameraActor;

/**
 * Wave-geometry debug: where the CREST LINE, the IMPACT POINT (where the lip is landing now) and the
 * WHITEWATER band are, drawn in the world so they can be checked by eye against the rendered wave and
 * the Niagara foam. specs/broken-wave-no-consequences.md T0 — the gate every whitewater consequence
 * waits behind: if the physics cannot say where the wave is breaking to within a board length, no
 * shove / drive cut / control loss may be built on it.
 *
 * Sources, per tick:
 *  - crest line   : the height scan along the wave's cross-shore axis (the same scan
 *                   ASharedCalculations uses for signedDistanceToCrest), repeated at positions along
 *                   the line around the pawn and followed as one wave. YELLOW spheres.
 *  - impact point : the +Y ("down-line") front of the foam clusters AParticleSystemsController already
 *                   computes for the crash audio, restricted to the band shoreward of the pawn's crest;
 *                   the furthest down the line is where this wave is breaking now. ORANGE sphere.
 *                   (Other fronts stay the controller's small red spheres.)
 *  - whitewater   : down-line of the impact point, between the crest line and `bore` cm shoreward.
 *                   CYAN outline.
 *  - model        : the peel model from UWaveGeometrySubsystem (Content/WaveGeometry/<map>.json, or
 *                   surf.wavegeo.model for an A/B): MAGENTA sphere + line, to compare with the
 *                   foam-derived orange. The board's sample (zone / distance behind the impact /
 *                   Broken) goes to the WAVEGEO-BOARD log line.
 *
 * Also: a camera parked on the shore side of the board looking at the face
 * (surf.debug.wavegeo.cam "dShore up dLine"), and screenshots taken by the game itself on given
 * frame numbers (surf.debug.wavegeo.shots "886:24:8" = start:step:count, on loop
 * surf.debug.wavegeo.shotloop), so the frames are exact and the debug draw is in them.
 *
 * All of it is off unless surf.debug.wavegeo is set (launch -ExecCmds; the CVars are read here
 * directly, so they stick). One WAVEGEO log line per tick in wave-frame coordinates
 * (s = along the line, + = down the line; c = cross-shore, + = toward the back of the wave) so the
 * two-point model can be fitted from a run.
 */
UCLASS()
class GONESURFING_API UWaveGeoDebugSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override
	{
		return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
	}
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UWaveGeoDebugSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickable() const override;

private:
	TWeakObjectPtr<AWaveHeight> WaveHeight;
	TWeakObjectPtr<AParticleSystemsController> Foam;
	TWeakObjectPtr<ACameraActor> Camera;
	bool bLookedUp = false;

	int32 LastFrame = -1;
	int32 Loop = 0;
	bool bAnchored = false;
	FVector Anchor = FVector::ZeroVector; // fixed world reference when surf.debug.wavegeo.fix is set
	TSet<int32> ShotsTakenThisLoop;

	void LookUp();
	FVector SurfaceAt(const FVector& XY, int32 Frame) const;
	void UpdateCamera(const FVector& PawnPos, const FVector& BackDir, const FVector& LineDir);
	void MaybeScreenshot(int32 Frame);
};
