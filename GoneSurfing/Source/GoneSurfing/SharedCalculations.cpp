// Fill out your copyright notice in the Description page of Project Settings.


#include "SharedCalculations.h"
#include "SurfLog.h"
#include "DrawDebugHelpers.h"
#include "SurfDebug.h"
#include "SurfRails.h"
#include "SurfAssist.h"
#include "SurfTuningSubsystem.h"
#include "WeightDistribution.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "GridLODActor.h"
#include "InfiniteWaveManager.h"
#include "ParticleSystemsController.h"
#include "WaveGeometrySubsystem.h"

namespace
{
	// Resolve the wave's cross-shore "back" axis (toward the back of the wave) from the
	// InfiniteWaveManager tile layout. Tiles step by (YOffsetPerActor, ActorSpacing) in world XY
	// — that's the down-line axis — so the crest runs along it and the cross-shore/back axis is the
	// perpendicular: (ActorSpacing, -YOffsetPerActor). Oriented to point roughly +X (this project's
	// back-of-wave convention; see [[wave-breaking-direction]]). Returns `fallback` if no manager /
	// not yet inferred. Cheap enough to resolve once per SC instance (see resolvedWaveBackDirection).
	FVector ResolveWaveBackDirectionFromManager(UWorld* World, const FVector& fallback)
	{
		if (World)
		{
			for (TActorIterator<AInfiniteWaveManager> It(World); It; ++It)
			{
				const float spacing = It->ActorSpacing;
				const float yoff    = It->YOffsetPerActor;
				if (FMath::Abs(spacing) > KINDA_SMALL_NUMBER)
				{
					FVector back = FVector(spacing, -yoff, 0.0f).GetSafeNormal();
					if (!back.IsNearlyZero())
					{
						if (back.X < 0.0f) back = -back; // convention: back points +X-ish
						return back;
					}
				}
			}
		}
		return fallback.GetSafeNormal();
	}

	// Resolve the WaterController BP the same way StateTriggerAutoPilot does — via the first
	// GridLODActor that has one wired up. Cached so the per-tick crossing log doesn't scan
	// actors every frame.
	AActor* FindWaterController(UWorld* World)
	{
		static TWeakObjectPtr<AActor> Cached;
		if (Cached.IsValid()) { return Cached.Get(); }
		if (!World) { return nullptr; }
		TArray<AActor*> Found;
		UGameplayStatics::GetAllActorsOfClass(World, AGridLODActor::StaticClass(), Found);
		for (AActor* A : Found)
		{
			if (AGridLODActor* Grid = Cast<AGridLODActor>(A))
			{
				if (Grid->WaterController)
				{
					Cached = Grid->WaterController;
					return Grid->WaterController;
				}
			}
		}
		return nullptr;
	}

	// WaterController.SecondsElapsed — the BP-set canonical in-sim elapsed-seconds anchor
	// (what the on-screen "time elapsed" reads). Prefer this over engine-frame/60, which
	// includes non-fixed warmup frames at level load. BP reals are FDoubleProperty in UE5.
	float SC_ReadWaterControllerSeconds(AActor* Controller)
	{
		if (!Controller) return -1.0f;
		FProperty* Prop = Controller->GetClass()->FindPropertyByName(TEXT("SecondsElapsed"));
		if (!Prop) return -1.0f;
		if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
			return F->GetPropertyValue_InContainer(Controller);
		if (FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
			return static_cast<float>(D->GetPropertyValue_InContainer(Controller));
		if (FIntProperty* I = CastField<FIntProperty>(Prop))
			return static_cast<float>(I->GetPropertyValue_InContainer(Controller));
		return -1.0f;
	}

	int32 SC_ReadWaterControllerFrame(AActor* Controller)
	{
		if (!Controller) return -1;
		FProperty* Prop = Controller->GetClass()->FindPropertyByName(TEXT("CurrentFrame"));
		if (!Prop) return -1;
		if (FIntProperty* I = CastField<FIntProperty>(Prop))
			return I->GetPropertyValue_InContainer(Controller);
		if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
			return FMath::RoundToInt(F->GetPropertyValue_InContainer(Controller));
		return -1;
	}
}

// Sets default values
ASharedCalculations::ASharedCalculations()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

}

void ASharedCalculations::RegisterAppliedForce(const FString& category, const FVector& worldPosition, const FVector& force)
{
	if (force.IsNearlyZero())
	{
		return;
	}
	FTorqueContribution& bucket = torqueBudget.FindOrAdd(category);
	const FVector relPos = worldPosition - this->cachedCenterOfMass;
	const FVector torque = FVector::CrossProduct(relPos, force);
	bucket.totalForce += force;
	bucket.totalTorque += torque;
	bucket.yawTorque  += FVector::DotProduct(torque, this->up.GetSafeNormal());
	bucket.count      += 1;
}

void ASharedCalculations::RegisterAppliedForceWithTorque(const FString& category, const FVector& force, const FVector& torque)
{
	if (force.IsNearlyZero() && torque.IsNearlyZero())
	{
		return;
	}
	FTorqueContribution& bucket = torqueBudget.FindOrAdd(category);
	bucket.totalForce += force;
	bucket.totalTorque += torque;
	bucket.yawTorque  += FVector::DotProduct(torque, this->up.GetSafeNormal());
	bucket.count      += 1;
}

// Called when the game starts or when spawned
void ASharedCalculations::RefreshFromTuningSubsystem()
{
	// Copy the planing thresholds from the subsystem onto this actor's scratch
	// UPROPERTYs each tick so HUD edits / JSON overrides apply live and override
	// any stale per-instance level values. Same pattern as AFluidDynamics.
	if (!Tuning)
	{
		return;
	}
	PlaningStartsVelocity = Tuning->PlaningStartsVelocity;
	PlaningStopsVelocity  = Tuning->PlaningStopsVelocity;
	PlaningFullVelocity   = Tuning->PlaningFullVelocity;
	MaxPlaning            = Tuning->MaxPlaning;
	waveNormalHeightDelta = Tuning->waveNormalHeightDelta;
}

void ASharedCalculations::BeginPlay()
{
	Super::BeginPlay();

	Tuning = SurfTuning::Get(this);
	RefreshFromTuningSubsystem();

	// One-shot dump of tunable defaults; see specs/runtime-tuning.md.
	// Dev-only diagnostics; stripped from Shipping builds.
#if !UE_BUILD_SHIPPING
#if WITH_EDITOR
	const FString lbl = GetActorLabel();
#else
	const FString lbl = GetName();
#endif
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: SharedCalculations [%s] PlaningStartsVelocity=%g PlaningStopsVelocity=%g PlaningFullVelocity=%g MaxPlaning=%g PlaningDecayTime=%g velocitySmoothingFactor=%g enableVelocitySmoothing=%d"),
		*lbl, PlaningStartsVelocity, PlaningStopsVelocity, PlaningFullVelocity, MaxPlaning, PlaningDecayTime, velocitySmoothingFactor, enableVelocitySmoothing ? 1 : 0);
#endif // !UE_BUILD_SHIPPING
}

// Called every frame
void ASharedCalculations::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	RefreshFromTuningSubsystem();
	this->PlaningForces.Empty();
	UpdateBrokenGeo();
	UpdateBrokenAmount(DeltaTime);
}

// specs/broken-wave-no-consequences.md A4 (T2): the one broken-water signal. A few multiplies,
// no allocation (specs/wave-geometry.md NFR). Runs on both SC actors - each reads its own position,
// so the nose can be in the whitewater while the tail is still in the pocket.
void ASharedCalculations::UpdateBrokenGeo()
{
	UWaveGeometrySubsystem* Geo = GetWorld() ? GetWorld()->GetSubsystem<UWaveGeometrySubsystem>() : nullptr;
	if (!Geo || !Geo->HasModel())
	{
		this->brokenGeo = 0.0f;
		this->waveZone = EWaveZone::Unknown;
		this->distBehindImpact = 0.0f;
		this->crossFromImpact = 0.0f;
		return;
	}
	const FWaveGeoSample Smp = Geo->SampleNow(GetActorLocation());
	this->brokenGeo = Smp.bValid ? Smp.Broken : 0.0f;
	this->waveZone = Smp.bValid ? Smp.Zone : EWaveZone::Unknown;
	this->distBehindImpact = Smp.bValid ? Smp.DistBehindImpact : 0.0f;
	this->crossFromImpact = Smp.bValid ? Smp.CrossFromImpact : 0.0f;
}

