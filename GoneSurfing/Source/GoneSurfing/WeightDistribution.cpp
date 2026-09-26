// Fill out your copyright notice in the Description page of Project Settings.

#include "WeightDistribution.h"
#include "SurfLog.h"
#include "DrawDebugHelpers.h"
#include "SurfDebug.h"
#include "SurfTuningSubsystem.h"
#include "SharedCalculations.h"
#include "Components/PrimitiveComponent.h"

// Sets default values
AWeightDistribution::AWeightDistribution()
{
	// Tick is needed so the weight-shift impulses can be applied each frame.
	PrimaryActorTick.bCanEverTick = true;
}

// Called when the game starts or when spawned
void AWeightDistribution::BeginPlay()
{
	Super::BeginPlay();

	Tuning = SurfTuning::Get(this);

	// One-shot dump of tunable defaults; see specs/runtime-tuning.md.
#if WITH_EDITOR
	const FString lbl = GetActorLabel();
#else
	const FString lbl = GetName();
#endif
	UE_LOG(LogSurf, Warning, TEXT("TuningDefaults: WeightDistribution [%s] torqueMagnitude=%g maxTiltAngle=%g offsetDistance=%g invertXAxis=%d invertLateralAxis=%d"),
		*lbl, torqueMagnitude, maxTiltAngle, offsetDistance, invertXAxis ? 1 : 0, invertLateralAxis ? 1 : 0);
}

