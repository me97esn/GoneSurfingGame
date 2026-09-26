// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FluidDynamics.h"
#include "SharedCalculations.h"
#include "ShadowController.generated.h"

/**
 * Makes the surfboard cast a shadow that lands where the board LOOKS like it is.
 *
 * The board renders ~30cm above the water mesh while riding — a deliberate visual compensation (the
 * hull planes submerged relative to the wave data, the render meshes were dropped ~40cm to match,
 * and the remaining lift keeps the board from vanishing inside the mesh). Switching on Cast Shadow
 * exposes that gap: the shadow lands `clearance × tan(sun angle)` away and the board reads as
 * levitating.
 *
 * The fix is a SHADOW-ONLY PROXY of the board's static meshes — hidden in game, cast hidden shadow,
 * same mesh/materials/rotation/scale — pushed down each tick by the board's measured clearance above
 * the RENDERED water surface, clamped:
 *
 *     offset = clamp( min over hull points of (pointZ - renderedSurfaceZ), 0, MaxGroundingOffset )
 *
 * Riding: clearance is under the cap, so the correction is full and the contact point's shadow is
 * planted. Airborne: the clamp saturates and the proxy still floats by the real air, so the shadow
 * separates. Pitched back: a rigid Z translation preserves every relative height in the mesh, so
 * the planted tail and the displaced nose come out right with no extra work — anchoring on the
 * LOWEST hull point is what makes the wetted end the one that plants.
 *
 * The rider is NOT proxied. His shadow lands on the board rather than on the water, so it has no
 * grounding error to correct, while a hidden skeletal duplicate that fails to hide is a second
 * visible surfer — which happened once in user testing at a large offset. He keeps casting from his
 * own mesh at his own position and this actor leaves him alone.
 *
 * The correction is light-independent: dropping the caster by its true clearance grounds the shadow
 * for any sun direction. Purely cosmetic — reads transforms and wave height, applies no force.
 * Ticks in TG_PostPhysics so the board transform is final. One instance per surfboard; with
 * `Surfboard` unset it resolves the board itself. See specs/board-shadow-grounding.md.
 */
UCLASS()
class GONESURFING_API AShadowController : public AActor
{
	GENERATED_BODY()

public:
	AShadowController();