// DIAGNOSTIC (specs/broken-wave-no-consequences.md M11-M13, superseded by UpdateBrokenGeo): the
// foam-density read, kept so the `foam` log can still put the counts next to brokenGeo on a
// device run. O(N) over the ~1.5-3k cached foam points, so it only runs while that flag is set -
// nothing reads the result for behaviour.
void ASharedCalculations::UpdateBrokenAmount(float DeltaTime)
{
#if WITH_EDITOR
	const bool bWanted = SurfDebug::IsFlagSet(TEXT("foam"));
#else
	const bool bWanted = false;
#endif
	if (!bWanted)
	{
		this->brokenAmount = 0.0f;
		this->brokenSurroundAmount = 0.0f;
		this->foamPointsNearby = 0;
		this->foamPointsAhead = 0;
		this->foamPointsThinnestSector = 0;
		return;
	}
	if (!FoamController.IsValid())
	{
		for (TActorIterator<AParticleSystemsController> It(GetWorld()); It; ++It)
		{
			FoamController = *It;
			break;
		}
	}
	const AParticleSystemsController* Foam = FoamController.Get();
	if (!Foam || !Tuning)
	{
		this->brokenAmount = 0.0f;
		this->brokenSurroundAmount = 0.0f;
		this->foamPointsNearby = 0;
		this->foamPointsAhead = 0;
		this->foamPointsThinnestSector = 0;
		return;
	}
	const FVector Here = GetActorLocation();
	// Ahead = along the board's motion; below 100 cm/s the velocity direction is noise and the
	// nose stands in (a board at rest facing the foam is about to be driven into it).
	const FVector velH = FVector(this->componentVelocity.X, this->componentVelocity.Y, 0.0f);
	const FVector moveDir = velH.Size() > 100.0f ? velH.GetSafeNormal()
		: FVector(this->forwards.X, this->forwards.Y, 0.0f).GetSafeNormal();
	const double RDense2 = (double)Tuning->FoamBrokenRadius * (double)Tuning->FoamBrokenRadius;
	const double RAhead2 = (double)Tuning->FoamAheadRadius * (double)Tuning->FoamAheadRadius;
	const double ROuter2 = FMath::Max(RDense2, RAhead2);
	// Sectors for the SURROUNDED test: the dense disc split into four 90-degree wedges around the
	// motion (ahead / behind / left / right). A board beside the lip's foam, or turning its nose at
	// the broken section, fills one or two wedges and leaves the others empty; a board sitting in
	// the whitewater fills all four. Left of the motion = (-moveDir.Y, moveDir.X).
	int32 NDense = 0, NAhead = 0;
	int32 NSector[4] = { 0, 0, 0, 0 };
	int32 NSectorNear[4] = { 0, 0, 0, 0 }; // same wedges within half the radius (diagnostic)
	const double RNear2 = RDense2 * 0.25;
	for (const FVector& P : Foam->GetWhiteWaterWorldPoints())
	{
		const double dx = P.X - Here.X, dy = P.Y - Here.Y;
		const double d2 = dx * dx + dy * dy;
		if (d2 >= ROuter2) continue;
		const double along = dx * moveDir.X + dy * moveDir.Y;
		if (d2 < RDense2)
		{
			++NDense;
			const double across = -dx * moveDir.Y + dy * moveDir.X;
			const int32 Sector = (FMath::Abs(along) >= FMath::Abs(across)) ? (along >= 0.0 ? 0 : 1) : (across >= 0.0 ? 2 : 3);
			++NSector[Sector];
			if (d2 < RNear2) ++NSectorNear[Sector];
		}
		if (d2 < RAhead2 && along > 0.0) ++NAhead;
	}
	for (int32 k = 0; k < 4; ++k) { this->foamSectorCounts[k] = NSector[k]; this->foamSectorCountsNear[k] = NSectorNear[k]; }
	this->foamPointsNearby = NDense;
	this->foamPointsAhead = NAhead;
	this->foamPointsThinnestSector = FMath::Min(FMath::Min(NSector[0], NSector[1]), FMath::Min(NSector[2], NSector[3]));
	const float DenseTest = FMath::SmoothStep(
		Tuning->FoamBrokenCountLow,
		FMath::Max(Tuning->FoamBrokenCountLow + 1.0f, Tuning->FoamBrokenCountHigh),
		(float)NDense);
	const float AheadTest = FMath::SmoothStep(
		Tuning->FoamAheadCountLow,
		FMath::Max(Tuning->FoamAheadCountLow + 1.0f, Tuning->FoamAheadCountHigh),
		(float)NAhead);
	// 4 x the thinnest sector: equals the all-round count when the foam is uniform, so the same
	// FoamBrokenCount knee applies; collapses toward 0 as soon as one side is clear.
	const float SurroundTest = FMath::SmoothStep(
		Tuning->FoamBrokenCountLow,
		FMath::Max(Tuning->FoamBrokenCountLow + 1.0f, Tuning->FoamBrokenCountHigh),
		4.0f * (float)this->foamPointsThinnestSector);
	const float Raw = FMath::Max(DenseTest, AheadTest);
	const float Tau = Tuning->FoamBrokenSmoothSeconds;
	const float Alpha = (Tau > 0.0f && DeltaTime > 0.0f) ? 1.0f - FMath::Exp(-DeltaTime / Tau) : 1.0f;
	this->brokenAmount = FMath::Lerp(this->brokenAmount, Raw, Alpha);
	this->brokenSurroundAmount = FMath::Lerp(this->brokenSurroundAmount, SurroundTest, Alpha);
}


void ASharedCalculations::calculateAngleOfAttacks(FVector _relativeWaterVelocityNormalized)
{
	auto surfboardLeft = this->left;
	auto surfboardForwards = this->forwards;
	auto surfboardUp =  this->up;

	// VectorPlaneProject requires a unit-length plane normal -- it calls ProjectOnToNormal
	// internally, which doesn't divide by |N|^2. this->left/forwards/up carry the actor's
	// transform scale, so normalize before passing as the plane normal. The dot products
	// below keep the unnormalized basis vectors so the cosPitch/Yaw values retain their
	// actor-scale weighting that downstream formulas are calibrated against.
	auto surfboardLeftN     = surfboardLeft.GetSafeNormal();
	auto surfboardForwardsN = surfboardForwards.GetSafeNormal();
	auto surfboardUpN       = surfboardUp.GetSafeNormal();

	this->relativeWaterVelocityDirectionProjectedLeft = FVector::VectorPlaneProject(_relativeWaterVelocityNormalized, surfboardLeftN).GetSafeNormal();
	this->relativeWaterVelocityDirectionProjectedForwards = FVector::VectorPlaneProject(_relativeWaterVelocityNormalized, surfboardForwardsN).GetSafeNormal();
	this->relativeWaterVelocityDirectionProjectedUp = FVector::VectorPlaneProject(_relativeWaterVelocityNormalized, surfboardUpN).GetSafeNormal();

	this->cosPitchAngleOfAttack = surfboardForwards | this->relativeWaterVelocityDirectionProjectedLeft;

	this->pitchSinAngleOfAttack = -surfboardUp | this->relativeWaterVelocityDirectionProjectedLeft;

	this->cosYawAngleOfAttack =  -surfboardForwards | this->relativeWaterVelocityDirectionProjectedUp;
	this->cosYawAngleOfAttackLeft = surfboardLeft | this->relativeWaterVelocityDirectionProjectedUp;
	// True-cosine sibling for the fin terms: surfboardLeft carries the actor's transform scale
	// (~0.2), so cosYawAngleOfAttackLeft is ~0.2× the real cos. The fins need the real angle of
	// attack, so dot the NORMALIZED left here. (cosYawAngleOfAttackLeft is kept un-normalized for
	// the rail-engagement gate, which only reads its sign.) See specs/fin-force-normalization.md.
	this->cosYawAngleOfAttackLeftN = surfboardLeftN | this->relativeWaterVelocityDirectionProjectedUp;

	auto worldLeft = FVector(-1,0,0);
	auto worldLeftProjectedForwards = FVector::VectorPlaneProject(worldLeft, surfboardForwardsN).GetSafeNormal();
	this->rollCos = FVector::VectorPlaneProject(this->left, surfboardForwardsN).GetSafeNormal() | worldLeftProjectedForwards;
	this->rollSin = FVector::VectorPlaneProject(-this->up, surfboardForwardsN) | worldLeftProjectedForwards;
}

void ASharedCalculations::RegisterForwardDrive(float alongForwards)
{
	// Drive only — a governed term whose forward component came out negative is not propulsion this
	// tick and must not earn the budget back for the others (FR6.4).
	if (alongForwards > 0.0f)
	{
		this->PendingForwardDrive += alongForwards;
	}
}

void ASharedCalculations::RollOverPropulsionBudget(UPrimitiveComponent* BasePrimComp)
{
	const float previousThisHalf = this->PendingForwardDrive;
	this->PendingForwardDrive = 0.0f;
	this->PreviousForwardDrive = previousThisHalf;

	const USurfTuningSubsystem* T = SurfTuning::Get(this);
	const float ceilingW = T ? T->propulsionCeilingWeights : 0.0f;
	const float kneeW    = T ? T->propulsionKneeWeights    : 0.0f;
	// Disabled, or a knee at/above the ceiling (nothing to compress into) — pass everything through.
	if (!T || ceilingW <= 0.0f || kneeW >= ceilingW || !BasePrimComp)
	{
		this->PropulsionScale = 1.0f;
		this->LastGovernedWeights = 0.0f;
		return;
	}

	// Board weight, so the tunables mean the same thing if the board's mass changes.
	const float gZ = GetWorld() ? FMath::Abs(GetWorld()->GetGravityZ()) : 980.0f;
	const float boardWeight = BasePrimComp->GetMass() * gZ;
	if (boardWeight <= KINDA_SMALL_NUMBER)
	{
		this->PropulsionScale = 1.0f;
		return;
	}

	// Board-wide: this half plus the other. Each half governs itself with the same board-wide scale,
	// so both sides of the board are compressed identically and no yaw is injected by the governor.
	const float boardWideDrive = previousThisHalf
		+ (this->otherHalfSharedCalculations ? this->otherHalfSharedCalculations->PreviousForwardDrive : previousThisHalf);
	this->LastGovernedWeights = boardWideDrive / boardWeight;

	// FR6.5 — the ceiling rides on the local wave energy, so a bigger, faster wave still drives the
	// board harder. Clamped both ways: a near-still sample must not collapse the ceiling mid-ride.
	float energyScale = 1.0f;
	if (T->propulsionCeilingRefFlow > 0.0f)
	{
		const float refSq  = T->propulsionCeilingRefFlow * T->propulsionCeilingRefFlow;
		const float flowSq = this->absoluteWaterVelocity.SizeSquared();
		const float clamp  = FMath::Max(1.0f, T->propulsionCeilingFlowClamp);
		energyScale = FMath::Clamp(flowSq / refSq, 1.0f / clamp, clamp);
	}

	const float knee    = kneeW    * boardWeight * energyScale;
	const float ceiling = ceilingW * boardWeight * energyScale;

	if (boardWideDrive <= knee || boardWideDrive <= KINDA_SMALL_NUMBER)
	{
		this->PropulsionScale = 1.0f;
		return;
	}

	// FR6.3 — soft knee. Everything up to the knee passes; the excess is compressed into the
	// remaining headroom asymptotically, so the total approaches the ceiling but never reaches it.
	// No hard clip means no wall for the board to hit, which is the difference between "the wave can
	// only push you so hard" and "the speed is capped".
	const float headroom = ceiling - knee;
	const float excess   = boardWideDrive - knee;
	const float allowed  = knee + headroom * (1.0f - FMath::Exp(-excess / headroom));
	this->PropulsionScale = FMath::Clamp(allowed / boardWideDrive, 0.0f, 1.0f);
}

