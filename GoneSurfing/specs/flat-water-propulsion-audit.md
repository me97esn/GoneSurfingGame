# Spec: Flat-water propulsion audit — the wave stopped driving the board

## Status
- [x] Measured (2026-09-05). Seven headless replays and scripted runs, shortboard throughout.
  Findings reproduced across two independent player traces plus a scripted autopilot line.
- [x] Root cause identified: `bottomHydrofoilYaw` supplies drive **where the wave has none to give**
  — off the face — and that is what lets a rider hold speed on flat water, behind the crest, and
  while inverted.
- [~] Challenge tested (M6–M8) — but see **M10**: the autopilot comparisons those rested on were
  confounded by frame rate, so their numbers are not trustworthy and the question is **reopened**.
- [x] **FR1 + FR2 + FR4 implemented and SHIPPED (M11).** The M10 "regression" was the confounded
  variable-timestep run of this same configuration; re-measured properly it is a small improvement.
- [x] FR1 — slope gate (a minimum) + front-face gate, applied as a floor. **Both gates are needed:**
  front-face alone measures 79%/496, the pair measures 95–96%/620–636.
- [x] FR2 — `slopeThrustMinSlopeSin` / `waveMassMinSlopeSin` 0.35 → 0.10. Required by FR1, not
  independently beneficial.
- [x] FR3 — withdrawn as confused (speed ≠ position on the wave); see FR3 below.
- [x] FR4 — per-board `yawFwdOffFaceFloor`, foamie 0.35 → shortboard 0.00.
- [ ] **Player-validated on device — the remaining gate on this being right.** AC2 is only partly
  met and AC3 is untested (M11).
- [ ] Player-validated on device — **must be a fresh ride, not a trace replay** (see M9)
- [ ] Per-board flat-glide values for all five boards (the original task this came out of)

## Motivation

While starting per-board tuning, three symptoms were reported on the shortboard, each from its own
recorded ride:

1. The board keeps or gains speed on flat water — in the whitewash or on the far side of the wave.
2. It keeps speed "a little too long" on the far side of the wave.
3. It keeps surfing after the crest has passed to the other side — and in one ride it accelerated
   **while upside down**.

## Measurements

Method: `RunGameAndCollectLogs.ps1` replaying the player's own input traces
(`-ReplayTrace=<file> -usefixedtimestep -fps=60 -BoardInTests -Board=shortboard`) with
`surf.debug.flags 'crossing,torque'` / `'thrust'`, plus scripted `surf.autopilots` runs. Replays
diverge chaotically from the original rides — every figure is a distribution over many ticks, not a
reproduced moment. A/B conditions were set through `Saved/TuningOverrides.json` (global layer, beats
every board profile), no recompile.

Traces: `phone-2026-09-05-10-34-49` (21 s, includes the capsize) and `phone-2026-09-05-10-37-31`
(34 s, the long ride).

**All measurements are post-handoff.** The intro plays on rails with forces suppressed, and physics
resumes at a stamped state (`Rails: STARTED — 238 rows, 2.37s`, then `HANDOFF STATE ... slopeSin=0.383
surgeSpeed=529.3 planing=0.750`). Nothing here measures wave-catch, and nothing here can change it.

### M1. The wave is too flat for its own gates

`slopeThrustMinSlopeSin` and `waveMassMinSlopeSin` are both **0.35**. Slope under the board while
riding (`|v| > 300`):

| slopeSin | share of riding | 10-34-49 | 10-37-31 |
|---|---|---|---|
| 0.00–0.10 | **58%** | 514 | 430 |
| 0.10–0.20 | 17% | 148 | 124 |
| 0.20–0.30 | 11% | 95 | 83 |
| 0.30–0.40 | 14% | 127 | 105 |

`slopeSin >= 0.35` on **9.5%** of all ticks; two independent rides agree to within a percent.

### M2. One ungated term supplies essentially all the drive

Mean board-forward force per tick:

