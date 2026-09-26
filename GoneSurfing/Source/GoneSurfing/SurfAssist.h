// The gradual control handoff: a closed-loop assist whose authority fades as the player
// accumulates unassisted ride time. See specs/gradual-control-handoff.md.
//
// Why this is a closed-loop controller and not a fading autopilot: AStateTriggerAutoPilot is open
// loop (FStateTriggerStep holds fixed weightRight/weightNose; its trigger conditions only select
// which canned command plays, they never shape it). The moment the player perturbs the board off the
// recorded trajectory those numbers are wrong for the state the board is actually in, so blending
// them in fights the player. This controller reads live wave-relative state every tick and emits a
// CORRECTION, so it is valid from any board state and at any authority.
//
// Two rules this file keeps, same as SurfTilt.h:
//   - No UObject, no world, no actors. Everything the controller needs arrives in FInputs.
//     (The credit store at the bottom is the one impure part — it touches GConfig.)
//   - All mutable controller state is a caller-owned FState, never a global. The pawn owns one.
//
// The two shapes that matter, and why they differ (FR2/FR4):
//   - STEERING is a guard band. Zero correction while the board is inside the rideable strip of
//     face; it only speaks at the two edges where a beginner's ride actually ends (drifting out the
//     back, or sliding into the flats). So the player's carve is never damped mid-face, and what
//     they feel is the wave keeping them on it.
//   - TRIM is continuous. Fore/aft is invisible to a beginner and getting it wrong ends the ride
//     with no legible cause (nose-dive, or stall), so it is nudged the whole time.

#pragma once

#include "CoreMinimal.h"

namespace SurfAssist
{
	// ---------------------------------------------------------------------------------------------
	// Tuning. The pawn fills this from USurfTuningSubsystem each tick so the whole feel is dialled in
	// through Saved/TuningOverrides.json with no rebuild. Defaults here are the first guesses from the
	// spec and are what runs if the subsystem is unavailable.
	// ---------------------------------------------------------------------------------------------
	struct FTuning
	{
		// ---- FR2: the guard band, in signedDistanceToCrest units (cm, negative = on the front face)
		/** Closest to the crest the board may sit before the "out the back" edge engages. */
		float BandNear = 100.0f;
		/** Furthest down the face before the "into the flats" edge engages. */
		float BandFar = 200.0f;
		/** How far outside an edge the correction takes to reach full strength. Smoothstepped, so
		 *  the guard has no step at the boundary — it cannot be felt switching on. */
		float BandSoftness = 150.0f;
		/** TIRED only (StaminaTiredGuardEverywhere): run the controller on a band collapsed onto
		 *  the crest (near = far = 0) instead of the authored one. Every point on the face then
		 *  reads "too far down", so the REVERSED guard pushes the board down the face from
		 *  everywhere on it, at full strength from BandSoftness below the crest - no dead zone,
		 *  and no point where the push is zero. A reversed guard with the authored dead zone never
		 *  touched a rider who held the pocket and made no big movement, and on the foamie that
		 *  rider surfed for ever (owner, 2026-09-22). Not a narrowed band about its centre line:
		 *  signedDistanceToCrest is a 50 cm scan, so a centre line is really a 50 cm bucket where
		 *  the error reads exactly 0, and the un-steered board sat in it for 60 % of a run.
		 *  Scoring never sees this band - FOutput::bOutsideBand reads the authored one - and
		 *  neither does the band's debug drawing, which draws the score's band. */
		bool bGuardEverywhere = false;

		// ---- FR3: the cascade
		/** Largest heading offset from down-the-line the outer loop will ask for, as a sine.
		 *  0.5 ~= 30 degrees. */
		float MaxHeadingSin = 0.5f;
		/** Inner loop: heading error (sine) -> weight units. */
		float HeadingP = 0.6f;
		/** Inner loop damping, on d(heading error)/dt. Without it the loop weaves: a weight shift
		 *  commands a turn RATE, so heading lags the command by an integrator. Measured down from
		 *  0.15 — see HeadingRateSmoothingSeconds. */
		float HeadingD = 0.03f;
		/** Low-pass on the heading rate before the D term sees it. At 0.15 raw, the D term drove the
		 *  correction to its clamp on alternate ticks (measured 2026-08-25: half of all samples sat
		 *  at +/-MaxAuthority with a median of ~0 — bang-bang, not damping). Heading is a noisy
		 *  signal on a board this roll-stiff, and differentiating noise amplifies it. */
		float HeadingRateSmoothingSeconds = 0.15f;

