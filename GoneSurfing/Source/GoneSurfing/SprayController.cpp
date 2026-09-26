// Fill out your copyright notice in the Description page of Project Settings.

#include "SprayController.h"
#include "SurfLog.h"
#include "EngineUtils.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "SurfDebug.h"
#include "WaveHeight.h"

namespace
{
	const FName SprayPosALeftParam(TEXT("SprayPosALeft"));
	const FName SprayPosBLeftParam(TEXT("SprayPosBLeft"));
	const FName SprayVelLeftParam(TEXT("SprayVelLeft"));
	const FName SpawnRateLeftParam(TEXT("SpawnRateLeft"));
	const FName SprayPosARightParam(TEXT("SprayPosARight"));
	const FName SprayPosBRightParam(TEXT("SprayPosBRight"));
	const FName SprayVelRightParam(TEXT("SprayVelRight"));
	const FName SpawnRateRightParam(TEXT("SpawnRateRight"));
	const FName SprayPosATailParam(TEXT("SprayPosATail"));
	const FName SprayPosBTailParam(TEXT("SprayPosBTail"));
	const FName SprayVelTailParam(TEXT("SprayVelTail"));
	const FName SpawnRateTailParam(TEXT("SpawnRateTail"));
	const FName WaterPlanePosParam(TEXT("WaterPlanePos"));
	const FName WaterPlaneNormalParam(TEXT("WaterPlaneNormal"));
	const FName WaterVelocityParam(TEXT("WaterVelocity"));
	const FName WaterHeightGridParam(TEXT("WaterHeightGrid"));
	const FName HeightGridOriginParam(TEXT("HeightGridOrigin"));
	const FName HeightGridSpacingParam(TEXT("HeightGridSpacing"));
	const FName HeightGridSizeParam(TEXT("HeightGridSize"));
}

ASprayController::ASprayController()
{
	PrimaryActorTick.bCanEverTick = true;
	// Read the force accumulators only after every FluidDynamics actor's pre-physics tick has
	// finished applying its impulses, so a site never sees a half-summed frame.
	PrimaryActorTick.TickGroup = TG_PostPhysics;
}

bool ASprayController::InitializeIfNeeded()
{
	if (this->bInitialized)
	{
		return true;
	}

	// Site membership: manual arrays win; otherwise classify this board's actors by ESide.
	this->LeftSite.Actors.Empty();
	this->RightSite.Actors.Empty();
	this->TailSite.Actors.Empty();
	for (AFluidDynamics* fd : this->LeftRailActors)  { if (fd) this->LeftSite.Actors.Add(fd); }
	for (AFluidDynamics* fd : this->RightRailActors) { if (fd) this->RightSite.Actors.Add(fd); }
	for (AFluidDynamics* fd : this->TailActors)      { if (fd) this->TailSite.Actors.Add(fd); }

	const bool anyManual = this->LeftSite.Actors.Num() + this->RightSite.Actors.Num() +
	                       this->TailSite.Actors.Num() > 0;
	if (!anyManual)
	{
		if (!this->Surfboard)
		{
			if (!this->bInitFailedWarned)
			{
				UE_LOG(LogSurf, Warning,
					TEXT("SprayController %s: no Surfboard set and no manual site actors — spray disabled."),
					*GetName());
				this->bInitFailedWarned = true;
			}
			return false;
		}
		for (TActorIterator<AFluidDynamics> it(GetWorld()); it; ++it)
		{
			AFluidDynamics* fd = *it;
			if (!fd || fd->Surfboard != this->Surfboard)
			{
				continue;
			}
			switch (fd->side)
			{
			case ESide::VE_Left:  this->LeftSite.Actors.Add(fd);  break;
			case ESide::VE_Right: this->RightSite.Actors.Add(fd); break;
			case ESide::VE_Tail:
			case ESide::VE_Fin:   this->TailSite.Actors.Add(fd);  break;
			case ESide::VE_Down:
			{
				// Bottom actors carry no left/right in their ESide; the label does
				// (e.g. "bottom_left_middle_..."), same convention SurfDebug matches on.
#if WITH_EDITOR
				const FString label = fd->GetActorLabel().ToLower();
				if (label.Contains(TEXT("left")))       { this->LeftSite.Actors.Add(fd); }
				else if (label.Contains(TEXT("right"))) { this->RightSite.Actors.Add(fd); }
#endif
				break;
			}
			default: break; // VE_Nose, VE_Unset: no spray site
			}
		}
	}

	// FluidDynamics BPs assign `side` in BeginPlay; VE_Unset here means we ran too early — retry.
	const int32 totalActors = this->LeftSite.Actors.Num() + this->RightSite.Actors.Num() +
	                          this->TailSite.Actors.Num();
	if (totalActors == 0)
	{
		return false;
	}

	AFluidDynamics* firstActor =
		this->LeftSite.Actors.Num() > 0 ? this->LeftSite.Actors[0] :
		this->RightSite.Actors.Num() > 0 ? this->RightSite.Actors[0] : this->TailSite.Actors[0];

	if (!this->sharedCalculations)
	{
		this->sharedCalculations = firstActor->sharedCalculations;
	}

	// The mesh the FluidDynamics actors push on is what the spray component rides on.
	UPrimitiveComponent* mesh = firstActor->surfboardMesh;
	if (!mesh || !this->sharedCalculations)
	{
		return false; // BP BeginPlay hasn't wired these yet — retry next tick
	}

	if (this->SpraySystem && !this->SprayComponent)
	{
		this->SprayComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			this->SpraySystem, mesh, NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
			EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
	}
	if (!this->SprayComponent)
	{
		if (!this->bInitFailedWarned)
		{
			UE_LOG(LogSurf, Warning,
				TEXT("SprayController %s: no SpraySystem asset assigned — spray disabled."), *GetName());
			this->bInitFailedWarned = true;
		}
		return false;
	}

	UE_LOG(LogSurf, Log, TEXT("SprayController %s: initialized (left=%d right=%d tail=%d actors)"),
		*GetName(), this->LeftSite.Actors.Num(), this->RightSite.Actors.Num(),
		this->TailSite.Actors.Num());
	this->bInitialized = true;
	return true;
}