	/** The surfboard whose components get shadow proxies, and whose AFluidDynamics actors supply the
	 *  hull points. Auto-resolved from the first AFluidDynamics in the world when unset — enough for
	 *  a single-board level, so placing this actor needs no wiring at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	TObjectPtr<AActor> Surfboard;

	/** Any of the board's SharedCalculations actors — only used to reach AWaveHeight for the surface
	 *  sample. Auto-resolved from the first discovered FluidDynamics actor when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	TObjectPtr<ASharedCalculations> sharedCalculations;

	/** Master switch. When off the proxies stop being corrected and simply track the board, so the
	 *  shadow falls back to the raw (too low) placement rather than disappearing. Live-toggleable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	bool bShadowEnabled = true;

	/** Wave DATA surface -> RENDERED water mesh surface (cm). The display meshes sit ~40cm below the
	 *  height data the physics samples, so a raw wave sample is NOT where the visible water is. Same
	 *  quantity and same value as ASprayController::sprayWaterlineZBias — deliberately not a second,
	 *  independently-drifting constant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	float shadowWaterlineZBias = -40.0f;

	/** Extra nudge on the contact plane (cm). **NEGATIVE pulls the shadow IN**, toward directly
	 *  beneath the board; positive lets it drift back out. (It lowers the contact plane, which raises
	 *  the measured clearance, which drops the caster further — and a lower caster throws its shadow
	 *  closer to straight down.) Absorbs the constant offset between the hand-placed FluidDynamics
	 *  sampler positions and the visible hull they sit on, and is the knob for "the shadow sits too
	 *  far from the board".
	 *
	 *  Applied BEFORE the clamp, so air still separates — but keep shadowMaxGroundingOffset
	 *  comfortably above the drop this produces while riding, or riding saturates the clamp too and
	 *  air stops reading as air.
	 *
	 *  Overwritten every tick by Tuning->ShadowContactZBias; this is only the fallback when no tuning
	 *  subsystem is present, kept in sync with the tuning default (90 = correction off). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	float shadowContactZBias = 90.0f;

	/** THE tuning number (cm): the declared line between artifact lift and real air. Clearance below
	 *  this is treated as the rendering offset and fully corrected away (shadow plants); clearance
	 *  above it is treated as genuine air and rendered honestly (shadow separates). Set just above
	 *  the highest clearance seen while riding — real air is metres, so the ambiguity band is narrow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow", meta = (ClampMin = "0.0"))
	float shadowMaxGroundingOffset = 60.0f;

	/** Exponential smoothing time constant (s) on the offset. Per-tick clearance is a min over ~20
	 *  points against sampled wave data and is noisy; without this the clamp engaging at takeoff
	 *  pops. A shadow may lag a frame or two invisibly — it may not jitter. 0 disables smoothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow", meta = (ClampMin = "0.0"))
	float shadowOffsetSmoothingSeconds = 0.12f;

	/** Who casts the board's shadow. 0 = the board itself (no grounding correction, but an ordinary
	 *  visible mesh casting an ordinary shadow — the fallback if a platform will not render hidden
	 *  shadow casters). 1 = the proxy, board silenced (default). 2 = as 1 but the proxies are drawn
	 *  too. Applied live every tick, so it can be A/B'd on a phone through the tuning HUD where there
	 *  is no console. Overwritten each tick by Tuning->ShadowMode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	float shadowMode = 0.0f;

	/** Current smoothed grounding offset (cm) — how far the shadow casters sit below the board. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Shadow|State")
	float shadowGroundingOffset = 0.0f;

	/** Unclamped, unsmoothed clearance (cm) of the lowest hull point above the rendered water. The
	 *  number to read when choosing shadowMaxGroundingOffset. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Shadow|State")
	float shadowRawClearance = 0.0f;

	/** Per-tick log of clearance/offset/surface. Also reachable at runtime via
	 *  `surf.debug.flags 'shadow'` (needs a non-empty surf.debug.actors matching this actor's label). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow|Debug")
	bool bDebugShadow = false;

	/** Draw the lowest hull point, where it gets grounded to, and the contact plane itself. The plane
	 *  is the calibration tool: screenshot it against the visible water and dial shadowWaterlineZBias
	 *  until they coincide. Gated by the same flag as the log. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow|Debug")
	bool bDebugDrawContact = false;

	/** Include the fin FluidDynamics actors as hull contact candidates. Off by design: fins hang well
	 *  below the hull and are "fully submerged by construction" (AFluidDynamics::calcThrustForce), so
	 *  they win the lowest-point search on every single tick and anchor the shadow to something that
	 *  is never the thing touching the surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shadow")
	bool bAnchorOnFins = false;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	/** Cached tuning subsystem. Null if the game instance isn't running, in which case the UPROPERTY
	 *  values above stand as the fallback. */
	UPROPERTY()
	TObjectPtr<class USurfTuningSubsystem> Tuning = nullptr;

	/** Pull live tuning onto this actor's UPROPERTY scratch fields, so everything below can keep
	 *  reading `this->X` and transparently see tuned values. Same shape as
	 *  ASurfboardUtils::RefreshFromTuningSubsystem. */
	void RefreshFromTuningSubsystem();

private:
	/** One visible component and the shadow-only clone that stands in for it. */
	struct FShadowProxy
	{
		TObjectPtr<USceneComponent> Source;
		TObjectPtr<UPrimitiveComponent> Proxy;
	};

	TArray<FShadowProxy> Proxies;

	/** The board's FluidDynamics actors, used purely as hull contact candidates. */
	TArray<TObjectPtr<AFluidDynamics>> HullPoints;

	bool bInitialized = false;
	bool bInitFailedWarned = false;

	/** Retry-until-wired: the FluidDynamics BPs assign their fields in BeginPlay, so the first few
	 *  ticks can legitimately find nothing. Returns true once the proxies exist. */
	bool InitializeIfNeeded();

	/** World-space wave DATA surface at `pos` (NOT yet biased to the rendered mesh). */
	bool SampleSurface(const FVector& pos, FVector& outLoc, FVector& outNormal) const;

	/** Lowest hull point's clearance (cm) above the rendered water surface. False if the wave could
	 *  not be sampled this tick, in which case the caller holds the previous offset. */
	bool ComputeClearance(float& outClearance, FVector& outLowestPoint, float& outLowestPlaneZ,
		FString& outLowestLabel, FVector& outPlaneOrigin, FVector& outPlaneNormal) const;

	/** Build the shadow-only clone of one visible component. Null if the component has no geometry. */
	UPrimitiveComponent* CreateProxyFor(USceneComponent* Source);

	/** Show or hide a proxy in the main pass, applying every mechanism that can hide it — no single
	 *  one covers both Nanite static meshes and the editor viewport. */
	static void SetProxyVisible(UPrimitiveComponent* Proxy, bool bVisible);
};