void ASharedCalculations::calculateAll(FString rowName, UPrimitiveComponent *BasePrimComp)
{
	// Propulsion budget roll-over. Same "every actor has finished applying for the prior tick"
	// moment the torque budget below relies on. See specs/wave-mass-thrust-closing-speed.md FR6.
	RollOverPropulsionBudget(BasePrimComp);

	if (SurfDebug::ShouldDebug(this, TEXT("propulsion")))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("PROPULSION [%s] governedDrive=%.2f weights scale=%.3f (knee=%.2f ceiling=%.2f waveFlow=%.0f)"),
			*GetName(), this->LastGovernedWeights, this->PropulsionScale,
			SurfTuning::Get(this) ? SurfTuning::Get(this)->propulsionKneeWeights : 0.0f,
			SurfTuning::Get(this) ? SurfTuning::Get(this)->propulsionCeilingWeights : 0.0f,
			this->absoluteWaterVelocity.Size());
	}

	// Torque budget diagnostic: log the PREVIOUS tick's accumulated per-category force
	// and yaw-torque, then clear for the new tick. FluidDynamics actors register their
	// applied forces via RegisterAppliedForce() between calculateAll() calls; this is
	// the only spot we know all of them have finished applying for the prior tick.
	if (SurfDebug::ShouldDebug(this, TEXT("torque")) && torqueBudget.Num() > 0)
	{
		FVector totalForce = FVector::ZeroVector;
		FVector totalTorque = FVector::ZeroVector;
		// Decompose forces and torques into BOARD space so they're interpretable without knowing the
		// board's world orientation. F[fwd,left,up]: fwd=propulsion/drag, left=sideways(out the rail),
		// up=lift/thrust (e.g. bottom Bernoulli suction should be pure -up; rail lift pure ±left).
		// T[roll,pitch,yaw]: roll about board.forwards, pitch about board.left, yaw about board.up.
		// Correlate roll torque sign with waveRelativeRollSin (logged below). See specs/barrel-glide-through-bug.md.
		const FVector fwdN  = this->forwards.GetSafeNormal();
		const FVector leftN = this->left.GetSafeNormal();
		const FVector upN   = this->up.GetSafeNormal();
		// World cross-shore axes for the into/out-of-wave force budget: Fin>0 pushes the board INTO the wave
		// (toward the back/crest, +resolved back axis); Fin<0 pushes it OUT (toward the front/shore). Fdown =
		// down-the-line component. Fz = vertical. See specs/submerged-downline-glide.md.
		const FVector crossN = this->resolvedWaveBackDirection.IsZero()
			? this->waveBackDirection.GetSafeNormal() : this->resolvedWaveBackDirection;
		const FVector tanN = FVector(-crossN.Y, crossN.X, 0.0f);
		for (const auto& kv : torqueBudget)
		{
			const FTorqueContribution& c = kv.Value;
			UE_LOG(LogSurf, Warning, TEXT("TORQUE [%s] cat=%-22s n=%-2d F[fwd/left/up]=(%.0f, %.0f, %.0f) magF=%.0f Fin=%.0f Fdown=%.0f Fz=%.0f T[roll/pitch/yaw]=(%.0f, %.0f, %.0f)"),
				*GetName(), *kv.Key, c.count,
				FVector::DotProduct(c.totalForce, fwdN), FVector::DotProduct(c.totalForce, leftN), FVector::DotProduct(c.totalForce, upN), c.totalForce.Size(),
				FVector::DotProduct(c.totalForce, crossN), FVector::DotProduct(c.totalForce, tanN), c.totalForce.Z,
				FVector::DotProduct(c.totalTorque, fwdN), FVector::DotProduct(c.totalTorque, leftN), FVector::DotProduct(c.totalTorque, upN));
			totalForce  += c.totalForce;
			totalTorque += c.totalTorque;
		}
		// Gravity (engine, whatever the fork's reduced value is): a board-wide weight force. Logged by only
		// ONE of the two SC actors (pointer guard) so summing both SCs in analysis doesn't double-count it.
		// Included in TOTAL so the net vertical/into-wave balance is complete alongside buoyancy + fluid.
		if (BasePrimComp && (this->otherHalfSharedCalculations == nullptr
			|| (void*)this < (void*)this->otherHalfSharedCalculations))
		{
			const float gZ = GetWorld() ? GetWorld()->GetGravityZ() : -980.0f;
			const FVector gForce = FVector(0.0f, 0.0f, BasePrimComp->GetMass() * gZ);
			UE_LOG(LogSurf, Warning, TEXT("TORQUE [%s] cat=%-22s n=%-2d F[fwd/left/up]=(%.0f, %.0f, %.0f) magF=%.0f Fin=%.0f Fdown=%.0f Fz=%.0f T[roll/pitch/yaw]=(0, 0, 0)"),
				*GetName(), TEXT("gravity"), 1,
				FVector::DotProduct(gForce, fwdN), FVector::DotProduct(gForce, leftN), FVector::DotProduct(gForce, upN), gForce.Size(),
				FVector::DotProduct(gForce, crossN), FVector::DotProduct(gForce, tanN), gForce.Z);
			totalForce += gForce;
		}
		UE_LOG(LogSurf, Warning, TEXT("TORQUE [%s] cat=TOTAL                  F[fwd/left/up]=(%.0f, %.0f, %.0f) magF=%.0f Fin=%.0f Fdown=%.0f Fz=%.0f T[roll/pitch/yaw]=(%.0f, %.0f, %.0f)  waveRollSin=%.4f"),
			*GetName(),
			FVector::DotProduct(totalForce, fwdN), FVector::DotProduct(totalForce, leftN), FVector::DotProduct(totalForce, upN), totalForce.Size(),
			FVector::DotProduct(totalForce, crossN), FVector::DotProduct(totalForce, tanN), totalForce.Z,
			FVector::DotProduct(totalTorque, fwdN), FVector::DotProduct(totalTorque, leftN), FVector::DotProduct(totalTorque, upN), this->waveRelativeRollSin);
	}
	torqueBudget.Empty();

	// Cache CoM for this tick's RegisterAppliedForce calls. Falls back to SC actor location
	// if the surfboard primitive isn't resolvable (e.g., early-init).
	if (BasePrimComp)
	{
		this->cachedCenterOfMass = BasePrimComp->GetCenterOfMass();
	}
	else
	{
		this->cachedCenterOfMass = GetActorLocation();
	}

	// Store the current frame name (this is the tile-adjusted frame used for wave height)
	this->currentFrame = UKismetStringLibrary::Conv_StringToName(rowName);

	// Extract the frame number from rowName - this is already tile-adjusted for this location
	// We should use the SAME frame for velocity as we use for wave height
	int32 tileAdjustedFrame = 0;
	if (rowName.Contains(TEXT("Frame_")))
	{
		FString frameStr = rowName.Replace(TEXT("Frame_"), TEXT(""));
		tileAdjustedFrame = FCString::Atoi(*frameStr);
	}
	else
	{
		tileAdjustedFrame = FCString::Atoi(*rowName);
	}

	this->lastTileAdjustedFrame = tileAdjustedFrame;

	// Calculate absolute water velocity at this location
	// NOTE: calculateWaveVelocity returns velocity in Blender/data space (not world space)
	// We need to rotate it to world space using the WaveHeight actor's rotation
	FVector actorLocation = GetActorLocation();
	FVector velocityBlenderSpace = this->waveVelocity->calculateWaveVelocity(actorLocation, tileAdjustedFrame);
	FQuat waveActorRot = this->waveVelocity->GetActorQuat();
	FVector rawAbsoluteWaterVelocity = waveActorRot.RotateVector(velocityBlenderSpace);

	// Apply temporal smoothing to reduce velocity striping artifacts
	if (enableVelocitySmoothing && !previousAbsoluteWaterVelocity.IsZero())
	{
		// Lerp between raw velocity and previous velocity
		// velocitySmoothingFactor = 0 means no smoothing (use raw)
		// velocitySmoothingFactor = 1 means full smoothing (use previous, very laggy)
		this->absoluteWaterVelocity = FMath::Lerp(rawAbsoluteWaterVelocity, previousAbsoluteWaterVelocity, velocitySmoothingFactor);
	}
	else
	{
		this->absoluteWaterVelocity = rawAbsoluteWaterVelocity;
	}

	// Store for next frame
	previousAbsoluteWaterVelocity = this->absoluteWaterVelocity;

	this->absoluteWaterVelocityNormalized = this->absoluteWaterVelocity.GetSafeNormal();

	// DEBUG: Log SharedCalculations velocity calculation details with actor name
	if (this->waveVelocity && this->waveVelocity->bEnableVelocityLogging)
	{
		UE_LOG(LogSurf, Warning, TEXT("[%s] BLUE ARROW - TileAdjustedFrame %d (rowName=%s), World Pos: (%.2f, %.2f, %.2f), Velocity Blender: (%.6f, %.6f, %.6f), Velocity World: (%.6f, %.6f, %.6f)"),
			*GetName(),
			tileAdjustedFrame, *rowName,
			actorLocation.X, actorLocation.Y, actorLocation.Z,
			velocityBlenderSpace.X, velocityBlenderSpace.Y, velocityBlenderSpace.Z,
			this->absoluteWaterVelocity.X, this->absoluteWaterVelocity.Y, this->absoluteWaterVelocity.Z);
	}
	this->relativeWaterVelocity = this->calcRelativeWaterVelocity(BasePrimComp, absoluteWaterVelocity);
	this->relativeWaterVelocityLocalSpace = GetTransform().InverseTransformVector(this->relativeWaterVelocity);
	this->relativeWaterVelocityMagnitude = this->relativeWaterVelocity.Size();
	this->absoluteWaterVelocityMagnitude = this->absoluteWaterVelocity.Size();
	this->relativeWaterVelocityNormalized = this->relativeWaterVelocity.GetSafeNormal();

	if (SurfDebug::IsFlagSet(TEXT("abs_velocity")))
	{
		UE_LOG(LogSurf, Display, TEXT("AbsWaterVel: name=%s mag=%.2f vec=(%.2f, %.2f, %.2f)"),
			*GetName(), this->absoluteWaterVelocityMagnitude,
			this->absoluteWaterVelocity.X, this->absoluteWaterVelocity.Y, this->absoluteWaterVelocity.Z);
	}

	// Debug visualization for velocities
	if (debug || SurfDebug::ShouldDebug(this, TEXT("shared")))
	{
		UWorld* World = GetWorld();
		if (World)
		{
			// Use actorLocation already declared above

			// Draw absolute water velocity (blue arrow)
			if ((debugDrawAbsoluteVelocity || SurfDebug::ShouldDebug(this, TEXT("abs_velocity"))) && absoluteWaterVelocityMagnitude > 0.01f)
			{
				FVector absoluteEnd = actorLocation + (absoluteWaterVelocity * velocityDebugScale);
				DrawDebugDirectionalArrow(
					World,
					actorLocation,
					absoluteEnd,
					50.0f,                    // Arrow size
					FColor::Red,             // Blue for absolute velocity
					false,                    // Not persistent
					0.1f,                     // Lifetime (0 = one frame)
					0,                        // Depth priority
					3.0f                      // Thickness
				);

				// Optional: Draw text label
				if (debugShowVelocityText || SurfDebug::ShouldDebug(this, TEXT("velocity_text")))
				{
					FString label = FString::Printf(TEXT("Abs: %.1f"), absoluteWaterVelocityMagnitude);
					DrawDebugString(World, actorLocation + FVector(0, 0, 50), label, nullptr, FColor::Blue, 0.0f, true);
				}
			}

			// Draw relative water velocity (emerald arrow — distinct from FluidDynamics force colors)
			if ((debugDrawRelativeVelocity || SurfDebug::ShouldDebug(this, TEXT("rel_velocity"))) && relativeWaterVelocityMagnitude > 0.01f)
			{
				FVector relativeEnd = actorLocation + (this->relativeWaterVelocity * velocityDebugScale);
				DrawDebugDirectionalArrow(
					World,
					actorLocation,
					relativeEnd,
					50.0f,                    // Arrow size
					FColor::Emerald,
					false,                    // Not persistent
					0.1f,                     // Lifetime (0 = one frame)
					0,                        // Depth priority
					3.0f                      // Thickness
				);

				// Optional: Draw text label
				if (debugShowVelocityText || SurfDebug::ShouldDebug(this, TEXT("velocity_text")))
				{
					FString label = FString::Printf(TEXT("Rel: %.1f"), relativeWaterVelocityMagnitude);
					DrawDebugString(World, actorLocation + FVector(0, 0, 80), label, nullptr, FColor::Emerald, 0.0f, true);
				}
			}
		}
	}
	// Refresh orientation vectors BEFORE computing angles of attack — calculateAngleOfAttacks reads this->left/forwards/up.
	auto upLocal = FVector(0.0, 0.0, 1.0);
	auto forwardsLocal = FVector(0.0, 1.0, 0.0); // Points toward the nose. The SharedCalculations actors have no rotation, so no 180 degree compensation needed.
	auto leftLocal = FVector(-1.0, 0.0, 0.0);
	this->up = GetTransform().TransformVector(upLocal);
	this->forwards = GetTransform().TransformVector(forwardsLocal);
	this->left = GetTransform().TransformVector(leftLocal);
	this->calculateAngleOfAttacks(this->relativeWaterVelocityNormalized);
	this->calculateAmountPlaning(this->relativeWaterVelocityLocalSpace);

	// Wave-relative roll: how rolled is the board relative to the LOCAL wave surface?
	// Wave-aligned board → zero. Horizontal board on a tilted wave → non-zero.
	// Used by hydrodynamic forces that care about rail depth in water (carving), not gravity orientation.
	if (this->waveVelocity)
	{
		auto locationAndNormal = this->waveVelocity->calculateWaveLocationAndNormal(actorLocation, tileAdjustedFrame);
		this->waveNormal = locationAndNormal[1];

		// Height-derived normal. The stored normals are noisy (M6), and because slopeSin is a
		// MAGNITUDE that noise inflates it rather than cancelling. Heights are clean, so central-
		// difference them instead. Absolute heights from this sampler are known to read oddly
		// (~20 where the board rides at ~280) but only DIFFERENCES are used here, which is exactly
		// the case that has always been sound. See specs/wave-interaction-damping-and-redirect.md.
		const bool bProbeSlope = SurfDebug::ShouldDebug(this, TEXT("slopeprobe"));
		if (this->waveNormalHeightDelta > 0.0f || bProbeSlope)
		{
			const float d = (this->waveNormalHeightDelta > 0.0f) ? this->waveNormalHeightDelta : 60.0f;
			const FVector dx(d, 0.0f, 0.0f), dy(0.0f, d, 0.0f);
			const double zxp = this->waveVelocity->calculateWaveLocationAndNormal(actorLocation + dx, tileAdjustedFrame)[0].Z;
			const double zxm = this->waveVelocity->calculateWaveLocationAndNormal(actorLocation - dx, tileAdjustedFrame)[0].Z;
			const double zyp = this->waveVelocity->calculateWaveLocationAndNormal(actorLocation + dy, tileAdjustedFrame)[0].Z;
			const double zym = this->waveVelocity->calculateWaveLocationAndNormal(actorLocation - dy, tileAdjustedFrame)[0].Z;
			const double gx = (zxp - zxm) / (2.0 * d);
			const double gy = (zyp - zym) / (2.0 * d);
			const FVector nFromHeights = FVector(-gx, -gy, 1.0).GetSafeNormal();

			if (bProbeSlope)
			{
				const float storedSin  = (float)FMath::Sqrt(FMath::Max(0.0, 1.0 - this->waveNormal.Z * this->waveNormal.Z));
				const float derivedSin = (float)FMath::Sqrt(FMath::Max(0.0, 1.0 - nFromHeights.Z * nFromHeights.Z));
				UE_LOG(LogSurf, Warning,
					TEXT("SLOPEPROBE [%s] d=%.0f storedSin=%.4f derivedSin=%.4f ratio=%.2f | storedN=(%.3f, %.3f, %.3f) derivedN=(%.3f, %.3f, %.3f) grad=(%.4f, %.4f)"),
					*GetName(), d, storedSin, derivedSin,
					derivedSin > 1e-4f ? storedSin / derivedSin : -1.0f,
					this->waveNormal.X, this->waveNormal.Y, this->waveNormal.Z,
					nFromHeights.X, nFromHeights.Y, nFromHeights.Z, gx, gy);
			}
			if (this->waveNormalHeightDelta > 0.0f && !nFromHeights.IsNearlyZero())
			{
				this->waveNormal = nFromHeights;
			}
		}
		// Wave-tangent "left": perpendicular to wave normal AND board forward, lying in the wave plane.
		// Cross product order chosen so that for a wave-aligned board, waveTangentLeft equals board.left.
		FVector waveTangentLeft = FVector::CrossProduct(this->waveNormal, this->forwards).GetSafeNormal();
		// VectorPlaneProject assumes a unit-length plane normal; this->forwards carries the
		// actor's transform scale so it must be normalized before being used as one.
		const FVector forwardsN = this->forwards.GetSafeNormal();
		this->waveRelativeRollCos = FVector::VectorPlaneProject(this->left, forwardsN).GetSafeNormal() | waveTangentLeft;
		this->waveRelativeRollSin = FVector::VectorPlaneProject(-this->up, forwardsN) | waveTangentLeft;

		// World-relative roll: same construction but against the HORIZONTAL plane instead of the wave
		// surface. worldTangentLeft = cross(worldUp, forwards) is always horizontal (z=0), so this reads
		// ~0 for a board level in the world regardless of wave slope — isolating the rider's lean from
		// the wave tilt. Drives lateralTurnForce. See specs/lateral-turn-world-relative-roll.md.
		const FVector worldUp = FVector(0.0, 0.0, 1.0);
		const FVector worldTangentLeft = FVector::CrossProduct(worldUp, this->forwards).GetSafeNormal();
		this->worldRelativeRollCos = FVector::VectorPlaneProject(this->left, forwardsN).GetSafeNormal() | worldTangentLeft;
		this->worldRelativeRollSin = FVector::VectorPlaneProject(-this->up, forwardsN) | worldTangentLeft;

		// Wave-slope downhill vector: world-down projected onto the wave-tangent plane.
		// |waveSlopeDownVec| = sin(slope_angle), direction = down the slope.
		// Used by FluidDynamics' waveSlopeGravity supplement and by StateTriggerAutoPilot
		// as a slope-magnitude metric.
		FVector worldDown = FVector(0.0, 0.0, -1.0);
		this->waveSlopeDownVec = worldDown - (worldDown | this->waveNormal) * this->waveNormal;

		// Board-wide slopeSin and waterColumnAbove — one sample at the SC actor's position,
		// for forces that represent a board-wide phenomenon and shouldn't see per-actor sampling
		// noise. See specs/per-actor-vs-board-wide-sampling.md.
		this->boardWideSlopeSin = (float)FMath::Sqrt(FMath::Max(0.0, 1.0 - this->waveNormal.Z * this->waveNormal.Z));
		this->boardWideWaterColumnAbove = FMath::Max(0.0f, (float)(locationAndNormal[0].Z - actorLocation.Z));

		// === SIGNED DISTANCE TO CREST ===
		// Scan the wave-surface height toward the back of the wave to find the local crest (height peak),
		// then report this point's signed offset from it: + = behind the crest (back/far side), - = front-face
		// side. This is front/back RELATIVE TO THE MOVING WAVE, which absolute world position can't give.
		//
		// Back-direction: the wave's cross-shore axis, derived ONCE from the InfiniteWaveManager tile
		// geometry (perpendicular to the down-line tiling axis). This is stable per tick, unlike deriving
		// it from the instantaneous wave normal (which goes vertical => ill-conditioned right at the crest),
		// and correct on this level's diagonally-tiled wave, unlike the old fixed waveBackDirection (+X) which
		// was ~34deg off and mislocated/saturated the crest. See specs/wave-crossing-deceleration.md.
		//
		// Search: hill-climb outward in the uphill direction until height stops rising, so the result doesn't
		// saturate at a fixed window when the crest is far. Cost scales with the actual distance to the crest.
		{
			if (this->resolvedWaveBackDirection.IsZero())
			{
				this->resolvedWaveBackDirection =
					ResolveWaveBackDirectionFromManager(GetWorld(), this->waveBackDirection);
			}
			const FVector backDir = this->resolvedWaveBackDirection;
			if (!backDir.IsNearlyZero())
			{
				constexpr float kStepCm = 50.0f;
				constexpr int32 kMaxSteps = 60;   // give up past ±3000 cm (still reported, but flagged far)
				auto sampleH = [&](float off) -> double
				{
					return std::get<0>(this->waveVelocity->waveHeightAndNormal(
						actorLocation + backDir * off, tileAdjustedFrame));
				};
				const double h0 = sampleH(0.0f);
				const double hPlus = sampleH(kStepCm);
				const double hMinus = sampleH(-kStepCm);
				double bestHeight = h0;
				float bestOffset = 0.0f;
				float dir = 0.0f;

				// Robust scan: symmetric outward sweep, nearest significant peak wins.
				//
				// The legacy hill-climb below commits to a direction on ONE +/-50cm comparison and
				// then walks that way until height stops rising. Measured 2026-08-26 over a 12k-sample
				// run: it set off SHOREWARD on 60% of ticks. In a locally flat trough (between waves,
				// or across a tile seam) the three seed samples agree to within ~0.02cm, so the
				// direction is chosen by numerical noise — and it then walks up to 10m to a leftover
				// whitewater bump of height 17.5 while the actual face sits at height 20, 300cm
				// seaward. Same height profile, opposite answer.
				//
				// Two changes fix that. Sweeping BOTH ways removes the noisy commitment: the global
				// peak wins regardless of which side it is on. And requiring a real improvement
				// (kCrestNoiseFloorCm) to move the answer means the nearest peak holds unless
				// something meaningfully taller turns up — so a 0.02cm-higher ripple 10m away can no
				// longer steal the crest. The range is bounded too: a crest 30m off is not the wave
				// being ridden.
				const bool bRobustScan = (this->Tuning ? this->Tuning->CrestScanRobust : 1.0f) >= 0.5f;
				if (bRobustScan)
				{
					const float rangeCm = this->Tuning ? this->Tuning->CrestScanRangeCm : 1200.0f;
					constexpr double kCrestNoiseFloorCm = 0.5;
					const int32 steps = FMath::Max(1, FMath::RoundToInt(rangeCm / kStepCm));
					for (int32 i = 1; i <= steps; ++i)
					{
						const float offsets[2] = { -(float)i * kStepCm, (float)i * kStepCm };
						for (const float off : offsets)
						{
							const double h = sampleH(off);
							if (h > bestHeight + kCrestNoiseFloorCm)
							{
								bestHeight = h;
								bestOffset = off;
							}
						}
					}
					dir = FMath::Sign(bestOffset);
				}
				else if (hPlus > h0 && hPlus >= hMinus) { dir = 1.0f;  bestHeight = hPlus;  bestOffset = kStepCm; }
				else if (hMinus > h0)              { dir = -1.0f; bestHeight = hMinus; bestOffset = -kStepCm; }
				// else already at/straddling the peak (dir stays 0 => distance ~0)
				if (!bRobustScan && dir != 0.0f)
				{
					for (int32 i = 2; i <= kMaxSteps; ++i)
					{
						const float off = dir * i * kStepCm;
						const double h = sampleH(off);
						if (h > bestHeight) { bestHeight = h; bestOffset = off; }
						else break; // passed the crest
					}
				}
				this->crestWorldPosition = actorLocation + backDir * bestOffset;
				// (this point, offset 0) minus (crest, offset bestOffset), along backDir => -bestOffset.
				this->signedDistanceToCrest = -bestOffset;

				// Hill-climb diagnostic: which way the scan set off, how far it walked, and the
				// cross-shore height profile it walked over. The scan is winner-takes-all on the
				// first +/-50cm comparison, so if leftover whitewater shoreward is momentarily
				// higher than the face seaward, it walks the wrong way and latches onto the wrong
				// crest. This says whether that is what happens.
				if (this->bAssistBandReference && SurfDebug::IsFlagSet(TEXT("assist")))
				{
					FString profile;
					for (int32 o = -1200; o <= 1800; o += 300)
					{
						profile += FString::Printf(TEXT("%d:%.0f "), o, sampleH((float)o));
					}
					UE_LOG(LogSurf, Warning,
						TEXT("CrestScan frame=%d dir=%+.0f bestOffset=%.0f d=%.0f h0=%.1f hMinus=%.1f hPlus=%.1f bestH=%.1f | %s"),
						tileAdjustedFrame, dir, bestOffset, this->signedDistanceToCrest,
						h0, hMinus, hPlus, bestHeight, *profile);
				}

				// === ASSIST BAND OVERLAY (specs/gradual-control-handoff.md FR2) ==================
				// Draws the two edges of the no-assist band on the wave face, so a playtest can tell
				// "the band is in the wrong place" apart from "the controller is misbehaving" —
				// which are otherwise indistinguishable from the deck.
				//
				// Toggled by AssistDrawBand in the tuning HUD, because that is the only switch
				// reachable on a phone (no console there).
				//
				// The edge positions come from crestWorldPosition, and the in/out colouring from
				// SurfAssist::BandError — the same function the controller gates on, not a copy of
				// its logic. A drawn band that disagreed with the enforced one would send you
				// hunting for the wrong bug.
				// Only the SC the assist itself reads draws the overlay. A board has a front and a
				// back SC and each hill-climbs its own crest, so without this the whole thing is
				// drawn twice at two slightly different estimates — which looks like a bug in the
				// band rather than two honest samples. The pawn marks its resolved one.
				if (const USurfTuningSubsystem* tuningForBand = this->bAssistBandReference ? SurfTuning::Get(this) : nullptr)
				{
					if (tuningForBand->AssistDrawBand >= 0.5f)
					{
						const float bandNear = tuningForBand->AssistBandNear;
						const float bandFar  = tuningForBand->AssistBandFar;

						// A point at signed distance d sits at crest + backDir*d, and the band lives
						// at negative d (the front face). Derived above, not assumed.
						const FVector nearEdgeCentre = this->crestWorldPosition - backDir * bandNear;
						const FVector farEdgeCentre  = this->crestWorldPosition - backDir * bandFar;

						// Along the wave, perpendicular to the cross-shore axis.
						const FVector downLine =
							FVector::CrossProduct(FVector::UpVector, backDir).GetSafeNormal();

						// Drawn at the BOARD's height, not at a sampled wave height.
						//
						// waveHeightAndNormal's return value is not directly comparable to world Z:
						// measured 2026-08-26, it reads ~20 at the edge centres while the board is
						// riding at Z~280, so lines placed at the sampled height ended up ~2.8m under
						// the water and were depth-tested away. The crest scan above never noticed
						// because it only COMPARES samples to find a peak — this drawing was the
						// first consumer of the absolute value.
						//
						// Using the board's own Z sidesteps the question entirely and is honest: the
						// band is a horizontal-plane concept (distance to crest), the edges are at
						// most a few metres from the board, and board height is where the eye is
						// looking anyway. Lifted slightly so it clears the deck.
						//
						// Deliberately NOT SDPG_Foreground: that routes to ForegroundLineBatcher,
						// which short-circuits LifeTime entirely (DrawDebugHelpers.cpp
						// GetDebugLineBatcher) and is Flush()ed every frame in
						// UGameViewportClient::Draw — foreground lines cannot survive a pause.
						constexpr float kBandDrawZLift = 15.0f;
						// Lifetime rather than one frame: a single-frame line is at the mercy of
						// whatever the frame rate is doing, and on a phone that reads as flicker.
						// Tunable because it doubles as the screenshot knob — pausing stops this
						// actor ticking, so the redraw stops and whatever is on screen expires.
						//
						// Redraw CADENCE is tied to the lifetime, and that is the whole trick.
						//
						// Drawing every frame with a lifetime means ~18 copies at 0.3s/60fps, and
						// because the board moves they fan out rather than stacking — with any
						// thickness at all they merge into screen-filling wedges. Drawing every
						// frame with a one-frame lifetime is crisp but vanishes the instant the game
						// is paused, which is exactly when a screenshot gets taken.
						//
						// So: redraw only as the previous copy is about to expire. At most two
						// copies exist at once, the band stays crisp, and it survives a pause for
						// whatever lifetime was asked for. Winding AssistDrawBandSeconds up for a
						// screenshot now buys persistence WITHOUT multiplying copies, because the
						// cadence stretches with it. Cost: the band updates at 1/lifetime Hz, so it
						// lags the board by up to one lifetime — keep it short while riding.
						const float kBandDrawSeconds = FMath::Max(0.0f, tuningForBand->AssistDrawBandSeconds);
						constexpr float kBandDrawHalfSpanCm = 1200.0f;

						// One straight segment per edge, at a SINGLE sampled height.
						//
						// This used to be a 20-point polyline that height-sampled the wave along its
						// whole length so it hugged the face. It sampled out to +/-12m from the
						// board, well past where waveHeightAndNormal returns anything meaningful,
						// and the bad points turned thick-line quads into screen-filling red wedges.
						// The band is a horizontal-plane concept — distance to crest — so hugging
						// the surface bought nothing and cost robustness. One height, taken at the
						// board where the sample is trustworthy.
						auto drawBandEdge = [&](const FVector& centre, const FColor& colour)
						{
							FVector c = centre;
							c.Z = actorLocation.Z + kBandDrawZLift;
							DrawDebugLine(GetWorld(),
								c - downLine * kBandDrawHalfSpanCm,
								c + downLine * kBandDrawHalfSpanCm,
								colour, false, kBandDrawSeconds, SDPG_World, 4.0f);
						};

						const float nowSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
						const bool bRedrawDue =
							(nowSeconds - this->lastAssistBandDrawTime) >= (kBandDrawSeconds * 0.8f);

						const float bandErr = SurfAssist::BandError(this->signedDistanceToCrest, bandNear, bandFar);
						const bool bInsideBand = (bandErr == 0.0f);

						if (bRedrawDue)
						{
						this->lastAssistBandDrawTime = nowSeconds;

						// Red = the edge the board is currently outside of; green = the band holding.
						// Colouring both edges by one in/out flag would hide WHICH edge was breached,
						// which is the thing worth seeing.
						drawBandEdge(nearEdgeCentre, bandErr > 0.0f ? FColor::Red : FColor::Green);
						drawBandEdge(farEdgeCentre,  bandErr < 0.0f ? FColor::Red : FColor::Green);

						if (SurfDebug::IsFlagSet(TEXT("assist")))
						{
							UE_LOG(LogSurf, Warning,
								TEXT("AssistBandDraw actor=(%.0f,%.0f,%.0f) crest=(%.0f,%.0f,%.0f) d=%.0f ")
								TEXT("nearC=(%.0f,%.0f) farC=(%.0f,%.0f) drawZ=%.0f halfSpan=%.0f %s"),
								actorLocation.X, actorLocation.Y, actorLocation.Z,
								this->crestWorldPosition.X, this->crestWorldPosition.Y, this->crestWorldPosition.Z,
								this->signedDistanceToCrest,
								nearEdgeCentre.X, nearEdgeCentre.Y,
								farEdgeCentre.X, farEdgeCentre.Y,
								actorLocation.Z + kBandDrawZLift, kBandDrawHalfSpanCm,
								bInsideBand ? TEXT("INSIDE") : (bandErr > 0.0f ? TEXT("OUT-THE-BACK") : TEXT("FLATS")));
						}

						// Where the board actually is on that axis, and the live number. Short: this
						// is a tick on the band, not a flagpole.
						DrawDebugLine(GetWorld(),
							actorLocation,
							actorLocation + FVector(0, 0, 110.0f),
							bInsideBand ? FColor::Green : FColor::Red, false, kBandDrawSeconds, SDPG_World, 3.0f);
						DrawDebugString(GetWorld(), actorLocation + FVector(0, 0, 125.0f),
							FString::Printf(TEXT("d=%.0f  band=[-%.0f,-%.0f]  %s"),
								this->signedDistanceToCrest, bandFar, bandNear,
								bInsideBand ? TEXT("INSIDE (no assist)")
											: (bandErr > 0.0f ? TEXT("OUT THE BACK") : TEXT("IN THE FLATS"))),
							nullptr, bInsideBand ? FColor::Green : FColor::Red, kBandDrawSeconds, true);
						}
					}
				}

				// === WAVESCAN DIAGNOSTIC (gated on the "wavescan" flag; heavy — one-off investigations) ===
				// Cross-shore line dump of the wave DATA (surface height + velocity) through this actor's
				// position: offsets along backDir, - = shoreward (lip/landing side), + = toward the back.
				// Answers "what does the velocity grid hold around the crest/lip" — e.g. whether the lip
				// jet's fast water exists in neighbouring columns or is missing from the export entirely.
				// See specs/pitch-righting-and-redirect-escape.md follow-ons / lip-impact investigation.
				if (SurfDebug::IsFlagSet(TEXT("wavescan")))
				{
					const FQuat waveRot = this->waveVelocity->GetActorQuat();
					for (int32 s = -16; s <= 16; ++s)
					{
						const float off = s * 50.0f;
						const FVector p = actorLocation + backDir * off;
						const double h = std::get<0>(this->waveVelocity->waveHeightAndNormal(p, tileAdjustedFrame));
						const FVector v = waveRot.RotateVector(this->waveVelocity->calculateWaveVelocity(p, tileAdjustedFrame));
						UE_LOG(LogSurf, Display, TEXT("WAVESCAN [%s] wcFrame=%d off=%.0f pos=(%.0f,%.0f) h=%.3f v=(%.0f,%.0f,%.0f) |v|=%.0f"),
							*GetName(), tileAdjustedFrame, off, p.X, p.Y, h, v.X, v.Y, v.Z, v.Size());
					}
				}
			}
		}

		// === WAVE-FACE PITCH ALIGNMENT ===
		// Pitch analog of waveRelativeRollSin: how far the board's bottom is from parallel to the local
		// wave face, in the pitch plane (about board.left). 0 = following the face. Geometry-based (board
		// attitude vs the wave-data normal), so it holds while on the face regardless of the momentary
		// water-velocity direction (which is only briefly up-the-face). Drives a restoring torque so the
		// nose pitches up to follow a steepening face instead of driving the flat hull through it.
		// See specs/wave-face-pitch-alignment.md.
		{
			// NOTE: normalize the basis vectors first. this->forwards/left/up carry the SC actor's ~0.2
			// transform scale; using them raw deflates the result 5x (the recurring un-normalized-basis
			// bug — see barrel-glide-through-bug.md). waveRelativePitchSin must be a TRUE sine in [-1,1].
			const FVector leftUnit = this->left.GetSafeNormal();
			const FVector upUnit = this->up.GetSafeNormal();
			const FVector fwdUnit = this->forwards.GetSafeNormal();
			// Wave-tangent "forward": in the wave plane, equals board.forwards for a face-aligned board.
			const FVector waveTangentForward = FVector::CrossProduct(leftUnit, this->waveNormal).GetSafeNormal();
			this->waveRelativePitchCos = FVector::VectorPlaneProject(fwdUnit, leftUnit).GetSafeNormal() | waveTangentForward;
			this->waveRelativePitchSin = FVector::VectorPlaneProject(-upUnit, leftUnit) | waveTangentForward;

			{
				// Deadzone ramp, mirroring slope thrust: 0 below the threshold, smoothly to full above it.
				const float slopeGate = FMath::SmoothStep(
					this->pitchAlignMinSlopeSin, this->pitchAlignMinSlopeSin + 0.06f, this->boardWideSlopeSin);
				// Two SC actors each apply half so the board-wide torque isn't double-counted.
				const float share = this->otherHalfSharedCalculations ? 0.5f : 1.0f;
				// Restoring torque about board.left to drive waveRelativePitchSin -> 0. Applied as an
				// angular acceleration (bAccelChange) so the coefficient is inertia-independent. Sign set
				// so a board pitched into the face rotates nose-up; verify empirically (see spec).
				const FVector pitchAlignTorque = leftUnit *
					(-this->pitchAlignCoefficient * (float)this->waveRelativePitchSin * slopeGate * share);
				// "nopitchalign" debug flag gates only the torque (keeps the diagnostic log) so the
				// pitch-align's contribution can be isolated against the raw buoyancy differential.
				const bool pitchActive = (this->pitchAlignCoefficient > 0.0f && BasePrimComp
					&& this->boardWideSlopeSin > this->pitchAlignMinSlopeSin
					&& !SurfDebug::IsFlagSet(TEXT("nopitchalign"))
					&& !SurfRails::AreForcesSuppressed());
				if (pitchActive)
				{
					BasePrimComp->AddTorqueInRadians(pitchAlignTorque, NAME_None, /*bAccelChange=*/true);
				}

				if (SurfDebug::IsFlagSet(TEXT("pitch")))
				{
					UE_LOG(LogSurf, Warning, TEXT("PitchAlign [%s] active=%d - waveRelPitchSin: %.3f cos: %.3f slopeSin: %.3f slopeGate: %.3f share: %.2f -> torque(accel): (%.2f, %.2f, %.2f)"),
						*GetName(), pitchActive ? 1 : 0, this->waveRelativePitchSin, this->waveRelativePitchCos,
						this->boardWideSlopeSin, slopeGate, share,
						pitchAlignTorque.X, pitchAlignTorque.Y, pitchAlignTorque.Z);
				}
			}
		}

		// === FOAM DIAGNOSTIC (gated on the "foam" flag) ===
		// The broken-water signal's inputs (UpdateBrokenAmount, specs/broken-wave-no-consequences.md
		// A4/M11-M13): the all-round count, the ahead-of-motion count, the thinnest of the four
		// sectors, both results (score read and surround read), the total cached points (device
		// budget check) and the nearest point's distance.
		if (SurfDebug::IsFlagSet(TEXT("foam")))
		{
			if (const AParticleSystemsController* Foam = FoamController.Get())
			{
				const FVector Here = GetActorLocation();
				double NearestSq = 1e18;
				for (const FVector& P : Foam->GetWhiteWaterWorldPoints())
				{
					const double dx = P.X - Here.X, dy = P.Y - Here.Y;
					NearestSq = FMath::Min(NearestSq, dx * dx + dy * dy);
				}
				UE_LOG(LogSurf, Warning, TEXT("FOAM [%s] geo=%s brokenGeo=%.2f behind=%.0f | total=%d dense=%d (r=%.0f) ahead=%d (r=%.0f) sectors=%d/%d/%d/%d near=%d/%d/%d/%d thinnest=%d brokenFoam=%.2f surround=%.2f nearest=%.0f"),
					*GetName(), UWaveGeometrySubsystem::ZoneName(this->waveZone), this->brokenGeo, this->distBehindImpact,
					Foam->GetWhiteWaterWorldPoints().Num(), this->foamPointsNearby,
					Tuning ? Tuning->FoamBrokenRadius : 0.0f, this->foamPointsAhead,
					Tuning ? Tuning->FoamAheadRadius : 0.0f,
					this->foamSectorCounts[0], this->foamSectorCounts[1], this->foamSectorCounts[2], this->foamSectorCounts[3],
					this->foamSectorCountsNear[0], this->foamSectorCountsNear[1], this->foamSectorCountsNear[2], this->foamSectorCountsNear[3],
					this->foamPointsThinnestSector,
					this->brokenAmount, this->brokenSurroundAmount,
					NearestSq < 1e17 ? FMath::Sqrt(NearestSq) : -1.0);
			}
		}

		// === CROSSING DIAGNOSTIC (gated on the "crossing" flag) ===
		// Per-tick board velocity & position projected onto the LOCAL wave normal (across-face) and the
		// crest tangent (down-line), so "crossing the face vs riding down the line" is unambiguous. The
		// tick-to-tick change of vCrossFace IS the net across-face force (Newton: captures buoyancy,
		// gravity, damping, everything), sidestepping the impossible per-force sum. Front and back SC log
		// their own waveNormal, so the crest-straddle (normal flips front/back) is visible directly.
		if (SurfDebug::IsFlagSet(TEXT("crossing")) && BasePrimComp)
		{
			const FVector vWorld = BasePrimComp->GetPhysicsLinearVelocity();
			const FVector nHoriz = FVector(this->waveNormal.X, this->waveNormal.Y, 0.0f).GetSafeNormal();
			const FVector tHoriz = FVector(-nHoriz.Y, nHoriz.X, 0.0f); // crest tangent (perp to normal, horizontal)
			const float vCrossFace = FVector::DotProduct(vWorld, nHoriz); // + = along the outward wave normal
			const float vDownLine  = FVector::DotProduct(vWorld, tHoriz);
			const FVector pos = BasePrimComp->GetComponentLocation();
			AActor* WC = FindWaterController(GetWorld());
			const float wcSecs = SC_ReadWaterControllerSeconds(WC);
			const int32 wcFrame = SC_ReadWaterControllerFrame(WC);
			// THROUGH vs OVER the crest: is the board below the wave surface (submerged, cutting through the
			// wave body) or above it (clearing the lip / over the top)? submersion = wave surface height at
			// the board minus the board's Z, SIGNED and same-frame (both from calculateWaveLocationAndNormal):
			// + => water above the board (THROUGH); - => board pokes above the surface (OVER). underW =
			// amountUnderWater (0=above surface .. 1=deeply submerged). (crestWorldPosition.Z is not used
			// here: it and waveHeightAndNormal are in a different vertical frame than the board's world Z.)
			const auto surfLN = this->waveVelocity->calculateWaveLocationAndNormal(GetActorLocation(), tileAdjustedFrame);
			const float submersion = (float)(surfLN[0].Z - GetActorLocation().Z);
			const float underW     = this->amountUnderWater;
			// Nose orientation + into-wave motion decomposition (for penetrate-vs-glide analysis):
			//   fwdH  = board.forwards horizontal, NORMALIZED (this->forwards carries the ~0.2 basis scale).
			//   bVel  = board world velocity horizontal (X,Y).
			//   wVel  = absolute water velocity horizontal (X,Y) — the wave's own water motion.
			// Project fwdH/bVel/wVel offline onto the stable cross-shore axis (into-wave) vs the crest
			// tangent (down-line) to answer "nose into the wave vs down the line" and "board penetrating
			// vs wave gliding over the board".
			const FVector fwdH = FVector(this->forwards.X, this->forwards.Y, 0.0f).GetSafeNormal();
			// Independent world-anchored crest position (cm along the cross-shore back axis): a fine, wide
			// GLOBAL-peak scan of the wave surface (not the board-relative hill-climb), so d(crestCross)/dt
			// gives the crest's phase velocity independent of the board's own motion. Debug-only cost.
			float crestCross = 0.0f;
			{
				const FVector bd = this->resolvedWaveBackDirection.IsZero()
					? this->waveBackDirection.GetSafeNormal() : this->resolvedWaveBackDirection;
				if (!bd.IsNearlyZero())
				{
					double bestH = -1.0e30; float bestOff = 0.0f;
					for (int32 i = -48; i <= 48; ++i) // +/-1200 cm at 25 cm steps
					{
						const float off = i * 25.0f;
						const double h = std::get<0>(this->waveVelocity->waveHeightAndNormal(
							GetActorLocation() + bd * off, tileAdjustedFrame));
						if (h > bestH) { bestH = h; bestOff = off; }
					}
					crestCross = (float)FVector::DotProduct(GetActorLocation() + bd * bestOff, bd);
				}
			}
			const FVector wV = this->absoluteWaterVelocity; // wave's water velocity (world), to confirm flowDir
			// The wave-geometry service's read of this point (specs/wave-geometry.md): zone, cm behind
			// the impact point (+ = up the line of it), cm cross-shore from it (+ = back), Broken.
			FString Geo = TEXT("geo=none");
			if (UWaveGeometrySubsystem* G = GetWorld() ? GetWorld()->GetSubsystem<UWaveGeometrySubsystem>() : nullptr)
			{
				const FWaveGeoSample Smp = G->Sample(pos, wcFrame);
				if (Smp.bValid)
				{
					Geo = FString::Printf(TEXT("geo=%s behind=%.0f cross=%.0f broken=%.2f"),
						UWaveGeometrySubsystem::ZoneName(Smp.Zone), Smp.DistBehindImpact, Smp.CrossFromImpact, Smp.Broken);
				}
			}
			UE_LOG(LogSurf, Warning, TEXT("CROSSING [%s] tileFrame=%d wcSecs=%.2f wcFrame=%d pos=(%.0f, %.0f, %.0f) distToCrest=%.0f (>0=behind) submersion=%.0f underW=%.2f waveN=(%.3f, %.3f) fwdH=(%.3f, %.3f) bVel=(%.0f, %.0f) wVel=(%.0f, %.0f) crestCross=%.0f vCrossFace=%.1f vDownLine=%.1f |v|=%.1f planing=%.2f slopeSin=%.3f %s"),
				*GetName(), tileAdjustedFrame, wcSecs, wcFrame, pos.X, pos.Y, pos.Z, this->signedDistanceToCrest, submersion, underW, nHoriz.X, nHoriz.Y,
				fwdH.X, fwdH.Y, vWorld.X, vWorld.Y, wV.X, wV.Y, crestCross,
				vCrossFace, vDownLine, vWorld.Size(), this->AmountPlaning, this->boardWideSlopeSin, *Geo);
		}
	}

	// surf.debug.flags=wavesurface: draw the PHYSICS' water surface so it can be compared, in a
	// screenshot, with the RENDERED wave mesh. Spheres along the cross-shore axis through the board
	// sit on calculateWaveLocationAndNormal's surface (world Z) — green shoreward of the board,
	// blue seaward, red at the physics' crest — and a red line marks that crest running down the
	// line. Drawn in the foreground depth group so the lowered render mesh cannot hide them; where
	// the red crest line lands relative to the visible lip IS the mesh-vs-data cross-shore
	// registration. One SC actor draws (the one that computes signedDistanceToCrest is enough).
	if (this->waveVelocity && SurfDebug::IsFlagSet(TEXT("wavesurface"))
		&& (this->otherHalfSharedCalculations == nullptr || (void*)this < (void*)this->otherHalfSharedCalculations))
	{
		const FVector bd = this->resolvedWaveBackDirection.IsZero()
			? this->waveBackDirection.GetSafeNormal() : this->resolvedWaveBackDirection;
		if (!bd.IsNearlyZero())
		{
			const FVector along = FVector(-bd.Y, bd.X, 0.0f); // down-the-line (crest tangent)
			const FVector here = GetActorLocation();
			UWorld* W = GetWorld();
			for (int32 off = -1200; off <= 1800; off += 50)
			{
				const FVector p = here + bd * (float)off;
				const FVector surf = this->waveVelocity->calculateWaveLocationAndNormal(p, this->lastTileAdjustedFrame)[0];
				const bool bCrest = FMath::Abs((float)off + this->signedDistanceToCrest) < 1.0f;
				const FColor c = bCrest ? FColor::Red : (off < 0 ? FColor::Green : (off == 0 ? FColor::Yellow : FColor::Cyan));
				DrawDebugSphere(W, surf, bCrest ? 25.0f : (off % 300 == 0 ? 14.0f : 7.0f), 8, c, false, -1.0f, SDPG_Foreground, 1.0f);
			}
			// Crest line down the line, on the surface, +/-1500 cm from the board's cross-shore station.
			FVector prev = FVector::ZeroVector; bool bHavePrev = false;
			for (int32 s = -1500; s <= 1500; s += 100)
			{
				const FVector p = this->crestWorldPosition + along * (float)s;
				const FVector surf = this->waveVelocity->calculateWaveLocationAndNormal(p, this->lastTileAdjustedFrame)[0];
				if (bHavePrev) DrawDebugLine(W, prev, surf, FColor::Red, false, -1.0f, SDPG_Foreground, 4.0f);
				prev = surf; bHavePrev = true;
			}
			DrawDebugString(W, this->crestWorldPosition + FVector(0, 0, 120.0f),
				FString::Printf(TEXT("PHYSICS CREST  d=%.0f cm"), this->signedDistanceToCrest), nullptr, FColor::Red, 0.0f, true);
		}
	}

	// Lateral weight-shift intent signal — magnitude of deviation from centered (0.5).
	// Range: 0 (centered) to 0.5 (fully shifted to one rail). Read by the bottom-hydrofoil
	// turn-gate in AFluidDynamics::calcThrustForce.
	if (this->weightDistribution)
	{
		this->lateralShift = FMath::Abs(this->weightDistribution->amountToTheRight - 0.5f);
	}
	else
	{
		this->lateralShift = 0.0f;
	}

	++debugFrameCounter;
	if ((debugStateLog || SurfDebug::ShouldDebug(this, TEXT("state"))) && (debugLogEveryNthTick <= 1 || debugFrameCounter % debugLogEveryNthTick == 0))
	{
		UE_LOG(LogSurf, Warning, TEXT("STATE [%s] componentVelMag: %.2f, relWaterVelMag: %.2f, AmountPlaning: %.3f, amountUnderWater: %.3f, waveRollSin: %.3f, waveRollCos: %.3f, worldRollSin: %.3f, slopeSin: %.3f"),
			*GetName(), this->componentVelocityMagnitude, this->relativeWaterVelocityMagnitude,
			this->AmountPlaning, this->amountUnderWater,
			this->waveRelativeRollSin, this->waveRelativeRollCos, this->worldRelativeRollSin,
			this->waveSlopeDownVec.Size());
		UE_LOG(LogSurf, Warning, TEXT("  WAVE NORMAL: (%.3f, %.3f, %.3f), waveSlopeDownVec: (%.3f, %.3f, %.3f)"),
			this->waveNormal.X, this->waveNormal.Y, this->waveNormal.Z,
			this->waveSlopeDownVec.X, this->waveSlopeDownVec.Y, this->waveSlopeDownVec.Z);
		UE_LOG(LogSurf, Warning, TEXT("  board.forwards: (%.3f, %.3f, %.3f), board.left: (%.3f, %.3f, %.3f), board.up: (%.3f, %.3f, %.3f)"),
			this->forwards.X, this->forwards.Y, this->forwards.Z,
			this->left.X, this->left.Y, this->left.Z,
			this->up.X, this->up.Y, this->up.Z);
		// Board-frame decomposition of relativeWaterVelocity — answers "is the glide-through
		// slip horizontal-sideways or vertical?" See specs/barrel-glide-through-bug.md.
		// Axes carry the actor's transform scale, so normalize before projecting.
		const FVector relWV = this->relativeWaterVelocity;
		const float relFwd  = FVector::DotProduct(relWV, this->forwards.GetSafeNormal());
		const float relLeft = FVector::DotProduct(relWV, this->left.GetSafeNormal());
		const float relUp   = FVector::DotProduct(relWV, this->up.GetSafeNormal());
		UE_LOG(LogSurf, Warning, TEXT("  relWaterVel BOARD-FRAME: alongFwd=%.1f alongLeft=%.1f alongUp=%.1f | world=(%.1f, %.1f, %.1f) mag=%.1f | horizMag=%.1f vertMag=%.1f"),
			relFwd, relLeft, relUp,
			relWV.X, relWV.Y, relWV.Z, relWV.Size(),
			FVector(relWV.X, relWV.Y, 0.0f).Size(), FMath::Abs(relWV.Z));
	}
}

