// See SurfTuningSubsystem.h and specs/runtime-tuning.md.

#include "SurfTuningSubsystem.h"
#include "SurfLog.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr float kSaveDebounceSeconds = 0.3f;

	// Property-name -> category-string mapping. UPROPERTY(Category=...) metadata
	// would normally provide this, but FProperty::HasMetaData / GetMetaData are
	// stripped from non-editor builds (e.g. Android Development packaging), so
	// the table has to be supplied explicitly. Adding a new UPROPERTY to the
	// subsystem requires also adding it to this table — otherwise it falls
	// through with an empty category and shows up under "" in the tuner panel.
	// Dev switches: global by nature, never per-board feel. SurfTuningSubsystem.h has asserted this
	// about the global overrides file for as long as boards have existed, but nothing enforced it, and
	// the HUD writes to the ACTIVE BOARD's overlay — the innermost layer, which beats the global file.
	// So toggling one in the HUD baked it into Saved/BoardTuning/<id>.json, where it stayed on for
	// every ride of that board with no log line to say so. Measured cost: "RecordIntro": 1 sat in
	// shortboard.json suppressing the rails intro, and two runs were spent finding it.
	//
	// These names are skipped when writing a BOARD overlay and ignored when reading one. They are
	// still written to and read from the global file, which is where they belong.
	bool IsGlobalOnlyTunable(const FName& Name)
	{
		static const TSet<FName> GlobalOnly = {
			FName(TEXT("StartScreenSkip")),
			FName(TEXT("AssistSkipFirstPlayCardInPIE")),
			FName(TEXT("RailsDisable")),
			FName(TEXT("RecordIntro")),
		};
		return GlobalOnly.Contains(Name);
	}

	struct FCategoryPair { const TCHAR* Field; const TCHAR* Category; };
	const FCategoryPair kCategoryTable[] = {
		// Damping
		{ TEXT("SurfboardSidewaysDamping"),     TEXT("Tuning|Damping") },
		{ TEXT("SidewaysDampingAirScale"),      TEXT("Tuning|Damping") },
		{ TEXT("SurfboardForwardsDamping"),     TEXT("Tuning|Damping") },
		{ TEXT("SurfboardVerticalDamping"),     TEXT("Tuning|Damping") },
		{ TEXT("WorldDownwardsDamping"),        TEXT("Tuning|Damping") },
		{ TEXT("WorldUpwardsDamping"),          TEXT("Tuning|Damping") },
		{ TEXT("AngularDampingX"),              TEXT("Tuning|Damping") },
		{ TEXT("AngularDampingY"),              TEXT("Tuning|Damping") },
		{ TEXT("AngularDampingZ"),              TEXT("Tuning|Damping") },
		{ TEXT("AngularDampingYAwayExtra"),     TEXT("Tuning|Damping") },
		{ TEXT("AngularDampingYTowardReduction"), TEXT("Tuning|Damping") },
		{ TEXT("YawDampingTiltBackInfluence"),  TEXT("Tuning|Damping") },
		{ TEXT("ClampYVelocityAt"),             TEXT("Tuning|Damping") },
		{ TEXT("MaxVelocityX"),                 TEXT("Tuning|Damping") },
		{ TEXT("MaxVelocityZUp"),               TEXT("Tuning|Damping") },
		{ TEXT("MaxVelocityZDown"),             TEXT("Tuning|Damping") },
		{ TEXT("VelocityDampingThreshold"),     TEXT("Tuning|Damping") },
		{ TEXT("VelocityDampingScale"),         TEXT("Tuning|Damping") },
		{ TEXT("MaxVelocityDamping"),           TEXT("Tuning|Damping") },
		{ TEXT("AmountUnderWaterEquilibrium"),  TEXT("Tuning|Damping") },
		{ TEXT("PlaningRedirectMaxAngle"),      TEXT("Tuning|Damping") },
		{ TEXT("PlaningRedirectOutwardScale"),  TEXT("Tuning|Damping") },
		{ TEXT("PlaningRedirectCrestFade"),     TEXT("Tuning|Damping") },
		{ TEXT("CarveGripCrestFade"),           TEXT("Tuning|Damping") },
		{ TEXT("PitchRightingRate"),            TEXT("Tuning|Damping") },
		{ TEXT("CarveGripRate"),                TEXT("Tuning|Damping") },
		{ TEXT("CarveGripContactBlend"),        TEXT("Tuning|Damping") },
		{ TEXT("CarveGripTurnUnfadeSymmetric"), TEXT("Tuning|Damping") },
		{ TEXT("AntiSlipForceScale"),           TEXT("Tuning|Damping") },
		// Lift
		{ TEXT("bottomLiftMagnitude"),          TEXT("Tuning|Lift") },
		{ TEXT("railLiftMagnitude"),            TEXT("Tuning|Lift") },
		{ TEXT("railLiftRollDecouple"),         TEXT("Tuning|Lift") },
		{ TEXT("finLiftMagnitude"),             TEXT("Tuning|Lift") },
		// Drag
		{ TEXT("bottomDragCoefficient"),        TEXT("Tuning|Drag") },
		{ TEXT("maxDragAmount"),                TEXT("Tuning|Drag") },
		{ TEXT("finDragCoefficient"),           TEXT("Tuning|Drag") },
		{ TEXT("railDragCoefficient"),          TEXT("Tuning|Drag") },
		{ TEXT("tailDragCoefficient"),          TEXT("Tuning|Drag") },
		// Thrust
		{ TEXT("alongThrustCoefficient"),       TEXT("Tuning|Thrust") },
		{ TEXT("upwardsThrustCoefficient"),     TEXT("Tuning|Thrust") },
		{ TEXT("thrustMagnitude"),              TEXT("Tuning|Thrust") },
		{ TEXT("forwardsThrustCoefficient"),    TEXT("Tuning|Thrust") },
		{ TEXT("actorForwardsThrustCoefficient"), TEXT("Tuning|Thrust") },
		{ TEXT("upwardsThrustPitchSensitivity"),TEXT("Tuning|Thrust") },
		{ TEXT("yawHydrofoilCoefficient"),      TEXT("Tuning|Thrust") },
		{ TEXT("yawFwdSlopeGateMin"),         TEXT("Tuning|Thrust") },
		{ TEXT("yawFwdSlopeGateWidth"),       TEXT("Tuning|Thrust") },
		{ TEXT("yawFwdOffFaceFloor"),         TEXT("Tuning|Thrust") },
		{ TEXT("maxHydrofoilForceAmount"),      TEXT("Tuning|Thrust") },
		{ TEXT("slopeThrustCoefficient"),       TEXT("Tuning|Thrust") },
		{ TEXT("slopeThrustMinSlopeSin"),       TEXT("Tuning|Thrust") },
		{ TEXT("slopeThrustFinRedirect"),       TEXT("Tuning|Thrust") },
		{ TEXT("lateralTurnCoefficient"),       TEXT("Tuning|Thrust") },
		{ TEXT("lateralTurnSpeedCap"),          TEXT("Tuning|Thrust") },
		{ TEXT("lateralTurnRollDeadzone"),      TEXT("Tuning|Thrust") },
		{ TEXT("lateralTurnHardCarveBoost"),    TEXT("Tuning|Thrust") },
		{ TEXT("lateralTurnHardCarveStart"),    TEXT("Tuning|Thrust") },
		{ TEXT("lateralTurnHardCarveFull"),     TEXT("Tuning|Thrust") },
		// Buoyancy
		{ TEXT("basicFloatBuoyancyCoefficient"),     TEXT("Tuning|Buoyancy") },
		{ TEXT("horizontalVelocityBuoyancyCoefficient"), TEXT("Tuning|Buoyancy") },
		{ TEXT("verticalVelocityBuoyancyCoefficient"),   TEXT("Tuning|Buoyancy") },
		{ TEXT("amountUnderWaterPower"),        TEXT("Tuning|Buoyancy") },
		{ TEXT("weightForceMaxMultiplier"),     TEXT("Tuning|Buoyancy") },
		{ TEXT("distanceWhereMaxForceShouldBeApplied"),  TEXT("Tuning|Buoyancy") },
		{ TEXT("wettedTransitionDistance"),     TEXT("Tuning|Buoyancy") },
		{ TEXT("airborneForceScale"),           TEXT("Tuning|Drag") },
		{ TEXT("airborneFadeDistance"),         TEXT("Tuning|Drag") },
		{ TEXT("waveFaceNormalInfluence"),      TEXT("Tuning|Buoyancy") },
		{ TEXT("horizontalVelocityBuoyancyFrontBias"), TEXT("Tuning|Buoyancy") },
		{ TEXT("boardHalfLength"),              TEXT("Tuning|Buoyancy") },
		// WaveMass
		{ TEXT("waveSlopeGravityCoefficient"),  TEXT("Tuning|WaveMass") },
		{ TEXT("maxSupplementForce"),           TEXT("Tuning|WaveMass") },
		{ TEXT("waveMassThrustCoefficient"),    TEXT("Tuning|WaveMass") },
		{ TEXT("waveMassFlowDragCoefficient"),  TEXT("Tuning|WaveMass") },
		{ TEXT("waveMassMinSlopeSin"),          TEXT("Tuning|WaveMass") },
		{ TEXT("waveMassRollDecouple"),         TEXT("Tuning|WaveMass") },
		{ TEXT("wavePenetrationCoefficient"),   TEXT("Tuning|WaveMass") },
		{ TEXT("wavePenetrationThreshold"),     TEXT("Tuning|WaveMass") },
		{ TEXT("PerActorWaterVelocityBlend"),   TEXT("Tuning|WaveMass") },
		{ TEXT("LipImpactCoefficient"),         TEXT("Tuning|WaveMass") },
		{ TEXT("LipImpactBandOffset"),          TEXT("Tuning|WaveMass") },
		{ TEXT("LipImpactMinJetSpeed"),         TEXT("Tuning|WaveMass") },
		{ TEXT("LipImpactMinSlopeSin"),         TEXT("Tuning|WaveMass") },
		{ TEXT("LipImpactCrestRange"),          TEXT("Tuning|WaveMass") },
		{ TEXT("LipImpactMaxForce"),            TEXT("Tuning|WaveMass") },
		{ TEXT("baseHeight"),                   TEXT("Tuning|WaveMass") },
		{ TEXT("slopeHeight"),                  TEXT("Tuning|WaveMass") },
		{ TEXT("maxEffectiveWaterHeight"),      TEXT("Tuning|WaveMass") },
		// Planing
		{ TEXT("PlaningStartsVelocity"),        TEXT("Tuning|Planing") },
		{ TEXT("PlaningStopsVelocity"),         TEXT("Tuning|Planing") },
		{ TEXT("PlaningFullVelocity"),          TEXT("Tuning|Planing") },
		{ TEXT("MaxPlaning"),                   TEXT("Tuning|Planing") },
		// Pump
		{ TEXT("MaxPumpForce"),                 TEXT("Tuning|Pump") },
		{ TEXT("TestPumpInput"),                TEXT("Tuning|Pump") },
		{ TEXT("PumpForwardOffset"),            TEXT("Tuning|Pump") },
		{ TEXT("PumpLateralOffset"),            TEXT("Tuning|Pump") },
		{ TEXT("PumpSlopeAttenuationStart"),    TEXT("Tuning|Pump") },
		{ TEXT("PumpSlopeAttenuationEnd"),      TEXT("Tuning|Pump") },
		{ TEXT("PumpSpeedAttenuationStart"),    TEXT("Tuning|Pump") },
		{ TEXT("PumpSpeedAttenuationEnd"),      TEXT("Tuning|Pump") },
		{ TEXT("PumpSpeedRampInStart"),         TEXT("Tuning|Pump") },
		{ TEXT("PumpSpeedRampInEnd"),           TEXT("Tuning|Pump") },
		{ TEXT("PumpForwardForceCap"),          TEXT("Tuning|Pump") },
		{ TEXT("PumpAccelForFull"),             TEXT("Tuning|Pump") },
		{ TEXT("PumpDeadzone"),                 TEXT("Tuning|Pump") },
		{ TEXT("PumpLowPassHz"),                TEXT("Tuning|Pump") },
		// Crest scan + assist (specs/gradual-control-handoff.md)
		{ TEXT("CrestScanRobust"),              TEXT("Tuning|Assist") },
		{ TEXT("CrestScanRangeCm"),             TEXT("Tuning|Assist") },
		{ TEXT("AssistDisable"),                TEXT("Tuning|Assist") },
		{ TEXT("AssistAlphaForce"),             TEXT("Tuning|Assist") },
		{ TEXT("AssistDrawBand"),               TEXT("Tuning|Assist") },
		{ TEXT("AssistSkipFirstPlayCardInPIE"),  TEXT("Tuning|Assist") },
		// Start screen
		{ TEXT("StartScreenSkip"),              TEXT("Tuning|Start Screen") },
		{ TEXT("RailsDisable"),                 TEXT("Tuning|Start Screen") },
		{ TEXT("RecordIntro"),                  TEXT("Tuning|Start Screen") },
		{ TEXT("AssistDrawBandSeconds"),        TEXT("Tuning|Assist") },
		{ TEXT("AssistBandNear"),               TEXT("Tuning|Assist") },
		{ TEXT("AssistBandFar"),                TEXT("Tuning|Assist") },
		{ TEXT("AssistBandSoftness"),           TEXT("Tuning|Assist") },
		{ TEXT("AssistMaxHeadingSin"),          TEXT("Tuning|Assist") },
		{ TEXT("AssistHeadingP"),               TEXT("Tuning|Assist") },
		{ TEXT("AssistHeadingD"),               TEXT("Tuning|Assist") },
		{ TEXT("AssistHeadingRateSmoothingSeconds"), TEXT("Tuning|Assist") },
		{ TEXT("AssistMaxAuthority"),           TEXT("Tuning|Assist") },
		{ TEXT("AssistSteerGain"),              TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimGain"),               TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimNeutral"),            TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimSlopeGain"),          TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimSpeedRef"),           TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimSpeedGain"),          TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimTargetMin"),          TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimTargetMax"),          TEXT("Tuning|Assist") },
		{ TEXT("AssistTrimP"),                  TEXT("Tuning|Assist") },
		{ TEXT("AssistPumpInputThreshold"),     TEXT("Tuning|Assist") },
		{ TEXT("AssistPumpReleaseSeconds"),     TEXT("Tuning|Assist") },
		{ TEXT("AssistRateLimitAtFullAssist"),  TEXT("Tuning|Assist") },
		{ TEXT("AssistRateLimitAtNoAssist"),    TEXT("Tuning|Assist") },
		{ TEXT("AssistCreditFullSeconds"),      TEXT("Tuning|Assist") },
		{ TEXT("AssistCreditMidSeconds"),       TEXT("Tuning|Assist") },
		{ TEXT("AssistCreditZeroSeconds"),      TEXT("Tuning|Assist") },
		{ TEXT("AssistAlphaAtMid"),             TEXT("Tuning|Assist") },
		{ TEXT("AssistAssistedCreditRate"),     TEXT("Tuning|Assist") },
		{ TEXT("AssistSpeedCreditRefLow"),      TEXT("Tuning|Assist") },
		{ TEXT("AssistSpeedCreditRefHigh"),     TEXT("Tuning|Assist") },
		{ TEXT("AssistSpeedCreditMaxMult"),     TEXT("Tuning|Assist") },
		{ TEXT("AssistSteerSign"),              TEXT("Tuning|Assist") },
		{ TEXT("ScoreWhitewaterCreditRate"),    TEXT("Tuning|Assist") },
		// Trick scoring (specs/trick-scoring.md)
		{ TEXT("TrickTurnEntryRateDeg"),        TEXT("Tuning|Tricks") },
		{ TEXT("TrickTurnExitRateDeg"),         TEXT("Tuning|Tricks") },
		{ TEXT("TrickTurnMinSeconds"),          TEXT("Tuning|Tricks") },
		{ TEXT("TrickTurnMinSweepDeg"),         TEXT("Tuning|Tricks") },
		{ TEXT("TrickHeadingRateSmoothingSeconds"), TEXT("Tuning|Tricks") },
		{ TEXT("TrickTurnGuardDiscardFraction"), TEXT("Tuning|Tricks") },
		{ TEXT("TrickGradeRefSweepDeg"),        TEXT("Tuning|Tricks") },
		{ TEXT("TrickGradeBiteFloor"),          TEXT("Tuning|Tricks") },
		{ TEXT("TrickGradeDriveFloor"),         TEXT("Tuning|Tricks") },
		{ TEXT("TrickBigTurnGrade"),            TEXT("Tuning|Tricks") },
		{ TEXT("TrickWindowSeconds"),           TEXT("Tuning|Tricks") },
		{ TEXT("TrickWindowPeakMultiplier"),    TEXT("Tuning|Tricks") },
		{ TEXT("TrickTurnExitLumpCredit"),      TEXT("Tuning|Tricks") },
		{ TEXT("TrickChainMaxMultiplier"),      TEXT("Tuning|Tricks") },
		{ TEXT("TrickWindowGuardRate"),         TEXT("Tuning|Tricks") },
		{ TEXT("TrickBoardDifficultyK"),        TEXT("Tuning|Tricks") },

		// Board shadow grounding
		{ TEXT("ShadowMode"),                   TEXT("Tuning|Shadow") },
		{ TEXT("ShadowContactZBias"),           TEXT("Tuning|Shadow") },
		{ TEXT("ShadowMaxGroundingOffset"),     TEXT("Tuning|Shadow") },
		{ TEXT("ShadowWaterlineZBias"),         TEXT("Tuning|Shadow") },
		{ TEXT("ShadowOffsetSmoothingSeconds"), TEXT("Tuning|Shadow") },
		// Tilt
		{ TEXT("TiltPitchDegreesForFullDeflection"), TEXT("Tuning|Tilt") },
		{ TEXT("TiltRollDegreesForFullDeflection"),  TEXT("Tuning|Tilt") },
		// Weight
		{ TEXT("WeightTorqueMagnitude"),        TEXT("Tuning|Weight") },
		{ TEXT("WeightMaxInFront"),             TEXT("Tuning|Weight") },
		{ TEXT("WeightInputSource"),            TEXT("Tuning|Weight") },
		{ TEXT("StickFullDeflectionPx"),        TEXT("Tuning|Weight") },
		{ TEXT("StickDeadzoneFrac"),            TEXT("Tuning|Weight") },
		{ TEXT("StickForeAftReturnSeconds"),    TEXT("Tuning|Weight") },
		{ TEXT("StickLatch"),                   TEXT("Tuning|Weight") },
		{ TEXT("MirrorTouchControls"),          TEXT("Tuning|Weight") },
		{ TEXT("TouchStickRadiusFrac"),         TEXT("Tuning|Weight") },
		{ TEXT("TouchPumpRadiusFrac"),          TEXT("Tuning|Weight") },
		{ TEXT("TouchControlsYFrac"),           TEXT("Tuning|Weight") },
		{ TEXT("TouchControlsHint"),            TEXT("Tuning|Weight") },
		{ TEXT("TouchPumpStyle"),               TEXT("Tuning|Weight") },
		{ TEXT("TouchControlsRestArt"),         TEXT("Tuning|Weight") },
		{ TEXT("TouchControlsRestText"),        TEXT("Tuning|Weight") },
		// Hold-to-pump
		{ TEXT("PumpChargeSeconds"),            TEXT("Tuning|Pump") },
		{ TEXT("PumpReleaseSeconds"),           TEXT("Tuning|Pump") },
		{ TEXT("PumpMinCharge"),                TEXT("Tuning|Pump") },
		{ TEXT("PumpRailOffset"),               TEXT("Tuning|Pump") },
		{ TEXT("PumpLateralAttenuation"),       TEXT("Tuning|Pump") },
		{ TEXT("PumpLateralRampSeconds"),       TEXT("Tuning|Pump") },
		// Stamina (specs/stamina.md). The category is LOAD-BEARING here, not just a HUD header:
		// IsTiredLayerKey keeps Tuning|Stamina out of the tired layer, and with these missing from
		// the table StaminaForceTired saved itself into the tired file and the layer flapped on
		// and off every frame (2026-09-15). The name check in IsTiredLayerKey is the backstop.
		{ TEXT("StaminaEnabled"),               TEXT("Tuning|Stamina") },
		{ TEXT("StaminaPassiveRideSeconds"),    TEXT("Tuning|Stamina") },
		{ TEXT("StaminaPumpCostPerSecond"),     TEXT("Tuning|Stamina") },
		{ TEXT("StaminaTurnFreeRateDeg"),       TEXT("Tuning|Stamina") },
		{ TEXT("StaminaTurnCostPerDegree"),     TEXT("Tuning|Stamina") },
		{ TEXT("StaminaLowFraction"),           TEXT("Tuning|Stamina") },
		{ TEXT("StaminaRecoverySeconds"),       TEXT("Tuning|Stamina") },
		{ TEXT("StaminaTiredExitFraction"),     TEXT("Tuning|Stamina") },
		{ TEXT("StaminaTiredAssistAlpha"),      TEXT("Tuning|Stamina") },
		{ TEXT("StaminaTiredSteerGain"),        TEXT("Tuning|Stamina") },
		{ TEXT("StaminaTiredTrimGain"),         TEXT("Tuning|Stamina") },
		{ TEXT("StaminaTiredGuardEverywhere"),  TEXT("Tuning|Stamina") },
		{ TEXT("StaminaForceTired"),            TEXT("Tuning|Stamina") },
	};
}

void USurfTuningSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	SnapshotDefaults();
	// The board layer arrives later, when a board is applied; until then the global file is all
	// there is, which is also the no-boards-installed case.
	LoadGlobalOverrides(GlobalBaseline);

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &USurfTuningSubsystem::TickDebounce),
		0.0f);

	UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: Initialized with %d tunable defaults. JSON path=%s"),
		Defaults.Num(), *GetJsonPath());
}

void USurfTuningSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (bDirty)
	{
		SaveToDisk();
	}
	Super::Deinitialize();
}

void USurfTuningSubsystem::SnapshotDefaults()
{
	Defaults.Empty();
	Categories.Empty();

	// Every FFloatProperty on this subsystem class is by definition a tunable,
	// so iterate them all — no metadata filter (metadata is editor-only).
	for (TFieldIterator<FFloatProperty> It(GetClass()); It; ++It)
	{
		FFloatProperty* Prop = *It;
		Defaults.Add(Prop->GetFName(), Prop->GetPropertyValue_InContainer(this));
	}

	// Populate the category map from the static table. Names not in the table
	// fall through with an empty category — the tuner panel groups those under
	// a blank header so they're still tunable, just visually unsorted.
	for (const FCategoryPair& P : kCategoryTable)
	{
		Categories.Add(FName(P.Field), FString(P.Category));
	}
}

