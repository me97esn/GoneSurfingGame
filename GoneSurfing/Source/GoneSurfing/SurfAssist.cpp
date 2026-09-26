// See SurfAssist.h and specs/gradual-control-handoff.md.

#include "SurfAssist.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	// Free functions here get an Assist_ prefix: duplicate names in anonymous namespaces across
	// .cpp files break the Android unity build (which desktop builds never catch).

	/** Horizontal component, normalised. Zero vector if the input is vertical or degenerate. */
	FVector Assist_FlattenNormalize(const FVector& V)
	{
		FVector Flat(V.X, V.Y, 0.0);
		return Flat.GetSafeNormal();
	}

	/** Config section/key for the persistent credit store (FR9). */
	const TCHAR* kAssistConfigSection = TEXT("SurfAssist");
	const TCHAR* kAssistCreditKey     = TEXT("AccumulatedCreditSeconds");
	const TCHAR* kAssistPersistKey    = TEXT("bPersistCredit");

	/** Session-mode backing store. Lives for the process, so it resets on app launch — which is the
	 *  entire point of session mode: a fresh tester gets a genuine first run. */
	float GAssist_SessionCredit = 0.0f;
	// Best ride, per board. A single global best is owned for ever by whichever board scores most
	// easily - once the foamie sets it, a shortboard run can never touch it - so the number stops
	// being a target and quietly punishes trying a harder board. Keyed by board id instead, each
	// board keeps its own record and a board never ridden has none, which is an invitation.
	TMap<FString, float> GAssist_SessionBestRide;
	int32 GAssist_SessionLevel = -1;          // -1 = Auto
	int32 GAssist_SessionLastShownLevel = -1;

	const TCHAR* kAssistLevelKey     = TEXT("ManualLevel");
	const TCHAR* kAssistLastLevelKey = TEXT("LastShownLevel");
	const TCHAR* kAssistBestRideKey  = TEXT("BestRideCredit");

	/** Ride records (best-ride-replay.md FR9). One key per {board, slot}, holding the whole record
	 *  as a single delimited string - so the score and the filename are written by one SetString and
	 *  can never come apart (FR2). Five keys would give five ways to half-write it. */
	const TCHAR* kRideRecordPrefix = TEXT("RideRecord");

	/** Session-mode store, same seam as the credit above: keyed "<slot>_<boardid>". */
	TMap<FString, FString> GAssist_SessionRideRecords;
}

namespace SurfAssist
{
	// ---- CVars ----------------------------------------------------------------------------------