/**
 * If the board were to follow the water surface perfectly, I would be able to use amount under water to determine if the
 * board is planing. But it isn't doing this, because of performance limitations. Therefor the amount under water varies too much to
 * use for planing, and I use threshold values of Z forces to measure planing instead.
 */
void ASharedCalculations::calculateAmountPlaning(FVector _relativeWaterVelocityLocalSpace){

	float lastTickAmountPlaning = this->AmountPlaning;
	float newAmountPlaning;

	// Planing is gated on the board's absolute world-frame speed. The earlier
	// attempt to use relative water velocity failed because the local water
	// velocity at the board's position oscillates wildly with wave orbital
	// motion (-2900 to +1000 cm/s during paddle slowly under VelocityScale=1000),
	// so it doesn't track "board faster than wave" usefully. Board speed is
	// stable and matches the user's intent: low during paddle, higher at pop-up,
	// highest while surfing.
	float boardSpeed = this->componentVelocityMagnitude;

	if (this->debugPlaning || SurfDebug::ShouldDebug(this, TEXT("planing")))
	{
#if WITH_EDITOR
		UE_LOG(LogSurf, Warning, TEXT("=== PLANING DEBUG Frame [%s] ==="), *GetActorLabel());
#else
		UE_LOG(LogSurf, Warning, TEXT("=== PLANING DEBUG Frame ==="));
#endif
		UE_LOG(LogSurf, Warning, TEXT("  boardSpeed: %.2f"), boardSpeed);
		UE_LOG(LogSurf, Warning, TEXT("  PlaningStartsVelocity: %.2f, PlaningStopsVelocity: %.2f, PlaningFullVelocity: %.2f"),
			this->PlaningStartsVelocity, this->PlaningStopsVelocity, this->PlaningFullVelocity);
	}

	if (boardSpeed < this->PlaningStartsVelocity)
	{
		newAmountPlaning = 0;
		if (this->debugPlaning || SurfDebug::ShouldDebug(this, TEXT("planing")))
		{
			UE_LOG(LogSurf, Warning, TEXT("  Decision: boardSpeed < PlaningStartsVelocity -> newAmountPlaning = 0"));
		}
	}
	else if (boardSpeed >= this->PlaningFullVelocity)
	{
		newAmountPlaning = this->MaxPlaning;
	}
	else
	{
		newAmountPlaning = FMath::Clamp(
			(boardSpeed - this->PlaningStartsVelocity) / (this->PlaningFullVelocity - this->PlaningStartsVelocity),
			0.0f, this->MaxPlaning);
	}

	if (newAmountPlaning < lastTickAmountPlaning)
	{
		// Hysteresis: once planing, the board keeps planing until board speed drops below PlaningStopsVelocity
		if (boardSpeed > this->PlaningStopsVelocity)
		{
			newAmountPlaning = lastTickAmountPlaning;
		}
		else
		{
			newAmountPlaning = FMath::Clamp(
				(boardSpeed - this->PlaningStopsVelocity) / (this->PlaningFullVelocity - this->PlaningStopsVelocity),
				0.0f, this->MaxPlaning);
		}
		newAmountPlaning = FMath::Clamp(newAmountPlaning, 0.0f, lastTickAmountPlaning);

		// Temporal decay: planing can't drop faster than maxDecayThisFrame
		float deltaTime = GetWorld()->GetDeltaSeconds();
		float maxDecayPerSecond = this->MaxPlaning / FMath::Max(this->PlaningDecayTime, 0.1f);
		float maxDecayThisFrame = maxDecayPerSecond * deltaTime;
		float actualDecay = lastTickAmountPlaning - newAmountPlaning;
		if (actualDecay > maxDecayThisFrame)
		{
			newAmountPlaning = lastTickAmountPlaning - maxDecayThisFrame;
			if (this->debugPlaning || SurfDebug::ShouldDebug(this, TEXT("planing")))
			{
				UE_LOG(LogSurf, Warning, TEXT("  Temporal decay applied: limited decay from %.3f to %.3f per frame"),
					actualDecay, maxDecayThisFrame);
			}
		}
	}

	if (this->debugPlaning || SurfDebug::ShouldDebug(this, TEXT("planing")))
	{
		UE_LOG(LogSurf, Warning, TEXT("  Final AmountPlaning: %.3f (was %.3f)"), newAmountPlaning, lastTickAmountPlaning);
	}

	this->AmountPlaning = newAmountPlaning;
}