bool ASprayController::SampleSurface(const FVector& pos, FVector& outLoc, FVector& outNormal) const
{
	if (!this->sharedCalculations || !this->sharedCalculations->waveVelocity)
	{
		return false;
	}
	const TArray<FVector> sample =
		this->sharedCalculations->waveVelocity->calculateWaveLocationAndNormalAuto(pos);
	if (sample.Num() < 2 || sample[1].IsNearlyZero())
	{
		return false;
	}
	outLoc = sample[0];
	outNormal = sample[1].GetSafeNormal();
	return true;
}

void ASprayController::ComputeSite(FSpraySite& site, float speedGate, float horizBoardSpeed,
                                   const FVector& outboardAxis, bool signFromCenterline) const
{
	site.Velocity = FVector::ZeroVector;
	site.SpawnRate = 0.0f;
	if (site.Actors.Num() == 0)
	{
		site.PositionA = site.PositionB = FVector::ZeroVector;
		return;
	}

	FVector forceSum = FVector::ZeroVector;
	float wettedSum = 0.0f;
	for (const AFluidDynamics* fd : site.Actors)
	{
		forceSum += fd->getSprayDriveForce();
		wettedSum += fd->actorWetted;
	}
	const float wettedMean = wettedSum / site.Actors.Num();

	// Emission line = the furthest-apart pair of site actors (the rail's front/back extremes),
	// so emitters can scatter spawns along the whole rail instead of one mean point.
	int32 bestI = 0, bestJ = 0;
	float bestDistSq = -1.0f;
	for (int32 i = 0; i < site.Actors.Num(); ++i)
	{
		for (int32 j = i; j < site.Actors.Num(); ++j)
		{
			const float distSq = FVector::DistSquared(site.Actors[i]->GetActorLocation(),
			                                          site.Actors[j]->GetActorLocation());
			if (distSq > bestDistSq)
			{
				bestDistSq = distSq;
				bestI = i;
				bestJ = j;
			}
		}
	}
	site.PositionA = site.Actors[bestI]->GetActorLocation();
	site.PositionB = site.Actors[bestJ]->GetActorLocation();

	// The spray sheet leaves the water AT the (render-calibrated) waterline, so pin each endpoint's
	// Z DIRECTLY to the surface sampled at ITS OWN (X,Y) + bias + clearance — per-endpoint because
	// one midpoint plane put spawns high/low on curved faces while pitching/turning. Direct set,
	// not Max-with-actor-Z: the old floor stuck spawns at the deck-edge rail actors once the bias
	// pulled the target below them (bias values under ~-40 had no effect, observed). A dry or
	// airborne rail nominally "spawns" at the waterline below it, but actorWetted gates its rate
	// to ~0. The clearance is against the SAME biased surface the settle module uses, so spawns
	// stay just above it at any bias — no instant-settle.
	FVector midpoint = 0.5f * (site.PositionA + site.PositionB);

	// Push the emission line outward from the centerline: the bottom actors sit inboard of the
	// rail edge and drag the furthest-apart pair toward the middle, but the sheet leaves at the
	// rail edge. For rail sites the axis sign is resolved geometrically (which side of the SC
	// actor the site midpoint sits on), so both rails pass the same board.left axis.
	if (this->sprayOutboardOffset > 0.0f && !outboardAxis.IsNearlyZero())
	{
		FVector outboard = outboardAxis;
		if (signFromCenterline)
		{
			const float side = FVector::DotProduct(
				midpoint - this->sharedCalculations->GetActorLocation(), outboard);
			if (side < 0.0f)
			{
				outboard = -outboard;
			}
		}
		const FVector shift = this->sprayOutboardOffset * outboard;
		site.PositionA += shift;
		site.PositionB += shift;
		midpoint += shift;
	}

	site.RailZ = midpoint.Z;
	site.PlaneZ = midpoint.Z;
	// Spawn height: biased waterline + clearance, pushed DOWN by spraySpawnDepth so spray is born
	// at the hull bottom while the bias keeps settled foam on the rendered water. The settle
	// module's grace age covers the below-surface start.
	const float spawnZOffset =
		this->sprayWaterlineZBias + this->spraySurfaceOffset - this->spraySpawnDepth;
	FVector surfLoc, surfNorm;
	if (SampleSurface(site.PositionA, surfLoc, surfNorm))
	{
		site.PositionA.Z = (float)surfLoc.Z + spawnZOffset;
	}
	if (SampleSurface(site.PositionB, surfLoc, surfNorm))
	{
		site.PositionB.Z = (float)surfLoc.Z + spawnZOffset;
	}

	// Midpoint sample: normal for the ejection deflection + the debug railZ/planeZ pair
	// (planeZ reports the BIASED lift target so the log matches what spawns actually use).
	FVector planeNormal = FVector::UpVector;
	bool planeValid = false;
	if (SampleSurface(midpoint, surfLoc, surfNorm))
	{
		planeNormal = surfNorm;
		planeValid = true;
		site.PlaneZ = (float)surfLoc.Z + this->sprayWaterlineZBias;
	}

	const float forceMag = forceSum.Size();
	if (forceMag < KINDA_SMALL_NUMBER)
	{
		return;
	}

	// Newton 3: the water thrown off the surface carries the momentum the board gained — spray
	// ejects OPPOSITE the applied force. Magnitude ∝ force (∝ v² through the force formulas).
	// Only the SURFACE-PARALLEL part of the reaction becomes visible sheet — in spawn RATE as
	// well as ejection speed and direction. At takeoff the net force is ≈ pure upthrust (huge
	// total, ~nothing planar): full-force keying made the rate saturate the instant the speed
	// gate cracked open (vertical geyser of particles at low speed, observed at the wave catch);
	// planar keying keeps the catch quiet while leaving the at-speed fan untouched (drag/carve
	// forces are mostly planar, so planar ≈ total once the board is moving). A vertical push's
	// momentum goes into the deep, not the sheet.
	const FVector reaction = -forceSum;
	float sprayForceMag = forceMag;
	if (planeValid)
	{
		const float into = FVector::DotProduct(reaction, planeNormal);
		FVector planar = reaction - FMath::Min(into, 0.0f) * planeNormal;
		// Carving is distinguished by CROSS-BOARD flow: weight the sideways component of the
		// planar reaction above fore-aft (the straight-line wake) so hard turns out-spray
		// down-the-line riding by as much as the knob says.
		if (this->sprayLateralWeight != 1.0f)
		{
			const FVector fwdN = this->sharedCalculations->forwards.GetSafeNormal();
			if (!fwdN.IsNearlyZero())
			{
				const FVector foreAft = FVector::DotProduct(planar, fwdN) * fwdN;
				planar = foreAft + this->sprayLateralWeight * (planar - foreAft);
			}
		}
		sprayForceMag = planar.Size();
		const float ejectSpeed = FMath::Min(sprayForceMag * this->sprayForceToVelocity, this->sprayMaxEjectSpeed);
		const FVector dir = (planar.GetSafeNormal() + this->sprayUpwardTilt * planeNormal).GetSafeNormal();
		if (ejectSpeed > 1.0f && !dir.IsNearlyZero())
		{
			// Board-speed cap, PER COMPONENT: the wake (fore-aft) is sheared off along the track
			// and can't exceed ~board speed, but the carve fan is a REDIRECTED JET — a deflected
			// jet in world frame reaches up to ~2× the flow speed. Split matters because hard
			// carves BLEED board speed: the old whole-vector cap was tightest exactly at the
			// biggest-fan moment (observed: sideways too slow at the hard turn, wake fine).
			FVector vel = dir * ejectSpeed;
			const FVector fwdN = this->sharedCalculations->forwards.GetSafeNormal();
			if (!fwdN.IsNearlyZero())
			{
				const float foreAftMag = FVector::DotProduct(vel, fwdN);
				FVector foreAft = foreAftMag * fwdN;
				FVector cross = vel - foreAft;
				const float foreAftCap = this->sprayMaxEjectVsBoardSpeed * horizBoardSpeed;
				const float crossCap = this->sprayMaxEjectVsBoardSpeedSideways * horizBoardSpeed;
				if (FMath::Abs(foreAftMag) > foreAftCap)
				{
					foreAft *= foreAftCap / FMath::Abs(foreAftMag);
				}
				const float crossMag = cross.Size();
				if (crossMag > crossCap && crossMag > KINDA_SMALL_NUMBER)
				{
					cross *= crossCap / crossMag;
				}
				vel = foreAft + cross;
			}
			else
			{
				vel = vel.GetClampedToMaxSize(this->sprayMaxEjectVsBoardSpeed * horizBoardSpeed);
			}
			site.Velocity = vel;
		}
	}
	else
	{
		const float ejectSpeed = FMath::Min3(forceMag * this->sprayForceToVelocity,
			this->sprayMaxEjectSpeed, this->sprayMaxEjectVsBoardSpeed * horizBoardSpeed);
		site.Velocity = (reaction / forceMag) * ejectSpeed;
	}

	site.SpawnRate = this->sprayBaseRate *
		FMath::Clamp(sprayForceMag / this->sprayForceForFullRate, 0.0f, 1.0f) * wettedMean * speedGate;
}

