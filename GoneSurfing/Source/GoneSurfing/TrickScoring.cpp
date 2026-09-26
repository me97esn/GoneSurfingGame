#include "TrickScoring.h"

#include "SurfAssist.h"

// Helpers are file-unique on purpose. Duplicate free-function names in anonymous namespaces across
// .cpp files break the Android unity build (they collide in the merged translation unit) while
// compiling fine on desktop, so every local here carries the Trick_ prefix.
namespace
{
	/** Shortest signed difference between two headings, degrees, in [-180, 180]. */
	float Trick_WrapDelta(float ToDeg, float FromDeg)
	{
		float D = ToDeg - FromDeg;
		while (D > 180.0f) { D -= 360.0f; }
		while (D < -180.0f) { D += 360.0f; }
		return D;
	}

	FVector Trick_FlattenNormalize(const FVector& V)
	{
		FVector F(V.X, V.Y, 0.0);
		return F.IsNearlyZero() ? FVector::ZeroVector : F.GetSafeNormal();
	}
}

namespace TrickScore
{
	float WaveRelativeHeadingDeg(const FVector& BoardForward, const FVector& WaveBackDirection)
	{
		const FVector BackH = Trick_FlattenNormalize(WaveBackDirection);
		const FVector FwdH  = Trick_FlattenNormalize(BoardForward);
		if (BackH.IsNearlyZero() || FwdH.IsNearlyZero())
		{
			return 0.0f;   // degenerate frame; the caller's previous heading is left to stand
		}

		// The wave breaks toward the front face, so "down the face" is away from the back of it.
		// Same construction as SurfAssist's wave frame, so the two cannot disagree about which way
		// the board is pointing.
		const FVector FaceDown = -BackH;
		const FVector DownLine = FVector::CrossProduct(FVector::UpVector, FaceDown).GetSafeNormal();
		if (DownLine.IsNearlyZero())
		{
			return 0.0f;
		}

		// atan2 rather than asin(dot) deliberately: asin saturates at +/-90 and folds back, so a
		// bottom turn that swings past 90 degrees would read as UNWINDING halfway through and score
		// as two small turns instead of one big one.
		const float AlongFace = (float)FVector::DotProduct(FwdH, FaceDown);
		const float AlongLine = (float)FVector::DotProduct(FwdH, DownLine);
		return FMath::RadiansToDegrees(FMath::Atan2(AlongFace, AlongLine));
	}

	float BoardMultiplier(int32 RatingDifficulty, float K)
	{
		// An unrated board (0, the struct default when the JSON omits ratings) is treated as the
		// easiest rather than as difficulty 0, which would otherwise pay LESS than the foamie.
		const int32 D = FMath::Clamp(RatingDifficulty <= 0 ? 1 : RatingDifficulty, 1, 5);
		return FMath::Max(1.0f, 1.0f + K * (float)(D - 1));
	}

	int32 ScoreShown(float RideCreditSeconds, float TrickCreditSeconds, float BoardMult)
	{
		const float Total = FMath::Max(0.0f, RideCreditSeconds) + FMath::Max(0.0f, TrickCreditSeconds);
		return SurfAssist::ScoreFromCredit(Total * FMath::Max(1.0f, BoardMult));
	}

