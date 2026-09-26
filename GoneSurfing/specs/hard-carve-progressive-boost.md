# Spec: Progressive hard-carve boost (quicken hard turns without touching down-the-line trim)

## Status
- [x] Spec drafted (2026-07-25)
- [x] Phase 1 — mechanism, default-off (`lateralTurnHardCarveBoost = 0`). Inert at defaults.
- [x] **Design revised after headless A/B (2026-07-25): ADDITIVE commanded-lean carve, not a multiplier.**
  The first cut multiplied the roll-gated carve by `(1 + boost·ramp)`, but the headless replay proved
  that inert: during a hard turn the board barely rolls (`worldRollSin ≈ 0.044`, right at the 0.04
  deadzone), so the roll-gated carve is itself ~0 — multiplying ~0 by any gain is ~0. A debug dump also
  showed the sign guard (`sign(commandedShift)==sign(worldRollSin)`) was satisfied **0%** of hard-commit
  ticks: the two use opposite sign conventions, so the guard blocked the boost entirely. Both replaced by
  an additive term (below).
- [x] **Validated headless** (replay of `phone-2026-07-24-22-27-17.csv`, see Validation). Direction
  correct; hard-turn rate ~2.3× at boost≥0.5; trim provably untouched.
- [x] **Phase 2 — player-validated on device; `lateralTurnHardCarveBoost = 0.5` baked as the default**
  (2026-07-25). Was 0 (off).

## Problem (grounded in a trace)

Player feedback on `phone-2026-07-24-22-27-17.csv`: down-the-line trim with small leans feels great and
controllable, but a deliberate **hard right** turns *very slowly*.

Measured from that trace:

| Section | Commanded lean (`tilt_roll_deg`) | Board actually banks (`board_roll`) | Mean \|yaw rate\| |
|---|---|---|---|
| Gentle down-the-line (\|tilt\|≤15°) | ≤ 15° | up to ~11° | **17.5 °/s** |
| Hard right (tilt > 25°) | up to **54°** | only ~9–11° | **20.8 °/s** |

A 3.5×+ increase in commanded lean yields only ~19% more turn rate.

## Root cause