		// ---- FR1: authority clamp, in weight units (the 0..1 range the player writes)
		/** Ceiling on |correction| per axis, in weight units. Player-tuned to 10 on device
		 *  (2026-08-27), which means the clamp no longer binds: the real ceiling is what the PD terms
		 *  produce, ~0.3 steering and up to 0.35 trim. 0.15 was cutting the controller off below its
		 *  own output. See the tuning subsystem for what that costs FR1's guarantee. */
		float MaxAuthority = 10.0f;

		// ---- Diagnosis amplifiers. Default 1.0 = the shipping controller, untouched.
		//
		// These exist because the guard's direction had never been PROVEN. Its correction is small
		// by design (FR1), it only speaks at the band edges, and on the deck "it caught me" and "the
		// wave caught me" look identical — so a sign error could sit here indefinitely.
		//
		// Crank a gain past ~2 and the channel becomes bang-bang: the correction swamps the whole
		// 0..1 weight range, so while the guard is speaking the board is pinned to full lean and the
		// player's own input cannot fight it. The answer is then unmissable — the board either
		// carves back toward the band (sign right) or drives itself off the wave in a second or two
		// (sign wrong, set SteerSign = -1).
		//
		// Deliberately scale the MaxAuthority clamp too. Clamping an amplified correction back to
		// the shipping ceiling would defeat the point of amplifying it, and FR1's guarantee is not
		// something these are meant to respect — they are a dev instrument, not a feel knob.
		//
		// 0.0 mutes a channel outright, which is the other half of the tool: the two corrections
		// share one board, and a trim term slamming the nose ends the ride before the steering
		// question gets an answer. Test one channel at a time.
		float SteerGain = 1.0f;
		float TrimGain = 1.0f;

		// ---- FR4: trim
		/** Trim target on flat water at reference speed. 0.5 = centred. */
		float TrimNeutral = 0.5f;
		/** Weight moved off the nose per unit boardWideSlopeSin. Steeper face -> more tail. Halved
		 *  from 0.35 on measurement: a riding slopeSin never exceeds ~0.31, and 0.35 put the target
		 *  within a whisker of TrimTargetMin for an ordinary ride. Trim is continuous, so a bias
		 *  that large is applied the whole time. */
		float TrimSlopeGain = 0.20f;
		/** Speed (cm/s) above which trim starts shifting back. */
		float TrimSpeedRef = 1400.0f;
		/** Weight moved off the nose per TrimSpeedRef of excess speed. */
		float TrimSpeedGain = 0.10f;
		float TrimTargetMin = 0.30f;
		float TrimTargetMax = 0.65f;
		/** Trim error -> weight units, before the MaxAuthority clamp. */
		float TrimP = 0.50f;

		// ---- Pumping: the assist must not fight a DELIBERATE fore/aft input.
		//
		// Trim assist exists to correct a beginner's NEGLECT of fore/aft, not to override their
		// intent. A pump is the opposite of neglect: a committed, rhythmic fore/aft swing. Left
		// alone, the trim term reads the swing as error and cancels it — at TrimP 0.5 with a target
		// near 0.39, any command past ~0.69 draws the full -0.15 clamp, halving the excursion — and
		// the FR5 rate limit shaves what is left, because a pump is fast by definition. Reported from
		// device 2026-08-26: pitch stopped oscillating during pumping with the assist on.
		/** PumpInput above this counts as "the player is pumping". */
		float PumpInputThreshold = 0.15f;
		/** How long after the last pump the fore/aft assist stays out of the way. Covers the gap
		 *  between strokes so it cannot snap back between them. */
		float PumpReleaseSeconds = 0.7f;

		// ---- FR5: input rate limit on STEERING ONLY (NOT a gain reduction, and NOT applied fore/aft
		// — pumping lives on that axis and is fast by definition; see Evaluate).
		/** Max steering weight change per second at alpha = 1. Full travel is 1.0 units. */
		float RateLimitAtFullAssist = 1.5f;
		/** Max steering weight change per second at alpha = 0. Large enough to be no limit at all. */
		float RateLimitAtNoAssist = 1000.0f;