	FOutput Tick(const FInputs& In, const FTuning& T, float DeltaTime, FState& State)
	{
		FOutput Out;
		if (DeltaTime <= 0.0f)
		{
			Out.bWindowOpen      = State.WindowRemaining > 0.0f;
			Out.WindowMultiplier = FMath::Max(1.0f, State.WindowPeakMultiplier);
			Out.ChainStep        = State.ChainStep;
			return Out;
		}

		// ---- Heading, unwrapped ------------------------------------------------------------------
		// Track a continuous angle so a turn through the +/-180 seam is one arc, not two.
		if (!State.bHasPrevHeading)
		{
			State.PrevHeadingDeg      = In.WaveRelativeHeadingDeg;
			State.UnwrappedHeadingDeg = In.WaveRelativeHeadingDeg;
			State.bHasPrevHeading     = true;
		}
		const float Delta = Trick_WrapDelta(In.WaveRelativeHeadingDeg, State.PrevHeadingDeg);
		State.PrevHeadingDeg       = In.WaveRelativeHeadingDeg;
		State.UnwrappedHeadingDeg += Delta;

		const float RawRate = Delta / DeltaTime;
		const float Alpha   = DeltaTime / (FMath::Max(T.HeadingRateSmoothingSeconds, KINDA_SMALL_NUMBER) + DeltaTime);
		State.SmoothedRateDeg += Alpha * (RawRate - State.SmoothedRateDeg);
		const float Rate = State.SmoothedRateDeg;

		// ---- The window pays FIRST, on the multiplier it already had ------------------------------
		// Before any turn closing this tick can raise it. A turn must not retroactively pay for the
		// tick it ended on.
		if (State.WindowRemaining > 0.0f)
		{
			const float Frac = FMath::Clamp(State.WindowRemaining / FMath::Max(T.WindowSeconds, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
			const float Mult = 1.0f + (State.WindowPeakMultiplier - 1.0f) * Frac;

			// THE trap in this feature, and it fails silently. Only the EXCESS is trick points; the
			// survival term has already been paid its own base, untouched, by the caller. Multiply
			// the credit accumulator instead and the per-board best is quietly corrupted while the
			// score on screen still looks correct.
			Out.BonusCredit += In.BaseCreditEarned * (Mult - 1.0f);

			State.WindowRemaining = FMath::Max(0.0f, State.WindowRemaining - DeltaTime);
			if (State.WindowRemaining <= 0.0f)
			{
				State.WindowPeakMultiplier = 1.0f;
				State.ChainStep            = 0;   // the chain ends with the window it lived in
			}
		}

		// ---- The turn state machine (FR1) ---------------------------------------------------------
		if (!State.bTurnOpen)
		{
			if (FMath::Abs(Rate) >= T.TurnEntryRateDeg)
			{
				State.bTurnOpen            = true;
				State.TurnSign             = Rate > 0.0f ? 1.0f : -1.0f;
				State.TurnSeconds          = 0.0f;
				State.TurnStartHeadingDeg  = State.UnwrappedHeadingDeg;
				State.TurnEntrySpeed        = In.SpeedCmPerSecond;
				State.TurnBiteSeconds      = 0.0f;
				State.TurnGuardSeconds     = 0.0f;
			}
		}

		if (State.bTurnOpen)
		{
			State.TurnSeconds     += DeltaTime;
			State.TurnBiteSeconds += (1.0f - FMath::Clamp(In.SideslipAbs, 0.0f, 1.0f)) * DeltaTime;
			if (In.bGuardActive)
			{
				State.TurnGuardSeconds += DeltaTime;
			}

			// Close when the rate falls below the exit threshold or the sign flips. The gap between
			// entry and exit is the hysteresis that stops one held turn becoming a dozen events.
			if (Rate * State.TurnSign <= T.TurnExitRateDeg)
			{
				const float Sweep = FMath::Abs(State.UnwrappedHeadingDeg - State.TurnStartHeadingDeg);
				const bool  bBigEnough = Sweep >= T.TurnMinSweepDeg && State.TurnSeconds >= T.TurnMinSeconds;

				// FR5: the assist may not earn trick points on the player's behalf. This is the ONLY
				// place the guard flag discards anything - after the exit it means the opposite
				// thing (the player is working their way back, and gets paid for doing it).
				const bool bAssistDrove =
					State.TurnGuardSeconds > T.TurnGuardDiscardFraction * FMath::Max(State.TurnSeconds, KINDA_SMALL_NUMBER);

				if (bBigEnough && !bAssistDrove)
				{
					// ---- FR4: grade the arc on size, bite and drive.
					const float Size = FMath::Clamp(Sweep / FMath::Max(T.GradeRefSweepDeg, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
					const float Bite = FMath::Clamp(State.TurnBiteSeconds / FMath::Max(State.TurnSeconds, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
					const float Drive = State.TurnEntrySpeed > 50.0f
						? FMath::Clamp(In.SpeedCmPerSecond / State.TurnEntrySpeed, 0.0f, 1.0f)
						: 1.0f;

					// The carve (rail engaged, speed kept) grades about twice the slide (pivoting,
					// speed bled) through the same heading change. Both score; a slide that scored
					// as well as a carve would teach the player to throw the tail out and stall.
					const float Grade = FMath::Clamp(
						Size
						* (T.GradeBiteFloor  + (1.0f - T.GradeBiteFloor)  * Bite)
						* (T.GradeDriveFloor + (1.0f - T.GradeDriveFloor) * Drive),
						0.0f, 1.0f);

					// ---- D5: a small lump at the exit so the counter visibly moves...
					Out.BonusCredit += T.TurnExitLumpCredit * Grade;

					// ---- FR9/FR10: ...and the rest through the window, which this turn opens or,
					// if one was already open, EXTENDS. A linked turn adds to the multiplier it
					// found rather than replacing it, so a chain compounds - up to the cap, which is
					// what stops the third payment for one turn from running away.
					const float Rise = (T.WindowPeakMultiplier - 1.0f) * Grade;
					const bool  bLinked = State.WindowRemaining > 0.0f;
					const float Base = bLinked ? State.WindowPeakMultiplier : 1.0f;
					State.WindowPeakMultiplier = FMath::Min(Base + Rise, FMath::Max(1.0f, T.ChainMaxMultiplier));
					State.WindowRemaining      = T.WindowSeconds;
					State.ChainStep            = bLinked ? State.ChainStep + 1 : 1;

					Out.bTurnScored  = true;
					Out.bBigTurn     = Grade >= T.BigTurnGrade;
					Out.TurnGrade    = Grade;
					Out.TurnSweepDeg = Sweep;
				}
				else if (bBigEnough && bAssistDrove)
				{
					Out.bTurnDiscardedByGuard = true;
					Out.TurnSweepDeg          = Sweep;
				}

				State.bTurnOpen = false;
			}
		}

		State.TrickCreditSeconds += Out.BonusCredit;

		Out.bWindowOpen = State.WindowRemaining > 0.0f;
		Out.WindowMultiplier = Out.bWindowOpen
			? 1.0f + (State.WindowPeakMultiplier - 1.0f)
				* FMath::Clamp(State.WindowRemaining / FMath::Max(T.WindowSeconds, KINDA_SMALL_NUMBER), 0.0f, 1.0f)
			: 1.0f;
		Out.ChainStep = State.ChainStep;
		Out.HeadingDeg = In.WaveRelativeHeadingDeg;
		Out.HeadingRateDeg = Rate;
		return Out;
	}
}
