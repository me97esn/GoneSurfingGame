// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FluidDynamics.h"
#include "SharedCalculations.h"
#include "SprayController.generated.h"

class UNiagaraSystem;
class UNiagaraComponent;

/**
 * Drives the board-spray Niagara system from the per-tick forces the AFluidDynamics actors applied.
 * Spray is the Newton-3 reaction to the water-contact force, so each site ejects opposite the summed
 * force of its assigned actors, magnitude ∝ force. Three sites: left rail, right rail, tail. Purely
 * cosmetic — reads accumulators, never writes to the force pipeline. Emission is gated by a
 * smoothstep on board speed (sprayMinSpeed..sprayFullGateSpeed) — deliberately NOT AmountPlaning,
 * which is a thresholded switch and pops spray in at full blast. One instance per surfboard
 * (set `Surfboard`). Ticks in TG_PostPhysics so every FluidDynamics actor has finished applying.
 * See specs/board-spray-particles.md.
 *
 * Niagara user parameters written each tick (all on the one attached component):
 *   vec3  SprayPosA/BLeft/Right/Tail — world-space emission line endpoints (the site's two
 *         furthest-apart actors, lifted up onto the waterline — the board planes submerged, so the
 *         raw actor positions sit BELOW the surface and spray must leave at the waterline).
 *         Emitters spawn at lerp(A, B, random 0..1) to scatter along the rail.
 *   vec3  SprayVelLeft/Right/Tail   — world-space ejection velocity (cm/s): the Newton-3 reaction
 *         deflected to skim along the water surface + sprayUpwardTilt, at force-derived speed
 *   float SpawnRateLeft/Right/Tail  — particles/s, fully gated (planing, wetting, budget)
 *   vec3  WaterPlanePos, WaterPlaneNormal — local water surface for the settle-to-foam module
 *   vec3  WaterVelocity             — water velocity settled foam damps toward
 */
UCLASS()
class GONESURFING_API ASprayController : public AActor
{
	GENERATED_BODY()

public:
	ASprayController();