// Called every frame
void AWeightDistribution::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Apply the weight-shift angular impulse. Previously this lived in
	// WeightDistributionBP's tick graph (calculateWeightTorque -> AddAngularImpulseInRadians
	// with bVelChange=true). Moved into C++ so the whole weight pipeline is in one place and
	// can't be silently unwired by a BP edit. calculateWeightTorque() already bakes in the dt
	// scale and the maxTiltAngle roll/pitch gating, so this is applied every tick as a velocity
	// change on the board's root body. NOTE: the BP tick nodes must be removed, or the impulse
	// is applied twice.
	if (Surfboard)
	{
		if (UPrimitiveComponent* mesh = Cast<UPrimitiveComponent>(Surfboard->GetRootComponent()))
		{
			if (mesh->IsSimulatingPhysics())
			{
				const FVector weightTorque = calculateWeightTorque();
				mesh->AddAngularImpulseInRadians(weightTorque, NAME_None, /*bVelChange=*/true);
			}
		}
	}

	// Speed-attenuate PumpInput in place so both the impulse path below AND
	// the bottom-hydrofoil's pumpGate (read via WD->PumpInput from FluidDynamics)
	// see the same tapered value. Pumping in real surfing saturates because
	// drag scales with v² *through the water* — board velocity in world frame
	// isn't the right signal (a board carried by fast-moving wave water still
	// has headroom to pump). Use relativeWaterVelocityMagnitude from SC.
	// Without this, holding the space bar accelerates the board to absurd speeds.
	// See specs/pumping.md.
	const float pumpInputBeforeSpeedAtt = PumpInput;  // for diag log below
	float pumpSpeedAtt = 1.0f;                        // also for diag log
	float pumpRelWaterVel = 0.0f;
	if (Tuning && sharedCalculations)
	{
		pumpRelWaterVel = sharedCalculations->relativeWaterVelocityMagnitude;
		if (Tuning->PumpSpeedAttenuationEnd > 0.0f)
		{
			pumpSpeedAtt *= 1.0f - FMath::SmoothStep(
				Tuning->PumpSpeedAttenuationStart,
				Tuning->PumpSpeedAttenuationEnd,
				pumpRelWaterVel);
		}
		// Ramp-in: a pump needs water flowing past the bottom to push against. Without this the
		// taper above hands a stalled board the strongest pump of all, which is the loop behind
		// the full-rail pirouette (see PumpSpeedRampInStart in SurfTuningSubsystem.h).
		if (Tuning->PumpSpeedRampInEnd > 0.0f)
		{
			pumpSpeedAtt *= FMath::SmoothStep(
				Tuning->PumpSpeedRampInStart,
				Tuning->PumpSpeedRampInEnd,
				pumpRelWaterVel);
		}
		PumpInput *= pumpSpeedAtt;
	}

	// TEMPORARY DIAGNOSTIC — unconditional log when actively pumping. Throttled
	// to every 10 ticks so a 10s pump session yields ~120 log lines. Captures
	// the WeightDistribution side of the pipeline; the FluidDynamics side is in
	// calcThrustForce's matching block. Strip these once pump tuning is dialed
	// in on Android.
	{
		static int32 sPumpDiagFrameCounter = 0;
		++sPumpDiagFrameCounter;
		// Log when EITHER the raw or attenuated pump is non-trivial — captures
		// the "speed-att kills PumpInput to 0" case too.
		if ((PumpInput > 0.01f || pumpInputBeforeSpeedAtt > 0.01f) && (sPumpDiagFrameCounter % 10 == 0) && Surfboard && Tuning)
		{
			UPrimitiveComponent* meshDiag = Cast<UPrimitiveComponent>(Surfboard->GetRootComponent());
			if (meshDiag && meshDiag->IsSimulatingPhysics())
			{
				const FVector vel = meshDiag->GetPhysicsLinearVelocity();
				const float forceProxy = Tuning->MaxPumpForce * PumpInput;
				UE_LOG(LogSurf, Warning,
					TEXT("PumpDiag-WD: PumpInput(raw=%.3f post-speedAtt=%.3f) MaxF=%.0f forceProxy=%.0f relWV=%.1f speedAtt=%.3f | vel=(%.0f,%.0f,%.0f) speed=%.0f"),
					pumpInputBeforeSpeedAtt, PumpInput, Tuning->MaxPumpForce, forceProxy,
					pumpRelWaterVel, pumpSpeedAtt,
					vel.X, vel.Y, vel.Z, vel.Size());
			}
		}
	}

	// Slope attenuation, computed here rather than only inside the impulse block below: the
	// published PumpInputAttenuated has to be right on every tick, including the ticks where the
	// impulse never runs, because other actors gate on it.
	{
		float pumpSlopeAttNow = 1.0f;
		if (Tuning && sharedCalculations)
		{
			pumpSlopeAttNow = 1.0f - FMath::SmoothStep(
				Tuning->PumpSlopeAttenuationStart,
				Tuning->PumpSlopeAttenuationEnd,
				sharedCalculations->boardWideSlopeSin);
		}
		PumpInputAttenuated = PumpInput * pumpSlopeAttNow;
	}

	// Pump impulse — see specs/pumping.md. TestPumpInput (Tuning|Pump) acts as
	// a constant override for Phase 0 verification; otherwise PumpInput from
	// the pawn drives the magnitude. Force is attenuated on steep wave faces
	// via boardWideSlopeSin when the SC reference is wired.
	if (Tuning && Tuning->MaxPumpForce > 0.0f && Surfboard)
	{
		const float effectiveInput = (Tuning->TestPumpInput > 0.0f)
			? Tuning->TestPumpInput : PumpInput;
		if (effectiveInput > 0.0f)
		{
			UPrimitiveComponent* mesh = Cast<UPrimitiveComponent>(Surfboard->GetRootComponent());
			if (mesh && mesh->IsSimulatingPhysics())
			{
				float slopeAtt = 1.0f;
				if (sharedCalculations)
				{
					slopeAtt = 1.0f - FMath::SmoothStep(
						Tuning->PumpSlopeAttenuationStart,
						Tuning->PumpSlopeAttenuationEnd,
						sharedCalculations->boardWideSlopeSin);
				}
				const float forceMag = Tuning->MaxPumpForce * effectiveInput * slopeAtt;
				if (forceMag > 0.0f)
				{
					FVector applicationPoint = mesh->GetCenterOfMass();
					const bool bHasForward = !FMath::IsNearlyZero(Tuning->PumpForwardOffset);
					const bool bHasLateral = !FMath::IsNearlyZero(Tuning->PumpLateralOffset);
					if (bHasForward || bHasLateral)
					{
						const FTransform& boardTr = Surfboard->GetTransform();
						if (bHasForward)
						{
							const FVector boardFwd = boardTr
								.TransformVector(FVector(0.0f, 1.0f, 0.0f)).GetSafeNormal();
							applicationPoint += boardFwd * Tuning->PumpForwardOffset;
						}
						if (bHasLateral)
						{
							const FVector boardLat = boardTr
								.TransformVector(FVector(1.0f, 0.0f, 0.0f)).GetSafeNormal();
							applicationPoint += boardLat * Tuning->PumpLateralOffset;
						}
					}
					// Drive through the engaged rail. The lateral axis' own Z tells us which rail is
					// lower AND by how much (it is the sine of the bank angle), so one term carries
					// both direction and magnitude: flat board, no offset; committed board, offset
					// toward the rail it is already leaning on. Sign-safe by construction - it reads
					// the board's actual attitude rather than assuming which way a weight value maps.
					if (Tuning->PumpRailOffset != 0.0f)
					{
						const FVector boardLat = Surfboard->GetTransform()
							.TransformVector(FVector(1.0f, 0.0f, 0.0f)).GetSafeNormal();
						applicationPoint += boardLat * (-(float)boardLat.Z * Tuning->PumpRailOffset);
					}

					const FVector downImpulse(0.0f, 0.0f, -forceMag * DeltaTime);
					mesh->AddImpulseAtLocation(downImpulse, applicationPoint);

					const bool bDebugPump = debug || SurfDebug::ShouldDebug(this, TEXT("pump"));
					if (bDebugPump)
					{
						UE_LOG(LogSurf, Warning,
							TEXT("Pump impulse: input=%.3f slopeAtt=%.3f forceMag=%.1f fwdOff=%.1f latOff=%.1f dt=%.4f"),
							effectiveInput, slopeAtt, forceMag,
							Tuning->PumpForwardOffset, Tuning->PumpLateralOffset, DeltaTime);
					}

					// Debug draw for pump feedback, gated behind the default-off
					// pump debug flag (`debug` UPROPERTY or surf.debug.flags "pump")
					// so it stays invisible during normal play. Line length proxies
					// force magnitude (~50 cm at 50 kN); thickness scales with input
					// so a vigorous pump is visibly bigger. Cyan sphere marks the
					// application point; yellow sphere marks the COM so
					// PumpForwardOffset is obvious. 0.1 s persistence = one tick at
					// 60 fps so it doesn't trail.
					if (bDebugPump)
					{
						const float visualScale = 0.001f;
						const FVector arrowEnd = applicationPoint
							+ FVector(0.0f, 0.0f, -forceMag * visualScale);
						const float thickness = 1.0f + 4.0f * effectiveInput;
						DrawDebugLine(GetWorld(), applicationPoint, arrowEnd,
							FColor::Cyan, false, 0.1f, 0, thickness);
						DrawDebugSphere(GetWorld(), mesh->GetCenterOfMass(), 3.0f, 8,
							FColor(255, 255, 0), false, 0.1f);
					}
				}
			}
		}
	}

	// Diagnostic: log the values WeightDistribution actually holds at the end of
	// each tick. If something writes after the pawn (e.g. an autopilot BP),
	// these values will diverge from the pawn's tilt log.
	if (debug || SurfDebug::ShouldDebug(this, TEXT("weight")))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("WeightDistribution::Tick [%s]: amountInFront=%.3f amountToTheRight=%.3f"),
			*GetName(), amountInFront, amountToTheRight);
	}
}