void ASharedCalculations::AddPlaningForce(FVector force){
	this->PlaningForces.Add(force);
}

FVector ASharedCalculations::calcRelativeWaterVelocity(UPrimitiveComponent *BasePrimComp, FVector aboluteWaterVelocity)
{

	this->componentVelocity = calcComponentVelocity(BasePrimComp);
	this->componentVelocityMagnitude = this->componentVelocity.Size();

	FVector relativeVelocity = absoluteWaterVelocity - componentVelocity;

	// Debug log for relative water velocity calculation
	if (this->waveVelocity && this->waveVelocity->bEnableVelocityLogging)
	{
		UE_LOG(LogSurf, Warning, TEXT("calcRelativeWaterVelocity - AbsoluteWaterVel=(%.2f,%.2f,%.2f), ComponentVel=(%.2f,%.2f,%.2f), Relative=(%.2f,%.2f,%.2f)"),
			absoluteWaterVelocity.X, absoluteWaterVelocity.Y, absoluteWaterVelocity.Z,
			componentVelocity.X, componentVelocity.Y, componentVelocity.Z,
			relativeVelocity.X, relativeVelocity.Y, relativeVelocity.Z);
	}

	if (this->debugPlaning || SurfDebug::ShouldDebug(this, TEXT("planing")))
	{
#if WITH_EDITOR
		UE_LOG(LogSurf, Warning, TEXT("[%s] frame=%d WaterVel=(%.1f,%.1f,%.1f) mag=%.1f, CompVel=(%.1f,%.1f,%.1f) mag=%.1f, RelVel=(%.1f,%.1f,%.1f) mag=%.1f"),
			*GetActorLabel(), this->lastTileAdjustedFrame,
			absoluteWaterVelocity.X, absoluteWaterVelocity.Y, absoluteWaterVelocity.Z, absoluteWaterVelocity.Size(),
			componentVelocity.X, componentVelocity.Y, componentVelocity.Z, componentVelocity.Size(),
			relativeVelocity.X, relativeVelocity.Y, relativeVelocity.Z, relativeVelocity.Size());
#endif
	}

	return relativeVelocity;
}