		// ---- FR6: the fade schedule, in seconds of accumulated credit
		float CreditFullSeconds = 30.0f;    // alpha holds at 1.0 below this
		float CreditMidSeconds  = 90.0f;    // alpha = AlphaAtMid here
		float CreditZeroSeconds = 180.0f;   // alpha reaches 0 here
		float AlphaAtMid = 0.4f;
		/** Credit rate while the steering guard is actively correcting. Below 1.0 because those
		 *  seconds were earned by the assist, not the player — without this the player graduates on
		 *  time the assist bought them. Above 0.0 so someone who never quite holds it alone still
		 *  graduates eventually, just slower. */
		float AssistedCreditRate = 0.25f;

		/** specs/trick-scoring.md FR12 (RETIRED 2026-09-18): the credit rate while
		 *  ASharedCalculations::brokenAmount = 1. Held at 1.0 - whitewater scores like the face -
		 *  because brokenAmount reads foam NEAR the board, not the board IN broken water, and
		 *  false-positives through every turn toward the pocket. The factor stays so a real
		 *  broken-water signal can price the foam later without re-plumbing. */
		float WhitewaterCreditRate = 1.0f;

		// ---- specs/ride-score-counter.md FR2: speed multiplies the credit rate.
		//
		// Riding fast unassisted earns faster than drifting unassisted. D2 of that spec picked speed
		// over trick scoring precisely because it does not change what graduation MEANS: riding
		// faster is still surviving, only harder, so it sharpens the survival measure rather than
		// opening a route around it.
		//
		// Consequence: credit stops being literally seconds and becomes EFFECTIVE seconds. The
		// CreditXxxSeconds thresholds keep their names (renaming churns every saved tuning override)
		// but they are thresholds in credit, not wall-clock. Nothing player-facing says "seconds"
		// anyway - the player is shown points, never time.
		/** At or below this speed (cm/s) the multiplier is 1. */
		float SpeedCreditRefLow = 1400.0f;
		/** At or above this speed (cm/s) the multiplier is SpeedCreditMaxMult. */
		float SpeedCreditRefHigh = 2600.0f;
		/** Ceiling on the speed multiplier. Deliberately small: a large one turns the fade into a
		 *  speed-run and reopens the question D2 closed. 1.0 disables the multiplier entirely. */
		float SpeedCreditMaxMult = 2.0f;

		/** Escape hatch for a mis-signed steer correction found on device: -1 flips it. The sign is
		 *  derived at runtime from board and wave vectors (see EvaluateAssist), so this should never
		 *  be needed — but a one-value fix beats a recompile if it is. */
		float SteerSign = 1.0f;
	};

	// ---------------------------------------------------------------------------------------------
	// Per-tick inputs. All world-space; none of it is read from actors in here.
	// ---------------------------------------------------------------------------------------------
	struct FInputs
	{
		/** ASharedCalculations::signedDistanceToCrest (cm). Positive = behind the crest (out the
		 *  back), negative = on the front face. The rideable band lives at negative values. */
		float SignedDistanceToCrest = 0.0f;

		/** ASharedCalculations::boardWideSlopeSin. 0 on flat water, ~1 on a vertical wall. */
		float BoardWideSlopeSin = 0.0f;

		/** Direction toward the BACK of the wave (SharedCalculations::resolvedWaveBackDirection, or
		 *  waveBackDirection if unresolved). Need not be normalised or horizontal. */
		FVector WaveBackDirection = FVector(1.0, 0.0, 0.0);

		/** Board forward in world space. NEVER assume an axis: the mesh is rotated 90 degrees and
		 *  board.forwards is local +Y. Read it from the actor and pass it in. */
		FVector BoardForward = FVector(0.0, 1.0, 0.0);

		/** Board left in world space (local -X). Used to resolve which way a commanded lean carves,
		 *  which is what makes the steer sign self-correcting for either ride direction. */
		FVector BoardLeft = FVector(-1.0, 0.0, 0.0);

		/** Board linear velocity, cm/s. Only its magnitude is used (trim scheduling). */
		FVector BoardVelocity = FVector::ZeroVector;

		/** What the player commanded this tick, in AWeightDistribution units: 0..1, 0.5 centred. */
		float PlayerWeightRight = 0.5f;
		float PlayerWeightInFront = 0.5f;