FFloatProperty* USurfTuningSubsystem::FindFloatProperty(FName PropertyName) const
{
	return CastField<FFloatProperty>(GetClass()->FindPropertyByName(PropertyName));
}

float USurfTuningSubsystem::GetByName(FName PropertyName) const
{
	if (FFloatProperty* Prop = FindFloatProperty(PropertyName))
	{
		return Prop->GetPropertyValue_InContainer(this);
	}
	UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem::GetByName: unknown property '%s'"), *PropertyName.ToString());
	return 0.0f;
}

void USurfTuningSubsystem::SetByName(FName PropertyName, float NewValue)
{
	FFloatProperty* Prop = FindFloatProperty(PropertyName);
	if (!Prop)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem::SetByName: unknown property '%s'"), *PropertyName.ToString());
		return;
	}
	if (!Defaults.Contains(PropertyName))
	{
		// Reject non-tunable properties (those without a Tuning|* category) so
		// stale JSON entries don't accidentally overwrite non-tunable state.
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem::SetByName: property '%s' is not a tunable (no Tuning|* category)"), *PropertyName.ToString());
		return;
	}
	if (bTiredActive && IsTiredLayerKey(PropertyName))
	{
		// Tired layer up: this edit is a TIRED value. It joins the overlay (so it is lifted with the
		// rest when the rider recovers) and is saved to the tired file, not the board's. What the
		// key read before the spell is what recovery restores - captured on first touch.
		if (!TiredRestore.Contains(PropertyName))
		{
			TiredRestore.Add(PropertyName, Prop->GetPropertyValue_InContainer(this));
		}
		TiredOverlay.Add(PropertyName, NewValue);
	}
	Prop->SetPropertyValue_InContainer(this, NewValue);
	bDirty = true;
	SaveDebounceSeconds = kSaveDebounceSeconds;
	OnTuningChanged.Broadcast(PropertyName, NewValue);
}

