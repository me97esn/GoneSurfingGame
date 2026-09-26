// Trick scoring: turns count, not just survival. See specs/trick-scoring.md.
//
// The ride score used to be time alive. This adds a second term for how the board is being ridden -
// and, more importantly, for what the player does AFTER a turn, because a hard turn followed by a
// fall is the easy half.
//
// The shape, and the three things that are load-bearing:
//
//   - A turn is an EVENT (FR1), graded at its exit: entry, sustained arc, exit, with hysteresis and
//     a minimum sweep and duration. NOT an integral of |yaw rate| per tick - that makes wiggling the
//     board the optimal strategy, which is the opposite of what the score should teach.
//
//   - Heading is measured against the WAVE, not the world (FR2). A straight line across a steep face
//     is banked and sloped and produces plenty of world-space heading change with no turn in it. The
//     caller passes a heading already resolved in the wave frame; see WaveRelativeHeadingDeg.
//
//   - The turn is PAID OVER THE SECONDS THAT FOLLOW IT (FR9), not at the exit. Scoring it opens an
//     earning window, and the player collects by continuing to surf. Falling closes the window with
//     the ride.
//
// Why the window needs no new signal, which is the whole reason it is cheap: the credit rate is
// ALREADY a product of the things a turn should be paid on. SurfAssist::CreditEarned is
// DeltaTime * GuardRate * WaveRate * SpeedCreditMultiplier, and the window is one more factor on
// that product, so a turn that bleeds all its speed pays less through its own window and one taken
// into whitewater pays a quarter. That is why FInputs takes BaseCreditEarned rather than recomputing
// anything: the caller builds the base with the same call the survival term used.
//
// With one factor swapped (D9, 2026-09-18). The survival base pays 0.25 outside the guard band; the
// window's base pays TrickWindowGuardRate there, 1.0 by default. The band is 100-200 cm from the
// crest and any turn that qualifies (60+ degrees) leaves it, so when the two shared a guard factor
// every real turn's window ran on a quarter base: measured on a device ride, three turns chained to
// the 3x cap paid 2.1 credit-seconds in total, and the player earned LESS per second inside the
// celebration than riding straight. The pocket is still paid - by the survival term, at full rate
// against a quarter outside - so the clean carve held in band still out-earns the blowout (FR9's
// "must not become a recovery bonus" holds); the window just no longer taxes leaving it.
//
// Same two rules as SurfAssist.h, for the same reasons:
//   - No UObject, no world, no actors. Everything arrives in FInputs.
//   - All mutable state lives in a caller-owned FState, never a global. The pawn owns one. A window
//     timer is exactly the kind of thing that becomes a static by accident and then leaks between
//     rides and between PIE runs.
#pragma once

#include "CoreMinimal.h"

namespace TrickScore
{
	// ---------------------------------------------------------------------------------------------
	// Tuning. Defaults are the first-cut values measured 2026-09-11 from 89 s of real riding across
	// eight device traces (specs/trick-scoring.md, "First-cut values"). The pawn fills this from
	// USurfTuningSubsystem each tick so the whole feel is dialled in through
	// Saved/TuningOverrides.json with no rebuild.
	// ---------------------------------------------------------------------------------------------
	struct FTuning
	{
		// ---- FR1: what qualifies as a turn.
		//
		// Entry/exit are deliberately far apart. Without hysteresis a turn held near the threshold
		// chatters into a dozen events, each too short to score, and the player gets nothing for a
		// turn they plainly made.
		/** Smoothed wave-relative heading rate that opens a turn event. */
		float TurnEntryRateDeg = 35.0f;
		/** Rate the arc must fall below (or flip sign) to close. 0.4x entry. */
		float TurnExitRateDeg = 14.0f;
		/** Minimum duration for the arc to score at all. Measured median turn runs ~1.0 s, so this
		 *  excludes twitches and nothing else. */
		float TurnMinSeconds = 0.35f;
		/** Minimum total wave-relative heading change. Measured median turn sweeps ~49 deg, so 60
		 *  sits deliberately above the middle: ~4 qualifying turns per riding minute, and the window
		 *  below then sits open ~18% of a ride. Dropping this to 50 roughly doubles the turn rate and
		 *  takes the window to ~47%; below ~40 the window is open two thirds of the ride and the
		 *  multiplier has become the baseline, which is the failure D6 names. */
		float TurnMinSweepDeg = 60.0f;
		/** Low-pass on the heading rate before the detector sees it. Matches the assist's own
		 *  HeadingRateSmoothingSeconds - heading is noisy on a board this roll-stiff, and a second
		 *  smoother that disagreed with the first would be a second opinion about the same motion. */
		float HeadingRateSmoothingSeconds = 0.15f;
		/** FR5: discard an arc whose guard was active for more than this fraction of its duration.
		 *  The assist steered that turn, so the player does not get paid for it. NOTE this is the
		 *  ONLY place bGuardActive discards anything - after the exit the same flag means something
		 *  else entirely (see FR9/FR10 in the window below). */
		float TurnGuardDiscardFraction = 0.5f;