		/** ASurfboardPawn::PumpInput, 0..1. Non-zero means the player is deliberately working the
		 *  board fore/aft, and the fore/aft assist must get out of the way. */
		float PumpInput = 0.0f;
	};

	// ---------------------------------------------------------------------------------------------
	// Controller state. One instance per pawn, reset at each handoff.
	// ---------------------------------------------------------------------------------------------
	struct FState
	{
		/** Rate-limited player steering (FR5). Seeded on the first tick of a ride. LimitedInFront is
		 *  passed straight through — the fore/aft axis is not rate-limited. */
		float LimitedRight = 0.5f;
		float LimitedInFront = 0.5f;
		bool  bLimiterSeeded = false;

		/** Previous heading error, for the D term, and its low-passed rate. */
		float PrevHeadingError = 0.0f;
		float SmoothedHeadingRate = 0.0f;
		bool  bHasPrevHeadingError = false;

		/** Last usable sign of dot(boardLeft, faceDown). Held through the degenerate pose where the
		 *  board points straight down the face and the dot crosses zero. */
		float LastCarveSign = 1.0f;

		/** Time left in the post-pump release window. While > 0 the fore/aft assist stands down. */
		float PumpReleaseRemaining = 0.0f;

		/** Accumulated unassisted ride credit, seconds (FR6). Loaded at handoff, saved at ride end. */
		float CreditSeconds = 0.0f;

		/** Authority ceiling for THIS ride. Latched at handoff and held — see FR6 "constant within a
		 *  ride". A board that changes feel at second 30 of a good wave produces a fall the player
		 *  cannot attribute to anything they did. */
		float RideAlpha = 1.0f;

		/** True once a ride is underway and credit is accruing. */
		bool bRideActive = false;

		void Reset()
		{
			*this = FState();
		}
	};

	// ---------------------------------------------------------------------------------------------
	// Per-tick output.
	// ---------------------------------------------------------------------------------------------
	struct FOutput
	{
		/** What to write to AWeightDistribution. Player input plus alpha-scaled correction, clamped. */
		float WeightRight = 0.5f;
		float WeightInFront = 0.5f;

		/** True while the steering guard is doing something — i.e. the board is outside the band
		 *  the CONTROLLER runs on (the authored band, or the crest under bGuardEverywhere). */
		bool bGuardActive = false;
		/** True while the board is outside the AUTHORED band, bGuardEverywhere ignored. This is
		 *  what scoring reads (the FR6 credit rate, trick-scoring FR5): the pocket the score pays
		 *  for does not move when tired moves the guard's. Equals bGuardActive otherwise. */
		bool bOutsideBand = false;

		// Diagnostics, for the "assist" debug category.
		float BandError = 0.0f;        // cm outside the band; 0 inside. + = too far back.
		float TargetHeadingSin = 0.0f;
		float HeadingSin = 0.0f;
		float HeadingError = 0.0f;
		float SteerCorrection = 0.0f;  // pre-alpha, post-clamp
		float TrimCorrection = 0.0f;   // pre-alpha, post-clamp
		float TrimTarget = 0.5f;
		bool  bPumpRelease = false;    // fore/aft assist stood down for a pump
	};

	/** Run one tick of the controller. Alpha is the ride's authority ceiling (FState::RideAlpha),
	 *  passed explicitly so callers can force it from a CVar without touching the latch.
	 *  DeltaTime must be > 0. Pure: everything mutated lives in State. */
	FOutput Evaluate(const FInputs& In, const FTuning& T, float Alpha, float DeltaTime, FState& State);

	/** How far outside the guard band the board is, in cm. 0 while inside, and that zero is what
	 *  silences the steering channel entirely (FR2).
	 *    > 0  too far toward the back of the wave — about to lose it out the back
	 *    < 0  too far down the face — about to stall in the flats
	 *
	 *  Deliberately a free function taking the two edges rather than a method or an FTuning: the
	 *  band's debug drawing (ASharedCalculations) calls exactly this, so a picture of the band can
	 *  never disagree with the band that is actually enforced. A drawn band that lies is worse than
	 *  no drawing at all — it sends you hunting for a controller bug that is really a geometry bug,
	 *  or the reverse. */
	float BandError(float SignedDistanceToCrest, float BandNear, float BandFar);

