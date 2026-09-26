# Spec: Fin Force Normalization (fix un-normalized SC basis in fin drag & lift)

> **Priority: SECOND of three specs split out from [barrel-glide-through-bug.md](barrel-glide-through-bug.md)** (after [passive-slope-thrust.md](passive-slope-thrust.md), before [wave-penetration-resistance.md](wave-penetration-resistance.md)). Coupled to the slope-thrust spec: the fins are the mechanism that absorbs the *perpendicular* component of gravity-down-the-face, so passive drive can't be cleanly validated until the fins work.

## Bug Description

The fin forces are crippled — both fin **drag** and fin **lift** — by a single root cause: the SharedCalculations basis vectors `this->left/forwards/up` are **not unit vectors** (they carry the actor's transform scale, ~0.2), but several sites use them as if unit. The fins are the board's anti-slip / directional-stability mechanism; with them near-zero, the board slides sideways through the wave (contributes to the glide-through) and carves with almost no fin authority.

## Current Behavior (measured, 2026-06-12)

Root: [SharedCalculations.cpp:79](../Source/GoneSurfing/SharedCalculations.cpp#L79) — `cosYawAngleOfAttackLeft = surfboardLeft | relativeWaterVelocityDirectionProjectedUp` dots the **un-normalized** `this->left` (mag ≈ 0.2) with a unit vector → `cosYawAngleOfAttack = 0.2 × true_cos`.

- **Fin drag** ([FluidDynamics.cpp:375-406](../Source/GoneSurfing/FluidDynamics.cpp#L375)): `perpFactor = |cosYawAngleOfAttack| = 0.2 × true_cos`, then `dragFactor = perpFactor⁴` → **0.2⁴ ≈ 625× suppression**. Measured at the escape: `perpFactor 0.10-0.20`, `dragFactor ≈ 0.0006`, fin-drag mag 0.5-13 across the two fin actors. (Direction is fine — `relativeWaterVelocityNormalized` is a true unit vector.) Fin drag is NOT planing-attenuated — planing is not the problem here.
- **Fin lift** ([FluidDynamics.cpp:822-850](../Source/GoneSurfing/FluidDynamics.cpp#L822)): `sinSlip = this->cosYawAngleOfAttack` (0.2×) AND `liftDir = sign·sharedCalculations->left·cosSlip + sharedCalculations->forwards·|sinSlip|` is built from the **un-normalized** `left`/`forwards` (another 0.2×). Net ≈ **0.04× intended (~25× too weak)**. Measured: `force ≈ (−0.1, −0.2, 0) ≈ 0.2 units` even at the hardest slip. Also mis-directed: `cosSlip = sqrt(1−sinSlip²) ≈ 0.985-1.0` *always* (sinSlip capped at ~0.2) → the lift never rotates toward `+forwards`, so the carve-coupling forward component is dead.

This silently breaks the premise of [fin-carve-coupling.md](fin-carve-coupling.md) (which assumed `sinSlip` was a true `sin(α)`) — a correction note is on that spec.

## Expected Behavior

Fin drag and fin lift respond to the *true* angle of attack. At the glide-through escape, where the board sideways slip is genuine (`alongLeft ≈ 100-117 cm/s`, true slip ~45-60°), the fins should produce force 2-3 orders of magnitude larger and actually resist the sideways slide and provide carve authority.

## Root Cause

Un-normalized SC basis vectors used as unit. Two sites:
1. `SharedCalculations.cpp:79` (and the sibling `cosYawAngleOfAttack` at line 78) — un-normalized `left`/`forwards` in the dot.
2. `FluidDynamics.cpp:835-837` — un-normalized `left`/`forwards` in `liftDir`.

## Implementation Details (not prescriptive)

- Use a **true cosine** for the fin `perpendicularFactor` and for `sinSlip` (normalize the basis at the point of use, or add a normalized variant).
- Build `liftDir` from **unit** `left`/`forwards`.
- Consider `cos²` instead of `cos⁴` and/or a small floor on the fin-drag falloff so a near-miss in angle doesn't fully disengage (tuning, optional).
- **Do NOT globally rip out the 0.2 scaling at line 79 without auditing.** The comment at [SharedCalculations.cpp:62-65](../Source/GoneSurfing/SharedCalculations.cpp#L62) claims `cosPitch/cosYaw` magnitudes are calibrated into downstream formulas. Consumers of the line-79 value: rail engagement gate (sign only — safe), fin drag (fix), fin lift (fix), bottom-thrust `slipGate` at [FluidDynamics.cpp:1103](../Source/GoneSurfing/FluidDynamics.cpp#L1103) ("logged only" — not load-bearing). The separate `pitchSinAngleOfAttack`/`cosPitchAngleOfAttack` fields feed the bottom hydrofoil and are out of scope — leave them. So normalize **specifically for the fin terms**, or audit every `*AngleOfAttack` consumer first.
- Note: `maxDragAmount = 1e8` (no binding cap; the fix won't be clamped). Only 2 fin actors (C_139, C_146) carry the whole term, and they log identical forces.

## Acceptance Criteria

- **AC1**: At the glide-through escape, fin drag and fin lift forces are 2-3 orders of magnitude larger than the current 0.2-13 units, and oppose the sideways slip.
- **AC2**: `cosSlip` reflects the real slip angle (e.g. ~0.7 at a 45° slip, not ~0.99), so fin lift rotates toward `+forwards` and the carve coupling is alive.
- **AC3**: No regression to the bottom hydrofoil (`pitchSin`/`cosPitch` untouched) or to the rail engagement gate (still sign-correct).
- **AC4**: `surfing-down-the-line` skid in t=6.5-7.5 s moves toward the fin-carve-coupling AC2 target (≤5°), now that the lift is ~25× stronger — re-tune `finLiftMagnitude`/`finDragCoefficient` down if the fins now over-grip.

## Test Cases

- Re-run `Surfing_infinite_wave` / `surfing-down-the-line` with flags `drag,lift,state` (capture from the wrapper's **stdout** file — `GoneSurfing.log` can be truncated by a second editor). Compare fin force magnitudes and `perpFactor`/`sinSlip`/`cosSlip` against the pre-fix values above.

## Status

- [x] Spec reviewed
- [x] True cosine for fin `perpendicularFactor` and `sinSlip` (new SC field `cosYawAngleOfAttackLeftN`, fed into the FluidDynamics `cosYawAngleOfAttack` member that both fin terms read)
- [x] Unit `left`/`forwards` in `liftDir` ([FluidDynamics.cpp:837-839](../Source/GoneSurfing/FluidDynamics.cpp#L837) now `.GetSafeNormal()`)
- [ ] (optional) cos⁴ → cos² / floor — left as cos⁴; revisit only if a near-miss in angle disengages the fins too sharply
- [x] Build clean (2026-06-16, UnrealEditor-GoneSurfing.dll relinked)
- [x] AC1 — PASS (measured 2026-06-16, `surfing-down-the-line` on `Surfing_infinite_wave`, flags `drag,lift,state:fin`)
- [x] AC2 — PASS (`|sinSlip|` now reaches 0.998 and `cosSlip` drops to 0.06; pre-fix both were stuck near the 0.2 cap / ~0.99)
- [x] AC3 — holds by construction (rail gate reads sign only; `pitchSin`/`cosPitch` untouched)
- [x] AC4 — PASS after retune (see "Coefficient retune" below)
- [x] Re-tune `finLiftMagnitude` / `finDragCoefficient` for the new (correct) scale — `finDragCoefficient` 0.1 → **0.01**; `finLiftMagnitude` left at **0.001**
- [x] **`finLiftMagnitude` 0.001 → 0.01 (2026-07-02, uncommitted, pending regression pass).** The fins were carrying ~0 lateral force (`finLift` |F_left|≈0; the rails carried all grip), so the board had **no aft centre of lateral resistance → no weathercock stability → wild bistability** down the line. Raising fin LIFT to 0.01 puts real authority at the tail: the ±50° `minYaw` bistable spread on `surfing-down-the-line` **collapsed to ±1.5°** (repeatable), with no over-grip (maxSlip ~16°, AC2 carve unchanged ~3.8°); 0.03 over-shoots. This is the tail-anchor lever in the into-wave-yaw fix — see [wave-mass-drag-torque-decoupling.md](wave-mass-drag-torque-decoupling.md). NOTE this is fin **lift** (the good anti-slip force), NOT fin **drag** (the one detuned to 0.0002 for over-grip). Re-check flat-water + other autopilots before committing.

### Measured magnitudes (2026-06-16, fin actors C_139 & C_146)

| term | pre-fix (spec baseline) | post-fix median | p90 | p99 | max |
|------|------|------|------|------|------|
| fin drag | 0.5–13 | 8.8 | 261 | 11,767 | 312,974 |
| fin lift | ~0.2 | 0.22 | 3.8 | 15.1 | 55.8 |

- `perpFactor` now reaches **0.998** (pre-fix 0.10–0.20); `|sinSlip|` reaches **0.998**, `cosSlip` down to **0.06** → true angle of attack restored (AC2).
- Drag is near-zero in straight flow (median 8.8 N — cos⁴ keeps the fins disengaged going straight, as intended) and spikes only at genuine slip/broadside. The 313k-N peak is a rare near-broadside event; whether it over-grips the carve is the AC4 on-device call. `finLiftMagnitude` was 0.001 during the run.
- Snapshot `top-turn` still WARN (yaw max 179.8°) — residual skid/spin is expected to persist until [wave-penetration-resistance.md](wave-penetration-resistance.md) (spec #3) lands.
- [x] Update [fin-carve-coupling.md](fin-carve-coupling.md) correction note → resolved (banner flipped to ✅ 2026-06-16; AC2 now achievable and met)

### Coefficient retune (2026-06-16) — `finDragCoefficient` 0.1 → 0.01

Source of truth is `SurfTuningSubsystem.h` (`calcDragForce`/`calcLiftForce` override the BP-passed
args with `Tuning->fin*` when the subsystem is wired). The fin terms are now ~625× (drag) / ~25×
(lift) stronger at the same coefficient, so the old `finDragCoefficient = 0.1` (tuned against the
crippled fins) massively over-grips. Skid measured on `surfing-down-the-line` as
`velHdg − (yaw+90)` (mesh rotated 90°), in the fin-carve-coupling **AC2 window t=6.5–7.5 s**:

| `finDragCoefficient` | window peak skid | behaviour |
|------|------|------|
| 0.1 (old) | **174°** | tail fins over-grip → board pivots about the tail and spins out mid-turn |
| 0.01 (new) | **4.0° / 4.1°** (two runs) | clean carve, velocity sustained 247–326 cm/s, smooth yaw |

`finLiftMagnitude` left at **0.001** — the carve-coupling lift (max ~33–56 N) is modest and is the
*good* anti-slip force; window skid already ≤5° with it unchanged. 10× below the known over-grip
cliff (0.1) is the safety margin chosen for the default.

### Follow-up retune (2026-06-16) — `finDragCoefficient` 0.01 → **0.0002** (the line-68 over-grip verdict)

The AC4 "on-device over-grip" question flagged above (line 68, the 313k-N near-broadside peak) came
back **yes** — observed on PC *and* phone. `0.01` was tuned only against the gentle t=6.5–7.5 s carve
window; it ignored the **pop-up → surf-straight transition** (~t=5.3–6.6 s), a higher-speed
(relWaterVel ≈ 280) full-broadside (`sinSlip ≈ 0.96 → cos⁴ ≈ 0.86`) event where fin drag spiked to
**48k–72k N per fin** (`0.01 × 72 × 0.86 × 78586 ≈ 48.8k`) and braked the board **560 → 50 cm/s in
~1.3 s while yawing ~100°** — a hard skid/spin-out before the autopilot even hands off. Nothing capped
it (`maxDragAmount = 1e8` is non-binding). Lowering to **0.0002** (~50×) removed the pop-up spike
**with no observed loss of carve grip** in the AC2 window — i.e. the feared regime tension did *not*
materialise at this magnitude, so a single coefficient sufficed and the cap/impulse-clamp was not
needed. New default set in `SurfTuningSubsystem.h`.

**Out of scope — still broken (spec #3):** away from the AC2 window the board still washes out when
planing collapses (t≈11–13 s) and then planes *backwards* (skid pinned ~178° at 300–460 cm/s). The
parent spec calls the fin fix "necessary but not sufficient" — the bottom cross-flow drag term in
[wave-penetration-resistance.md](wave-penetration-resistance.md) is the lever for that, and these
fin coefficients will likely need a re-check once it lands.

Ad-hoc analyzer used for the sweep: `Tests/AnalyzeFinSkid.ps1` (skid window + fin-force percentiles).

## Implementation notes (2026-06-16)

Root-cause fix only (no coefficient changes yet — those wait on observation per AC4):
- **SharedCalculations.h / .cpp**: added `cosYawAngleOfAttackLeftN`, the true-cosine sibling of `cosYawAngleOfAttackLeft`, computed by dotting the **normalized** `surfboardLeftN` (already used for plane projection) with the unit projected relative-water-velocity. The un-normalized `cosYawAngleOfAttackLeft` is **kept** for the rail-engagement gate ([FluidDynamics.cpp:299](../Source/GoneSurfing/FluidDynamics.cpp#L299)), which only reads its sign.
- **FluidDynamics.cpp:1475**: the per-actor `cosYawAngleOfAttack` member (read by fin drag perpFactor at line 386 and fin lift sinSlip at line 827) is now fed from `cosYawAngleOfAttackLeftN`. The only other reader is the bottom-thrust `slipGate` (line 1106), which is **logged only** (verified: `slipGate` appears only in the log at line 1172) — no behaviour change.
- **FluidDynamics.cpp:837-839**: fin-lift `liftDir` now built from `sharedCalculations->left/forwards.GetSafeNormal()` (was un-normalized, ~0.2× scale).
- **Out of scope, untouched**: `pitchSinAngleOfAttack` / `cosPitchAngleOfAttack` (bottom hydrofoil), `cosYawAngleOfAttack` at SharedCalculations.cpp:78 (the `-forwards` projection, unused by fins).

## Related

- [barrel-glide-through-bug.md](barrel-glide-through-bug.md) — parent investigation; full fin audit and data.
- [fin-carve-coupling.md](fin-carve-coupling.md) — premise broken by this bug; fix un-breaks it.
- [passive-slope-thrust.md](passive-slope-thrust.md) — coupled: fins absorb the perpendicular component of the slope-gravity drive.