void USurfTuningSubsystem::SetTransient(FName PropertyName, float NewValue)
{
	FFloatProperty* Prop = FindFloatProperty(PropertyName);
	if (!Prop || !Defaults.Contains(PropertyName))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem::SetTransient: '%s' is not a tunable"), *PropertyName.ToString());
		return;
	}
	Prop->SetPropertyValue_InContainer(this, NewValue);
	OnTuningChanged.Broadcast(PropertyName, NewValue);
}

float USurfTuningSubsystem::GetDefault(FName PropertyName) const
{
	if (const float* V = Defaults.Find(PropertyName))
	{
		return *V;
	}
	return 0.0f;
}

void USurfTuningSubsystem::ResetToDefault(FName PropertyName)
{
	// The effective baseline, not the compiled default: on a board that sets this coefficient,
	// "reset" means "back to what this board rides at". Resetting to the global default would
	// quietly move the player onto a different board for that one value.
	if (!Defaults.Contains(PropertyName))
	{
		return;
	}
	if (bTiredActive && IsTiredLayerKey(PropertyName))
	{
		// Reset while tired = "this is not a tired value after all": back to what the un-tired
		// stack read, and out of the tired file.
		const float* Restore = TiredRestore.Find(PropertyName);
		const float Value = Restore ? *Restore : GetBaseline(PropertyName);
		if (FFloatProperty* Prop = FindFloatProperty(PropertyName))
		{
			Prop->SetPropertyValue_InContainer(this, Value);
		}
		TiredOverlay.Remove(PropertyName);
		TiredRestore.Remove(PropertyName);
		bDirty = true;
		SaveDebounceSeconds = kSaveDebounceSeconds;
		OnTuningChanged.Broadcast(PropertyName, Value);
		return;
	}
	SetByName(PropertyName, GetBaseline(PropertyName));
}

