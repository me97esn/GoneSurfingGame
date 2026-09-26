# Spec: waveMassThrust — an unbounded drive that ignores the board's own speed

> **SUPERSEDED (2026-09-08) by [wave-interaction-damping-and-redirect.md](wave-interaction-damping-and-redirect.md).**
> Everything here was an attempt to fix the surge by *clamping forces* — first one term (FR1'), then
> the summed forward drive (FR6, built and measured). FR6 failed: it engaged exactly as designed and
> moved the net surge force by 12%, because the excluded forces grew to replace the governed ones.
>
> This document is kept as **the measurement record** (M1–M10 — the sawtooth, the force budget, the
> water-vs-board speeds, the wave-field verification, the four-role map) and as the record of **why
> the force-clamping family was abandoned.** The measurements are all still valid; the requirements
> are not. Read this for the evidence, then read the successor for the design.

> **UPDATE 2026-09-23 — the term is not dead.** `waveMassThrustCoefficient` and
> `waveMassFlowDragCoefficient` were defaulted to 0 in `d4dbb60c1`, but never removed, and the owner
> has brought both back at **0.0001** for the lip's punch. Nothing measured below is invalidated:
> every measurement here was taken at **0.003**, thirty times higher, where the term genuinely was an
> unbounded drive. At 1e-4 it is a lip-impact contribution, not the surge source. Read M1-M10 as the
> record of what this family does when it is large.

## Status

- [x] Measured (2026-09-07). One player ride plus three headless replays, shortboard throughout.
- [x] Root cause identified: `waveMassThrust`'s energy input contains **no board-velocity term**, so
      it pushes just as hard at 1400 cm/s as at 200, and its input peaks *mid-turn*.
- [x] Tuning ruled out as a fix — the player tested `yawFwdOffFaceFloor`, the `yawThrustAtten`
      window and `SurfboardForwardsDamping` 0.048 → 0.01 by hand; surges were undiminished (M1).
- [x] Wave velocity data **verified physically correct** (M5) — so the fix belongs in the term, not
      in the data or its scaling.
- [x] **Role map completed (2026-09-07, R1–R4).** `waveMassThrust` is not only a propulsion term;
      the wave-mass family also blocks sideways glide-through and pitches the nose up, partly through
      forces and partly through custom Chaos fork damping. Mapped before designing, at the player's
      instruction.
- [~] **FR1 REVISED and still open.** The first draft — a closing-speed gate along `board.forwards`
      — is **rejected**: it would zero the sideways→forwards redirect (R2) exactly when the board is
      fast down the line, which is when that redirect matters most. See "Why the first design was
      wrong".
- [x] **M8 done.** R2's *directional* job is carried losslessly by the fork's carve grip, not by
      `waveMassThrust` — so reducing the thrust does not delete the sideways→forwards redirect.
- [x] **M9 done, and it changes the spec's direction.** Zeroing `waveMassThrust` entirely — the most
      extreme possible FR1' — reduces the net surge force by only **9%**. The other propulsion terms
      refill the budget. **A single-term fix cannot work.**
- [~] FR1' — apparent-flow formulation. Still correct as far as it goes, but **now known to be
      insufficient on its own.** Necessary, not sufficient — and **no longer the primary fix**.
- [x] **FR6 designed and IMPLEMENTED (2026-09-07)** — a soft-knee limit on the *summed* forward
      drive, wave-scaled, one tick lagged, braking untouched, R3/R4 forces excluded.
- [x] **FR6 MEASURED — it fails AC1 and AC4. Shipped OFF (`propulsionCeilingWeights` 0).** The
      predicted FR6.6 leak is real and dominant: see M10. The mechanism is kept as an A/B vehicle.
- [ ] FR6.7 — stagger the shared `slopeSin` gates. **This is the next move**, per the spec's own
      instruction, and it is now the only untried idea that does not require new machinery.
- [ ] FR2 — expose it as a tunable with a documented value that restores current behaviour exactly.
- [ ] FR3 — wave catch must survive.
- [ ] FR4 — observability.
- [ ] FR5 — R2/R3/R4 must be measurably preserved.
- [ ] Player-validated on device.

> **If you read nothing else: M9.** The propulsion budget is self-compensating. Remove one term and
> the others grow to replace it, because they are all functions of where the board sits on the wave,
> and removing drive moves the board somewhere the others are stronger. This is why every
> single-coefficient attempt in this investigation — and in per-board tuning before it — produced
> only small effects.

## Motivation

Two symptoms, reported from editor play on the shortboard:

1. The board **accelerates too much when turning.**
2. It **brakes too hard, too often, just outside the wave.**

Per-board tuning did not fix either (see [per-board-tuning.md](per-board-tuning.md)). The player then
tested by hand — dropping `SurfboardForwardsDamping` to 0.01, raising `yawFwdOffFaceFloor` to 0.10,
narrowing the yaw-thrust attenuation window — and reported "none of the tuning really solved it…
there is too much forwards velocity generated by the wave I think". The measurements below say that
is right, and name the term.

## Measurements

Method: `RunGameAndCollectLogs.ps1` replaying the player's own input trace
(`-ReplayTrace=<file> -Board=shortboard -BENCHMARK -FPS=60 -NoVSync`) with `surf.debug.flags`
`'torque'` / `'crossing'` / `'wavedump'`. Replays diverge chaotically from the ride that produced the
trace, so every figure is a distribution over many ticks, never a reproduced moment.

Trace: `phone-2026-09-07-19-13-56` (27.9 s), recorded under the player's own hand-test overlay
(`Saved/BoardTuning/shortboard.json`: forwards damping 0.01, atten 200/400, floor 0.10).

### M1 — the surge is a sawtooth, and tuning did not touch it

From the raw trace, smoothed horizontal speed:

| | before the player's hand-test | after (damping 0.01, floor 0.10, atten 200/400) |
|---|---|---|
| speed-up phases | +663 cm/s in 0.58 s | **+680 cm/s in 0.53 s** |
| peak rate | 1536 cm/s² | **2485 cm/s²** |
| slow-down phases | −695 cm/s over 1.37 s | −599 cm/s over 2.00 s |
| cycle period | 2.8 s | 4.2 s |

The surge got marginally *sharper*; only the decay stretched, which is what lowering damping does.
Board Z spans 248–342 cm across the whole ride — 94 cm of vertical relief for a 6.8 m/s gain.

### M2 — the force budget names the term

`surf.debug.flags 'torque'`, top decile of net-forward-force ticks. Board weight = 100,000 force
units (mass × 1000 cm/s² from `DefaultEngine.ini`), so the right column is "in board weights":

| category | F_fwd | × weight |
|---|---|---|
| `dragBottom_waveMassThrust` | **+72,749** | **0.73** |
| `bottomHydrofoilYaw` | +58,193 | 0.58 |
| `bottomSlopeThrust` | +54,078 | 0.54 |
| `dragBottom_forwardDrag` | −26,732 | −0.27 |
| buoyancy / `dragRail` / rest | −22,950 | −0.23 |
| **net** | **+146,432** | **1.46** |

Across all ticks the same three lead, in the same order (12,051 / 8,329 / 10,462).

### M3 — the torque budget double-counts bottom drag

`dragBottom` is registered as the returned `dragForce` (`= forwardDrag + waveMassThrust`) **in
addition to** its two sub-entries, and `cat=TOTAL` sums all of them. Subtract the `dragBottom` row.
Verified arithmetically: −26,732 + 72,749 = +46,017, exactly the `dragBottom` row. Raw TOTAL reads
192,449; the true net is 146,432. **Any future reader of this log must apply the same subtraction.**

### M4 — the board outruns the water it is supposedly being pushed by

`surf.debug.flags 'crossing'`, on ticks where the board exceeds 1000 cm/s (n=33):

| | |
|---|---|
| board speed | 1193 cm/s |
| water speed at the board | 233 cm/s |
| water component **along the board's heading** | **+93 cm/s** |
| closing speed | **−1100 cm/s** |
| behind the crest | 0% |
| mean `underW` | 0.12 |

Across all moving ticks the water **never once** overtakes the board (0%); the board is faster on
99%. The surges are not whitewater events — the board is on the open face and barely wetted.

### M5 — the wave velocity data is correct (so this is not a data bug)

`surf.debug.flags 'wavedump'`, extended for this investigation to dump velocities, the full stored
normal and the height gradient. Grid 41 × 434, `step_size` 2.0, dumped row `gy = grid_height/2`.
Wave data runs at **20 fps** (measured from `wcSecs`/`wcFrame` pairs, median 20.00).

| | |
|---|---|
| wave height, from the data | 334 cm |
| shallow-water breaking-crest particle speed, √(g·H) | **572 cm/s** |
| peak \|v\| in the field | **441 cm/s** (frame 886), **716 cm/s** (frame 896) |
| max **shoreward** component \|vx\| | 190 cm/s |

Peak flow is the same scale as theory and sits on/just in front of the face. The field is physically
sane. The fast water is mostly vertical and down-the-line — the falling lip — not shoreward.

**The board reaches 1371 cm/s: ~2× the fastest water anywhere in the field, and 7× the fastest
shoreward component.** No momentum-transfer mechanism from this water can produce that speed.

### M8 — how R2 is actually delivered: the carve grip does the alignment, the thrust does the energy

Four conditions on the same trace, one mechanism disabled at a time via `Saved/TuningOverrides.json`,
the player's hand-test board overlay held constant throughout. Measured over moving ticks
(> 150 cm/s) from `surf.debug.flags 'crossing'`, slip angle **signed** (−180…180) between the board's
horizontal heading `fwdH` and its velocity `bVel`:

| | baseline | `CarveGripRate` 0 | `PlaningRedirectMaxAngle` 0 | `waveMassThrustCoefficient` 0 |
|---|---|---|---|---|
| velocity tracks heading (<20°) | 62% | 50% | 77% | 82% |
| **travelling backwards (>90°)** | **0.0%** | **45.6%** | 0.0% | 10.2% |
| mean cos(slip) — 1.0 is perfect | 0.941 | **0.097** | 0.961 | 0.784 |
| useful forward speed | 548 | **83 (−85%)** | 523 (−4%) | 322 (−41%) |

- **The carve grip is not a refinement — it is what attaches the velocity to the heading at all.**
  Without it the board travels *backwards along its own heading* on 45.6% of moving ticks and mean
  cos(slip) collapses from 0.94 to 0.10. R2's **directional** job is carried here, losslessly.
- **The planing redirect has no detectable effect on this ride** (−4% useful forward speed, inside
  replay noise). It is gated on nose submersion, so it likely matters at drop-in rather than
  down-the-line; that is untested, not disproven.
- **`waveMassThrust` carries the energy, not the direction**: −41% useful forward speed, and R2's
  alignment survives without it (82% tracking).

**Consequence for FR1':** the directional half of R2 does not depend on `waveMassThrust`, so
reducing it does not delete the sideways→forwards redirect. The constraint on FR1' is R1/FR3
(drive and catch), not R2.

### M9 — the propulsion budget is self-compensating, and this is the finding that matters

Running the M2 force budget again with `waveMassThrustCoefficient` = 0 — the most extreme possible
version of FR1' — on the top decile of forward-force ticks:

| term | baseline | thrust zeroed | |
|---|---|---|---|
| `dragBottom_waveMassThrust` | +72,749 | **0** | removed |
| `bottomHydrofoilYaw` | +58,193 | +66,315 | +14% |
| `bottomSlopeThrust` | +54,078 | +43,647 | −19% |
| `dragBottom_waveMassFlow` | +3,540 | **+20,834** | **+488%** |
| `dragBottom_wavePenetration` | −2,177 | **+12,507** | **sign flip** |
| `dragRailFlow` | +2,514 | **+10,706** | +326% |
| `dragBottom_forwardDrag` | −26,732 | −11,606 | less braking (board is slower) |
| **true net forward** | **146,432** | **133,489** | **−9%** |

**Deleting the single largest contributor (+72,749, half the gross drive) reduced the net surge force
by 9%.** The other terms filled the gap almost exactly. Corroborated by M8's ride statistics: with
the thrust zeroed, time above 1000 cm/s fell only 2.2% → 1.4%, and p95 acceleration *rose* 35%.

The mechanism is structural, not coincidental. Every one of these terms is a function of where the
board sits and how it is oriented on the wave. Removing drive changes that position and attitude —
the board rides lower and moves more across the face — which *increases* the flow-driven terms
(`waveMassFlow`, `railFlow`) and flips `wavePenetration` from a brake into a forward contribution.

This explains the whole history of this investigation: the player's hand-tuning of three separate
coefficients barely moved anything, `yawFwdOffFaceFloor` 0.10 helped only slightly, and per-board
tuning before that kept finding small effects. **Single-coefficient changes cannot fix this, because
the budget refills itself.**

### M10 — FR6 implemented and measured: the leak wins

Same trace, same board overlay, `surf.debug.flags 'torque,propulsion'`. T0 = governor off, T7 =
knee 0.6 / ceiling 1.0.

**The governor works.** T0 reproduces the M2 baseline (true net forward 147,161 vs M2's 146,432 —
the escape hatch is honest). With it on, raw governed demand reaches p90 = 1.39 board weights,
p99 = 6.40, and the compression bites hard: scale 0.077 at p1, 0.206 at p5, 0.365 at p10.

**And it does not fix the ride.**

| | governor OFF | governor ON | |
|---|---|---|---|
| **net forward, surge decile** | **147,161** | **129,961** | **−12%** (AC1 needs ≤ 80,000) |
| `dragBottom_waveMassThrust` | 68,607 | 52,874 | governed down |
| `bottomSlopeThrust` | 54,519 | 25,114 | governed down |
| `bottomHydrofoilYaw` | 56,378 | 17,227 | governed down |
| `dragBottom_waveMassFlow` | 8,019 | **34,455** | **+330% — the leak** |
| `dragRailFlow` | — | **21,137** | **the leak** |
| speed SD | 317 | 280 | −12% |
| p99 acceleration | 1,542 | **1,947** | **+26%** |
| time above 1000 cm/s | 1.9% | **3.2%** | **+68%** |
| mean speed | 239 | 234 | −2% |

The three governed terms were cut by 45–70% and the net moved 12%, because `waveMassFlow` and
`dragRailFlow` — excluded by FR6.6 to protect R3 and R4 — grew to replace them. **AC1 fails. AC4
fails**: the sawtooth's amplitude fell slightly but its peaks got sharper and the board spent *more*
time above 1000 cm/s.

**What this proves beyond the leak.** The compensation is not merely "some terms were left
ungoverned". It is positional: compressing drive changes where and how the board sits on the wave,
and the flow-driven terms are functions of exactly that. A force-level governor cannot close a loop
that runs through the board's position. FR6.7 (staggering the shared `slopeSin` gates so the stack
stops firing in unison) attacks the correlation rather than the magnitudes and is the next thing to
try; if it also fails, the aggregate-governor family should be considered exhausted and the problem
re-approached from the wave data (M6) or from the drive terms' *shape* (FR1').

