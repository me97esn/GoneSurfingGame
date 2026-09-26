// See Stamina.h and specs/stamina.md.

#include "Stamina.h"

namespace Stamina
{
	FOutput Step(const FInputs& In, const FTuning& T, FState& State)
	{
		FOutput Out;
		if (!T.bEnabled || In.DeltaTime <= 0.0f)
		{
			Out.bLow = State.Pool <= T.LowFraction;
			return Out;
		}

		// FR2: three additive rates in pool-per-second, so that pumping THROUGH a hard turn is the
		// most expensive thing in the game - which is also the most speed-producing thing in it.
		const float BaseRate = (T.PassiveRideSeconds > 0.0f) ? 1.0f / T.PassiveRideSeconds : 0.0f;
		const float PumpRate = T.PumpCostPerSecond * FMath::Clamp(In.PumpEffort, 0.0f, 1.0f);
		const float HardRate = FMath::Max(0.0f, FMath::Abs(In.HeadingRateDeg) - T.TurnFreeRateDeg);
		const float TurnRate = T.TurnCostPerDegree * HardRate;

		const float Passive = BaseRate * In.DeltaTime;
		const float Pump    = PumpRate * In.DeltaTime;
		const float Turn    = TurnRate * In.DeltaTime;

		// Recovery runs only WHILE TIRED, and only when resting (no pump, no hard turn this tick).
		// Not whenever resting: recovery has to outpace the passive drain for a tired rider to ever
		// recover, and a refill that outpaces the drain while riding normally would net positive
		// for a cruiser - who then never tires, which is the unbounded passive ride this feature
		// exists to end. Confining it to the tired spell keeps both: riding always costs, being
		// tired is a breath you take, then the budget resumes.
		const bool bResting = (Pump <= 0.0f && Turn <= 0.0f);
		const float Recover = (State.bTired && bResting && T.RecoverySeconds > 0.0f)
			? In.DeltaTime / T.RecoverySeconds : 0.0f;

		State.SpentPassive += Passive;
		State.SpentPump    += Pump;
		State.SpentTurn    += Turn;
		State.Recovered    += Recover;
		State.RideSeconds  += In.DeltaTime;
		State.Pool = FMath::Clamp(State.Pool - (Passive + Pump + Turn) + Recover, 0.0f, 1.0f);

		if (!State.bTired && State.Pool <= 0.0f)
		{
			State.bTired = true;
			State.TiredSpells++;
			Out.bJustTired = true;
		}
		else if (State.bTired && State.Pool >= T.TiredExitFraction)
		{
			State.bTired = false;
			Out.bJustRecovered = true;
		}
		if (State.bTired)
		{
			State.TiredSeconds += In.DeltaTime;
		}
		Out.bLow = State.Pool <= T.LowFraction;
		return Out;
	}

	void Reset(FState& State)
	{
		State = FState();
	}
}