	/** The surfboard this controller sprays for. Used to auto-discover this board's FluidDynamics
	 *  actors (matched against AFluidDynamics::Surfboard) and to reach the mesh the Niagara
	 *  component attaches to. Required unless the site arrays below are filled manually. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray")
	TObjectPtr<AActor> Surfboard;

	/** Optional: one of the board's SharedCalculations actors (either half). Supplies the planing
	 *  gate, the water-surface sample, and the water velocity. Auto-resolved from the first
	 *  discovered FluidDynamics actor when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray")
	TObjectPtr<ASharedCalculations> sharedCalculations;

	/** The spray Niagara system asset. Spawned attached to the surfboard mesh on first tick.
	 *  Its emitters must simulate in WORLD space and read the user parameters listed above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray")
	TObjectPtr<UNiagaraSystem> SpraySystem;

	/** Manual site assignment (overrides auto-discovery when non-empty). Auto-discovery classifies
	 *  by ESide: VE_Left/VE_Right → rails, VE_Tail + VE_Fin → tail, VE_Down by "left"/"right" in the
	 *  actor label, VE_Nose skipped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray|Sites")
	TArray<TObjectPtr<AFluidDynamics>> LeftRailActors;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray|Sites")
	TArray<TObjectPtr<AFluidDynamics>> RightRailActors;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray|Sites")
	TArray<TObjectPtr<AFluidDynamics>> TailActors;

	/** Master switch. When off, all spawn rates are written as 0 (live-toggleable). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray")
	bool bSprayEnabled = true;

	/** Applied site force (force units) → ejection speed (cm/s). Strength = |force| × this, capped
	 *  at sprayMaxEjectSpeed. Forces at a hard carve run ~10k-100k, so 0.01 maps that to 100-1000. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayForceToVelocity = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayMaxEjectSpeed = 800.0f;

	/** FORE-AFT ejection cap: the wake is sheared off along the track and can't leave faster than
	 *  the flow past the hull (≈ board speed). Fore-aft component ≤ this × HORIZONTAL board speed
	 *  (on top of the absolute cap) — kills the "too fast at low speed" class (takeoff backward
	 *  streaks) at the source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayMaxEjectVsBoardSpeed = 1.0f;

	/** CROSS-BOARD (+ tilt) ejection cap, deliberately looser than fore-aft: the carve fan is a
	 *  REDIRECTED JET, and a deflected jet in world frame reaches up to ~2× the flow speed
	 *  (elastic-deflection limit) — unlike the wake. Split per-component because hard carves
	 *  BLEED board speed, so a whole-vector cap clamped the fan exactly at the biggest-fan
	 *  moment (observed: sideways too slow at the hard turn, wake fine). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayMaxEjectVsBoardSpeedSideways = 1.75f;

	/** Spawn rate (particles/s per site) at full force, before the wetting/planing gates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayBaseRate = 150.0f;

	/** Site force magnitude at which the spawn rate saturates to sprayBaseRate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "1.0"))
	float sprayForceForFullRate = 20000.0f;

	/** Speed-ramp gate (replaces AmountPlaning, whose 0 → 0.8 threshold jump at takeoff popped
	 *  spray in at full blast): spawn rates fade in smoothstep over
	 *  sprayMinSpeed..sprayFullGateSpeed HORIZONTAL board speed (cm/s) — vertical wave-carry on a
	 *  rolling swell must not open the gate. Below the min: silence at rest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayMinSpeed = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "1.0"))
	float sprayFullGateSpeed = 400.0f;

	/** Clearance (cm) above the waterline for the emission endpoints. Must stay > 0: spawning ON
	 *  the plane makes the settle module claim the particle on its first frame (killing the
	 *  ejection velocity), exactly like spawning at the submerged rail did. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float spraySurfaceOffset = 3.0f;

	/** Upward tilt of the ejection direction out of the water-tangent plane (0 = skim flat along
	 *  the surface, 1 = 45°-ish fan). The raw Newton-3 reaction often points INTO the water (the
	 *  board is pushed up); the visible sheet is the part deflected out along the surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float sprayUpwardTilt = 0.35f;

	/** Multiplier on the SIDEWAYS (cross-board) component of the planar reaction before it drives
	 *  spray speed and spawn rate. Carving is distinguished by cross-board flow — >1 makes hard
	 *  turns out-spray straight-line riding (whose wake reaction is almost purely fore-aft drag)
	 *  without touching the wake. 1 = neutral. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayLateralWeight = 2.0f;

	/** Vertical calibration (cm) between the wave DATA surface and the RENDERED water, added to
	 *  every waterline the spray uses (endpoint lifts, settle plane, height grid). Player-calibrated
	 *  at -40: the data surface reads ~40cm above the visible water at the board. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray")
	float sprayWaterlineZBias = -40.0f;

	/** Emission-line depth (cm) BELOW the biased waterline — decouples where spray is born from
	 *  where foam settles (the bias). NEGATIVE = spawn above the settle line (used when the
	 *  foam-calibrated bias sits below the hull). Below-surface spawns need the settle module's
	 *  grace age (NormAge >= SettleGraceAge) or they're instantly claimed; above-surface spawns
	 *  never trigger it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray")
	float spraySpawnDepth = 15.0f;

	/** Pushes the emission line outward from the board centerline (cm) — the bottom actors in a
	 *  rail site sit inboard of the rail edge, dragging the furthest-apart pair toward the middle.
	 *  Rails offset along ±board.left (sign auto-resolved from which side of the board the site
	 *  sits on); the tail site offsets backward along -board.forwards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.0"))
	float sprayOutboardOffset = 20.0f;

	/** Tier 2 per-particle settle: N×N surface heights on a world-XY grid around the board,
	 *  shipped to Niagara each tick (WaterHeightGrid array + HeightGridOrigin/Spacing/Size params);
	 *  the settle module bilinearly samples the height under each particle. 0/1 = grid off.
	 *  Default 16×16 @ 200cm ≈ 30m span so the foam trail stays covered at speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0"))
	int32 heightGridSize = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "10.0"))
	float heightGridSpacing = 200.0f;

	/** Hard particle budget across all sites (Android). Spawn rates are scaled down uniformly when
	 *  totalRate × sprayAssumedLifetime would exceed this. Must be ≥ the Niagara system's own cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0"))
	int32 sprayMaxParticles = 400;

	/** Particle lifetime (s) assumed by the budget clamp — keep in sync with the Niagara asset's
	 *  max lifetime (ballistic phase + foam fade). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray", meta = (ClampMin = "0.01"))
	float sprayAssumedLifetime = 2.0f;

	/** PSO warm-up. The spray material's graphics pipeline state (PSO) compiles on its first DRAW,
	 *  which normally lands at takeoff/pop-up — the first frames the speed gate opens — and costs a
	 *  one-time hitch per app launch (the compiled PSO is cached for the rest of the process, so an
	 *  in-level Restart is smooth but relaunching the app brings the hitch back). When on, the
	 *  controller forces a brief low emission on the first ride after launch (once per process) so
	 *  that compile happens early, during the paddle phase, instead of at the visible stand-up.
	 *  Best-effort: a PSO only compiles when something actually draws, so this helps only while the
	 *  board/spray is on-screen during the window. The robust fix is a bundled PSO cache — see
	 *  specs/pso-precompile-and-spray-warmup.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray|Warmup")
	bool bSprayWarmup = true;

	/** Seconds of forced warm-up emission at ride start — long enough to spawn and render several
	 *  frames so every spray renderer's PSO compiles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray|Warmup", meta = (ClampMin = "0.0"))
	float sprayWarmupDuration = 0.5f;

	/** Per-site spawn rate (particles/s) during the warm-up window. Small — just enough to draw. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spray|Warmup", meta = (ClampMin = "0.0"))
	float sprayWarmupRate = 40.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugSpray = false;

	/** Throttle for the debug UE_LOG calls — only log on every Nth tick. 1 = every tick, 30 ≈ 2 Hz. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "1"))
	int32 debugLogEveryNthTick = 30;

public:
	virtual void Tick(float DeltaTime) override;

	/** Recorder access: this tick's per-site ejection velocity + spawn rate (post gate/budget),
	 *  written into the input-trace spray columns so replays play back the live spray verbatim.
	 *  See specs/on-device-ride-replay.md. */
	void GetSprayOutputs(FVector& OutVelLeft, float& OutRateLeft,
	                     FVector& OutVelRight, float& OutRateRight,
	                     FVector& OutVelTail, float& OutRateTail) const;