### M6 — separate finding: the stored normals disagree with the stored heights, in Y only

Comparing the gradient the normal implies, `(-nx/nz, -ny/nz)`, against the gradient the heights give:

| | normal implies | heights give | ratio |
|---|---|---|---|
| dh/dx (cross-shore) | 0.2224 | 0.2225 | **1.000** |
| dh/dy (down-the-line) | 1.5136 | 0.1299 | **11.7** |

Median over the row: X **1.000** (exact), Y **2.14**. Net inflation of `slopeSin`: **1.59× median,
3.35× on the flat shoulder**; on the actual steep face (gx 24–27) it is right or slightly *under*.

Not sub-grid mesh detail: on gx 0–8 the heights are a clean linear ramp (+0.445/cell exactly) while
the normal is *constant* at (−0.1217, −0.8281, 0.5471). A constant tilt, not varying detail.

**Out of scope here** — it belongs upstream in `E:\windowsgrejor\git\GoneSurfingSimulation` — but it
matters to this spec's acceptance criteria, because every propulsion term gates at `slopeSin` 0.10
and scales with it, so the game currently believes the wave is steeper than its own heights say,
worst where it is flattest.

### M7 — `CoordinateScale` is non-uniform

`CoordinateScale = (28.0, 20.1, 20.1)`, `VelocityScale` a single uniform 800. This answers open
question 6 in [per-board-tuning.md](per-board-tuning.md). The cross-shore velocity component is
therefore under-scaled relative to the geometry by 28/20.1 = 1.39×. Correcting it moves max shoreward
flow 190 → ~265 cm/s — a real inconsistency, far too small to change anything above.

