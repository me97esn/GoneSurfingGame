// Stamina: every ride is a run with an energy budget. See specs/stamina.md.
//
// The infinite wave never ends and the foamie's assist makes falling rare, so a ride could go on
// indefinitely - past the trace recorder's cap, and long enough that "best score" measured patience.
// This gives a ride a rhythm and, through it, a length: one pool per ride, drained by riding at
// all (slowly), by pumping, and by hard turns. Empty pool = the rider is TIRED - not a ride ending
// in itself, but a state the physics then resolves: the assist goes off, a tired tuning layer goes
// on (weaker pumps, slower weight response, whatever Saved/TiredTuning.json says), and the rider
// either recovers by resting (the pool refills while tired and unexerted) or loses the wave.
//
// Three things are load-bearing:
//
//   - This module writes NOTHING into the physics (NFR2). It decides WHEN the rider is tired; what
//     tired does to the board is a tuning layer on USurfTuningSubsystem, so the coefficients simply
//     read differently and no force has a "tired" branch. With the layer file absent, a ride with
//     stamina on is bit-identical to one without it.
//
//   - The turn drain reads the trick detector's smoothed wave-relative heading rate, not roll and
//     not a second smoother. The board is roll-stiff (worldRollSin ~0.04 in a hard turn) so a lean
//     detector reads noise, and two smoothers that disagreed about the same motion would charge for
//     turns the score does not see. Below the free rate - cruising, line adjustments - turning is
//     free; above it the cost is proportional to how much of the turn is "hard".
//
//   - The pump drain charges for the GESTURE, not for what the water paid back. The first cut
//     integrated PumpInputAttenuated - the value the physics consumes - and it read as free: that
//     value is speed- and slope-attenuated to exactly zero most of the time and non-zero only in
//     the 0.35 s release, so a tired rider pumping flat out counted as resting and recovered
//     (2026-09-15). Muscles spend whether or not the wave pays; a stroke in progress is effort.
//
// Same two rules as SurfAssist.h and TrickScoring.h, for the same reasons: no UObject, no world -
// everything arrives in FInputs; and all mutable state lives in a caller-owned FState. A per-ride
// pool is exactly the kind of value that becomes a static by accident and then leaks between rides.
#pragma once

#include "CoreMinimal.h"

namespace Stamina
{
	/** Filled from USurfTuningSubsystem each tick, so the feel is dialled in through
	 *  Saved/TuningOverrides.json with no rebuild. Defaults are the spec's first-cut guesses. */
	struct FTuning
	{
		bool  bEnabled = true;
		/** A full pool lasts this long just riding. 0 = riding costs nothing (D1's alternative). */
		float PassiveRideSeconds = 120.0f;
		/** Pool per second while a pump stroke is running (charge or release), on top of the
		 *  passive drain: ~25 s of continuous pumping. */
		float PumpCostPerSecond = 0.04f;
		/** Heading rate (deg/s) below which turning is free. Copied from the turn detector's entry
		 *  threshold so the two agree about what "hard" means. */
		float TurnFreeRateDeg = 35.0f;
		/** Pool per degree of heading change above the free rate. A full-size turn (120 deg at
		 *  ~80 deg/s) spends ~8 %. */
		float TurnCostPerDegree = 0.0012f;
		/** Below this the bar changes state: "spend it or lose it" (FR4). */
		float LowFraction = 0.2f;
		/** While TIRED and resting (no pump, no hard turn), the pool refills from empty in this
		 *  long. Only while tired - see Step for why a refill during normal riding would make a
		 *  cruiser immortal. Must beat the passive drain or nothing ever recovers: at 90 s passive,
		 *  25 s recovery nets ~1/35 s, so the 0.3 exit fraction is ~10 s of rest. 0 = never. */
		float RecoverySeconds = 25.0f;
		/** Tired clears once the pool has climbed back to this fraction - hysteresis, so the rider
		 *  gets a real breath rather than flickering at the zero line. */
		float TiredExitFraction = 0.3f;
	};

	struct FInputs
	{
		float DeltaTime = 0.0f;
		/** 1 while a pump stroke is running (WeightDistribution::bPumpActive - the gesture as the
		 *  player makes it, untouched by attenuation), else 0. */
		float PumpEffort = 0.0f;
		/** TrickScore::FState::SmoothedRateDeg - signed; the drain uses its magnitude. */
		float HeadingRateDeg = 0.0f;
	};

	struct FState
	{
		float Pool = 1.0f;
		/** Out of stamina: entered at 0, left at TiredExitFraction. */
		bool  bTired = false;
		// Per-ride bookkeeping for the end-of-ride log - the numbers that say which drain actually
		// shapes the ride, which is what tuning the rates needs to know.
		float SpentPassive = 0.0f;
		float SpentPump = 0.0f;
		float SpentTurn = 0.0f;
		float Recovered = 0.0f;
		float TiredSeconds = 0.0f;
		int32 TiredSpells = 0;
		float RideSeconds = 0.0f;
	};

	struct FOutput
	{
		/** True on the one tick the rider became tired. */
		bool bJustTired = false;
		/** True on the one tick the rider recovered. */
		bool bJustRecovered = false;
		/** Pool at or below LowFraction. */
		bool bLow = false;
	};

	/** One tick: drain by effort, refill by rest, flip tired at the edges. Nothing when disabled. */
	FOutput Step(const FInputs& In, const FTuning& T, FState& State);

	/** Full pool, not tired, nothing spent. */
	void Reset(FState& State);
}
