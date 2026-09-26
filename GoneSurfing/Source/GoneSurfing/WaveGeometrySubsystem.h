#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WaveGeometrySubsystem.generated.h"

class AWaveHeight;

/** Where a point is on the wave, relative to the peel. See specs/wave-geometry.md FR4. */
UENUM(BlueprintType)
enum class EWaveZone : uint8
{
	Unknown,     // no model for this level
	Behind,      // over the crest, on the back of the wave
	Flat,        // in front of the wave, beyond the bore band
	Shoulder,    // on the face, ahead of the impact point (unbroken, still to break)
	Pocket,      // at the impact point: PocketAhead ahead .. PocketBehind behind it
	Whitewater,  // behind the impact point, in the bore band
};

USTRUCT(BlueprintType)
struct FWaveGeoSample
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) bool bValid = false;
	/** The point in the wave frame: S along the line (+ = down the line), C cross-shore (+ = toward the back). */
	UPROPERTY(BlueprintReadOnly) float S = 0.0f;
	UPROPERTY(BlueprintReadOnly) float C = 0.0f;
	/** The impact point (where the lip is landing now) of the wave nearest this point along the line. */
	UPROPERTY(BlueprintReadOnly) FVector ImpactWorld = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly) float ImpactS = 0.0f;
	UPROPERTY(BlueprintReadOnly) float ImpactC = 0.0f;
	/** ImpactS - S: > 0 = up the line of the impact, on the broken side. cm. */
	UPROPERTY(BlueprintReadOnly) float DistBehindImpact = 0.0f;
	/** C - ImpactC: > 0 = toward the back of the wave. cm. */
	UPROPERTY(BlueprintReadOnly) float CrossFromImpact = 0.0f;
	/** DistBehindImpact / peel speed: how long ago the peel passed this point along the line.
	 *  Negative = seconds until it gets here. For a point in the whitewater, the age of the bore there. */
	UPROPERTY(BlueprintReadOnly) float SecondsSinceBroken = 0.0f;
	/** 0..1, how far into the broken water this point is (FR4). 0 on the face and over the back. */
	UPROPERTY(BlueprintReadOnly) float Broken = 0.0f;
	UPROPERTY(BlueprintReadOnly) EWaveZone Zone = EWaveZone::Unknown;
};

/** The peel model + zone constants, from Content/WaveGeometry/<map>.json (specs/wave-geometry.md FR3/FR4). */
struct FWaveGeoModel
{
	bool bValid = false;
	FVector2D P0 = FVector2D::ZeroVector; float F0 = 0.0f;   // world XY of the impact point at frame F0
	FVector2D P1 = FVector2D::ZeroVector; float F1 = 0.0f;   // ... and at frame F1 (F1 != F0)
	float ImpactToCrestC = 200.0f;   // the breaking crest is this far behind the fresh foam (cm)
	float PocketAhead    = 600.0f;   // Pocket = from this far ahead of the impact ...
	float PocketBehind   = 200.0f;   // ... to this far behind it
	float BrokenRamp     = 400.0f;   // Broken ramps 0 -> 1 over this distance behind PocketBehind
	float BehindMargin   = 300.0f;   // over the crest = more than ImpactToCrestC + this toward the back
	float FaceExtent     = 900.0f;   // the face reaches this far shoreward of the impact point's c
	// The bore (the whitewater lump) starts BoreFrontStart shoreward of the impact point and drifts
	// shoreward at BoreDriftRate as it ages; its back edge starts at the crest and drifts with it.
	float BoreFrontStart = 800.0f;
	float BoreDriftRate  = 150.0f;   // cm/s
	// A young bore's front spreads from the impact at this speed (the lip lands, the foam runs
	// shoreward at ~3 m/s: a board riding the young front sat at -287 / -637 / -954 cm at ages
	// 1 / 2 / 3 s, broken-wave M15). The front is min(BoreFrontSpeed x age, BoreFrontStart +
	// BoreDriftRate x age) shoreward of the impact - the fast young line until it meets the slow
	// old one (~5 s), which is where the 800 + 150 x age numbers were fitted. Before this the
	// band was 8 m wide the moment the lip landed, and a board 4 m shoreward of a fresh landing -
	// ahead of the foam, on the low face, racing it - read whitewater (M20).
	float BoreFrontSpeed = 300.0f;   // cm/s
	// The band's shoreward edge is soft: a point within this of the front is still in the band, with
	// Broken fading to 0 over it. A board riding the bore front shoreward outruns the modelled front
	// (it goes 230-290 cm/s on the water, the front 150), and a hard edge switched every whitewater
	// consequence off in one tick - the board surged 240 -> 688 along its nose (broken-wave M19).
	float BoreFrontFade  = 400.0f;
	// Behind the impact, outside the bore band, the water is the bore's back / the trough behind it
	// (Behind) or the flat in front of it (Flat) until the bore is this old; after that the face has
	// re-formed and the point belongs to the next wave up the line (its shoulder / pocket).
	float ShoulderAfterSeconds = 4.0f;
	FString Source;                  // where it came from (json path / cvar), for the log
};

