# Spec: Rail Lift Direction — Steady Into the Wave, Not Roll-Slaved

Status: **⚠️ PREMISE CHALLENGED (2026-07-20) — do not implement as written.**

> **The roll-slaved `±board.left` direction is intentional: it is the weight-shift STEERING
> mechanism.** Rail Bernoulli lift is "a suction force pulling the board *toward* the engaged
> rail" ([bottom-yaw-hydrofoil.md:294](bottom-yaw-hydrofoil.md)) — dip a rail by leaning and
> you get pulled that way. It does double duty: banking *into* the wave dips the wave-side rail
> → pull = into-wave grip (commit `de491f293` "pull the board into the wave"); banking to turn
> → pull = carve translation (commit `26b3c130a` "the board now turns"). Same force. Forcing
> the direction to steady `+resolvedWaveBackDirection` (this spec's original proposal) would make
> the rail suck the board toward the wave **regardless of lean → you could never carve away from
> the wave** (bottom turn, cutback, trim down the line). That is a steering regression, not a fix.
>
> The measured oscillation is the `±board.left` steering faithfully tracking the board's ~4 Hz
> **unintentional** roll wobble as if it were steering input. So the real problem is NOT the
> direction rule; it is one of: (a) damp the unintentional roll wobble (separate problem — note
> `railLiftRollDecouple = 1.0`, so rail lift does not itself drive it); (b) distinguish intentional
> lean from wobble (gate/derive from player weight-shift input, not total roll); or (c) recognize
> grip already moved to the engine velocity-redirect ([carve-grip-via-redirect.md](carve-grip-via-redirect.md),
> `CarveGripRate = 4`, default-on) — in which case rail lift should stay the *steering* term and
> the "grip" framing below is simply the wrong job to assign it. The follow-up flagged at
> [bottom-yaw-hydrofoil.md:296](bottom-yaw-hydrofoil.md) — "re-evaluate the three carve-related
> terms (lateral turn, rail Bernoulli lift, rail yaw hydrofoil) together" — is the right frame.
>
> Everything below was written under the (incorrect) premise that rail lift is a grip force whose
> direction should be steady into the wave. The **measurement/evidence sections remain valid**;
> the **objective and the D1 direction rule do not**. Retained for the record.

## Overview

Rail Bernoulli suction ("rail lift") currently points along `±board.left`, chosen by
which rail is momentarily dipped (`waveRelativeRollSin`). Its cross-shore sense therefore
**flips every time the board's roll crosses zero**. On a board that rocks on the wave face,
the "grip" force alternates between holding the board *into* the wave and shoving it *out* —
the opposite of what rail grip is for.

This spec changes the rail-lift **direction** to reference the wave, not the roll. It does
**not** change the *gate* (which rail is wetted — that is physically correct) and defers the
*magnitude* question to implementation time.

This is the sibling of [rail-lift-roll-gate.md](rail-lift-roll-gate.md): that spec fixed
*which rail fires*; this one fixes *which way the force points*.

## Objective

Rail lift should provide a **steady into-the-wave grip** for the whole time a rail is engaged,
independent of the board's instantaneous roll. Eliminate the sign oscillation.

## Background & evidence (replay of `phone-2026-07-17-14-51-09`, front SC)

Measured with `surf.debug.flags 'crossing,torque'` + `surf.debug.actors 'Calculations'`
(the torque dump uses `SurfDebug::ShouldDebug`, which requires BOTH a flag token AND an actor
token whose substring matches the SC actor's label — a flags-only run logs nothing). Parsed the
per-tick `TORQUE [...] cat=railLift ... Fin=...` lines, where `Fin > 0` = force into the wave
(toward the crest, `+resolvedWaveBackDirection`).

1. **Roll-slaved direction (the bug).** `waveRelativeRollSin < 0` → **265/265** substantial
   firing ticks push OUT of the wave; `waveRelativeRollSin > 0` → 243/291 push IN. Direction
   is 100% determined by roll sign. Front SC nets 43% into / 57% out; back SC 44% / 56%.

2. **Minor by net only because it self-cancels.** Rail lift's net cross-shore is −84k, trivial
   next to the dominant OUT-push `dragBottom_waveMassFlow` (−6.6M) and `dragRailFlow` (−4.8M).
   But its *per-tick* magnitude is real: mean |Fin| ≈ 4,075, peaks ≈ 16k. A steady into-wave
   rail lift would net **≈ +1.2M into the wave (4,075 × 303 firing ticks)** — a ~15–20% offset
   against the `waveMassFlow` force that pulls the board off the face. The fix recovers a real
   grip term, it is not cosmetic.

3. **The roll is a symmetric ~4 Hz wobble, not a bank.** Roll mean 0.002 (range −0.152…+0.138);
   over the planing ride, 34% roll>0 / 34% roll<0 / 32% in the ±0.02 deadband; sign flips ~every
   14 ticks. So the board genuinely rocks around level and rail lift dithers with it. The 32%
   deadband means a third of the ride has **zero** rail grip.

4. **The buggy direction is a destabilizer.** Roll sign correlates with position on the wave:
   roll>0 (rail pulls IN) at mean distToCrest −143 cm (down the face); roll<0 (rail pushes OUT)
   at +28 cm (at/just behind the crest). So rail lift shoves the board over the back exactly when
   it is nearest the lip — plausibly feeding the glide-through / over-the-back problem. A steady
   into-wave force grips *in* at both phases.

5. **Cleanly isolated from roll.** `railLiftRollDecouple = 1.0` → rail lift is applied at CoM
   height and makes no roll torque. It does not drive the wobble, and this fix cannot destabilize
   (or fix) the roll. The change is confined to the cross-shore grip.

## Requirements

**FR1.** Rail-lift force, whenever a rail is engaged, has a **non-negative projection onto
`+resolvedWaveBackDirection`** (into the wave) on every tick — no sign flip through roll = 0.

**FR2.** The *gate* (only the submerged rail fires, via `waveRelativeRollSin`, per
[rail-lift-roll-gate.md](rail-lift-roll-gate.md)) is unchanged.

**FR3.** The into-wave axis is `resolvedWaveBackDirection`, falling back to `waveBackDirection`
when the former is zero (mirror the fallback already used by the TORQUE budget dump in
`ASharedCalculations`). It is tile-derived and constant, so it is robust to heading reversal
(cutbacks) and to the board rocking.

**NFR1.** No change to roll behavior (guaranteed by `railLiftRollDecouple = 1.0`; verify the
roll trace is unchanged within run-variance).

## Design decisions

### D1 — Direction rule (recommended: **A**)

- **A — pure horizontal into-wave** *(recommended)*: `dir = resolvedWaveBackDirection` (normalized,
  horizontal). Rail lift becomes a pure cross-shore grip with **zero roll dependence**, so
  oscillation is structurally impossible. Aligns with rail lift's role (horizontal grip; vertical
  support is bottom-lift's job). Larger behavior change: drops the vertical component the current
  banked `board.left` carries.
- **B — sign-corrected `board.left`**: `dir = (board.left · backDir ≥ 0) ? board.left : −board.left`.
  Keeps the force along the board-lateral axis (preserves the banked/out-the-rail character incl.
  its vertical component) but guarantees the cross-shore sense is always into-wave. Smaller diff;
  still robust to the gate firing the "wrong" rail during a wobble. Retains a roll-dependent
  *vertical* component (not a cross-shore flip).

Recommendation **A** because findings #3–#4 show the roll coupling itself is the trouble; A removes
it entirely. Revisit B if the lost vertical component proves to matter.

### D2 — Magnitude (**deferred to implementation**)

The direction vector `board.left` is currently un-normalized (carries the ~0.2 actor scale), so
today's magnitude is `≈ 0.2 × railLiftAmount`. Normalizing the new direction multiplies the force
~5×. Two options, to be chosen when implementing:

- **(i) Preserve current magnitude** — scale the normalized direction by the old `|board.left|` so
  magnitude is byte-for-byte unchanged. Isolates the direction fix for a clean, confound-free
  verification that the oscillation is gone.
- **(ii) Normalize + re-tune** — physically cleaner (also resolves the un-normalized-basis issue),
  but requires dropping `railLiftMagnitude` ~5× and a feel pass. Couples two changes.

First pass should use (i); (ii) is a documented follow-up.

### D3 — Deadband / continuous grip (enhancement, out of scope for the first fix)

With direction no longer able to flip, the ±0.02 roll deadband (32% zero-grip ticks) is no longer
needed to suppress jitter. A follow-up could fire the wetted rail (or both rails weighted by
per-actor wetting) continuously for uninterrupted grip. Not required for the direction fix.

## Acceptance criteria

Given the `phone-2026-07-17-14-51-09` replay with `surf.debug.flags 'crossing,torque'` +
`surf.debug.actors 'Calculations'`:

1. **No sign flip:** front SC `cat=railLift` `Fin ≥ 0` on ≥ 95% of substantial firing ticks
   (magF > 100), vs. 43% today. Same for the back SC.
2. **Net into-wave:** front SC net `railLift Fin` is strongly positive (target ≥ +1.0M, from a
   pre-fix −0.084M). Back SC net positive.
3. **Roll unchanged:** the `waveRelativeRollSin` trace matches the pre-fix run within run-variance
   (confirms `railLiftRollDecouple` isolation held).
4. **No magnitude jump under D2(i):** front SC `cat=railLift` `magF` per-tick distribution matches
   pre-fix (peak ≈ 20k) — confirms the direction fix was isolated from magnitude.

(PC replay diverges from the on-device ride; use trends and sign statistics, not exact per-tick
values or a snapshot regression.)

## Implementation notes

Single site: `AFluidDynamics::calcLiftForce` at
[FluidDynamics.cpp:755-759](../Source/GoneSurfing/FluidDynamics.cpp#L755-L759). Replace the
`railLiftDirection = (side == VE_Left) ? -left : +left` line with the chosen D1 rule, referencing
`this->sharedCalculations->resolvedWaveBackDirection` (fallback `waveBackDirection`), and apply the
chosen D2 magnitude handling. The gate block ([:732-743](../Source/GoneSurfing/FluidDynamics.cpp#L732-L743))
and `calcRailLiftForceAmount` are untouched.

## Status tracking

- [x] Bug reproduced & mechanism proven (roll-slaved) — 2026-07-20
- [x] Cross-shore budget quantified (rail lift self-cancels; steady-into ≈ +1.2M grip)
- [x] Roll characterized (~4 Hz wobble; deadband 32%; roll↔crest-position correlation)
- [x] Isolation confirmed (`railLiftRollDecouple = 1.0`, no roll torque)
- [ ] D1 locked (A vs B)
- [ ] D2 locked (i vs ii)
- [ ] Implemented
- [ ] Acceptance criteria verified on replay
- [ ] Baselines re-approved if needed

## Related

- [rail-lift-roll-gate.md](rail-lift-roll-gate.md) — sibling; fixed *which rail fires*. This spec
  fixes *which way it points*.
- [RailLift_spec.md](../Source/GoneSurfing/RailLift_spec.md) — original rail-lift spec; its
  "Final Force" `±left` direction is what this spec supersedes.
- Memory `rail-lift-oscillates-not-steady-into-wave` — the running investigation note.
- `barrel-glide-through-bug.md` / `submerged-downline-glide.md` — the over-the-back / glide-through
  problem this destabilizer plausibly feeds (finding #4).