## The wave-mass family — what each mechanism is actually for

**Read this before changing anything here.** The wave-mass forces are not a propulsion system with
some drag attached; they carry four distinct jobs, split across project-side forces and custom Chaos
fork behaviour. A change that only reasons about forward drive will silently damage the other three.

### R1 — accelerate the board forwards when going down the line

| mechanism | kind | where |
|---|---|---|
| `waveMassThrust` (`waveMassThrustCoefficient` 0.003) | force, `+board.forwards` | FluidDynamics, bottom |

(`bottomSlopeThrust` and `bottomHydrofoilYaw` also serve R1 but are not wave-mass terms.)

### R2 — redirect the wave breaking sideways into forwards velocity

Two mechanisms, working on **completely different principles**:

| mechanism | kind | energy | where |
|---|---|---|---|
| `waveMassThrust` | force ∝ \|sideways wave flow\|², applied `+board.forwards` | **adds kinetic energy** | FluidDynamics |
| planing redirect — `RotateVelocityTowardDir` toward the wave up-slope, `PlaningRedirectMaxAngle` 2.0, gate = nose submersion × `PlaningRedirectCrestFade` 600 | velocity rotation | **speed-preserving, adds none** | fork, `PBDRigidsEvolutionGBF.cpp` |
| carve grip — `RotateVelocityTowardDir` toward the nose actor's horizontal forwards, `CarveGripRate` 4.0 rad/s, same gate | velocity rotation | **speed-preserving, adds none** | fork |