void ASprayController::GetSprayOutputs(FVector& OutVelLeft, float& OutRateLeft,
                                       FVector& OutVelRight, float& OutRateRight,
                                       FVector& OutVelTail, float& OutRateTail) const
{
	OutVelLeft = this->LeftSite.Velocity;
	OutRateLeft = this->LeftSite.SpawnRate;
	OutVelRight = this->RightSite.Velocity;
	OutRateRight = this->RightSite.SpawnRate;
	OutVelTail = this->TailSite.Velocity;
	OutRateTail = this->TailSite.SpawnRate;
}

void ASprayController::SetReplaySpray(const FVector& VelLeft, float RateLeft,
                                      const FVector& VelRight, float RateRight,
                                      const FVector& VelTail, float RateTail)
{
	this->bReplaySprayOverride = true;
	this->ReplayVel[0] = VelLeft;
	this->ReplayRate[0] = RateLeft;
	this->ReplayVel[1] = VelRight;
	this->ReplayRate[1] = RateRight;
	this->ReplayVel[2] = VelTail;
	this->ReplayRate[2] = RateTail;
}

void ASprayController::ClearReplaySpray()
{
	this->bReplaySprayOverride = false;
	for (int32 i = 0; i < 3; ++i)
	{
		this->ReplayVel[i] = FVector::ZeroVector;
		this->ReplayRate[i] = 0.0f;
	}
}