| | flat + fast (10-34-49) | flat + fast (10-37-31) | behind crest + fast |
|---|---|---|---|
| window | slopeSin<0.10, \|v\|>400 | same | distToCrest>0, \|v\|>300 |
| ticks | 359 | 509 | 226 |
| **TOTAL** | **+9,826** | **+11,695** | **+8,672** |
| net force POSITIVE on | 64% | 76% | **89%** |
| `bottomHydrofoilYaw` | +9,735 (87%) | +10,274 (94%) | +6,461 (**100%**) |
| `bottomSlopeThrust` | +185 (1%) | ~0 | **absent** |
| `dragBottom_waveMassThrust` | +1,202 (1%) | +312 (0%) | absent |
| total drag | ≈ −4,000 | ≈ −4,000 | ≈ −2,000 |

The existing gates work: slope thrust fires on 1% of flat ticks, and behind the crest
`slopeThrustFrontFaceDir` shuts it off entirely, as
[wave-crossing-deceleration](wave-crossing-deceleration.md) intended.

### M3. It also drives a board that is not in the water

`amountUnderWater < 0.05`, `|v| > 300`, 91 ticks: `bottomHydrofoilYaw` = **+3,571 forward, present
on 100% of ticks**.

### M4. The speed attenuation is weak where the drive is unwanted (and is NOT a slope gate)

`yawFwdSpeedAtten = 1 - smoothstep(300, 700, relWaterVelMag)`. Measured `relWaterVelMag` mean
**399**; attenuation fully open on **37.7%** of samples, mean 0.65. Per-actor, by slope band:

| slopeSin | mean `speedAtten` | fwd drive / actor |
|---|---|---|
| 0.00–0.10 | **0.69** | 2,813 |
| 0.10–0.20 | 0.61 | 8,534 |
| 0.20–0.30 | 0.45 | 9,818 |
| 0.30–0.40 | **0.41** | 8,430 |

**Read this table carefully: `yawFwdSpeedAtten` is a function of SPEED only** —
`1 - smoothstep(300, 700, relWaterVelMag)`. Nothing in the codebase gates any force *above* a slope
threshold, and nothing should; every slope gate here (`slopeThrustMinSlopeSin`, `waveMassMinSlopeSin`,
`yawFwdSlopeGateMin`) is a **minimum**, zero below and full above. The apparent slope dependence above
is an artefact of binning a speed-driven quantity by slope: mean `relWaterVelMag` rises 376 → 451 →
500 → 501 across those same bands, so the attenuation falls with it. Same variable, wrong axis.

What the table does show is that the attenuation is weakest on flat water, where this is the only
drive — but it is weak there because the board is *slow* there, not because the water is flat.

The attenuation's actual purpose is a stability limiter, not a wave-position judgement: the forward
term is `coef × v² × sin²(slip)`, so v² grows as the board pulls away from the slower water — an
unbounded positive feedback ([yaw-thrust-speed-attenuation](yaw-thrust-speed-attenuation.md)).

### M5. Lowering the coefficient alone destroys the ride — but this test was confounded

`yawHydrofoilCoefficient` 0.02 → 0.001 on trace 10-37-31, **gates left at 0.35**:

| | 0.02 | 0.001 |
|---|---|---|
| riding (\|v\|>300) | 33% of ticks | **20%** |
| mean speed | 248 cm/s | 168 cm/s |
| mean speed t=8–12 s | 307 | **15** |

**This test varied two things at once and cannot be read as evidence about the wave forces.** It
lowered the yaw term while the slope gates were still shut, so it cannot distinguish "the wave
forces are correctly sized but switched off" from "the wave forces are undersized and the 20× raise
was covering for them." M6–M8 exist to settle that; keep M5 only as evidence that the ride *today*
depends on the yaw term.

### M6. Opening the gates recovers nothing — because there is no slope to gate in