		// ---- FR4: the turn's quality, graded on three axes.
		/** Sweep that counts as a full-size turn. Beyond this, size stops adding. */
		float GradeRefSweepDeg = 120.0f;
		/** Floors on the bite and drive factors. A turn with no rail engagement and no speed kept
		 *  still scores - it just scores much less than the carve. At these values a clean carve
		 *  grades ~2x a slide through the same heading change, which is FR4's "materially higher". */
		float GradeBiteFloor = 0.40f;
		float GradeDriveFloor = 0.40f;

		// ---- FR6: the rare tier. The celebration earns its impact from rarity; a bar a competent
		// rider clears twice a wave turns it from an event into an obstruction.
		/** Graded turn quality at or above which the full snap/burst/caption fires. */
		float BigTurnGrade = 0.65f;   // mirrors USurfTuningSubsystem::TrickBigTurnGrade, which wins at runtime

		// ---- FR9: the earning window.
		//
		// Re-sized 2026-09-18 (D9) so one isolated big turn is worth ~15 s of straight riding and
		// three linked turns about a ride's whole survival term. At the first-cut 4.0 / 2.5 / 0.5 /
		// 3.0 a chain topped out near 4 credit-seconds even in band at full grade - a tenth of a
		// straight ride - which is why the owner's cutback-plus-two-turns ride scored 156 against
		// 300+ for going straight. Mirror USurfTuningSubsystem's Trick* defaults, which win at runtime.
		/** How long the window runs after a turn's exit. */
		float WindowSeconds = 6.0f;
		/** Earn-rate multiplier at the moment of exit, at full grade. Decays linearly to 1 across the
		 *  window - a cliff would stop paying at a moment the player cannot see a reason for. */
		float WindowPeakMultiplier = 4.0f;
		/** D5: the lump paid at the exit itself, in credit-seconds, scaled by grade. Exists so the
		 *  counter visibly moves on every scored turn (FR6 tier one) - a change of earning RATE alone
		 *  is too subtle to read while watching a wave. Most of a turn's value is still in the window
		 *  (~80% at these values); this knob and WindowPeakMultiplier set the split. */
		float TurnExitLumpCredit = 2.0f;

		// ---- FR10: chains.
		/** Ceiling on the window multiplier however long the chain runs. NOT optional: a hard turn is
		 *  already paid in speed by lateralTurnHardCarveBoost, and speed already multiplies the
		 *  credit rate, so the window is the third payment for one turn. Uncapped compounding on top
		 *  of a loop that already feeds itself is how a score stops being about surfing. Two big
		 *  turns reach it. */
		float ChainMaxMultiplier = 6.0f;

		// ---- FR11: the board's difficulty multiplies the score.
		/** Slope of the per-board multiplier, 1 + K * (difficulty - 1). At 0.25 the foamie
		 *  (difficulty 1) pays 1.00x and the shortboard (difficulty 5) pays 2.00x. */
		float BoardDifficultyK = 0.25f;
	};

	// ---------------------------------------------------------------------------------------------
	// Per-tick inputs.
	// ---------------------------------------------------------------------------------------------
	struct FInputs
	{
		/** FR2. The board's heading IN THE WAVE FRAME, degrees, 0 = straight down the line, positive
		 *  = nose swung toward the face. Full +/-180; the detector unwraps it.
		 *
		 *  It must be wave-relative or a straight line across a steep face scores a turn it did not
		 *  make. Resolve it from the same vectors the assist uses (resolvedWaveBackDirection and the
		 *  board's forward axis read off the actor - the mesh is rotated 90 degrees and forwards is
		 *  local +Y, so never assume an axis here). WaveRelativeHeadingDeg() below does exactly
		 *  that. */
		float WaveRelativeHeadingDeg = 0.0f;

		/** Board speed, cm/s. Only used for the drive axis of the grade (exit / entry). */
		float SpeedCmPerSecond = 0.0f;

		/** abs(ASharedCalculations::cosYawAngleOfAttackLeftN), 0..1. Sideslip: 0 when the flow runs
		 *  along the board (rail engaged), 1 when it runs straight across it (pivoting). Already
		 *  normalised, already computed every tick for the fin terms. */
		float SideslipAbs = 0.0f;

		/** True while the steering guard is correcting. Read TWICE here, for opposite purposes:
		 *  during an arc it can discard the turn (FR5 - the assist steered it); after the exit it
		 *  only makes the window pay less (FR9 - the player is out of position and working back).
		 *  It never breaks a chain. */
		bool bGuardActive = false;

		/** The window's base this tick: SurfAssist::CreditEarned with the survival term's speed and
		 *  whitewater factors, and TrickWindowGuardRate (default 1.0) in place of the survival
		 *  term's 0.25 out-of-band rate (D9). Built by the caller with the same call the survival
		 *  term used, so no factor but that one can drift between the two. */
		float BaseCreditEarned = 0.0f;
	};