FVector AWeightDistribution::calculateWeightTorque()
{
	if (!Surfboard)
	{
		return FVector::ZeroVector;
	}

	// Weight offset from center: -0.5 to +0.5
	float rollOffset = amountToTheRight - 0.5f;   // positive = lean right
	float pitchOffset = amountInFront - 0.5f;     // positive = lean forward

	if (invertLateralAxis) rollOffset = -rollOffset;
	if (invertXAxis) pitchOffset = -pitchOffset;

	// Measure roll and pitch angles independently to cap them separately.
	// The mesh has: local X = lateral (left/right), local Y = longitudinal (forward/back), local Z = up
	// GetActorForwardVector() = local +X = lateral axis
	// GetActorRightVector() = local +Y = longitudinal axis
	FVector lateralAxis = Surfboard->GetActorForwardVector();   // local X = left/right
	FVector longitudinalAxis = Surfboard->GetActorRightVector(); // local Y = forward/back

	float cosLimit = FMath::Cos(FMath::DegreesToRadians(maxTiltAngle));

	// Roll: signed tilt — positive = local X points up (tilted right), negative = tilted left
	float rollTiltSigned = FVector::DotProduct(lateralAxis, FVector::UpVector);
	float rollTilt = FMath::Abs(rollTiltSigned);
	float rollCos = 1.0f - rollTilt;
	float rollFactor = FMath::Clamp((rollCos - cosLimit) / (1.0f - cosLimit), 0.0f, 1.0f);

	// Only apply rollFactor if torque pushes further from flat.
	// If torque pushes back toward flat, allow full strength.
	// Check: does the torque (rollOffset) push the tilt further in the same direction?
	// torqueLocal.Y = rollOffset * magnitude, and rollTiltSigned measures the current tilt.
	// The torque increases tilt when rollOffset and rollTiltSigned have the SAME sign as torqueLocal.Y.
	// Since torqueLocal.Y = rollOffset * ..., we compare rollOffset sign with rollTiltSigned sign.
	// But invertLateralAxis flips rollOffset, so the relationship between rollOffset and tilt may be inverted.
	// Use the actual torque direction: if torqueLocal.Y would push tilt further, limit it.
	bool rollPushesFurtherFromFlat = (rollOffset > 0 && rollTiltSigned < 0) || (rollOffset < 0 && rollTiltSigned > 0);
	if (!rollPushesFurtherFromFlat) rollFactor = 1.0f;

	// Pitch: signed tilt — positive = local Y points up (nose up), negative = nose down
	float pitchTiltSigned = FVector::DotProduct(longitudinalAxis, FVector::UpVector);
	float pitchTilt = FMath::Abs(pitchTiltSigned);
	float pitchCos = 1.0f - pitchTilt;
	float pitchFactor = FMath::Clamp((pitchCos - cosLimit) / (1.0f - cosLimit), 0.0f, 1.0f);

	bool pitchPushesFurtherFromFlat = (pitchOffset > 0 && pitchTiltSigned < 0) || (pitchOffset < 0 && pitchTiltSigned > 0);
	if (!pitchPushesFurtherFromFlat) pitchFactor = 1.0f;

	// Scale by delta time so the impulse is frame-rate independent
	// Clamp dt to prevent large impulse spikes on long frames
	float dt = FMath::Min(GetWorld()->GetDeltaSeconds(), 0.02f);

	// Effective magnitude: prefer the live-tunable subsystem value (tunable from the HUD,
	// persisted to TuningOverrides.json) so it overrides the actor's UPROPERTY default.
	const float effectiveTorqueMagnitude = Tuning ? Tuning->WeightTorqueMagnitude : torqueMagnitude;

	// Torque in local space:
	// Roll = torque around local Y (forward axis)
	// Pitch = torque around local X (lateral axis)
	FVector torqueLocal = FVector(
		-pitchOffset * effectiveTorqueMagnitude * pitchFactor * dt,   // pitch torque around local X
		rollOffset * effectiveTorqueMagnitude * rollFactor * dt,      // roll torque around local Y
		0.0f
	);

	// Transform to world space
	FVector torqueWorld = Surfboard->GetTransform().TransformVector(torqueLocal);

	if (debug || SurfDebug::ShouldDebug(this, TEXT("weight")))
	{
		UE_LOG(LogSurf, Warning, TEXT("WeightTorque: rollOffset=%.2f, pitchOffset=%.2f, rollFactor=%.3f, pitchFactor=%.3f, torqueLocal=%s, torqueWorld=%s"),
			rollOffset, pitchOffset, rollFactor, pitchFactor, *torqueLocal.ToString(), *torqueWorld.ToString());
	}

	return torqueWorld;
}