`yawHydrofoilCoefficient` 0.001 **and** `slopeThrustMinSlopeSin` = `waveMassMinSlopeSin` = 0.0,
trace 10-37-31. Gate opening verified in the log: below slopeSin 0.35, `bottomSlopeThrust` is now
present on 777 ticks and `waveMassThrust` on 2,177 (previously ~1%).

| condition | riding (\|v\|>300) | mean \|v\| |
|---|---|---|
| yaw 0.02, gates 0.35 (today) | 33% | 248 |
| yaw 0.001, gates 0.35 | 20% | 168 |
| **yaw 0.001, gates 0.0** | **21%** | 207 |

Fully opening both gates bought **one percentage point**. The flat-water force budget under those
conditions says why:

| category (slopeSin<0.10, 376 ticks) | Ffwd |
|---|---|
| `gravity` | +7,588 (100%) |
| `bottomSlopeThrust` | **+114** |
| `dragBottom_waveMassThrust` | **+162** |

Slope thrust is **proportional to slope**. Opening its gate on water with no slope admits nothing.
The gate was never the limiter on flat water — the physics is. (`gravity` +7,588 is a board-frame
projection of the board's nose-down attitude, not propulsion; see Resolved questions.)

### M7. The rideable face is ~4.5 m wide, with a 3 m core and a cliff behind the crest

At 150 cm resolution, on the good-line autopilot run:

| distToCrest (cm) | mean slopeSin |
|---|---|
| −750 … −600 | 0.084 |
| −600 … −450 | 0.097 |
| −450 … −300 | 0.100 |
| −300 … −150 | 0.138 |
| **−150 … 0** | **0.247** |
| **0 … +150** | **0.197** |
| +150 … +300 | 0.055 |
| +300 … +450 | 0.107 |
| +450 … +600 | 0.058 |

The steep band runs roughly **−300 to +150 cm (4.5 m)**, and the core where slope exceeds 0.19 is
only **−150 to +150 — three metres**. It is asymmetric: a gradual shoulder in front of the crest
(0.247 → 0.138 → 0.100) and a **cliff behind it**, 0.197 collapsing to 0.055 within 150 cm. A
coarser ±300 cm binning reports ~6 m and overstates the rideable band by about a third.

What that costs in play, with the board at ~2 m:

- the core is about **1.5 board lengths** wide
- mean cross-face drift on a good line is **345 cm/s** (max 1162), so uncorrected the board leaves
  the 3 m core in **under a second**, and the full band in ~2.2 s
- it is nonetheless holdable: the autopilot spent 49% of ticks in the core and held unbroken
  stretches of **13.1 s, 8.5 s and 6.8 s** — but only by steering continuously

The player traces spent 58% of riding time off the face, which the yaw hydrofoil made costless.

### M8. On a good line, the wave alone drives the board — better than today

Scripted `surfing-down-the-line` autopilot (a line that holds the face), same board, same map:

| | riding (\|v\|>300) | mean \|v\| | time below slopeSin 0.10 |
|---|---|---|---|
| stock (yaw 0.02, gates 0.35) | 91% | 546 cm/s | 40% |
| **corrected (yaw 0.001, gates 0.0)** | **97%** | **648 cm/s** | **31%** |

**⚠ These numbers are not trustworthy — see M10.** This pair was run without a fixed timestep, so
frame rate (and with it the physics) varied between the two sides. The conclusion below may still be
right, but it is no longer supported by this measurement and needs re-running deterministically.

Sustained 640–716 cm/s across the whole corrected run, with no decay. **The wave forces are not
undersized.** A board that stays in the pocket surfs on them alone, marginally *faster* than it does
with the yaw term at 0.02. The yaw hydrofoil's contribution is decisive only when the board is off
the face — exactly where it should not be helping.

### M9. Trace replay cannot validate the fix

The player traces were recorded in a build where flat water costs nothing, so the recorded line
spends 58% of its time off the face. Replaying that line under corrected physics produces a dead
ride (M6) no matter how good the fix is — the line itself is an artefact of the bug. Validation must
use a scripted line that holds the face (M8) or a fresh player ride, never a replay of a trace
recorded before the fix.

### M10. The implementation regressed, and the method that approved it was broken

FR1+FR2+FR4 were implemented, built and measured on 2026-09-05. Two things went wrong, one in the
code and one in how everything above was measured.

**The code premise was void.** FR1 said to gate the forward component and "leave the anti-slip side
component untouched". There is no side component to leave: `AntiSlipForceScale` is **0.0 by tuned
default** ("grip redirect carries it"), so the forward drive is the *entire* output of the yaw
hydrofoil, and the slip-into-turn redirection commit `936a0bce9` raised is delivered *through* it.
Gating the forward half gates the whole term — which is exactly the open question this spec had
marked "resolved" after M8.

**The measurement method was confounded.** The autopilot runs in M8 had no `-usefixedtimestep`, so
frame rate varied with debug-logging volume, and frame rate changes the physics. The same
configuration measured **95% / 626 with `torque` logging on and 79% / 507 with it off**. Every
autopilot comparison above that pair-crosses logging regimes is therefore uninformative, M8's
91→97% included.

Re-measured properly — shortboard, `surfing-down-the-line`, **fixed 60 fps, identical light
logging**, which reproduces to ~1.5% on mean speed:

| configuration | riding (\|v\|>300) | mean \|v\| |
|---|---|---|
| **pre-change baseline** | **94%** | **602** |
| FR2 only (gates 0.10, FR1 inert) | 89% | 512 |
| FR1 + FR2 + FR4 as implemented | 79% | 496 |
| shipped inert defaults (verification) | 94% | 593 |

Both halves appeared to regress and both were reverted. **M11 overturns this**: the FR1+FR2 row
above was measured at variable timestep with heavy logging — the very confound described in this
section — and reproduces as a small *improvement* under fixed timestep. What survives from M10 is the
`AntiSlipForceScale` finding and the method requirement below. The FR2-only row (89%/512) stands as
measured, which is why FR2 ships as a dependency of FR1 rather than on its own merits.

**Method requirement for everything after this point:** every A/B must be `-usefixedtimestep
-fps=60` with identical `surf.debug.flags` on both sides. Runs that differ in logging volume are not
comparable, and single unpaired runs cannot resolve differences under ~5 points.

### M11. The design works; M10's regression was a measurement artefact

M10 concluded that gating regressed the ride. That was wrong, and the error is instructive: the
76%/497 figure it rested on came from a **variable-timestep run with heavy `torque` logging**, the
exact confound M10 itself identified two paragraphs later. Re-measured under fixed timestep, the same
configuration is a small improvement. Worse, M10's response was to ship the *front-face-only*
variant as "the validated best" — which is genuinely the bad configuration.

Shortboard (`yawFwdOffFaceFloor` 0.00, the strictest board), `surfing-down-the-line`, fixed 60 fps,
identical light logging:

| configuration | riding (\|v\|>300) | mean \|v\| |
|---|---|---|
| pre-gate baseline | 94% | 602 |
| front-face gate only (no slope gate) | 79% | 496 |
| **slope gate 0.10 + front-face gate, run 1** | **96%** | **620** |
| **same, run 2 (reproducibility)** | **95%** | **636** |

**The two gates need each other.** Front-face alone is worse than no gate at all; the pair beats the
baseline. Time spent below slopeSin 0.10 also falls, 40% → 35–38%: the board sits on the face more,
which is the point.

**AC1 passes** (≥90% riding, ≥600 mean). **AC2 partially**: the yaw hydrofoil's flat-water forward
contribution fell **+10,274 → +3,918 (−62%)**, the intended direction but not to zero. The residual
is unexplained — with a 0.10 gate and a 0.00 floor it should be ~0 below slopeSin 0.10, so either the
front and back `ASharedCalculations` disagree on `boardWideSlopeSin` (each `AFluidDynamics` gates
against its own), or the binning here misclassifies ticks. Worth one look before trusting AC2.
**AC3 is untested, not passed**: the autopilot never went behind the crest, so the 0 ticks there are
an absent test condition rather than a result.

Note also that the earlier "TOTAL excluding projections" figure is not a usable propulsion metric —
several drag-named categories carry positive board-forward components, so the sum is not
interpretable without vetting each one. AC2 should be judged on `bottomHydrofoilYaw` alone.

## Root cause

[FluidDynamics.cpp:1565-1572](../Source/GoneSurfing/FluidDynamics.cpp#L1565-L1572):

```
mag = yawHydrofoilCoefficient × v²InBottomPlane × absSinSlip × wettedForce × effectiveWaterHeight
fwd = mag × absSinSlip × yawFwdSpeedAtten
```

The forward (carve-coupling) component has:

- **no slope gate** — unlike `bottomSlopeThrust` and every wave-mass force
- **no front-face gate** — unlike `bottomSlopeThrust`, so it drives behind the crest
- **`absSinSlip` unsigned, appearing twice** — drive scales with slip² and is sign-blind
- **no orientation term** — nothing reads `board.up`
- **weak wetting** — `amountWetted` mean 0.73 (exactly 1.0 on only 7.5% of samples: a weak gate,
  not the dead one an earlier note claimed); `effectiveWaterHeight` mean 133
- **v² self-sustain** — faster board → more relative flow → more thrust

`yawHydrofoilCoefficient` was raised **0.001 → 0.02 (20×)** in commit `936a0bce9` (2026-08-21),
whose message is explicit that the intent was **handling, not propulsion**: "changes how much
sideways slip the bottom redirects into a turn rather than sheds as drag — a real handling change."
The forward drive was a side effect. The gates added in July were never applied to it because at
0.001 it did not matter.

**The correct framing** (M6–M8): this term is not substituting for weak wave forces. It is
supplying drive where the wave has none to give. Removing it does not weaken the ride — it makes
staying on the face necessary, which is the game.

Note the consequence: this is a **gameplay change**, not only a physics fix. Today a rider can leave
the pocket and keep speed. After the fix they cannot. That is the intent, but it should be a
conscious decision, not a side effect.

## Requirements

### Functional

- **FR1** The yaw hydrofoil's FORWARD component is gated as passive slope thrust is: a slope gate
  plus the `slopeThrustFrontFaceDir` front-face gate. The **anti-slip side component is left
  untouched** — commit `936a0bce9` shows slip-into-turn redirection was the point of the raise, and
  M8 shows the ride does not need the forward half. The gate is a **floor, not a switch**:

  ```
  fwdGate = yawFwdOffFaceFloor + (1 - yawFwdOffFaceFloor) × slopeGate × frontGate
  ```

  so `yawFwdOffFaceFloor` = 0 removes off-face drive entirely and 1 restores today's behaviour.
  New tunables: `yawFwdSlopeGateMin` (0.10), `yawFwdSlopeGateWidth` (0.06), `yawFwdOffFaceFloor`
  (0.0). The floor is what makes FR4 possible — without it every board would be equally unforgiving
  and there would be nothing left to tune per board.
- **FR2** `slopeThrustMinSlopeSin` and `waveMassMinSlopeSin` are lowered to bite over the
  **0.10–0.30** band, where ungated slope thrust measures +12,194 to +19,498 and therefore does real
  work — concretely **0.35 → 0.10** for both. Below 0.10 the setting is irrelevant (M6), so this is
  not a flat-water fix and must not be justified as one; the 0.10 floor is kept so genuinely flat
  water between waves stays unpowered, which is the deadzone's original purpose. The 0.35 values were set on 2026-07-23 to fix a **wave-catch** problem, and
  wave-catch is no longer force-driven (NFR1) — so they are legacy constraints carrying a job they
  no longer have, and the lower bound is now a ride-quality question, not a takeoff-timing one.
- **FR3** ~~The forward drive attenuates on flat/slow water rather than on fast water.~~
  **Withdrawn — the requirement was confused.** It conflated *speed* with *position on the wave*: a
  slow board on a steep face should get more drive, not less, so "attenuate when slow" is the wrong
  rule. The variable that should decide off-face drive is slope (FR1), and the existing speed taper
  is a v²-runaway limiter that is not making a claim about the wave at all. Previously recorded as
  "subsumed by FR1", which was the right conclusion for the wrong reason. FR1's slope gate already takes the forward drive to
  `yawFwdOffFaceFloor` on flat water, which is what M4 was asking for. Reversing the existing
  `yawThrustAttenStart/End` band on top of that would be a second, redundant change to a mechanism
  that exists for a different job — taming the down-the-line surge spike
  ([yaw-thrust-speed-attenuation](yaw-thrust-speed-attenuation.md)) — so it is left alone. Revisit
  only if surging reappears after FR1.
- **FR4** `yawFwdOffFaceFloor` is set per board in `Content/Boards/<id>.json`. Because FR1 makes
  off-face drive the thing the floor controls, this knob **is** "how much does leaving the pocket
  cost you" — a wide effective pocket on the foamie, none on the shortboard. That is precisely what
  [board-selection](board-selection.md) already claims boards are ("the board *is* the difficulty
  setting"), so the per-board tuning task this audit started from and the fix turn out to be the
  same dial. Shipped values, monotonic with the existing difficulty ratings:

  | board | difficulty | `yawFwdOffFaceFloor` |
  |---|---|---|
  | foamie | 1 | 0.35 |
  | funboard | 2 | 0.25 |
  | fish | 3 | 0.18 |
  | hybrid | 4 | 0.12 |
  | shortboard | 5 | 0.00 |

### Non-functional

- **NFR1** ~~No re-introduction of the early takeoff the 0.35 gates fixed.~~ **Largely void as of
  2026-08-25**: the intro runs on rails ([deterministic-ride-handoff](deterministic-ride-handoff.md)).
  `SurfRails::AreForcesSuppressed()` gates every impulse site while the board is kinematic, so the
  pop-up and wave-catch are a replayed trace, not a force balance, and the board is handed to physics
  *already planing* — measured `HANDOFF STATE: slopeSin=0.383 surgeSpeed=529.3 planing=0.750`.
  Forces only have to work post-handoff. The early-takeoff constraint that produced the 0.35 gates
  ([steep-face-takeoff](steep-face-takeoff.md), [wave-catch-propulsion-budget](wave-catch-propulsion-budget.md))
  therefore no longer binds FR2. **One residue:** `StartRails` falls back to the live physics intro
  when its trace is missing, so keep the force path from being catastrophic at low gates — but do
  not tune FR2 for it.
- **NFR2** Snapshot baselines are stale and cannot arbitrate this change; their REGRESSION verdicts
  are not evidence either way.

## Acceptance criteria

- **AC1** On the `surfing-down-the-line` autopilot, the board sustains **≥ 600 cm/s mean** with
  riding (`|v| > 300`) **≥ 90%** — i.e. no worse than stock's 546 / 91%, and ideally near the
  corrected run's 648 / 97% (M8).
- **AC2** In the window `slopeSin < 0.10, |v| > 400`, the TOTAL board-forward force excluding the
  `gravity` and `buoyancy` board-frame projections is **negative** on a clear majority of ticks
  (today: positive on 76%).
- **AC3** In the window `distToCrest > 0, |v| > 300`, `bottomHydrofoilYaw` forward drive is
  approximately zero (today: +6,461 on 100% of ticks).
  *(The former AC3 — "riding fraction on the player trace stays within ~10% of 33%" — was **wrong**
  and is withdrawn. It made the bug its own success criterion, and M9 shows trace replay cannot
  measure this at all.)*
- **AC4** Coast-down after the drive ends shortens measurably from the recorded baseline: 3.1–3.7 s
  and 5.2–7.9 m from the last moment above 400 cm/s, consistent across four rides.
- **AC5** ~~Takeoff timing unchanged.~~ Withdrawn — takeoff is kinematic (NFR1), so no force change
  can move it. Replaced by: the `HANDOFF STATE` line is unchanged, confirming the fix is measured
  entirely post-handoff.
- **AC6** Player confirms on a **fresh ride** (never a replay, per M9) that the board slows on flat
  water and behind the crest, and that the wave still drives it on the face.

## Resolved questions

- **Are the other forces simply tuned too low, with the 20× yaw raise compensating?** No — M6 and
  M8. Ungated slope thrust delivers +12,194 to +19,498 in the 0.10–0.30 band, and on a line that
  holds the face the wave alone sustains 648 cm/s, better than stock. The wave forces are correctly
  sized where slope exists; they are near-zero off the face because they are proportional to slope,
  which no coefficient can change.
- **The buoyancy board-forward anomaly.** Resolved as a projection artefact, not a force bug. These
  are board-frame components of world-vertical forces and they swing with board attitude: `gravity`
  measured −223 in one window and **+7,588** in another; `buoyancy` +3,428 and −1,436. Any
  propulsion metric that includes them is misleading — hence AC2's exclusion.

## Open questions

- **The inverted case is not directly measured.** No replay capsized (max roll 43° and 32°), so
  symptom 3 rests on the code reading plus M3. FR1 should fix it incidentally — an inverted board
  off the face gets no slope drive — but that is a prediction, not a measurement.
- **How low can FR2 go?** No longer bounded by takeoff (NFR1), so this is purely a ride-quality
  sweep over the 0.10–0.35 range — and 0.0 is now a legitimate candidate rather than an extreme, as
  M6 and M8 both ran there without a takeoff to disturb.
- **Is the 3 m core (M7) the intended wave shape?** FR1 promotes a wave-geometry property nobody
  chose deliberately into the game's primary difficulty parameter. The profile comes from the
  precomputed wave dataset (`Content/Waves/chunks_ratio_*` via `AWaveHeight`); **no coefficient in
  `SurfTuningSubsystem` widens it**, so if 3 m is too tight the answer is different wave data, not
  tuning. FR4's floor is the cheap mitigation; softening the cliff behind the crest specifically is
  the next one.
- **Does the player see the face they must now hold?** The render meshes sit ~40 cm below the
  wave-height data. Cosmetic while off-face drive was free; once FR1 makes pocket-holding decisive,
  a visual/physical offset becomes a fairness problem. Cheap to check, worth checking before ship.
- **`distToCrest` carries residual uncertainty.** The crest scan once picked its direction from
  ~0.02 cm of noise. Fixed, but the ±150 cm boundaries in M7 are softer than they look.

## Reproducing the measurements

```bash
$env:TEST_MAP="Surfing_infinite_wave"

# force budget on a player trace
$env:EXTRA_ARGS="-ReplayTrace=phone-2026-09-05-10-37-31.csv -usefixedtimestep -fps=60 -BoardInTests -Board=shortboard"
RunGameAndCollectLogs.ps1 -Argument "crossing,torque:SharedCalculations"

# yaw-hydrofoil internals
RunGameAndCollectLogs.ps1 -Argument "thrust,crossing:bottom"

# the validation run — a scripted line that holds the face (M8)
$env:EXTRA_ARGS="-BoardInTests -Board=shortboard"
RunGameAndCollectLogs.ps1 -Argument "crossing,torque:SharedCalculations:surfing-down-the-line"
```

`CROSSING` is `IsFlagSet` and needs no actor list. `TORQUE` and `Yaw Hydrofoil` are `ShouldDebug`
and produce **nothing** unless `surf.debug.actors` is non-empty. Join the two line families on the
log's `][ frame ]` index.

A/B conditions go in `Saved/TuningOverrides.json` (global layer, beats every board profile);
`SurfTuningSubsystem` logs "Loaded N GLOBAL overrides ... (0 stale skipped)" — check that count
before trusting a run. See [runtime-tuning](runtime-tuning.md) for why a launch-time CVar will not
stick.