The carve force (`lateralTurnForce`, [FluidDynamics.cpp:981](../Source/GoneSurfing/FluidDynamics.cpp#L981))
keys on the board's **achieved** roll (`worldRelativeRollSin`):

```
lateralAmount = gatedRollSin × absSinPitch × effectiveTurnSpeed × AmountPlaning
              × lateralTurnCoefficient × lateralTurnMultiplier
```

But the board is **roll-stiff** (see [[surface-carve-roll-spring]]): it banks only ~11° even on a 54°
tilt command. So a hard lean and a gentle lean feed the carve nearly the same ~0.2 roll signal, and the
turn rates come out nearly equal.

Two existing tamers make this worse, and both suppress exactly the hard-turn regime:
- `lateralTurnSpeedCap = 300` — saturates the speed factor a hard turn spins up (relWaterVel → 1200+).
- `lateralTurnCoefficient` was cut 20000 → 4000 to kill the small-weight over-turn ([[small-weight-shift-sharp-turn]]).

Raising either would speed up hard turns **but equally amplify the gentle trim** the player likes — the
current formula can't tell the two regimes apart because both see ~0.2 board roll.

## Design

Add an **additive carve term keyed on the COMMANDED lean** (`amountToTheRight`), not the achieved roll.
The commanded lean is what cleanly separates the two regimes (gentle trim commands a small lean; a hard
turn commands a large one, up to the full ±0.5), and — crucially — it **bypasses the roll-spring
compression entirely**. This is the same intent signal the rail-lift gate
([FluidDynamics.cpp:852](../Source/GoneSurfing/FluidDynamics.cpp#L852)) and the bottom-hydrofoil turn-gate
already trust, and for the same reason: it is wobble-free player intent.

The term is computed **outside the roll deadzone gate**, so it fires even when the roll-stiff board won't
bank (the whole point — see the multiplier post-mortem in Status):

```
cShift        = amountToTheRight - 0.5             // -0.5..+0.5, 0 = centered
commitRamp    = smoothstep(start, full, |cShift|)  // 0 below `start` (trim band never reaches it)
commandedCarveAmount = commitRamp × boost × absSinPitch × effectiveTurnSpeed
                     × AmountPlaning × lateralTurnCoefficient × lateralTurnMultiplier
commandedDir  = sign(cShift) × left                // toward the commanded rail
lateralTurnForce += commandedDir × commandedCarveAmount
```

- **Below `start`** (the gentle trim band, |cShift| ≤ 0.28 on the trace): `commitRamp = 0` → the additive
  term is **exactly zero** → down-the-line trim is **bit-for-bit untouched**. This is the guarantee the
  player asked for, and it holds per-tick regardless of trajectory.
- **Above `full`** (hard commit): the term injects carve authority proportional to `boost`, independent of
  how little the board actually rolled — so a hard lean bites even though the board won't bank.
- **Direction from the command**, not `worldRollSin`: `sign(cShift)×left` matches the base carve's
  `-sign(worldRollSin)×left` for the same command (**verified via signed-yaw A/B** — net yaw stays the same
  sign and grows). Command-driven direction is robust to the tiny/noisy achieved-roll sign during a
  roll-stiff hard turn. **No sign guard** — the earlier `sign(cShift)==sign(worldRollSin)` guard was
  satisfied 0% of hard-commit ticks (opposite conventions) and killed the feature.

At full commit `commandedCarveAmount` equals `boost ×` a **fully-banked** board's base carve (the base
uses `gatedRollSin` ≤ 1 in the same product), so even `boost = 0.5` is a large force relative to the ~0.004
`gatedRollSin` a real hard turn produces — hence the strong, quickly-saturating response measured below.

### Why not the alternatives
- **Multiply the roll-gated carve** (the first cut) — the board barely rolls in a hard turn, so the
  multiplicand is ~0; a gain can't rescue it. Proven inert headless. Rejected → additive.
- **Raise `lateralTurnCoefficient`** — amplifies gentle trim too. Rejected.
- **Raise `lateralTurnSpeedCap`** — re-opens the small-roll runaway the cap was added to fix; also amplifies
  any high-speed carve regardless of intent. Rejected.
- **Reduce roll stiffness so the board banks more** — the [[surface-carve-roll-spring]] note warns this
  makes turns unpredictable; larger, separate change. Deferred.

## Parameters (new tuning coefficients)

All on `USurfTuningSubsystem` (auto-tunable via `Saved/TuningOverrides.json`, see [[tuning-overrides-json-ab]]),
mirrored as fallbacks on `AFluidDynamics` and pushed each `RefreshFromTuningSubsystem`.

| Name | Default | Meaning |
|---|---|---|
| `lateralTurnHardCarveBoost` | **0.5** (player-validated; 0 = off) | additive carve scale at full commit |
| `lateralTurnHardCarveStart` | 0.30 | \|commandedShift\| where the ramp begins |
| `lateralTurnHardCarveFull` | 0.45 | \|commandedShift\| where the ramp reaches full |

Default `boost = 0.5` (player-validated); `boost = 0` reproduces the old behavior exactly.

### Threshold calibration (grounded in `phone-2026-07-24-22-27-17.csv`)

Reconstructing `|commandedShift|` from `tilt_roll_deg` (deadzone 2°, full 25°, no invert) over the ride:

| Regime | mean `|commandedShift|` | max | rows above 0.30 |
|---|---|---|---|
| Gentle trim (\|tilt\|≤15°, n=577) | 0.060 | 0.281 | **0** |
| Hard right (tilt>25°, n=411) | 0.500 | 0.500 | 100% |

The regimes are cleanly separated: trim clusters at 0.06 and tops out at 0.28; a hard turn saturates full
deflection (0.50). `start = 0.30` clears trim entirely; `full = 0.45` is fully reached by the hard turn.
This is the static proof of the regime-separation claim (AC2's "gentle unchanged"); the yaw-rate magnitude
of the boost is a feel/tuning question for on-device validation.

## Acceptance criteria
- **AC1**: `boost = 0` reproduces current behavior bit-for-bit (snapshot tests unchanged).
- **AC2**: With `boost` tuned up, hard-right yaw rate rises materially above the ~20 °/s baseline while the
  gentle-trim yaw rate (~17.5 °/s) is unchanged — verify by A/B replaying `phone-2026-07-24-22-27-17.csv`.
- **AC3**: No direction inversion at turn onset (sign-gate holds).

## Validation (headless replay A/B, 2026-07-25)

Replayed `phone-2026-07-24-22-27-17.csv` on `Surfing_infinite_wave` via
`-ReplayTrace=` (auto-records a trajectory CSV with yaw), varying `lateralTurnHardCarveBoost` through
`Saved/TuningOverrides.json`. Yaw rate bucketed by the input trace's `tilt_roll_deg` at each moment.

| boost | gentle mean\|yawrate\| | hard mean\|yawrate\| | hard net yaw |
|---|---|---|---|
| 0 (baseline) | 13.8 °/s | 30.6 °/s | +163° |
| 0.5 | 17.4 °/s | **71.3 °/s** | +485° |
| 1.0 | 17.2 °/s | **70.2 °/s** | +480° |

- **Direction correct**: net yaw stays positive and grows (not reversed) → `sign(cShift)×left` is right.
- **Large, saturating effect**: hard-turn rate ~2.3× at boost≥0.5, and 0.5≈1.0 (plateaus — no runaway).
  Usable tuning range ≈ **0.1–0.5**; a good on-device starting point is **~0.3**.
- **Trim untouched**: the gentle column moves only within the sim's run-to-run noise. Proven by comparing
  the gentle window **before the first hard commit** (t < 7.46 s), where the additive term is
  mathematically zero so both runs apply *identical* forces — yet they still differed 14.7 vs 19.1 °/s.
  That gap is pure physics nondeterminism (documented run variance), and it fully accounts for the gentle
  differences seen across all boost levels. So the boost provably does not alter trim; the whole-ride
  gentle wiggle is trajectory divergence downstream of the (intentionally) changed hard turns.

**Caveat**: single runs per config; the sim is nondeterministic/bistable, so treat the exact numbers as
±4 °/s. The hard-turn effect dwarfs that; the precise feel is an on-device call.

## Test plan
1. A/B replay `phone-2026-07-24-22-27-17.csv`: current vs `boost` on. Compare mean \|yaw rate\| in the
   gentle (\|tilt\|≤15°) vs hard-right (tilt>25°) sections — gentle must be unchanged, hard must rise.
2. On-device feel pass: does a hard lean now bite without the small-lean trim feeling twitchy?
3. Snapshot regression at defaults (AC1).

## Related
- [[carve-grip-via-redirect]] — the grip/redirect side of the carve; the yaw driver this spec boosts is
  "kept unchanged" there.
- [[small-weight-shift-sharp-turn]] — why the coefficient was cut to 4000 (the constraint this works around).
- [[surface-carve-roll-spring]] — why the board won't bank past ~11°, the reason the achieved-roll signal
  can't distinguish the regimes.
- [lateral-turn-world-relative-roll.md](lateral-turn-world-relative-roll.md) — the yaw driver being boosted.
- [[tuning-overrides-json-ab]] — how to A/B the new coefficients on device without a recompile.