	// ---------------------------------------------------------------------------------------------
	// Controller state. One per pawn, reset at the start of each ride.
	// ---------------------------------------------------------------------------------------------
	struct FState
	{
		// ---- heading tracking
		float PrevHeadingDeg = 0.0f;
		float UnwrappedHeadingDeg = 0.0f;
		bool  bHasPrevHeading = false;
		float SmoothedRateDeg = 0.0f;

		// ---- the arc currently open, if any
		bool  bTurnOpen = false;
		float TurnSign = 0.0f;
		float TurnSeconds = 0.0f;
		float TurnStartHeadingDeg = 0.0f;
		float TurnEntrySpeed = 0.0f;
		float TurnBiteSeconds = 0.0f;    // integral of (1 - sideslip) dt
		float TurnGuardSeconds = 0.0f;

		// ---- the window (FR9) and the chain (FR10)
		float WindowRemaining = 0.0f;
		float WindowPeakMultiplier = 1.0f;
		int32 ChainStep = 0;

		// ---- the ride's trick total, in credit-seconds so it shares ScoreFromCredit's units
		float TrickCreditSeconds = 0.0f;

		void Reset() { *this = FState(); }
	};

	// ---------------------------------------------------------------------------------------------
	// Per-tick output.
	// ---------------------------------------------------------------------------------------------
	struct FOutput
	{
		/** Credit-seconds earned by tricks this tick: the window's excess plus any exit lump. Add
		 *  this to the TRICK accumulator.
		 *
		 *  NEVER add it to AssistState.CreditSeconds. The survival term has to stay identical to a
		 *  build with this feature absent (FR3) - it feeds the per-board best, and the failure is
		 *  silent because the score on screen looks right either way. */
		float BonusCredit = 0.0f;

		/** A turn completed and qualified this tick. */
		bool bTurnScored = false;
		/** ...and it cleared the high bar, so the full celebration may fire (FR6 tier two). */
		bool bBigTurn = false;
		/** 0..1 quality of the turn that just scored. Only meaningful when bTurnScored. */
		float TurnGrade = 0.0f;
		/** Its total wave-relative heading change, degrees. Only meaningful when bTurnScored. */
		float TurnSweepDeg = 0.0f;
		/** Set when an arc completed but was thrown away because the assist drove it (FR5). Exists
		 *  for the debug log: "why did that turn not score" is otherwise unanswerable. */
		bool bTurnDiscardedByGuard = false;

		/** Wave-relative heading and its smoothed rate this tick, for the "tricks" debug category.
		 *  Without these, "no turn scored" and "the heading signal is dead" look identical from
		 *  outside, which is exactly the ambiguity that wastes a tuning session. */
		float HeadingDeg = 0.0f;
		float HeadingRateDeg = 0.0f;

		/** Live window state, for the overlay. The counter changes colour while the window is open
		 *  and shows the chain step above 1 - a change of earning rate alone is too subtle to read in
		 *  peripheral vision while the player is watching a wave. */
		bool  bWindowOpen = false;
		float WindowMultiplier = 1.0f;
		int32 ChainStep = 0;
	};

	/** Run one tick. DeltaTime must be > 0. Pure: everything mutated lives in State.
	 *
	 *  Call it only while a ride is actually underway. A fall needs no special case - stop calling
	 *  and the window stops paying, which IS the requirement that a turn followed by a fall is worth
	 *  almost nothing. */
	GONESURFING_API FOutput Tick(const FInputs& In, const FTuning& T, float DeltaTime, FState& State);

	/** FR2's projection, kept here so every caller measures heading the same way.
	 *
	 *  @param BoardForward       board forward in world space (read off the actor; the mesh is
	 *                            rotated 90 degrees, so this is local +Y, never assume it)
	 *  @param WaveBackDirection  toward the BACK of the wave (resolvedWaveBackDirection)
	 *  @return degrees, 0 = straight down the line, positive = nose toward the face, +/-180 range.
	 *          Returns 0 on a degenerate frame (flat water before the wave resolves). */
	GONESURFING_API float WaveRelativeHeadingDeg(const FVector& BoardForward, const FVector& WaveBackDirection);

	/** FR11. The board's difficulty rating (1..5, from the rack card) -> score multiplier.
	 *
	 *  Floored at 1.0 and never zero. A board-dependent factor that can reach 0 deletes the whole
	 *  feature for whichever board sits at the bottom, and that board is the foamie - the one a new
	 *  player meets first. That was exactly the defect the draft FR5 carried. */
	GONESURFING_API float BoardMultiplier(int32 RatingDifficulty, float K);

	/** Credit (survival + tricks) -> the points the player sees, with the board multiplier applied.
	 *
	 *  Applied HERE, at presentation, and never baked into a stored value: the per-board best is
	 *  stored in raw credit, so re-tuning the multiplier would otherwise silently invalidate every
	 *  best already recorded and leave one board's history in two different currencies. Both sides of
	 *  a best-vs-current comparison go through this, so they are always in the same units. */
	GONESURFING_API int32 ScoreShown(float RideCreditSeconds, float TrickCreditSeconds, float BoardMult);
}