	/** Kinematic-replay override: substitute the recorded per-site velocities/rates for the
	 *  force-derived ones. During replay the force pipeline is tick-disabled, so the accumulators
	 *  freeze at their pre-replay values — without this override the controller would spray a
	 *  CONSTANT stale fan for the whole replay (or nothing, depending on the trigger moment).
	 *  Emission positions, waterline and the settle grid are still computed live (the wave clock
	 *  is re-synced, so they're deterministic). Terminal like the rest of replay mode: Restart
	 *  reloads the level. Push zero rates to stop emission while holding on the last frame. */
	void SetReplaySpray(const FVector& VelLeft, float RateLeft,
	                    const FVector& VelRight, float RateRight,
	                    const FVector& VelTail, float RateTail);

	/** Hand the spray back to the live force pipeline. Replay used to be terminal - only a level
	 *  reload left it - so nothing ever needed to undo the override. It is not terminal any more
	 *  (best-ride-replay.md D7): a player can leave a replay and carry on surfing, and without this
	 *  they would surf behind a frozen fan of the last replayed frame. */
	void ClearReplaySpray();

private:
	/** One emission site (rail or tail): its assigned actors plus this tick's derived outputs.
	 *  PositionA/B span the emission line (furthest-apart actor pair, lifted to the waterline);
	 *  emitters scatter spawns along it. */
	struct FSpraySite
	{
		TArray<AFluidDynamics*> Actors;
		FVector PositionA = FVector::ZeroVector;
		FVector PositionB = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float SpawnRate = 0.0f;
		float RailZ = 0.0f;   // debug: pre-lift emission-midpoint height
		float PlaneZ = 0.0f;  // debug: sampled waterline height at the site midpoint
	};

	/** Resolve site membership + sharedCalculations + spawn the attached Niagara component.
	 *  Runs on the first tick (not BeginPlay): FluidDynamics BPs assign `side` and surfboardMesh
	 *  in their own BeginPlay, so the data isn't trustworthy earlier. True once ready. */
	bool InitializeIfNeeded();

	/** Compute one site's emission line/velocity/rate from its actors' spray-drive force sums.
	 *  Samples the waterline plane at the site's OWN midpoint (the FD actor positions are live;
	 *  the board ACTOR's location is not guaranteed to track the simulated mesh — sampling there
	 *  lifted spawns onto a far-away waterline a metre above the board, observed). */
	/** outboardAxis: direction to push the emission line away from the centerline.
	 *  signFromCenterline: true for the rail sites — the axis sign is auto-resolved from which
	 *  side of the SC actor the site's midpoint sits on, so left/right pass the same axis. */
	void ComputeSite(FSpraySite& site, float speedGate, float horizBoardSpeed,
	                 const FVector& outboardAxis, bool signFromCenterline) const;

	void PushSiteToNiagara(const FSpraySite& site, const FName& posAParam, const FName& posBParam,
	                       const FName& velParam, const FName& rateParam) const;

	/** Wave surface (world location + unit normal) at a world position, false when no wave data. */
	bool SampleSurface(const FVector& pos, FVector& outLoc, FVector& outNormal) const;

	/** Reused per-tick buffer for the Tier 2 height grid (avoids a per-tick allocation). */
	TArray<float> HeightGridScratch;

	FSpraySite LeftSite;
	FSpraySite RightSite;
	FSpraySite TailSite;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> SprayComponent;

	bool bInitialized = false;
	bool bInitFailedWarned = false;
	int32 debugFrameCounter = 0;

	/** Seconds of warm-up emission elapsed on this instance (see bSprayWarmup). The once-per-process
	 *  guard lives in Tick as a static — the PSO is process-cached, so only the first ride needs it. */
	float warmupElapsed = 0.0f;

	/** Replay override state (see SetReplaySpray). */
	bool bReplaySprayOverride = false;
	FVector ReplayVel[3] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
	float ReplayRate[3] = { 0.0f, 0.0f, 0.0f };
};