	/** FR6 schedule: accumulated credit (seconds) -> authority ceiling. Monotonically non-increasing
	 *  in Credit. */
	float AlphaFromCredit(float CreditSeconds, const FTuning& T);

	/** Credit earned this tick. Full rate only while the steering guard is silent, scaled up by
	 *  board speed (ride-score-counter.md FR2). Speed is cm/s; pass 0 to opt out of the multiplier. */
	float CreditEarned(float DeltaTime, bool bGuardActive, float SpeedCmPerSecond, const FTuning& T,
		float BrokenAmount = 0.0f);

	/** The speed component of the credit rate on its own, 1.0 .. SpeedCreditMaxMult. Exposed so the
	 *  debug log and any future "why is this ticking fast" readout quote the same number the accrual
	 *  used, rather than a second estimate of it. */
	float SpeedCreditMultiplier(float SpeedCmPerSecond, const FTuning& T);

	// ---------------------------------------------------------------------------------------------
	// specs/ride-score-counter.md D4 - the player is shown POINTS. Seconds are an internal unit and
	// appear nowhere the player can see: not in the ride, not on a card, not in the copy.
	//
	// This is a presentation constant and nothing more. It does not make the score a second quantity
	// - the schedule thresholds stay in credit where AlphaFromCredit already reads them, and the
	// counter on screen is the same accumulator that drives the fade. That identity is the whole
	// point of the feature: a score that graduates you off a DIFFERENT number leaves the step-down
	// unexplained, which is what the counter exists to fix.
	//
	// 10 puts the fade schedule on round numbers a card can quote (300 / 900 / 1800) and ticks fast
	// enough to feel alive - a 1/second counter reads as static.
	// ---------------------------------------------------------------------------------------------
	static constexpr float kScorePerCredit = 10.0f;

	/** Credit (internal seconds) -> the points the player sees. */
	int32 ScoreFromCredit(float CreditSeconds);

	// ---------------------------------------------------------------------------------------------
	// FR9 — the credit store. THE ONLY two functions in the codebase that touch the stored value.
	//
	// Credit is session-only today: it resets on app launch, because the phone gets handed to a fresh
	// tester and every tester must start at alpha = 1.0 (D2, 2026-08-25). At release it needs to
	// persist. Both modes are implemented here and selected by IsPersistenceEnabled(), so shipping is
	// a flag flip whose other branch has already been exercised — not new code written under release
	// pressure.
	//
	// TO SHIP WITH PERSISTENCE: change the default of `surf.assist.persist` to 1 in SurfAssist.cpp
	// (or set [SurfAssist] bPersistCredit=True in DefaultGame.ini). Nothing else changes.
	// ---------------------------------------------------------------------------------------------

	/** 0.0 on a fresh session, or the persisted value when persistence is on. */
	float LoadCredit();

	/** No-op in session mode; a GConfig write in persistent mode. */
	void SaveCredit(float CreditSeconds);

	// ---- Manual level (FR7). -1 = Auto, follow the ride-time schedule. 0..kAssistLevelCount-1 pin
	// alpha and stop the fade until the player sets Auto again — conventional difficulty-setting
	// behaviour: once they have chosen, the board stops changing under them.
	static constexpr int32 kAssistLevelCount = 5;
	static constexpr int32 kAssistLevelAuto  = -1;

	/** Alpha for a manual level. Level 0 = off, kAssistLevelCount-1 = full. */
	float AlphaForLevel(int32 Level);

	/** Player-facing name for a level, or "Auto". */
	const TCHAR* LevelName(int32 Level);

	/** The discrete level an alpha reads as, 0..kAssistLevelCount-1. THE single definition of "what
	 *  level am I on", shared by the badge's lit segments, the panel's pips and the level-down
	 *  trigger — they disagreed before, so the card announced a step down while the badge still
	 *  showed the old level.
	 *
	 *  Ceil, not round: the badge lights a segment while any of it remains, so alpha 0.9 is still
	 *  four segments and still level 4. */
	int32 LevelForAlpha(float Alpha);

	/** Stored manual level, or kAssistLevelAuto. Same session-vs-persistent seam as the credit. */
	int32 LoadManualLevel();
	void  SaveManualLevel(int32 Level);

	/** The LEVEL the player was last shown, so a step down is celebrated exactly once. -1 if never.
	 *  Deliberately the level and not the alpha: the fade schedule is continuous, so alpha drops a
	 *  little on every single ride and celebrating any decrease fired the card on every restart. */
	int32 LoadLastShownLevel();
	void  SaveLastShownLevel(int32 Level);