FVector ASharedCalculations::calcComponentVelocity(UPrimitiveComponent* BasePrimComp)
{
    if (!BasePrimComp)
    {
        UE_LOG(LogSurf, Error, TEXT("BasePrimComp is nullptr"));
        return FVector::ZeroVector;
    }

    // A rails intro drives the board kinematically, and a kinematic body still carries a real
    // velocity (Chaos derives V/W from the kinematic target). Returning zero here would tell every
    // force computation downstream that the board is stationary for the whole intro — which is
    // exactly the failure the kinematic-target drive exists to prevent, and it would be silent
    // apart from this log line. See specs/deterministic-ride-handoff.md.
    if (!BasePrimComp->IsSimulatingPhysics() && !SurfRails::IsKinematicallyDriven())
    {
        UE_LOG(LogSurf, Error, TEXT("BasePrimComp is not simulating physics"));
        return FVector::ZeroVector;
    }

	// Use velocity at the mesh center to avoid angular velocity feedback loop.
	// Previously used GetActorLocation() (SharedCalculations position at nose/tail),
	// which included large angular velocity contributions when the board pitches,
	// causing a positive feedback loop where nose dips amplified thrust forces.
	// Not using GetCenterOfMass() because COM is intentionally offset to the tail.
	FVector meshCenter = BasePrimComp->GetComponentLocation();
	FVector comLocation = BasePrimComp->GetCenterOfMass();
	FVector actorLocation = GetActorLocation();
	if (this->debugPlaning || SurfDebug::ShouldDebug(this, TEXT("planing")))
	{
#if WITH_EDITOR
		UE_LOG(LogSurf, Warning, TEXT("[%s] meshCenter=(%.1f,%.1f,%.1f), COM=(%.1f,%.1f,%.1f), actorLoc=(%.1f,%.1f,%.1f)"),
			*GetActorLabel(),
			meshCenter.X, meshCenter.Y, meshCenter.Z,
			comLocation.X, comLocation.Y, comLocation.Z,
			actorLocation.X, actorLocation.Y, actorLocation.Z);
#endif
	}
	return BasePrimComp->GetPhysicsLinearVelocityAtPoint(meshCenter);
}
