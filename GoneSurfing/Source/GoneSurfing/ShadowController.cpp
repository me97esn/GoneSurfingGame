// Fill out your copyright notice in the Description page of Project Settings.

#include "ShadowController.h"
#include "SurfLog.h"
#include "SurfDebug.h"
#include "WaveHeight.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "SurfTuningSubsystem.h"

static TAutoConsoleVariable<int32> CVarShadowEnable(
	TEXT("surf.shadow.enable"),
	1,
	TEXT("0 = build no shadow proxies and leave the board's own Cast Shadow alone. Read once at ")
	TEXT("init, so it must be set at launch. The A/B control for the whole feature."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarShadowDebugDraw(
	TEXT("surf.shadow.debugdraw"),
	0,
	TEXT("1 = draw the shadow contact plane, the lowest hull point and where it grounds to. ")
	TEXT("Independent of surf.debug.flags so a calibration screenshot needs one CVar, not three."),
	ECVF_Default);

AShadowController::AShadowController()
{
	PrimaryActorTick.bCanEverTick = true;
	// After physics, so the board transform the proxies copy is the final one for the frame — same
	// reason ASprayController ticks here.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	USceneComponent* root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	root->SetMobility(EComponentMobility::Movable);
	RootComponent = root;
}

// Pull live tuning onto this actor's UPROPERTY scratch BEFORE anything reads them, so the rest of
// Tick can keep saying this->X and transparently see tuned values. Warn-once if the subsystem is
// missing - the hand-edited UPROPERTYs are the fallback. Same shape as ASurfboardUtils.
void AShadowController::RefreshFromTuningSubsystem()
{
	if (!this->Tuning)
	{
		return;
	}
	this->shadowMode                   = this->Tuning->ShadowMode;
	this->shadowContactZBias           = this->Tuning->ShadowContactZBias;
	this->shadowMaxGroundingOffset     = this->Tuning->ShadowMaxGroundingOffset;
	this->shadowWaterlineZBias         = this->Tuning->ShadowWaterlineZBias;
	this->shadowOffsetSmoothingSeconds = this->Tuning->ShadowOffsetSmoothingSeconds;
}

void AShadowController::BeginPlay()
{
	Super::BeginPlay();

	this->Tuning = SurfTuning::Get(this);
	if (!this->Tuning)
	{
		UE_LOG(LogSurf, Warning,
			TEXT("AShadowController::BeginPlay: SurfTuningSubsystem unavailable; falling back to actor UPROPERTYs."));
	}
	RefreshFromTuningSubsystem();

	UE_LOG(LogSurf, Log, TEXT("ShadowController %s: present (enabled=%d, cap=%.1fcm, waterlineBias=%.1fcm)"),
		*GetName(), this->bShadowEnabled ? 1 : 0, this->shadowMaxGroundingOffset, this->shadowWaterlineZBias);
}

bool AShadowController::SampleSurface(const FVector& pos, FVector& outLoc, FVector& outNormal) const
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

void AShadowController::SetProxyVisible(UPrimitiveComponent* Proxy, bool bVisible)
{
	if (!Proxy)
	{
		return;
	}
	// Every lever, together. bHiddenInGame is a visibility flag the editor viewport ignores;
	// bRenderInMainPass is a rendering flag Nanite ignores. Neither one covers the whole surface on
	// its own, and bCastHiddenShadow keeps the shadow alive through the hidden path either way.
	Proxy->SetRenderInMainPass(bVisible);
	Proxy->SetRenderInDepthPass(bVisible);
	Proxy->SetHiddenInGame(!bVisible);
}

UPrimitiveComponent* AShadowController::CreateProxyFor(USceneComponent* Source)
{
	UPrimitiveComponent* proxy = nullptr;

	if (UStaticMeshComponent* srcStatic = Cast<UStaticMeshComponent>(Source))
	{
		UStaticMesh* asset = srcStatic->GetStaticMesh();
		if (!asset)
		{
			return nullptr;
		}
		UStaticMeshComponent* p = NewObject<UStaticMeshComponent>(this);
		p->SetStaticMesh(asset);
		proxy = p;
	}

	if (!proxy)
	{
		// Skeletal meshes, Niagara, audio, collision-only, cameras: nothing proxied. See the skeletal
		// skip in InitializeIfNeeded for why the rider is deliberately not on this path.
		return nullptr;
	}

	// Materials carry over so masked geometry (fin webbing, decals in the base colour) casts a
	// masked shadow rather than a solid slab.
	if (UPrimitiveComponent* srcPrim = Cast<UPrimitiveComponent>(Source))
	{
		const int32 slots = srcPrim->GetNumMaterials();
		for (int32 i = 0; i < slots; ++i)
		{
			proxy->SetMaterial(i, srcPrim->GetMaterial(i));
		}
	}

	// Shadow-only, by every mechanism at once — see SetProxyVisible. Neither hidden-in-game nor
	// render-in-main-pass held on its own for the board's static mesh, and the failure mode is
	// invisible until the board gets high enough that the dropped copy clears the water, so a
	// belt-and-braces setup is worth more here than a minimal one.
	//
	// The two lighting flags matter — this mesh spends its life parked inside the water surface and
	// would otherwise leak into Lumen GI and the distance fields.
	proxy->SetMobility(EComponentMobility::Movable);
	proxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	proxy->SetGenerateOverlapEvents(false);
	proxy->SetCastShadow(true);
	proxy->SetCastHiddenShadow(true);

	// Nanite geometry does not honour bRenderInMainPass, so a Nanite board would keep drawing however
	// carefully it was hidden. A shadow-only proxy has no business on the Nanite path in any case.
	if (UStaticMeshComponent* proxyStatic = Cast<UStaticMeshComponent>(proxy))
	{
		proxyStatic->SetForceDisableNanite(true);
	}
	proxy->bReceivesDecals = false;
	proxy->SetAffectDynamicIndirectLighting(false);
	proxy->SetAffectDistanceFieldLighting(false);
	proxy->SetCanEverAffectNavigation(false);

	proxy->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	proxy->RegisterComponent();

	// After registration, so the flags land on a live render state rather than relying on them being
	// picked up when one is created.
	SetProxyVisible(proxy, false);

	return proxy;
}

bool AShadowController::InitializeIfNeeded()
{
	if (this->bInitialized)
	{
		return true;
	}

	// The board's FluidDynamics actors are both the hull contact candidates and — when Surfboard is
	// unset — the way to find the board at all.
	this->HullPoints.Empty();
	for (TActorIterator<AFluidDynamics> it(GetWorld()); it; ++it)
	{
		AFluidDynamics* fd = *it;
		if (!fd || !fd->Surfboard)
		{
			continue;
		}
		if (!this->Surfboard)
		{
			this->Surfboard = fd->Surfboard; // single-board level: adopt whichever board exists
		}
		if (fd->Surfboard != this->Surfboard)
		{
			continue;
		}
		// Fins are excluded: they hang below the hull and are fully submerged by construction, so
		// they would win the lowest-point search every tick and anchor the shadow to something that
		// never touches the surface.
		if (fd->side == ESide::VE_Fin && !this->bAnchorOnFins)
		{
			continue;
		}
		this->HullPoints.Add(fd);
	}

	// FluidDynamics BPs wire their fields in BeginPlay; finding nothing on an early tick is normal.
	if (this->HullPoints.Num() == 0 || !this->Surfboard)
	{
		return false;
	}

	// One controller proxies ONE board. A second board in the level keeps its own visible mesh and
	// its own Cast Shadow, which looks exactly like this feature duplicating the board — so say so
	// rather than leaving it to be diagnosed from a screenshot.
	TSet<AActor*> boards;
	for (TActorIterator<AFluidDynamics> it(GetWorld()); it; ++it)
	{
		if (*it && (*it)->Surfboard)
		{
			boards.Add((*it)->Surfboard);
		}
	}
	if (boards.Num() > 1)
	{
		FString names;
		for (AActor* b : boards)
		{
			names += (names.IsEmpty() ? TEXT("") : TEXT(", ")) + b->GetName();
		}
		UE_LOG(LogSurf, Warning,
			TEXT("ShadowController %s: %d surfboards in this level (%s) — proxying only %s. The others keep their own meshes and shadows."),
			*GetName(), boards.Num(), *names, *this->Surfboard->GetName());
	}

	if (!this->sharedCalculations)
	{
		this->sharedCalculations = this->HullPoints[0]->sharedCalculations;
	}
	if (!this->sharedCalculations || !this->sharedCalculations->waveVelocity)
	{
		return false;
	}

	// Everything below runs exactly once — past this point a failure is a real misconfiguration,
	// not a too-early tick, so it warns rather than retrying forever.
	if (CVarShadowEnable.GetValueOnGameThread() == 0)
	{
		// Build nothing and take nothing over, so the board is left exactly as it would be without
		// this actor in the world. That is what makes it a usable A/B control rather than a
		// half-disabled state.
		this->bInitialized = true;
		UE_LOG(LogSurf, Log, TEXT("ShadowController %s: disabled by surf.shadow.enable 0."), *GetName());
		return false;
	}

	TInlineComponentArray<UPrimitiveComponent*> prims;
	this->Surfboard->GetComponents(prims);
	for (UPrimitiveComponent* src : prims)
	{
		// Not drawn -> should not cast. Skips collision hulls, hidden LOD stand-ins and anything the
		// BP switches off, which would otherwise throw a shadow from geometry nobody can see.
		if (!src || src->bHiddenInGame || !src->GetVisibleFlag())
		{
			continue;
		}
		// The rider is deliberately NOT proxied. His shadow falls almost entirely on the board, not on
		// the water, so the grounding correction has nothing to do for him — while a hidden skeletal
		// duplicate that fails to hide (seen once in user testing, at a large offset) is a second
		// visible surfer standing beside the first. Cost nil, risk real: he keeps casting his own
		// shadow from his own mesh, at his true position, and this actor never touches him.
		if (Cast<USkeletalMeshComponent>(src))
		{
			UE_LOG(LogSurf, Log,
				TEXT("ShadowController %s: not proxying skeletal %s — it casts its own shadow (lands on the board, needs no grounding)."),
				*GetName(), *src->GetName());
			continue;
		}
		UPrimitiveComponent* proxy = CreateProxyFor(src);
		if (!proxy)
		{
			continue;
		}
		this->Proxies.Add({ src, proxy });
		UE_LOG(LogSurf, Log,
			TEXT("ShadowController %s: proxied %s [%s] -> %s (mainPass=%d depthPass=%d castShadow=%d hiddenInGame=%d)"),
			*GetName(), *src->GetName(), *src->GetClass()->GetName(), *proxy->GetName(),
			proxy->bRenderInMainPass ? 1 : 0, proxy->bRenderInDepthPass ? 1 : 0,
			proxy->CastShadow ? 1 : 0, proxy->bHiddenInGame ? 1 : 0);
	}

	// Everything on the board that is NOT being proxied, so a stray visible duplicate can be told
	// apart from a proxy at a glance.
	for (UPrimitiveComponent* src : prims)
	{
		if (!src)
		{
			continue;
		}
		const bool bProxied = this->Proxies.ContainsByPredicate(
			[src](const FShadowProxy& sp) { return sp.Source == src; });
		if (!bProxied)
		{
			UE_LOG(LogSurf, Log,
				TEXT("ShadowController %s: skipped %s [%s] (visible=%d hiddenInGame=%d)"),
				*GetName(), *src->GetName(), *src->GetClass()->GetName(),
				src->GetVisibleFlag() ? 1 : 0, src->bHiddenInGame ? 1 : 0);
		}
	}

	this->bInitialized = true;

	if (this->Proxies.Num() == 0)
	{
		UE_LOG(LogSurf, Warning,
			TEXT("ShadowController %s: surfboard %s has no visible mesh components — no shadow proxies built."),
			*GetName(), *this->Surfboard->GetName());
		return false;
	}

	UE_LOG(LogSurf, Log,
		TEXT("ShadowController %s: initialized (board=%s, %d proxies, %d hull points, mode=%.0f)"),
		*GetName(), *this->Surfboard->GetName(), this->Proxies.Num(), this->HullPoints.Num(),
		this->shadowMode);
	return true;
}

bool AShadowController::ComputeClearance(float& outClearance, FVector& outLowestPoint, float& outLowestPlaneZ,
	FString& outLowestLabel, FVector& outPlaneOrigin, FVector& outPlaneNormal) const
{
	if (!this->Surfboard || this->HullPoints.Num() == 0)
	{
		return false;
	}

	const FVector centre = this->Surfboard->GetActorLocation();
	FVector surfLoc, surfNorm;
	if (!SampleSurface(centre, surfLoc, surfNorm))
	{
		return false;
	}

	// ONE wave lookup plus its normal defines a tangent plane, and the wave is locally flat over a
	// 2-3m board — the same board-wide-plane approximation spray settles its foam onto. Twenty
	// per-point lookups would buy nothing at twenty times the cost.
	//
	// shadowWaterlineZBias is what turns the wave DATA surface into the RENDERED mesh surface. The
	// board's own submerged ride height is deliberately NOT baked in here: that is the thing being
	// measured, and it is what tells air apart from the rendering offset.
	const float centrePlaneZ = (float)surfLoc.Z + this->shadowWaterlineZBias + this->shadowContactZBias;
	const float nz = (float)surfNorm.Z;
	const bool bPlaneUsable = FMath::Abs(nz) > 0.1f; // near-vertical normal: the plane blows up, fall back to flat

	outPlaneOrigin = FVector(centre.X, centre.Y, centrePlaneZ);
	outPlaneNormal = bPlaneUsable ? surfNorm : FVector::UpVector;

	float best = TNumericLimits<float>::Max();
	for (const TObjectPtr<AFluidDynamics>& fd : this->HullPoints)
	{
		if (!fd)
		{
			continue;
		}
		const FVector p = fd->GetActorLocation();

		// VERTICAL distance to the plane, not perpendicular: the proxy is offset in Z, so measuring
		// in Z is what makes the correction exact instead of short by cos(face angle).
		float planeZ = centrePlaneZ;
		if (bPlaneUsable)
		{
			planeZ -= (float)(((p.X - centre.X) * surfNorm.X + (p.Y - centre.Y) * surfNorm.Y) / nz);
		}

		const float gap = (float)p.Z - planeZ;
		if (gap < best)
		{
			best = gap;
			outLowestPoint = p;
			outLowestPlaneZ = planeZ;
#if WITH_EDITOR
			outLowestLabel = fd->GetActorLabel();
#else
			outLowestLabel = fd->GetName();
#endif
		}
	}

	if (!FMath::IsFinite(best) || best == TNumericLimits<float>::Max())
	{
		return false;
	}
	outClearance = best;
	return true;
}

void AShadowController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Before anything reads a coefficient, so an edit in the tuning HUD (filter "shadow") or in
	// Saved/TuningOverrides.json takes effect on this very frame.
	RefreshFromTuningSubsystem();

	if (!InitializeIfNeeded())
	{
		return;
	}

	float clearance = 0.0f;
	FVector lowestPoint = FVector::ZeroVector;
	float lowestPlaneZ = 0.0f;
	FString lowestLabel;
	FVector planeOrigin = FVector::ZeroVector;
	FVector planeNormal = FVector::UpVector;
	const bool bSampled = ComputeClearance(clearance, lowestPoint, lowestPlaneZ, lowestLabel,
		planeOrigin, planeNormal);

	// Hoisted so the debug log below reports the cap actually in force. It read the raw UPROPERTY
	// while a CVar override was active, which made the diagnostic disagree with the behaviour — the
	// one thing a diagnostic must never do.
	const float cap = this->shadowMaxGroundingOffset;

	if (!this->bShadowEnabled)
	{
		// Not "no shadow" — the raw, uncorrected placement. Toggling this live shows exactly what
		// the correction is worth.
		this->shadowGroundingOffset = 0.0f;
	}
	else if (bSampled)
	{
		this->shadowRawClearance = clearance;

		// The whole behaviour, in one clamp. Under the cap the gap is the rendering offset and gets
		// corrected away, so the contact point's shadow plants. Over it the gap is real air and only
		// the cap is removed, so the shadow separates by however far the board actually flew.
		const float target = FMath::Clamp(clearance, 0.0f, cap);

		// Framerate-independent exponential smoothing. Per-tick clearance is a min over ~20 points
		// against sampled wave data, and without this the clamp engaging at takeoff pops. A shadow
		// may lag a frame or two invisibly; it may not jitter.
		const float alpha = this->shadowOffsetSmoothingSeconds > KINDA_SMALL_NUMBER
			? 1.0f - FMath::Exp(-DeltaSeconds / this->shadowOffsetSmoothingSeconds)
			: 1.0f;
		this->shadowGroundingOffset += (target - this->shadowGroundingOffset) * alpha;
	}
	// else: hold the previous offset. A stale correction beats a shadow that snaps.

	// Rigid Z translation, rotation and scale copied verbatim. That is what preserves every relative
	// height in the mesh: a tail-down board still throws its nose shadow further away than its tail
	// shadow, at the right ratio, with no per-vertex work.
	// Mode 0 hands casting back to the visible board, 1 is the proxy silently, 2 is the proxy drawn.
	// Applied every tick rather than at init so it can be flipped live in the tuning HUD — the phone
	// has no console, and the mobile renderer is exactly where "does this platform cast from hidden
	// primitives?" has to be answered. All three writes are guarded on a change: SetCastShadow and
	// friends dirty the render state, and doing that every frame for nothing is not free.
	const int32 mode = FMath::RoundToInt(this->shadowMode);
	const bool bProxyCasts = (mode != 0);
	const bool bShowProxies = (mode == 2);

	const FVector drop(0.0f, 0.0f, -this->shadowGroundingOffset);
	for (const FShadowProxy& sp : this->Proxies)
	{
		if (!sp.Source || !sp.Proxy)
		{
			continue;
		}
		FTransform t = sp.Source->GetComponentTransform();
		t.AddToTranslation(drop);
		sp.Proxy->SetWorldTransform(t, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

		if (sp.Proxy->CastShadow != bProxyCasts)
		{
			sp.Proxy->SetCastShadow(bProxyCasts);
		}
		if (sp.Proxy->bRenderInMainPass != bShowProxies)
		{
			SetProxyVisible(sp.Proxy, bShowProxies);
		}
		// The board casts exactly when the proxy does not, so the two can never double up and mode 0
		// leaves the board indistinguishable from having no controller at all.
		if (UPrimitiveComponent* src = Cast<UPrimitiveComponent>(sp.Source.Get()))
		{
			if (src->CastShadow == bProxyCasts)
			{
				src->SetCastShadow(!bProxyCasts);
			}
		}
	}

	if (this->bDebugShadow || SurfDebug::ShouldDebug(this, TEXT("shadow")))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("Shadow [%s] - rawClearance: %.1f, target: %.1f, offset: %.1f (cap %.1f), lowest: %s at (%.1f, %.1f, %.1f), planeZ: %.1f, boardZ: %.1f, waterlineBias: %.1f, sampled: %d, proxies: %d, hullPoints: %d"),
			*GetName(), this->shadowRawClearance,
			FMath::Clamp(this->shadowRawClearance, 0.0f, cap),
			this->shadowGroundingOffset, cap,
			*lowestLabel, lowestPoint.X, lowestPoint.Y, lowestPoint.Z, lowestPlaneZ,
			this->Surfboard ? this->Surfboard->GetActorLocation().Z : 0.0,
			this->shadowWaterlineZBias + this->shadowContactZBias,
			bSampled ? 1 : 0, this->Proxies.Num(), this->HullPoints.Num());
	}

	// Deliberately NOT behind the log gate: SurfDebug::ShouldDebug also needs a matching
	// surf.debug.actors token, and a calibration screenshot should cost one CVar, not three.
	if (bSampled && (this->bDebugDrawContact || CVarShadowDebugDraw.GetValueOnGameThread() != 0))
	{
		// The lowest hull point and where the correction grounds it to.
		const FVector grounded(lowestPoint.X, lowestPoint.Y, lowestPlaneZ);
		DrawDebugSphere(GetWorld(), lowestPoint, 12.0f, 8, FColor::Yellow, false, 0.0f, SDPG_World, 2.0f);
		DrawDebugSphere(GetWorld(), grounded, 12.0f, 8, FColor::Green, false, 0.0f, SDPG_World, 2.0f);
		DrawDebugLine(GetWorld(), lowestPoint, grounded, FColor::Green, false, 0.0f, SDPG_World, 2.0f);

		// THE CALIBRATION TOOL: the contact plane itself, as a tangent grid around the board.
		// shadowWaterlineZBias converts wave data to the rendered surface and cannot be derived
		// from anything in code — it is a number someone has to eyeball. Screenshot this grid
		// against the visible water and dial the bias until they coincide; everything else in
		// this actor is only as right as that one constant.
		const FVector along = FVector::CrossProduct(planeNormal, FVector::RightVector).GetSafeNormal();
		const FVector across = FVector::CrossProduct(planeNormal, along).GetSafeNormal();
		constexpr float kHalfSpan = 300.0f;
		constexpr float kStep = 75.0f;
		for (float d = -kHalfSpan; d <= kHalfSpan + 1.0f; d += kStep)
		{
			DrawDebugLine(GetWorld(),
				planeOrigin + along * d - across * kHalfSpan,
				planeOrigin + along * d + across * kHalfSpan,
				FColor::Cyan, false, 0.0f, SDPG_World, 1.5f);
			DrawDebugLine(GetWorld(),
				planeOrigin + across * d - along * kHalfSpan,
				planeOrigin + across * d + along * kHalfSpan,
				FColor::Cyan, false, 0.0f, SDPG_World, 1.5f);
		}
	}
}