	/** Best SINGLE ride, in credit (ride-score-counter.md FR4). Same session-vs-persistent seam as
	 *  everything else here, and cleared by ResetCredit() - otherwise "Reset assist" hands the next
	 *  tester a first-run board with a graduated-looking scoreboard on it. */
	/** Best single ride, PER BOARD, in credit. A global best is owned for ever by whichever board
	 *  scores most easily, so it stops being a target and discourages moving to a harder board -
	 *  which is the opposite of what board selection is for. An empty id keeps the old global key. */
	float LoadBestRide(const FString& BoardId);
	void  SaveBestRide(const FString& BoardId, float RideCreditSeconds);

	/** One watchable ride, as the ride list needs to describe it before it is played.
	 *  See specs/best-ride-replay.md FR9.
	 *
	 *  The float best above answers "what is this board's record"; this answers "which ride WAS
	 *  that, and what does its row say". They are kept side by side deliberately: a board can hold
	 *  a best score whose trace has since gone, and FR2 requires that to read as a score with no
	 *  replay offered rather than as a dead button or a substituted ride. */
	struct FRideRecord
	{
		float     ScoreCredit = 0.0f;   /**< RAW credit, survival + tricks, no board multiplier. */
		FString   TraceFile;            /**< File NAME only, not a path: the directory moves per platform. */
		FString   BoardId;
		float     DurationSeconds = 0.0f;
		FDateTime RecordedAt = FDateTime::MinValue();

		/** A record with no file can never be played, so it is not a record. */
		bool IsValid() const { return !TraceFile.IsEmpty(); }
	};

	/** Which of the two records per board. "Latest" is what the old Replay button played. */
	enum class ERideSlot : uint8 { Best, Latest };

	/** False if no record is stored. Both halves of the {score, file} pair come back together or
	 *  not at all - the whole reason this is one config string rather than five keys (FR2). */
	GONESURFING_API bool LoadRideRecord(const FString& BoardId, ERideSlot Slot, FRideRecord& Out);

	/** Written whole, then flushed. An interrupted write loses the record rather than leaving half
	 *  of one, which is the failure FR2 is specified against. */
	GONESURFING_API void SaveRideRecord(const FString& BoardId, ERideSlot Slot, const FRideRecord& Record);

	/** Every trace file NAME any record points at. The retention rule in FR3 is phrased against
	 *  this set: a trace no record references, and older than the grace period, may be deleted. */
	GONESURFING_API TSet<FString> ReferencedTraceFiles();

	/** Back to a first-run state, in both modes. Backs the FR7 "Reset assist" control — the thing
	 *  actually needed while passing the phone between testers, and the only route back to a first
	 *  run once credit persists. */
	void ResetCredit();

	/** Which storage mode is live. */
	bool IsPersistenceEnabled();

	// ---------------------------------------------------------------------------------------------
	// CVars. See specs/gradual-control-handoff.md "Implementation details".
	// ---------------------------------------------------------------------------------------------

	/** Master off switch. */
	bool IsEnabled();

	/** -1 = follow the FR6 schedule; 0..1 = force that alpha. `surf.assist.alpha 0` is the escape
	 *  hatch to use before any physics-tuning session, since session-only credit means every
	 *  playtest otherwise opens at alpha = 1.0 on an assisted board. */
	float AlphaOverride();

	/** FR8 override: re-enable the assist inside a filtered/unattended run. Tests only. */
	bool IsForcedInTests();

	/** -1 = leave credit alone; >= 0 = force accumulated credit to this many seconds, so a test can
	 *  land anywhere on the FR6 schedule without riding there. */
	float CreditOverride();

	/** Seed the last-shown level once, then behave normally (-2 = leave alone).
	 *
	 *  Exists because a fresh process always has LastShownLevel = -1, which makes MaybeShowAssistCard
	 *  open the FirstPlay card, which pauses the world - so NO headless capture could ever reach an
	 *  assist-on ride. That blocked eye-testing the level-up celebration and the level-down card, the
	 *  two surfaces most in need of it. Seed it to the current level and the ride just runs; seed it
	 *  above the current level and the LevelDown card opens at startup. */
	int32 LastShownLevelSeed();
}