float USurfTuningSubsystem::GetBaseline(FName PropertyName) const
{
	// Most specific first. This is what an edit is diffed against, and what "reset" returns to - so
	// resetting a value the global file pins returns to the GLOBAL value, not the board's, which is
	// the honest answer to "undo my change".
	if (const float* Global = GlobalBaseline.Find(PropertyName))
	{
		return *Global;
	}
	if (const float* Board = BoardBaseline.Find(PropertyName))
	{
		return *Board;
	}
	if (const float* Def = Defaults.Find(PropertyName))
	{
		return *Def;
	}
	return 0.0f;
}

void USurfTuningSubsystem::ApplyBoardBaseline(const TMap<FName, float>& BoardTuning, const FString& BoardId)
{
	// Flush any pending edit first, while the baseline it was made against is still installed -
	// otherwise the diff would be taken against the board being switched TO.
	if (bDirty)
	{
		SaveToDisk();
		bDirty = false;
	}

	// Four layers, most specific wins. Each is a sparse diff against the one beneath, so a layer
	// only ever holds what someone actually moved and deleting it reverts exactly that much:
	//
	//   1. compiled defaults
	//   2. the board profile        Content/Boards/<id>.json    - shipped identity, read-only on device
	//   3. global overrides         Saved/TuningOverrides.json  - hand-edited, every board
	//   4. this board's overlay     Saved/BoardTuning/<id>.json - what the HUD writes
	//
	// 1. Every tunable back to its compiled default, so switching boards cannot accumulate.
	for (const TPair<FName, float>& P : Defaults)
	{
		if (FFloatProperty* Prop = FindFloatProperty(P.Key))
		{
			Prop->SetPropertyValue_InContainer(this, P.Value);
		}
	}

	// 2. The board's own numbers.
	BoardBaseline.Reset();
	ActiveBoardId = BoardId;
	int32 Applied = 0;
	int32 SkippedUnknown = 0;
	for (const TPair<FName, float>& P : BoardTuning)
	{
		FFloatProperty* Prop = FindFloatProperty(P.Key);
		if (!Prop || !Defaults.Contains(P.Key))
		{
			++SkippedUnknown;
			UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: board tuning key '%s' unknown; ignored"),
				*P.Key.ToString());
			continue;
		}
		Prop->SetPropertyValue_InContainer(this, P.Value);
		BoardBaseline.Add(P.Key, P.Value);
		++Applied;
	}

	// 3. Global overrides, so a cross-cutting experiment still wins over any board.
	GlobalBaseline.Reset();
	LoadGlobalOverrides(GlobalBaseline);

	// 4. This board's overlay - the live tuning, and the only layer written from the device.
	LoadBoardOverlay();

	// 5. The tired layer, if a spell is in progress (a board switch mid-ride cannot happen from the
	// hub, but the rebuild must not silently un-tire the rider if it ever does).
	if (bTiredActive)
	{
		TiredRestore.Reset();
		LoadTiredOverlay();
	}

	UE_LOG(LogSurf, Display,
		TEXT("SurfTuningSubsystem: board '%s' baseline - %d coefficient(s), %d unknown skipped; global + overlay applied on top"),
		*BoardId, Applied, SkippedUnknown);

	OnTuningChanged.Broadcast(NAME_None, 0.0f);
}

