# Spec: Conservative slope thrust (stop carving from creating energy)

> **STATUS (2026-06-26): PAUSED — code reverted to the clamped slope-thrust.** The signed wave-thrust and
> slip-redirect work was reverted; the project is back at the surface-relative-damping state
> ([surface-relative-pitch-damping.md](surface-relative-pitch-damping.md), commit 2f80b1dbf) — the
> board-follows-the-wave improvement, which is the keeper. The crossing is being re-assessed by the author
> in interactive play. Findings below are kept for when this resumes.
>
> **Findings from the investigation (2026-06-26):**
> - **The crossing is a robust metastable bifurcation, not a clean energy-surplus bug.** The board sits
>   right at the crest-clearing threshold; cross-rate swung 75% → 0% across a *recompile of identical
>   source*, and v_top over N runs is a wide bimodal (≈40 stall vs ≈230–370 charge-over). So single/batch
>   headless runs can't reliably measure it, and incremental force tweaks shift the odds without giving
>   decisive margin.
> - **Energy IS added during the climb-over** (even on stall runs: E/m rose +48k while the board climbed
>   58 cm). But the *source* was never isolated — likely the wave legitimately lifting the board
>   (buoyancy + wave upward motion = real wave energy that propels a surfer up the face) rather than
>   manufactured propulsion. The decisive next measurement is a **per-force work budget (F·v per force)
>   during the climb**, which was not built.
> - **Signed slope-thrust alone did not fix it** (6/8 cross with redirect, 4/8 with redirect off).
> - **The slip-redirect chatters** (its `slipFraction` is built from noisy `relativeWaterVelocity`;
>   board-wide averaging didn't cure it) — so it's not viable as written.
> - **The asymmetric pitch damping's binary away/toward switch chatters** intermittently (visibility is
>   metastable). Smoothing it (product-blend or |misalignment|-gate) removes chatter but the v_top metric
>   used to reject those was itself metastable noise — so that trade-off was never cleanly established.
>
> **If resumed:** build the per-force work budget first to settle manufactured-vs-legitimate energy; if
> legitimate, pivot to a crest-retention mechanism using `signedDistanceToCrest` rather than more
> propulsion tuning. Any fix must be decisive (big margin) given the metastability.

> **(original spec below — the planned signed/conservative approach, now paused)**

## Problem

The board passes over the wave to the back side after a hard carve. Root cause is **not** insufficient
braking — the passive slope-thrust is a *non-conservative* forward propulsion that lets the board
**manufacture kinetic energy just by carving**, and that surplus carries it over the crest.

Confirmed three ways:

1. **Code.** `slopeThrustForce` ([FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp)) is applied
   along `board.forwards` and had two one-way (energy-adding) parts:
   - `slopeAlongFwdDrive = Max(0, slopeDown·forwards)` — clamped off the *uphill* (decelerating) half, so
     descending the face accelerated but climbing cost nothing. A one-way force = perpetual motion.
   - `redirectDrive = finRedirect·|slopeDown·left|` — converts *gravity's sideways pull* into free forward
     thrust (`Abs`), regardless of whether the board descends.
2. **Energy vs time.** During the climb-over, mechanical energy `½v² + g·z` *rose* +34k/unit-mass while the
   board gained 39 cm of height — a conservative climb must lose energy.
3. **Equal-height test (the decisive one).** Speed at equal-or-higher height:

   | | starts surfing | top of wave, 2nd time | conservative max |
   |---|---|---|---|
   | clamped (orig) | z=292, v=325 | z=344, **v=164** | v≈60 |
   | signed (step 1) | z=292, v=237 | z=349, **v=39** | (can't reach z=349 from v=237) |

   The board returns to an equal-or-higher height with far more speed than energy conservation allows.

## Fix — make the propulsion conservative, smallest changes first

Keep the thrust **along `board.forwards`** (do NOT redirect to the fall-line — that would break carving and
the fins' job of redirecting motion into the board's heading). Only fix the *energy* (sign/source):

### Step 1 — signed wave-thrust (DONE)
Drop the `Max(0,…)`: `slopeAlongFwdDrive = slopeDown·forwards` (signed). Now climbing the face decelerates,
like gravity. Work ∝ **net descent only** → conservative regardless of coefficient (so the old "~60 kN
reverse" fear, a magnitude issue at coef≈40000, doesn't apply to the *sign*). Result: speed at the second
top **164 → 39 cm/s** — the board nearly pays for its climb and stalls at the crest instead of charging over.

### Step 2 — fin redirect on slip, not gravity (IN PROGRESS)
A residual surplus remains (from v=237 the board still reached z=349, ~29k surplus). The leftover source is
`redirectDrive` driving off *gravity's* lateral component. A real fin redirects the board's **slip velocity**
(sideways motion) into forward motion via lift, which does **no work** (force ⊥ flow) — energy-neutral.
Re-derive the redirect from the board's lateral slip instead of `slopeDown·left`, so it can only redirect
existing momentum, not manufacture it.

## Acceptance

- **AC1 — no manufactured energy.** On `hard_turn_towards_the_wave`, the board cannot return to its
  starting surf height with more speed than conservation allows; speed at the second top → ~0 and it no
  longer charges over the crest.
- **AC2 — terminal/planing speed preserved (regression).** The straight-line drive must not collapse:
  `surf-straight` and `surfing-down-the-line` terminal speed stays close to baseline (the redirect/thrust is
  the planing drive — the velocity must not drop much). Re-baseline only after this holds.

## Status

- [x] Step 1: signed `slopeAlongFwdDrive` (no Max(0,…)).
- [ ] Step 2: fin redirect re-derived from board slip velocity.
- [ ] AC1 verified (hard_turn second-top speed ~0, no crossing).
- [ ] AC2 verified (surf-straight / surfing-down-the-line terminal speed within tolerance).

## Related

- [wave-crossing-deceleration.md](wave-crossing-deceleration.md) — earlier (superseded) "brake harder"
  framing; this spec is the energy-source fix instead.
- [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md) / passive-slope-thrust — origin of the redirect.