void ASprayController::PushSiteToNiagara(const FSpraySite& site, const FName& posAParam,
                                         const FName& posBParam, const FName& velParam,
                                         const FName& rateParam) const
{
	this->SprayComponent->SetVariableVec3(posAParam, site.PositionA);
	this->SprayComponent->SetVariableVec3(posBParam, site.PositionB);
	this->SprayComponent->SetVariableVec3(velParam, site.Velocity);
	this->SprayComponent->SetVariableFloat(rateParam, site.SpawnRate);
}

void ASprayController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!InitializeIfNeeded())
	{
		return;
	}

	// Speed-ramp gate. NOT AmountPlaning: that is a thresholded switch (0 → 0.8 within a few
	// ticks of takeoff) and popped spray in at full blast. Smoothstep on HORIZONTAL board speed —
	// a board sitting on a rolling wave is carried vertically at 100-300 cm/s, which opened a
	// full-3D-speed gate while the board wasn't actually travelling.
	const float horizSpeed = this->sharedCalculations->componentVelocity.Size2D();
	float gate = 0.0f;
	if (this->bSprayEnabled && this->sprayFullGateSpeed > this->sprayMinSpeed)
	{
		const float t = FMath::Clamp((horizSpeed - this->sprayMinSpeed) /
			(this->sprayFullGateSpeed - this->sprayMinSpeed), 0.0f, 1.0f);
		gate = t * t * (3.0f - 2.0f * t);
	}
	const FVector leftAxis = this->sharedCalculations->left.GetSafeNormal();
	const FVector fwdAxis = this->sharedCalculations->forwards.GetSafeNormal();
	ComputeSite(this->LeftSite, gate, horizSpeed, leftAxis, /*signFromCenterline=*/true);
	ComputeSite(this->RightSite, gate, horizSpeed, leftAxis, /*signFromCenterline=*/true);
	ComputeSite(this->TailSite, gate, horizSpeed, -fwdAxis, /*signFromCenterline=*/false);

	// Kinematic replay: the force pipeline is tick-disabled, so ComputeSite just derived
	// velocity/rate from FROZEN accumulators — substitute the recorded values (emission
	// positions/waterline above stay live; they're deterministic under the re-synced wave clock).
	if (this->bReplaySprayOverride)
	{
		this->LeftSite.Velocity = this->ReplayVel[0];
		this->LeftSite.SpawnRate = this->ReplayRate[0];
		this->RightSite.Velocity = this->ReplayVel[1];
		this->RightSite.SpawnRate = this->ReplayRate[1];
		this->TailSite.Velocity = this->ReplayVel[2];
		this->TailSite.SpawnRate = this->ReplayRate[2];
	}

	// PSO warm-up. The spray material's graphics pipeline state compiles on its first DRAW, which
	// normally lands at takeoff/pop-up (the first frames the speed gate opens) and shows up as a
	// one-time hitch per app launch — the PSO is cached for the rest of the process, so an in-level
	// Restart is smooth but relaunching the app brings it back. Force a brief, low emission on the
	// first ride after launch so that compile happens here (early paddle phase) instead of at the
	// visible pop-up. Once per process (static guard — the PSO stays cached); skipped during replay
	// (which pushes its own recorded rates). A non-zero velocity sends particles ballistic then
	// settling, so BOTH the ballistic-spray and settled-foam renderers draw and compile.
	static bool bSprayPSOWarmedThisProcess = false;
	if (this->bSprayWarmup && !bSprayPSOWarmedThisProcess && !this->bReplaySprayOverride &&
	    this->warmupElapsed < this->sprayWarmupDuration)
	{
		FVector upN = this->sharedCalculations->up.GetSafeNormal();
		if (upN.IsNearlyZero()) { upN = FVector::UpVector; }
		const FVector warmVel = upN * 200.0f;
		auto warmSite = [&](FSpraySite& s)
		{
			if (s.Actors.Num() > 0)
			{
				s.SpawnRate = FMath::Max(s.SpawnRate, this->sprayWarmupRate);
				if (s.Velocity.IsNearlyZero()) { s.Velocity = warmVel; }
			}
		};
		warmSite(this->LeftSite);
		warmSite(this->RightSite);
		warmSite(this->TailSite);
		this->warmupElapsed += DeltaTime;
		if (this->warmupElapsed >= this->sprayWarmupDuration)
		{
			bSprayPSOWarmedThisProcess = true;
		}
	}

	// Hard Android budget: scale every site down uniformly so live particles stay ≤ sprayMaxParticles.
	const float totalRate = this->LeftSite.SpawnRate + this->RightSite.SpawnRate + this->TailSite.SpawnRate;
	const float maxTotalRate = (float)this->sprayMaxParticles / this->sprayAssumedLifetime;
	if (totalRate > maxTotalRate && totalRate > 0.0f)
	{
		const float scale = maxTotalRate / totalRate;
		this->LeftSite.SpawnRate *= scale;
		this->RightSite.SpawnRate *= scale;
		this->TailSite.SpawnRate *= scale;
	}

	PushSiteToNiagara(this->LeftSite, SprayPosALeftParam, SprayPosBLeftParam, SprayVelLeftParam, SpawnRateLeftParam);
	PushSiteToNiagara(this->RightSite, SprayPosARightParam, SprayPosBRightParam, SprayVelRightParam, SpawnRateRightParam);
	PushSiteToNiagara(this->TailSite, SprayPosATailParam, SprayPosBTailParam, SprayVelTailParam, SpawnRateTailParam);

	// Tier 1 plane (kept for compatibility while the Niagara module migrates) + water velocity.
	// Sampled at the SC actor, which demonstrably tracks the board.
	const FVector scPos = this->sharedCalculations->GetActorLocation();
	FVector surfLoc, surfNorm;
	if (SampleSurface(scPos, surfLoc, surfNorm))
	{
		surfLoc.Z += this->sprayWaterlineZBias;
		this->SprayComponent->SetVariableVec3(WaterPlanePosParam, surfLoc);
		this->SprayComponent->SetVariableVec3(WaterPlaneNormalParam, surfNorm);
	}
	this->SprayComponent->SetVariableVec3(WaterVelocityParam, this->sharedCalculations->absoluteWaterVelocity);

	// Tier 2: N×N surface heights on a world-XY grid around the board. The settle module
	// bilinearly samples the height under each particle, making settle a continuous clamp to the
	// TRUE local surface (foam rides the actual wave — no board-phase bobbing, no stranded foam,
	// no spawn-height error on curved faces). See specs/board-spray-particles.md (Tier 2).
	if (this->heightGridSize >= 2)
	{
		const int32 n = this->heightGridSize;
		const float halfSpan = 0.5f * (n - 1) * this->heightGridSpacing;
		const FVector origin(scPos.X - halfSpan, scPos.Y - halfSpan, 0.0f);
		// Cells with no wave data fall back to the board-center surface height.
		float fallbackZ = (float)scPos.Z;
		if (SampleSurface(scPos, surfLoc, surfNorm))
		{
			fallbackZ = (float)surfLoc.Z;
		}
		this->HeightGridScratch.SetNumUninitialized(n * n);
		for (int32 y = 0; y < n; ++y)
		{
			for (int32 x = 0; x < n; ++x)
			{
				const FVector cellPos(origin.X + x * this->heightGridSpacing,
				                      origin.Y + y * this->heightGridSpacing, scPos.Z);
				const float z = SampleSurface(cellPos, surfLoc, surfNorm) ? (float)surfLoc.Z : fallbackZ;
				this->HeightGridScratch[y * n + x] = z + this->sprayWaterlineZBias;
			}
		}
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayFloat(
			this->SprayComponent, WaterHeightGridParam, this->HeightGridScratch);
		this->SprayComponent->SetVariableVec3(HeightGridOriginParam, origin);
		this->SprayComponent->SetVariableFloat(HeightGridSpacingParam, this->heightGridSpacing);
		this->SprayComponent->SetVariableFloat(HeightGridSizeParam, (float)n);
	}

	++this->debugFrameCounter;
	if ((this->bDebugSpray || SurfDebug::ShouldDebug(this, TEXT("spray"))) &&
	    (this->debugFrameCounter % this->debugLogEveryNthTick == 0))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("SPRAY %s gate=%.2f speed=%.0f | L rate=%.0f vel=(%.0f,%.0f,%.0f) railZ=%.0f planeZ=%.0f | R rate=%.0f vel=(%.0f,%.0f,%.0f) railZ=%.0f planeZ=%.0f | T rate=%.0f vel=(%.0f,%.0f,%.0f) railZ=%.0f planeZ=%.0f"),
			*GetName(), gate, horizSpeed,
			this->LeftSite.SpawnRate, this->LeftSite.Velocity.X, this->LeftSite.Velocity.Y, this->LeftSite.Velocity.Z,
			this->LeftSite.RailZ, this->LeftSite.PlaneZ,
			this->RightSite.SpawnRate, this->RightSite.Velocity.X, this->RightSite.Velocity.Y, this->RightSite.Velocity.Z,
			this->RightSite.RailZ, this->RightSite.PlaneZ,
			this->TailSite.SpawnRate, this->TailSite.Velocity.X, this->TailSite.Velocity.Y, this->TailSite.Velocity.Z,
			this->TailSite.RailZ, this->TailSite.PlaneZ);
	}
}