FString USurfTuningSubsystem::GetBoardOverlayPath(const FString& BoardId)
{
	if (BoardId.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BoardTuning"), BoardId + TEXT(".json"));
}

void USurfTuningSubsystem::LoadBoardOverlay()
{
	const FString Path = GetBoardOverlayPath(ActiveBoardId);
	if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
	{
		return;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *Path))
	{
		return;
	}
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem: cannot parse board overlay %s"), *Path);
		return;
	}

	int32 Applied = 0;
	for (const auto& Pair : Root->Values)
	{
		const FName Key(*Pair.Key);
		double Number = 0.0;
		if (!Pair.Value->TryGetNumber(Number)) { continue; }
		FFloatProperty* Prop = FindFloatProperty(Key);
		if (!Prop || !Defaults.Contains(Key)) { continue; }
		if (IsGlobalOnlyTunable(Key))
		{
			// An overlay written before the save-side guard existed, or hand-added. Warning, not
			// Display: one of these sitting in a board file silently changes what the game does for
			// that board only — RecordIntro did exactly that, suppressing the rails intro.
			UE_LOG(LogSurf, Warning,
				TEXT("SurfTuningSubsystem: IGNORING global dev switch '%s' found in board overlay %s. ")
				TEXT("It belongs in Saved/TuningOverrides.json; delete it from the board file."),
				*Key.ToString(), *Path);
			continue;
		}
		Prop->SetPropertyValue_InContainer(this, (float)Number);
		++Applied;
	}
	UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: %d overlay value(s) for board '%s' from %s"),
		Applied, *ActiveBoardId, *Path);
}