/**
 * One place that knows where the wave is: the wave frame (along the line / cross-shore), the peel
 * model (the impact point moves linearly down the line, one new break per loop), and a per-point
 * sample: distance behind the impact, cross-shore offset, seconds since broken, Broken 0..1, Zone.
 *
 * Pure query, no tick, no per-board state; lazily initialised from the level on first use. Measured
 * basis: specs/broken-wave-no-consequences.md M14 (impact 25.9 cm/frame, constant cross-shore, two
 * points reproduce the foam front to +-1.9 m). Consumers: specs/wave-geometry.md.
 */
UCLASS()
class GONESURFING_API UWaveGeometrySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override
	{
		return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
	}

	/** True once the level's axes are resolved; the model may still be absent (HasModel). */
	bool IsReady();
	bool HasModel() { return IsReady() && Model.bValid; }
	const FWaveGeoModel& GetModel() { IsReady(); return Model; }

	// ---- the wave frame (FR2) ----
	FVector LineDir() { IsReady(); return Line; }
	FVector BackDir() { IsReady(); return Back; }
	float S(const FVector& P) { IsReady(); return (float)(P.X * Line.X + P.Y * Line.Y); }
	float C(const FVector& P) { IsReady(); return (float)(P.X * Back.X + P.Y * Back.Y); }
	FVector World(float s, float c, float z = 0.0f) { IsReady(); return Line * s + Back * c + FVector(0, 0, z); }

	// ---- the peel model (FR3) ----
	/** Frames per loop (886..1078 = 193) and the impact's speed along the line, cm per frame. */
	int32 LoopFrames() { IsReady(); return NumLoopFrames; }
	float PeelSpeedCmPerFrame() { IsReady(); return PeelSpeed; }
	/** Distance along the line between consecutive breaks (one loop of peel travel). */
	float PeriodS() { IsReady(); return PeriodAlongLine; }
	/** World XY (z = 0) of wave k's impact point at this frame. k = 0 is the wave through (P0, F0). */
	FVector2D ImpactXY(int32 Frame, int32 WaveIndex);
	/** The wave whose impact point is nearest a point along the line. */
	int32 NearestWaveIndex(int32 Frame, float AtS);
	/** The current wave frame, read from the level's WaterController (what the physics samples). */
	int32 CurrentFrame();

	// ---- the sample (FR4) ----
	FWaveGeoSample Sample(const FVector& WorldPos, int32 Frame);
	FWaveGeoSample SampleNow(const FVector& WorldPos) { return Sample(WorldPos, CurrentFrame()); }

	static const TCHAR* ZoneName(EWaveZone Z);

private:
	bool bInitialised = false;
	FVector Line = FVector::ZeroVector;
	FVector Back = FVector::ZeroVector;
	int32 NumLoopFrames = 193;
	float PeelSpeed = 0.0f;          // cm per frame along the line (signed, + = down the line)
	FVector2D VelXY = FVector2D::ZeroVector; // cm per frame, world
	float PeriodAlongLine = 0.0f;
	FWaveGeoModel Model;
	TWeakObjectPtr<AWaveHeight> WaveHeight;

	void Initialise();
	bool LoadModelFromJson(const FString& MapName);
	bool ApplyModelOverride();
	void FinishModel();
};