	static TAutoConsoleVariable<int32> CVarAssistEnabled(
		TEXT("surf.assist.enabled"), 1,
		TEXT("Gradual control handoff assist. 1 = on (default), 0 = off entirely."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarAssistAlpha(
		TEXT("surf.assist.alpha"), -1.0f,
		TEXT("Force the assist authority ceiling. -1 = follow the ride-time schedule (default), 0..1 = force.\n")
		TEXT("Set 0 before any physics-tuning session: credit is session-only, so every playtest\n")
		TEXT("otherwise opens at alpha=1 on an assisted board and will mislead the tuning."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarAssistForce(
		TEXT("surf.assist.force"), 0,
		TEXT("Re-enable the assist inside a filtered/unattended run, which normally disables it.\n")
		TEXT("For the assist_guard_* tests only — leaving this on corrupts every snapshot baseline."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarAssistCredit(
		TEXT("surf.assist.credit"), -1.0f,
		TEXT("Force accumulated assist credit, in seconds. -1 = leave alone (default).\n")
		TEXT("Lets a test land anywhere on the fade schedule without riding there."),
		ECVF_Default);

	// FR9 / D2: THE RELEASE FLAG. 0 = session-only (credit resets on app launch, so testers passing
	// the phone around each get a genuine first run). 1 = persist across launches.
	// To ship with persistence, change this default to 1 — or set [SurfAssist] bPersistCredit=True
	// in DefaultGame.ini, which wins over it. No other code changes.
	static TAutoConsoleVariable<int32> CVarAssistPersist(
		TEXT("surf.assist.persist"), 0,
		TEXT("Persist assist credit across app launches. 0 = session-only (default, for tester\n")
		TEXT("hand-arounds), 1 = persist via GConfig. The release setting is 1."),
		ECVF_Default);

	bool IsEnabled()        { return CVarAssistEnabled.GetValueOnGameThread() != 0; }
	float AlphaOverride()   { return CVarAssistAlpha.GetValueOnGameThread(); }
	bool IsForcedInTests()  { return CVarAssistForce.GetValueOnGameThread() != 0; }
	float CreditOverride()  { return CVarAssistCredit.GetValueOnGameThread(); }

	static TAutoConsoleVariable<int32> CVarAssistLastShown(
		TEXT("surf.assist.lastshown"), -2,
		TEXT("Seed the last-shown assist level once (-2 = leave alone, default).\n")
		TEXT("A fresh process starts at -1, which opens the FirstPlay card and pauses the world, so no\n")
		TEXT("headless capture can reach an assist-on ride. Seed to the current level to ride through;\n")
		TEXT("seed above it to open the LevelDown card at startup."),
		ECVF_Default);

	int32 LastShownLevelSeed() { return CVarAssistLastShown.GetValueOnGameThread(); }

	bool IsPersistenceEnabled()
	{
		// Ini wins over the CVar default so the mode can be flipped without a rebuild.
		bool bFromIni = false;
		if (GConfig && GConfig->GetBool(kAssistConfigSection, kAssistPersistKey, bFromIni, GGameIni))
		{
			return bFromIni;
		}
		return CVarAssistPersist.GetValueOnGameThread() != 0;
	}

	// ---- FR9: the credit store ------------------------------------------------------------------

	float LoadCredit()
	{
		if (!IsPersistenceEnabled())
		{
			return GAssist_SessionCredit;
		}
		float Stored = 0.0f;
		if (GConfig && GConfig->GetFloat(kAssistConfigSection, kAssistCreditKey, Stored, GGameIni))
		{
			return FMath::Max(0.0f, Stored);
		}
		return 0.0f;
	}

	void SaveCredit(float CreditSeconds)
	{
		const float Clamped = FMath::Max(0.0f, CreditSeconds);

		// Session mode still writes the in-process value: within one session a graduated player
		// stays graduated across rides. It is only the app restart that clears it.
		GAssist_SessionCredit = Clamped;

		if (!IsPersistenceEnabled() || !GConfig)
		{
			return;
		}
		GConfig->SetFloat(kAssistConfigSection, kAssistCreditKey, Clamped, GGameIni);
		GConfig->Flush(false, GGameIni);
	}

	/** Per-board key. An empty id (no board profiles installed) keeps the original global key, so
	 *  the pre-boards save data is still read and written exactly as before. */
	static FString BestRideKey(const FString& BoardId)
	{
		return BoardId.IsEmpty() ? FString(kAssistBestRideKey)
								 : FString::Printf(TEXT("%s_%s"), kAssistBestRideKey, *BoardId);
	}

	float LoadBestRide(const FString& BoardId)
	{
		if (!IsPersistenceEnabled())
		{
			const float* Found = GAssist_SessionBestRide.Find(BoardId);
			return Found ? *Found : 0.0f;
		}
		float Stored = 0.0f;
		if (GConfig && GConfig->GetFloat(kAssistConfigSection, *BestRideKey(BoardId), Stored, GGameIni))
		{
			return FMath::Max(0.0f, Stored);
		}
		return 0.0f;
	}

	void SaveBestRide(const FString& BoardId, float RideCreditSeconds)
	{
		const float Clamped = FMath::Max(0.0f, RideCreditSeconds);
		GAssist_SessionBestRide.Add(BoardId, Clamped);
		if (!IsPersistenceEnabled() || !GConfig)
		{
			return;
		}
		GConfig->SetFloat(kAssistConfigSection, *BestRideKey(BoardId), Clamped, GGameIni);
		GConfig->Flush(false, GGameIni);
	}

	// ---- Ride records: what the ride list reads (best-ride-replay.md FR9) ------------------------

	/** `RideRecord_Best_shortboard`. Slot before board id so a prefix scan can find every record of
	 *  either kind, which is what ReferencedTraceFiles and ResetCredit both walk. */
	static FString RideRecordKey(const FString& BoardId, ERideSlot Slot)
	{
		const TCHAR* SlotName = (Slot == ERideSlot::Best) ? TEXT("Best") : TEXT("Latest");
		return FString::Printf(TEXT("%s_%s_%s"), kRideRecordPrefix, SlotName,
			BoardId.IsEmpty() ? TEXT("none") : *BoardId);
	}

	/** score|file|board|duration|iso-date. Pipe because a Windows path never contains one and a
	 *  board id is [a-z_]; the date is ISO so a human reading DefaultGame.ini can date a record. */
	static FString RideRecordToString(const FRideRecord& R)
	{
		return FString::Printf(TEXT("%.3f|%s|%s|%.3f|%s"),
			R.ScoreCredit, *R.TraceFile, *R.BoardId, R.DurationSeconds, *R.RecordedAt.ToIso8601());
	}

	static bool RideRecordFromString(const FString& In, FRideRecord& Out)
	{
		TArray<FString> Parts;
		In.ParseIntoArray(Parts, TEXT("|"), /*CullEmpty*/ false);
		if (Parts.Num() < 5)
		{
			return false;   // truncated or from a format that no longer exists: not a record
		}
		Out.ScoreCredit     = FCString::Atof(*Parts[0]);
		Out.TraceFile       = Parts[1];
		Out.BoardId         = Parts[2];
		Out.DurationSeconds = FCString::Atof(*Parts[3]);
		FDateTime::ParseIso8601(*Parts[4], Out.RecordedAt);
		return Out.IsValid();
	}

	bool LoadRideRecord(const FString& BoardId, ERideSlot Slot, FRideRecord& Out)
	{
		const FString Key = RideRecordKey(BoardId, Slot);
		FString Stored;
		if (!IsPersistenceEnabled())
		{
			const FString* Found = GAssist_SessionRideRecords.Find(Key);
			if (!Found) { return false; }
			Stored = *Found;
		}
		else if (!GConfig || !GConfig->GetString(kAssistConfigSection, *Key, Stored, GGameIni))
		{
			return false;
		}
		return RideRecordFromString(Stored, Out);
	}

	void SaveRideRecord(const FString& BoardId, ERideSlot Slot, const FRideRecord& Record)
	{
		if (!Record.IsValid())
		{
			return;   // never store a record that cannot be played
		}
		const FString Key = RideRecordKey(BoardId, Slot);
		const FString Value = RideRecordToString(Record);

		GAssist_SessionRideRecords.Add(Key, Value);
		if (!IsPersistenceEnabled() || !GConfig)
		{
			return;
		}
		GConfig->SetString(kAssistConfigSection, *Key, *Value, GGameIni);
		GConfig->Flush(false, GGameIni);
	}

	TSet<FString> ReferencedTraceFiles()
	{
		TSet<FString> Files;

		auto Collect = [&Files](const FString& Value)
		{
			FRideRecord R;
			if (RideRecordFromString(Value, R))
			{
				Files.Add(R.TraceFile);
			}
		};

		// Session mode is not a subset of persistent mode - a session record references a file that
		// is really on disk - so both stores are walked whichever mode is live. Collecting from the
		// inactive one costs nothing and cannot delete a file it should not.
		for (const TPair<FString, FString>& Pair : GAssist_SessionRideRecords)
		{
			Collect(Pair.Value);
		}
		if (GConfig)
		{
			if (const FConfigSection* Section = GConfig->GetSectionPrivate(kAssistConfigSection, false, true, GGameIni))
			{
				for (const TPair<FName, FConfigValue>& Pair : *Section)
				{
					if (Pair.Key.ToString().StartsWith(kRideRecordPrefix))
					{
						Collect(Pair.Value.GetValue());
					}
				}
			}
		}
		return Files;
	}

	// ---- Manual level and the last-shown alpha ---------------------------------------------------

	float AlphaForLevel(int32 Level)
	{
		if (Level <= 0) { return 0.0f; }
		const int32 Top = kAssistLevelCount - 1;
		return FMath::Clamp((float)Level / (float)Top, 0.0f, 1.0f);
	}

	const TCHAR* LevelName(int32 Level)
	{
		switch (Level)
		{
		case 0:  return TEXT("Off");
		case 1:  return TEXT("Light");
		case 2:  return TEXT("Medium");
		case 3:  return TEXT("Strong");
		case 4:  return TEXT("Full");
		default: return TEXT("Auto");
		}
	}

	int32 LevelForAlpha(float Alpha)
	{
		const int32 Top = kAssistLevelCount - 1;
		return FMath::Clamp(FMath::CeilToInt(FMath::Clamp(Alpha, 0.0f, 1.0f) * (float)Top), 0, Top);
	}

	int32 LoadManualLevel()
	{
		if (!IsPersistenceEnabled()) { return GAssist_SessionLevel; }
		int32 Stored = kAssistLevelAuto;
		if (GConfig && GConfig->GetInt(kAssistConfigSection, kAssistLevelKey, Stored, GGameIni))
		{
			return (Stored < 0 || Stored >= kAssistLevelCount) ? kAssistLevelAuto : Stored;
		}
		return kAssistLevelAuto;
	}

	void SaveManualLevel(int32 Level)
	{
		const int32 Clamped = (Level < 0 || Level >= kAssistLevelCount) ? kAssistLevelAuto : Level;
		GAssist_SessionLevel = Clamped;
		if (!IsPersistenceEnabled() || !GConfig) { return; }
		GConfig->SetInt(kAssistConfigSection, kAssistLevelKey, Clamped, GGameIni);
		GConfig->Flush(false, GGameIni);
	}

	int32 LoadLastShownLevel()
	{
		// One-time seed from the CVar, consumed on first read so the normal save/load path takes over
		// immediately afterwards. Same shape as the skip-paddle start-state injection: a single write
		// that sticks, rather than a value pushed every tick that nothing can then move.
		static bool bSeedConsumed = false;
		if (!bSeedConsumed)
		{
			bSeedConsumed = true;
			const int32 Seed = LastShownLevelSeed();
			if (Seed >= -1)
			{
				SaveLastShownLevel(Seed);
				return Seed;
			}
		}

		if (!IsPersistenceEnabled()) { return GAssist_SessionLastShownLevel; }
		int32 Stored = -1;
		if (GConfig && GConfig->GetInt(kAssistConfigSection, kAssistLastLevelKey, Stored, GGameIni))
		{
			return Stored;
		}
		return -1;
	}

	void SaveLastShownLevel(int32 Level)
	{
		GAssist_SessionLastShownLevel = Level;
		if (!IsPersistenceEnabled() || !GConfig) { return; }
		GConfig->SetInt(kAssistConfigSection, kAssistLastLevelKey, Level, GGameIni);
		GConfig->Flush(false, GGameIni);
	}

	void ResetCredit()
	{
		GAssist_SessionCredit = 0.0f;
		GAssist_SessionBestRide.Reset();
		GAssist_SessionRideRecords.Reset();
		GAssist_SessionLastShownLevel = -1;
		if (IsPersistenceEnabled() && GConfig)
		{
			GConfig->SetFloat(kAssistConfigSection, kAssistCreditKey, 0.0f, GGameIni);

			// Every board's record, not just one key. Bests are stored per board now, so zeroing the
			// bare key would leave four boards still holding scores and hand the next tester a
			// "first run" with a populated scoreboard - the exact thing this function exists to
			// prevent.
			if (FConfigSection* Section = GConfig->GetSectionPrivate(kAssistConfigSection, false, false, GGameIni))
			{
				TArray<FName> Stale;
				for (const TPair<FName, FConfigValue>& Pair : *Section)
				{
					const FString Key = Pair.Key.ToString();

					// Ride records go with the scores. A record left behind points at a trace the
					// prune would then keep for ever (FR3 protects anything a record names), so a
					// reset that cleared only the numbers would quietly pin files on the device.
					// The caller deletes the files themselves - see ASurfboardPawn::ResetAssistCredit.
					if (Key.StartsWith(kAssistBestRideKey) || Key.StartsWith(kRideRecordPrefix))
					{
						Stale.Add(Pair.Key);
					}
				}
				for (const FName& Key : Stale)
				{
					Section->Remove(Key);
				}
			}
			GConfig->Flush(false, GGameIni);
		}
	}

	int32 ScoreFromCredit(float CreditSeconds)
	{
		return FMath::FloorToInt(FMath::Max(0.0f, CreditSeconds) * kScorePerCredit);
	}

	// ---- FR2: the band predicate ----------------------------------------------------------------

	float BandError(float SignedDistanceToCrest, float BandNear, float BandFar)
	{
		if (SignedDistanceToCrest > -BandNear)
		{
			return SignedDistanceToCrest + BandNear;   // > 0: toward the back of the wave
		}
		if (SignedDistanceToCrest < -BandFar)
		{
			return SignedDistanceToCrest + BandFar;    // < 0: down into the flats
		}
		return 0.0f;
	}

	// ---- FR6: schedule and accrual --------------------------------------------------------------

	float AlphaFromCredit(float CreditSeconds, const FTuning& T)
	{
		const float C = FMath::Max(0.0f, CreditSeconds);
		if (C <= T.CreditFullSeconds)
		{
			return 1.0f;
		}
		if (C >= T.CreditZeroSeconds)
		{
			return 0.0f;
		}
		if (C < T.CreditMidSeconds)
		{
			const float Span = FMath::Max(KINDA_SMALL_NUMBER, T.CreditMidSeconds - T.CreditFullSeconds);
			const float U = (C - T.CreditFullSeconds) / Span;
			return FMath::Lerp(1.0f, T.AlphaAtMid, U);
		}
		const float Span = FMath::Max(KINDA_SMALL_NUMBER, T.CreditZeroSeconds - T.CreditMidSeconds);
		const float U = (C - T.CreditMidSeconds) / Span;
		return FMath::Lerp(T.AlphaAtMid, 0.0f, U);
	}

	float SpeedCreditMultiplier(float SpeedCmPerSecond, const FTuning& T)
	{
		// Smoothstepped rather than linear so there is no kink at the reference speeds: the counter's
		// rate is a signal the player reads continuously, and a rate that steps is a rate that looks
		// like a bug.
		const float MaxMult = FMath::Max(1.0f, T.SpeedCreditMaxMult);
		const float Lo = T.SpeedCreditRefLow;
		const float Hi = FMath::Max(Lo + KINDA_SMALL_NUMBER, T.SpeedCreditRefHigh);
		return FMath::Lerp(1.0f, MaxMult, FMath::SmoothStep(Lo, Hi, SpeedCmPerSecond));
	}

	float CreditEarned(float DeltaTime, bool bGuardActive, float SpeedCmPerSecond, const FTuning& T,
		float BrokenAmount)
	{
		if (DeltaTime <= 0.0f)
		{
			return 0.0f;
		}
		const float GuardRate = bGuardActive ? T.AssistedCreditRate : 1.0f;
		// FR12 (trick-scoring.md), retired 2026-09-18: WhitewaterCreditRate is 1.0 by default, so this
		// factor is a no-op until a signal that means "board IN broken water" exists. Kept in the
		// product so the trick window (Base * (mult - 1)) would drop with it and nothing be scored
		// twice if it is ever priced again.
		const float WaveRate = FMath::Lerp(1.0f, T.WhitewaterCreditRate, FMath::Clamp(BrokenAmount, 0.0f, 1.0f));
		return DeltaTime * GuardRate * WaveRate * SpeedCreditMultiplier(SpeedCmPerSecond, T);
	}

	// ---- The controller -------------------------------------------------------------------------

	FOutput Evaluate(const FInputs& In, const FTuning& T, float Alpha, float DeltaTime, FState& State)
	{
		FOutput Out;
		Out.WeightRight   = FMath::Clamp(In.PlayerWeightRight, 0.0f, 1.0f);
		Out.WeightInFront = FMath::Clamp(In.PlayerWeightInFront, 0.0f, 1.0f);
		Out.TrimTarget    = T.TrimNeutral;

		if (DeltaTime <= 0.0f)
		{
			return Out;
		}

		// ---- Pump release. Decided before anything else, because it relaxes both the rate limit and
		// the trim term below. A pump is a committed fore/aft input; the assist's job on that axis is
		// to correct neglect, never to cancel intent. The release window outlives the stroke so the
		// assist cannot snap the board back between strokes.
		if (In.PumpInput > T.PumpInputThreshold)
		{
			State.PumpReleaseRemaining = T.PumpReleaseSeconds;
		}
		else
		{
			State.PumpReleaseRemaining = FMath::Max(0.0f, State.PumpReleaseRemaining - DeltaTime);
		}
		const bool bPumpRelease = State.PumpReleaseRemaining > 0.0f;
		Out.bPumpRelease = bPumpRelease;

		// ---- FR5: rate-limit the player's STEERING, hard at high alpha, not at all at alpha 0.
		// A rate limit and not a gain reduction: reduced gain makes the board feel dead and lies
		// about how responsive it will be later. This keeps full authority and full travel and only
		// refuses to get there in one frame. Framerate-independent by construction (per-second rate
		// times DeltaTime).
		//
		// FORE/AFT IS DELIBERATELY NOT LIMITED. Pumping lives on that axis and a pump is fast by
		// definition: at 1.5 units/s the limiter allows 0.375 units per 250ms half-stroke, which
		// shaves the stroke going out AND coming back. On device it removed the pitch oscillation
		// from pumping entirely (confirmed 2026-08-26 by lifting this one value). Gating it on a
		// pump-detection threshold was the first attempt and is the wrong shape — it leaves a cliff
		// where a gentle pump is still eaten, and it makes a core mechanic depend on a magic number.
		//
		// Nothing is lost by dropping it. The jerky-input problem FR5 exists for is a STEERING
		// problem: this board over-carves on a small lateral shift. The fore/aft failure modes
		// (nose-dive, stall) are what the continuous trim term and MaxAuthority already cover.
		if (!State.bLimiterSeeded)
		{
			State.LimitedRight   = Out.WeightRight;
			State.bLimiterSeeded = true;
		}
		const float RateLimit = FMath::Lerp(T.RateLimitAtNoAssist, T.RateLimitAtFullAssist,
			FMath::Clamp(Alpha, 0.0f, 1.0f));
		const float MaxStep = RateLimit * DeltaTime;
		State.LimitedRight += FMath::Clamp(Out.WeightRight - State.LimitedRight, -MaxStep, MaxStep);
		State.LimitedInFront = Out.WeightInFront;   // straight through

		const float PlayerRight   = State.LimitedRight;
		const float PlayerInFront = State.LimitedInFront;

		// ---- Wave frame. Everything below is derived from vectors read at runtime; no axis is
		// assumed anywhere (the board mesh is rotated 90 degrees, and the wave frame is per-level).
		const FVector BackH = Assist_FlattenNormalize(In.WaveBackDirection);
		const FVector FwdH  = Assist_FlattenNormalize(In.BoardForward);
		const FVector LeftH = Assist_FlattenNormalize(In.BoardLeft);
		if (BackH.IsNearlyZero() || FwdH.IsNearlyZero() || LeftH.IsNearlyZero())
		{
			// Degenerate frame (flat water before the wave resolves, or a vertical board). Fall
			// through with the player's input untouched rather than pushing in a guessed direction.
			Out.WeightRight   = PlayerRight;
			Out.WeightInFront = PlayerInFront;
			return Out;
		}
		// The wave breaks toward the front face, so "down the face" is the opposite of "toward the
		// back of the wave".
		const FVector FaceDown = -BackH;

		// ---- FR2: the guard band. Zero error inside the rideable strip; signed distance outside it.
		// + = too far toward the back of the wave (about to lose it out the back)
		// - = too far down the face (about to stall in the flats)
		// Same free function the debug drawing calls, so the drawn band is the enforced band -
		// except under bGuardEverywhere (tired, see FTuning): the controller then runs on a band
		// collapsed onto the crest, so the whole face is "too far down" and the reversed guard
		// pushes down it from everywhere; the drawing and the score keep the authored band.
		const float CtlBandNear = T.bGuardEverywhere ? 0.0f : T.BandNear;
		const float CtlBandFar  = T.bGuardEverywhere ? 0.0f : T.BandFar;
		const float BandError = SurfAssist::BandError(In.SignedDistanceToCrest, CtlBandNear, CtlBandFar);
		Out.BandError = BandError;
		Out.bGuardActive = (BandError != 0.0f);
		Out.bOutsideBand = T.bGuardEverywhere
			? (SurfAssist::BandError(In.SignedDistanceToCrest, T.BandNear, T.BandFar) != 0.0f)
			: Out.bGuardActive;

		// Inside the band the steering correction is EXACTLY zero, and the whole cascade below is
		// skipped. This deadband has to sit on the cascade's OUTPUT, not just on the outer loop's
		// setpoint: with a zero band error the outer loop asks for heading 0, and an inner loop that
		// acted on that would hold the board straight down the line all the way through a carve —
		// a heading-hold controller, not a guard. (That is exactly what it did until measured on
		// 2026-08-25: silent-guard ticks were still emitting +/-0.03 of correction.)
		//
		// The clean-slate reset matters too. Without it, the D term re-enters carrying a rate
		// estimate from before the quiet stretch and kicks on the first active tick.
		if (!Out.bGuardActive)
		{
			State.PrevHeadingError = 0.0f;
			State.bHasPrevHeadingError = false;
			State.SmoothedHeadingRate = 0.0f;

			// Trim still runs — it is continuous by design (FR4) — so fall through with steering
			// zeroed rather than returning.
		}

		// ---- FR3: outer loop. Band error sets a TARGET HEADING, not a weight. Driving weight
		// straight off position error puts two integrators in the loop (weight commands a turn rate,
		// which is the derivative of heading, which is the derivative of position) and it oscillates.
		// Smoothstepped so the guard ramps in over BandSoftness with no step at the edge.
		const float BandRamp = FMath::SmoothStep(0.0f, T.BandSoftness, FMath::Abs(BandError));
		const float TargetHeadingSin = FMath::Sign(BandError) * T.MaxHeadingSin * BandRamp;
		Out.TargetHeadingSin = TargetHeadingSin;

		// Current heading, expressed as how much of the board's forward points down the face.
		// Needs no down-the-line axis and no ride-direction latch: it reads the same whichever way
		// along the wave the board is travelling.
		const float HeadingSin = (float)FVector::DotProduct(FwdH, FaceDown);
		Out.HeadingSin = HeadingSin;

		const float HeadingError = TargetHeadingSin - HeadingSin;
		Out.HeadingError = HeadingError;

		// ---- FR3: inner loop. PD on heading error -> a carve command.
		// The rate is low-passed before the D term sees it. Raw, it made the controller bang-bang:
		// heading on a board this roll-stiff is a noisy signal, differentiating noise amplifies it,
		// and the correction ended up pinned to alternating clamps with a median of ~0 (measured
		// headless, 2026-08-25). Time-constant form, so the smoothing is framerate-independent.
		float RawHeadingRate = 0.0f;
		if (State.bHasPrevHeadingError)
		{
			RawHeadingRate = (HeadingError - State.PrevHeadingError) / DeltaTime;
		}
		State.PrevHeadingError = HeadingError;
		State.bHasPrevHeadingError = true;

		const float Tau = FMath::Max(KINDA_SMALL_NUMBER, T.HeadingRateSmoothingSeconds);
		const float SmoothK = 1.0f - FMath::Exp(-DeltaTime / Tau);
		State.SmoothedHeadingRate = FMath::Lerp(State.SmoothedHeadingRate, RawHeadingRate, SmoothK);

		const float CarveMagnitude = T.HeadingP * HeadingError + T.HeadingD * State.SmoothedHeadingRate;

		// Which lean carves toward the face? FluidDynamics applies the commanded-lean carve along
		// sign(amountToTheRight - 0.5) * board.left, so the lean that steers toward FaceDown is the
		// one whose carve direction points that way. Derived rather than assumed, which is what makes
		// this correct for a board riding either direction along the wave — reverse the ride and
		// board.left points up the face instead, and the sign flips with it.
		const float CarveDot = (float)FVector::DotProduct(LeftH, FaceDown);
		float CarveSign = State.LastCarveSign;
		if (FMath::Abs(CarveDot) > 0.25f)
		{
			CarveSign = FMath::Sign(CarveDot);
			State.LastCarveSign = CarveSign;
		}
		// else: board is pointing straight down the face and the dot is crossing zero. Hold the last
		// usable sign rather than flapping. Outside the operating range anyway (target is clamped to
		// MaxHeadingSin), so this is a guard, not a regime.

		// SteerGain scales the correction AND its ceiling together (see FTuning): at gain 1 this is
		// exactly the shipping clamp; past ~2 the guard is bang-bang full-lean, which is what
		// makes its direction visible from the deck. Gain 0 mutes steering and leaves trim running.
		const float SteerAuthority = T.MaxAuthority * FMath::Max(0.0f, T.SteerGain);
		const float SteerCorrection = Out.bGuardActive
			? FMath::Clamp(CarveMagnitude * CarveSign * T.SteerSign * T.SteerGain,
				-SteerAuthority, SteerAuthority)
			: 0.0f;
		Out.SteerCorrection = SteerCorrection;

		// ---- FR4: trim. Continuous, no deadband — a beginner has no intuition for fore/aft and the
		// failures it prevents (nose-dive, stall) end the ride with no legible cause.
		const float Speed = (float)In.BoardVelocity.Size();
		const float SpeedExcess = FMath::Max(0.0f, Speed - T.TrimSpeedRef) /
			FMath::Max(KINDA_SMALL_NUMBER, T.TrimSpeedRef);
		const float TrimTarget = FMath::Clamp(
			T.TrimNeutral
			- T.TrimSlopeGain * FMath::Clamp(In.BoardWideSlopeSin, 0.0f, 1.0f)
			- T.TrimSpeedGain * SpeedExcess,
			T.TrimTargetMin, T.TrimTargetMax);
		Out.TrimTarget = TrimTarget;

		const float TrimAuthority = T.MaxAuthority * FMath::Max(0.0f, T.TrimGain);
		const float TrimCorrection = bPumpRelease
			? 0.0f
			: FMath::Clamp(T.TrimP * (TrimTarget - PlayerInFront) * T.TrimGain,
				-TrimAuthority, TrimAuthority);
		Out.TrimCorrection = TrimCorrection;

		// ---- FR1: additive, never a lerp toward an assist target. At every alpha the player's input
		// still moves the board the way they commanded; the assist only biases the result. A lerp
		// would mean their input does nothing at alpha 1, which teaches them the wrong system and
		// then takes it away.
		const float A = FMath::Clamp(Alpha, 0.0f, 1.0f);
		Out.WeightRight   = FMath::Clamp(PlayerRight   + A * SteerCorrection, 0.0f, 1.0f);
		Out.WeightInFront = FMath::Clamp(PlayerInFront + A * TrimCorrection,  0.0f, 1.0f);

		return Out;
	}
}