TArray<FName> USurfTuningSubsystem::GetAllPropertyNames() const
{
	TArray<FName> Names;
	Names.Reserve(Defaults.Num());
	for (const TPair<FName, float>& P : Defaults)
	{
		Names.Add(P.Key);
	}
	return Names;
}

FString USurfTuningSubsystem::GetCategoryForProperty(FName PropertyName) const
{
	if (const FString* Found = Categories.Find(PropertyName))
	{
		return *Found;
	}
	return FString();
}

FString USurfTuningSubsystem::GetJsonPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TuningOverrides.json"));
}

void USurfTuningSubsystem::LoadGlobalOverrides(TMap<FName, float>& OutApplied)
{
	const FString Path = GetJsonPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *Path))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem: Failed to read %s"), *Path);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem: Failed to parse JSON %s"), *Path);
		return;
	}

	int32 Applied = 0;
	int32 SkippedUnknown = 0;
	for (const auto& Pair : Root->Values)
	{
		const FName Key(*Pair.Key);
		double Number = 0.0;
		if (!Pair.Value->TryGetNumber(Number))
		{
			continue;
		}
		FFloatProperty* Prop = FindFloatProperty(Key);
		if (!Prop || !Defaults.Contains(Key))
		{
			// Unknown / stale property: ignore but don't crash. Schema drift is
			// expected and AC8 calls this out explicitly.
			++SkippedUnknown;
			UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: stale JSON key '%s' ignored"), *Pair.Key);
			continue;
		}
		Prop->SetPropertyValue_InContainer(this, (float)Number);
		OutApplied.Add(Key, (float)Number);
		++Applied;
	}

	UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: Loaded %d GLOBAL overrides from %s (%d stale skipped)"),
		Applied, *Path, SkippedUnknown);
}

void USurfTuningSubsystem::SaveToDisk()
{
	if (bTiredActive)
	{
		// Every edit made during a tired spell is a tired value. Diffing the live values into the
		// board overlay here would bake the tired feel into the board - the exact contamination the
		// per-board file exists to prevent, one layer up. (Stamina's own knobs, edited during a
		// spell, are the one exception: they stay live and are saved with the board on recovery,
		// when the pending edit is flushed - or not at all if nothing else moves. Acceptable for a
		// dev switch; a Stamina rate the owner wants kept goes in TuningOverrides.json anyway.)
		SaveTiredOverlay();
		return;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	// Resolved up here because it decides WHAT gets written, not just where: the global-only dev
	// switches are skipped for a board overlay but kept for the global file. See IsGlobalOnlyTunable.
	const FString OverlayPath = GetBoardOverlayPath(ActiveBoardId);
	const bool bWritingBoardOverlay = !OverlayPath.IsEmpty();

	// Sparse write against the EFFECTIVE BASELINE - compiled default, overlaid with the board
	// profile, overlaid with the global file. So this holds only what was moved from the board's
	// shipped feel, and deleting it returns that board to exactly that feel.
	//
	// Diffing against the raw default instead would bake the whole board profile in here, and those
	// values would then follow the player onto every OTHER board - which is precisely the
	// cross-contamination this file exists to avoid.
	for (const TPair<FName, float>& P : Defaults)
	{
		FFloatProperty* Prop = FindFloatProperty(P.Key);
		if (!Prop) continue;
		const float Current = Prop->GetPropertyValue_InContainer(this);
		if (!FMath::IsNearlyEqual(Current, GetBaseline(P.Key)))
		{
			if (bWritingBoardOverlay && IsGlobalOnlyTunable(P.Key))
			{
				// Toggled in the HUD, which only ever writes a board overlay. Keeping it out of the
				// file is the whole fix: a dev switch must not ride along with one board's feel.
				UE_LOG(LogSurf, Display,
					TEXT("SurfTuningSubsystem: '%s' is a global dev switch — not saved into board '%s'. ")
					TEXT("Put it in Saved/TuningOverrides.json to make it stick."),
					*P.Key.ToString(), *ActiveBoardId);
				continue;
			}
			Root->SetNumberField(P.Key.ToString(), Current);
		}
	}

	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Root, Writer);

	// The HUD always writes to the ACTIVE BOARD's overlay, never the global file. Tuning the
	// shortboard on the phone must not follow the player onto the foamie. With no board installed
	// there is nothing to be specific about, so it falls back to the global file - the behaviour
	// this project had before boards existed.
	const FString Path = bWritingBoardOverlay ? OverlayPath : GetJsonPath();

	if (!FFileHelper::SaveStringToFile(JsonString, *Path))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem: Failed to write %s"), *Path);
		return;
	}
	UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: wrote %d override(s) for board '%s' to %s"),
		Root->Values.Num(), ActiveBoardId.IsEmpty() ? TEXT("<none>") : *ActiveBoardId, *Path);
}