The fork already performs the sideways→forwards conversion **losslessly**. `waveMassThrust` performs
it by *injecting* energy. How the job is split between them is not currently known — that is M8, and
FR1' is blocked on it.

### R3 — resist gliding sideways through a breaking wave

| mechanism | kind | notes |
|---|---|---|
| `wavePenetrationDrag` (coef 0.0003, `wavePenetrationThreshold` 400) | force along the horizontal wave normal, opposing crossing **either way**, ∝ (\|vAcrossFace\| − 400)² | per-actor, so it also damps yaw/skid. Deadzone keeps normal riding unbraked. |
| `waveMassFlowDrag` (coef 0.003) | force along the **water's flow direction**, ∝ \|flow\|² | board-wide; EMA-smoothed, `waveMassSmoothingTau` 0.2. The "carried along by the whitewater" push. |
| `SurfboardSidewaysDamping` 0.0975 → `p.Chaos.Solver.DampingLocalY` | local-axis linear damping | fork `DampAsymmetricalLinear`; ~2× the shortboard's forwards damping (0.048) |
| `ClampYVelocityAt` 5000 → `p.Chaos.Solver.MaxVelocityY` | world-axis velocity ceiling | fork. Note: `Damping*` are **local** axes, `MaxVelocity*` are **world** axes. |

**None of these is `waveMassThrust`.** R3 is carried entirely by other mechanisms.

### R4 — a board going nose-first into the wave is turned nose-up instead

| mechanism | kind | notes |
|---|---|---|
| `AngularDampingYAwayExtra` 0.8 / `AngularDampingYTowardReduction` 0.10 | **asymmetric pitch damping**, keyed on pitch misalignment × contact gate | fork. Adds damping when rotating away from surface alignment, removes it when rotating toward. The dominant nose-up mechanism, and not a force at all. |
| `waveMassFlowDrag` + `wavePenetrationDrag` pitch torque | force applied at `getWaveMassApplyPoint()` (CoM longitudinal axis) with `getWaveMassKeptTorque` | roll stripped (`waveMassRollDecouple` 1.0) and yaw stripped (`waveMassFlowYawDecouple` / `wavePenetrationYawDecouple` 1.0) — **pitch is deliberately kept**. |
| `lipImpact` | nose actor, roll/yaw stripped, pitch kept, unsmoothed | the falling lip slamming the nose down |
| `waveMassThrust` — **keel effect** | applied by `AddImpulseAtLocation` at the **actor location**, not the CoM | bottom actors sit fore/aft and below the CoM, so a forward push there produces pitch. **This is an unintended but real contribution to R4.** |