// ---------------------------------------------------------------------------------------------
//  Tired layer (specs/stamina.md FR3)
// ---------------------------------------------------------------------------------------------

bool USurfTuningSubsystem::IsTiredLayerKey(FName PropertyName) const
{
	// Category first; the name prefix is the backstop for a Stamina knob someone adds without a
	// kCategoryTable row - the first such omission is how the tired layer came to flap.
	return !GetCategoryForProperty(PropertyName).StartsWith(TEXT("Tuning|Stamina"))
		&& !PropertyName.ToString().StartsWith(TEXT("Stamina"));
}

FString USurfTuningSubsystem::GetTiredJsonPath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TiredTuning.json"));
}

void USurfTuningSubsystem::LoadTiredOverlay()
{
	TiredOverlay.Reset();
	const FString Path = GetTiredJsonPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: no %s - tired changes nothing until it exists (open TUNE while tired and move a slider to create it)"), *Path);
		return;
	}
	FString JsonString;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(JsonString, *Path)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonString), Root) || !Root.IsValid())
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem: failed to read/parse %s"), *Path);
		return;
	}
	int32 Applied = 0;
	for (const auto& Pair : Root->Values)
	{
		const FName Key(*Pair.Key);
		double Number = 0.0;
		FFloatProperty* Prop = FindFloatProperty(Key);
		if (!Pair.Value->TryGetNumber(Number) || !Prop || !Defaults.Contains(Key) || !IsTiredLayerKey(Key))
		{
			UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: tired key '%s' unknown or not a tired value; ignored"), *Pair.Key);
			continue;
		}
		if (!TiredRestore.Contains(Key))
		{
			TiredRestore.Add(Key, Prop->GetPropertyValue_InContainer(this));
		}
		Prop->SetPropertyValue_InContainer(this, (float)Number);
		TiredOverlay.Add(Key, (float)Number);
		++Applied;
	}
	UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: tired layer ON - %d value(s) from %s"), Applied, *Path);
}

void USurfTuningSubsystem::SaveTiredOverlay()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	for (const TPair<FName, float>& P : TiredOverlay)
	{
		Root->SetNumberField(P.Key.ToString(), P.Value);
	}
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Root, Writer);
	const FString Path = GetTiredJsonPath();
	if (!FFileHelper::SaveStringToFile(JsonString, *Path))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfTuningSubsystem: Failed to write %s"), *Path);
		return;
	}
	UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: wrote %d tired value(s) to %s"), Root->Values.Num(), *Path);
}

void USurfTuningSubsystem::SetTired(bool bOn)
{
	if (bOn == bTiredActive)
	{
		return;
	}
	if (bDirty)
	{
		// Flush whichever file the pending edit belongs to BEFORE the layer flips, or a board edit
		// would be written as a tired one (or the reverse).
		SaveToDisk();
		bDirty = false;
	}
	if (bOn)
	{
		bTiredActive = true;
		TiredRestore.Reset();
		LoadTiredOverlay();
	}
	else
	{
		// Lift the layer: every key it touched (from the file or from a slider during the spell)
		// goes back to what the un-tired stack read.
		for (const TPair<FName, float>& P : TiredRestore)
		{
			if (FFloatProperty* Prop = FindFloatProperty(P.Key))
			{
				Prop->SetPropertyValue_InContainer(this, P.Value);
			}
		}
		bTiredActive = false;
		TiredRestore.Reset();
		TiredOverlay.Reset();
		UE_LOG(LogSurf, Display, TEXT("SurfTuningSubsystem: tired layer OFF"));
	}
	OnTuningChanged.Broadcast(NAME_None, 0.0f);
}

bool USurfTuningSubsystem::TickDebounce(float DeltaTime)
{
	if (!bDirty)
	{
		return true; // keep ticker alive
	}
	SaveDebounceSeconds -= DeltaTime;
	if (SaveDebounceSeconds <= 0.0f)
	{
		SaveToDisk();
		bDirty = false;
	}
	return true;
}

namespace SurfTuning
{
	USurfTuningSubsystem* Get(const UObject* WorldContext)
	{
		if (!WorldContext) return nullptr;
		const UWorld* World = WorldContext->GetWorld();
		if (!World) return nullptr;
		UGameInstance* GI = World->GetGameInstance();
		if (!GI) return nullptr;
		return GI->GetSubsystem<USurfTuningSubsystem>();
	}
}