That last row is the trap: `waveMassThrust` was not designed as a pitch term, but it is applied at
the actor, so weakening it changes the pitch budget too.

## Root cause

`AFluidDynamics::calcDragForce`, bottom branch ([FluidDynamics.cpp:250-270](../Source/GoneSurfing/FluidDynamics.cpp#L250)):

```
absSidewaysVel = absoluteWaterVelocity, projected out of boardUp, then out of boardForwards
waveMassThrust = coef · boardWideEffH · boardWideSlopeSin · waveMassSlopeGate · |absSidewaysVel|²
               → applied along +board.forwards, clamped to maxDragAmount (1e8 — i.e. never)
```

Two defects, both structural rather than numeric:

1. **No board-velocity term anywhere.** `absSidewaysVel` derives purely from the wave's *absolute*
   water velocity. Every other drive in the file shrinks as the board matches the water because they
   read `relativeWaterVelocity`. This one does not, so it cannot stop pushing — M4 shows it at full
   strength while the board outruns the water by 11 m/s.
2. **Its energy input peaks mid-turn.** The input is the wave flow *perpendicular to the board's
   heading*, squared. Pointing the board along the flow makes it ~0; turning across the flow
   maximises it. That is precisely symptom 1.

**Why only this term.** The other two large contributors are not structurally unbounded:

- `bottomHydrofoilYaw` forward drive grows with v², but v is *relative* water speed and it already
  carries the `yawThrustAttenStart/End` taper as a mitigation.
- `bottomSlopeThrust` has no speed term at all — it is `coef × wetted × gates × (slope · board axes)`,
  a constant force while on the face. That is the correct *form* for gravity-along-a-slope; its
  terminal speed is set by drag balancing it.

`waveMassThrust` is the only one of the three with neither a relative-velocity input nor a taper.

**Why the obvious fix does not work.** Swapping `absoluteWaterVelocity` for `relativeWaterVelocity`
in `absSidewaysVel` changes almost nothing: a surfing board's velocity is nearly all along
`board.forwards`, and that component is projected out when forming the *sideways* vector.

### Why the first design was wrong

The first draft of FR1 gated the thrust on closing speed along `board.forwards`:
`closing = (absoluteWaterVelocity − boardVelocity) · board.forwards`, zero when `closing ≤ 0`.

**Rejected.** Going down the line, `board.forwards` points along the wave, the wave's own down-line
flow is small, and the board's down-line speed is large — so `closing` is strongly negative and the
gate shuts. That kills **R2 exactly when the board is fast down the line**, which is when the
sideways→forwards redirect is the whole point. It would have fixed the surge by deleting a feature.

It also rested on a bound that does not apply here. "A fluid cannot push a body faster than itself"
is true of **momentum transfer** (a drag-type interaction), and it is what M4/M5 measure. It is
**not** true of a **lifting** surface: an iceboat or a sailboat on a beam reach exceeds the wind
speed precisely because a lift device converts flow across it into thrust along its heading. R2 is a
lift/sail mechanism, so M4's "the board outruns the water" is not on its own proof of a bug.

**What M4/M5 do still prove:** `waveMassThrust` is *modelled* as momentum transfer — magnitude from
the **absolute** flow squared, direction fixed to the body axis — while doing a **lift** job. A real
lifting surface is driven by the **apparent** flow (water velocity *relative to the board*), and its
driving component collapses as the board accelerates and the apparent flow angle swings toward the
bow. That angle change is the self-limit this term is missing, and it is missing because the term
never looks at the board's velocity at all. The defect is the *model*, not the magnitude.

## Requirements

### FR1' — drive the term from the apparent flow, not the absolute flow

`waveMassThrust` must read the water velocity **relative to the board** — the apparent flow — for
both its magnitude and the geometry that sets how much of it becomes forward drive. The requirement
is behavioural, not a formula:

1. As the board accelerates along its heading with the wave flow unchanged, the forward drive must
   **decrease monotonically** and reach zero at a finite board speed. Today it is constant.
2. That limiting speed must be a function of the apparent-flow geometry, **not** a fixed cap, so a
   faster wave still drives the board faster — a sail device, not a speed clamp.
3. It must remain non-zero when the board is slow and the wave is moving across it, whatever the
   board's heading. This is R2 and wave catch (FR3), and it is what the rejected closing-speed gate
   broke.

The natural form is the standard lifting-surface decomposition: build the apparent flow
`vApp = absoluteWaterVelocity − boardVelocity` in the bottom plane, take the angle of attack between
`vApp` and `board.forwards`, and let the drive be the component along `board.forwards` of a force
built from `|vApp|²` and that angle. That is a suggestion, not a mandate — any formulation meeting
1–3 satisfies FR1'.

**Do not settle this formula before M8**, which says how much of R2 the fork's lossless redirect is
already carrying. If the redirect carries most of it, `waveMassThrust` can be reduced far more
aggressively than if it does not.

### FR2 — one tunable, with a documented value that restores today's behaviour exactly

`waveMassThrustClosingRef` (cm/s) on `USurfTuningSubsystem`, in the same category as the other
wave-mass coefficients, so it reaches the in-game tuning HUD and `Saved/TuningOverrides.json` without
a recompile. Follow the house convention (`yawFwdOffFaceFloor` 1.0, `forwardDragWettingGate` 0):
**one specific value must reproduce the pre-fix behaviour exactly**, and its comment must say which.

Default = the new behaviour, so the fix ships on.

### FR3 — wave catch must survive

`waveMassThrust` is load-bearing for takeoff: it is one of the three terms in the wave-catch
propulsion budget (slope thrust ~25k + wave-mass flow ~35k + tail drag ~8.5k), and zeroing those
coefficients was previously verified to make the wave unrideable. FR1 must not be implemented as a
flat coefficient cut. During catch the board is slow and the water is still overtaking it, so `closing > 0` and the
gate should be open; that is the design's whole point, and AC3 is what proves it.

### FR4 — observability

The existing `drag` debug line for the bottom branch must additionally print `closing` and the
resulting gate, so the fix can be measured the same way it was diagnosed. The `torque` budget
category stays `dragBottom_waveMassThrust` — do not rename it; M2/M3 and the analysis scripts key on
that string.

### FR6 — a soft-knee limit on the summed forward drive *(the primary requirement)*

M9 makes single-term scoping unworkable, so the limit is applied to the **sum**. If the total is
held, it does not matter which term supplies it, and there is nothing left for the others to refill.

**FR6.1 — accumulate the drive.** Sum the components along `board.forwards` of the designated
propulsion terms — `waveMassThrust`, `bottomSlopeThrust`, `bottomHydrofoilYaw` (forward component),
`waveSlopeGravity` — across every FluidDynamics actor, board-wide, per tick.

**FR6.2 — one tick of lag is acceptable and expected.** Forces are applied per-actor as they are
computed, so the total is only known after every actor has run. Use the previous tick's total, the
way the `torque` budget already does in `ASharedCalculations::calculateAll`. At 60–90 Hz the lag is
invisible. Do **not** restructure the force pipeline for this.

**FR6.3 — soft knee, not a clip.** Below `propulsionKneeWeights` the drive passes untouched; above
it, compress progressively toward `propulsionCeilingWeights`. A hard clip will read as the board
hitting a wall — the knee is what keeps it feeling like water. Both expressed in **board weights**
(1.0 = mass × gravity = 100,000 force units today), so the numbers stay meaningful if the board's
mass changes.

Starting values from the data, to be measured not trusted: knee **0.6**, ceiling **1.0**. Today's
top-decile net is 1.46 and AC1 asks for below 0.8.

**FR6.4 — compress drive only, never brake.** Only the positive forward components of the designated
terms are scaled. Drag, buoyancy and every negative contribution pass through untouched — otherwise
this becomes a speed clamp with the braking removed too, which is the opposite of the fix.

**FR6.5 — the ceiling scales with the wave, not with a constant.** A bigger, faster wave must still
drive the board harder, or this is a governor rather than physics. Scale the ceiling with the local
wave energy the board is actually sitting in — `|absoluteWaterVelocity|²` is already sampled per tick
and is the natural choice. `propulsionCeilingWeights` then sets the ceiling at a reference flow
speed, with that reference a second tunable.

**FR6.6 — do not govern the R3/R4 forces.** `wavePenetrationDrag`, `waveMassFlowDrag` and
`dragRailFlow` are **excluded** from FR6.1, even though M9 shows their forward projections growing.
They carry the sideways-glide-through block and the nose-up pitch; compressing them to chase a
forward-force number would trade R3 and R4 away for R1.

> **The known hole in this design, stated rather than hidden.** Excluding the R3/R4 forces (FR6.6)
> leaves exactly the terms M9 measured growing by +488%, +326% and a sign flip. Compensation can
> therefore still leak through them. This is why **AC1 is written on the *net* forward force, not on
> the governed sum** — if the leak is material, AC1 fails and we will know, rather than shipping a
> governor that governs nothing. If it does fail, the next move is FR6.7, not a wider governor.

**FR6.7 — if AC1 fails, stagger the gates before widening the governor.** The terms compensate partly
because they all key on `slopeSin` at the *same* 0.10 threshold (`slopeThrustMinSlopeSin`,
`waveMassMinSlopeSin`, `yawFwdSlopeGateMin`) and so switch on together. Separating those thresholds
decorrelates the stack, is three tunables already exposed, needs no new code, and is the cheapest
experiment available. Try it before extending FR6.1's membership.

### Deferred — the `slopeSin` inflation (M6)

All three drives gate on `slopeSin` at 0.10 and scale with it, and M6 measures it inflated ~1.6×
median (3.35× on the shoulder), so correcting it upstream could shrink the whole stack at once with
no gameplay code changed. **Deferred by the player's call** — their read is that it is most likely a
scaling issue rather than a data bug.

What the current data says about that read, for whoever picks this up: a **single constant scale
factor does not fit**. The ratio between the normal-implied and height-derived Y gradient runs 11.65,
10.91, 9.31, 2.65, 2.33, 2.08, 0.36, −0.67, 0.66, 0.80, 0.97, 1.25 across one row. The normals are
also **piecewise-constant in blocks** (`-ny/nz` holds 1.5136 for gx 0–9, then 0.4991 for gx 10–23)
while the heights vary smoothly — the signature of a source mesh coarser than the sampling grid, not
of a scale factor. What has **not** been ruled out is a **registration offset in Y**, the same class
of bug already known for the velocity grid: only one row (`gy = grid_height/2`) was dumped, so an
offset in Y is untestable with what exists. Dumping several rows would settle it cheaply.

Until then, FR6's tuning is provisional: if `slopeSin` later drops ~1.6×, the knee and ceiling must
be re-measured, not carried over.

### FR5 — R2, R3 and R4 must be measurably preserved

The role map says R3 is carried entirely by `wavePenetrationDrag`, `waveMassFlowDrag`,
`DampingLocalY` and `MaxVelocityY`, and R4 mostly by the fork's asymmetric pitch damping — so FR1'
should not touch either. "Should not" is not evidence. AC6 and AC7 are the evidence, and they must
be run, not reasoned about.

R4 has one real coupling to be careful of: `waveMassThrust` is applied at the **actor** location, so
on fore/aft bottom actors it produces pitch (the keel effect). Weakening it therefore removes some
nose-up authority. If AC7 regresses, the fix is to restore that pitch through the mechanism that owns
R4 — the fork's `AngularDampingYAwayExtra`, or the kept-pitch path the other two wave-mass forces
already use — **not** by weakening FR1'.

### M8 — required before FR1' is designed *(measurement, not a code requirement)*

Establish how R2 is currently split between the energy-adding force and the lossless fork redirect.
Suggested method: on a down-the-line ride, log per tick the forward drive from `waveMassThrust`
alongside the velocity rotation the carve grip and planing redirect apply
(`CarveGripRate × CarveGripGate × Dt`, and the planing-redirect equivalent), and compare the forward
speed each accounts for. Confirm by A/B: `CarveGripRate` 0 and `PlaningRedirectMaxAngle` 0 against
`waveMassThrustCoefficient` 0, one at a time, on the same trace.

### NFR1 — no new per-tick allocation, and no new sampling

`relativeWaterVelocity`, `absoluteWaterVelocity` and `forwards` are already on `ASharedCalculations`
and already read in this branch. The change should be arithmetic on values in hand.

### NFR2 — board-wide, not per-actor

`waveMassThrust` deliberately uses board-wide `slopeSin` and `waterColumnAbove` so symmetric
left/right actors cannot produce a spurious yaw torque (see `specs/per-actor-vs-board-wide-sampling.md`).
The closing-speed gate must be board-wide for the same reason: a per-actor gate would differ between
L/R pairs when the board is rolled and reintroduce exactly that torque.

## Acceptance criteria

**AC1 — the surge comes down.**
Given the trace `phone-2026-09-07-19-13-56` replayed on the shortboard at fixed 60 Hz,
when `waveMassThrustClosingRef` is at its shipped default,
then on the top decile of forward-force ticks `dragBottom_waveMassThrust` is **below 0.25 × board
weight** (from 0.73), and net forward force is **below 0.8 × board weight** (from 1.46).

**AC2 — the board no longer outruns the water it is driven by.**
Given the same replay, when measured with `surf.debug.flags 'crossing'`,
then **no tick** has the board above 1000 cm/s while the water component along its heading is below
200 cm/s. Ticks above 1000 cm/s should be rare rather than absent — the target is removing the
*unphysical* ones, not capping speed.

**AC3 — wave catch still works.** *(the guard on FR3)*
Given a fresh run from the standard start on each of the five boards,
then every board still catches the wave and reaches planing within the same step count as before,
and `surfing-down-the-line` still completes. If catch regresses, FR1's ramp is too aggressive —
widen `waveMassThrustClosingRef`, do not restore the absolute-velocity form.

**AC4 — the sawtooth flattens.**
Given the same replay, scored on **whole-ride aggregates** — speed SD, coefficient of variation, RMS
acceleration, p95 accel/decel, % time stalled, % time above 1000 cm/s —
then speed SD and p95 acceleration both fall, and mean speed does **not** fall more than 10%.

> Score aggregates, never single-event maxima. Ranking conditions on "largest single surge" produced
> a demonstrably wrong ordering during per-board tuning — a combination scored worse than either of
> its components — because chaotic divergence swamps single events. See
> [per-board-tuning.md](per-board-tuning.md).

**AC6 — a board cannot glide sideways through a breaking wave.** *(R3 — the guard on FR5)*
Given a run that drives the board across the face at speed with the rail leading, at the same
cross-face speeds as before the change,
then the cross-face excursion past the crest is no larger than before, measured as time-behind-crest
and excursion duration from `surf.debug.flags 'crossing'` — the same two numbers the
`forwardDragWettingGate` change was accepted against (20.6% → 35.2%, 0.41 s → 1.01 s; do not let
this change push them further).

**AC7 — nose-first into the wave still becomes nose-up.** *(R4 — the guard on FR5)*
Given the board driven nose-first into the face,
then peak nose-down pitch rate and the time to recover to surface alignment are within 15% of the
pre-change values. Measure with `surf.debug.flags 'torque'` (the pitch column, `T[roll/pitch/yaw]`)
plus the trace's `roll`/`pitch` columns.

**AC5 — the other boards are not collaterally retuned.**
Given the same replay on foamie and funboard,
then their mean speed changes by less than 10%. `waveMassThrustCoefficient` is not per-board today;
if FR1 turns out to need per-board values, that is a follow-up, not part of this change.

## Test cases

| # | setup | expectation |
|---|---|---|
| T0 | `propulsionCeilingWeights` at its documented restore value (governor off) | force budget matches M2 to within replay noise — proves the escape hatch |
| T7 | knee 0.6 / ceiling 1.0, top-decile ticks | governed sum sits at the ceiling; **net** forward force below 0.8 board weights (AC1). If the sum is capped but the net is not, the FR6.6 leak is real → FR6.7 |
| T8 | board slow, in the trough, wave arriving | governor inactive — the total is nowhere near the knee, so catch is untouched (FR3) |
| T9 | doubled wave flow speed at the same board speed | ceiling rises with it (FR6.5) — a bigger wave still drives harder |
| T1 | `waveMassThrustClosingRef` = the documented restore value | force budget matches M2 to within replay noise — proves the escape hatch |
| T2 | shipped default, trace `phone-2026-09-07-19-13-56` | AC1, AC2, AC4 |
| T3 | shipped default, trace `phone-2026-09-07-16-08-49` (a second, independent shortboard ride) | AC4 holds in the same direction — one trace is not evidence |
| T4 | all five boards, fresh run from the standard start | AC3 |
| T5 | board stationary in the trough, wave arriving | thrust is **non-zero** — `closing > 0` there, and this is the catch case |
| T6 | board at 1200 cm/s on the open face | thrust is **zero** |

## Open questions

1. **Do the other two drives need the same treatment?** At 0.58 and 0.54 board weights they are not
   small. Deliberately deferred: re-measure the budget after FR1 and decide on evidence. Ideally FR1
   alone brings the net inside AC1 and the others are left alone.
2. **Is `SmoothStep(0, ref, closing)` the right ramp?** A linear ramp, or scaling by `closing²` to
   match the term's own quadratic character, may feel better. Decide by riding, not by argument.
3. **Should `maxDragAmount` (1e8) become a real ceiling?** It is currently decorative. A meaningful
   clamp would be a second safety net, but a clamp is a symptom-fix and may mask FR1 being mis-tuned.
4. **Does M6 (inflated `slopeSin`) change the coefficient once fixed?** `waveMassThrust` scales
   linearly with `boardWideSlopeSin`. If the exporter fix lowers slopeSin by ~1.6× on the shoulder,
   this term — and slope thrust, and every gate at 0.10 — weakens there for free, and FR1's tuning
   will need revisiting. **Do not tune this term to death before M6 is resolved.**

## Reproducing the measurements

```powershell
$env:TEST_MAP    = "Surfing_infinite_wave"
$env:EXTRA_ARGS  = "-ReplayTrace=phone-2026-09-07-19-13-56.csv -Board=shortboard -BENCHMARK -FPS=60 -NoVSync"

# M2/M3 — force budget.  surf.debug.actors must be non-empty or the dump stays silent.
$env:EXTRA_EXECCMDS = "surf.debug.flags 'torque', surf.debug.actors 'SharedCalculations'"

# M4 — water vs board velocity
$env:EXTRA_EXECCMDS = "surf.debug.flags 'crossing'"

# M5/M6/M7 — the wave field itself.  One-shot at BeginPlay; no trace needed.
$env:EXTRA_ARGS     = "-Board=shortboard -BENCHMARK -FPS=60 -NoVSync"
$env:EXTRA_EXECCMDS = "surf.debug.flags 'wavedump'"

& "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1" -TimeoutSeconds 300
```

Gotchas that cost time here:

- A `-ReplayTrace` run has no test autopilot, so **it never calls Quit** — the runner exits 2 on its
  timeout. That is expected; parse the log anyway.
- Comma is the `-ExecCmds` separator, so CVar values containing commas need single quotes.
- The player's hand-test values live in `Saved/BoardTuning/shortboard.json`, which **outranks the
  board profile**. Check it before trusting any A/B, and archive it when done.
- A/B conditions go in `Saved/TuningOverrides.json` (global layer, beats every board profile) — no
  recompile, and `AFluidDynamics::setup()` re-reads tuning every tick, so values can also be changed
  mid-ride from the in-game HUD.
