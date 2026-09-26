# Spec: the wave interaction as damping and redirect, not forces

## Status

- [x] Superseded design established (2026-09-08). Replaces the force-clamping approach in
      [wave-mass-thrust-closing-speed.md](wave-mass-thrust-closing-speed.md), which stays as the
      measurement record (M1–M10) and as the reason that family was abandoned.
- [x] Principle agreed with the player: **forces accelerate well and hinder badly** — they overshoot
      into the opposite direction. That is why this project already uses engine damping instead of
      braking forces, and velocity redirects instead of rotating forces.
- [x] Ordering resolved: the redirect gets first claim on the velocity, damping takes only the
      residue. This is what makes the rocker character emergent rather than tuned.
- [x] **Corrected 2026-09-08**: an earlier draft claimed gravity would become the only energy source.
      It does not. The wave keeps driving the board through FR1's damping plus the redirect — the
      sail ratchet — which is correct physics and **is not bounded by FR1**. Terminal speed is set by
      drag and must be measured (AC7), not assumed.
- [x] **Open question 1 answered (2026-09-08): the planing redirect IS alive.** Gate mean 0.411,
      above 0.5 on 30% of ticks, ~47°/s of authority. M8's null result was because down the line the
      velocity already points near the target, so there is little angle to close. FR2 and FR4 do not
      rest on nothing.
- [x] **FR1 implemented** — `p.Chaos.Solver.WaveNormalDampingRate` + gate, normal and water velocity
      fed each tick from `ASurfboardUtils`. Tunable `waveNormalDampingRate`, **default 0 = off**.
- [x] **FR2 implemented** — the block sits after the planing redirect, before the velocity ceilings.
- [x] **FR3 implemented** — normal axis only.
- [x] **M11 measured: the swap kills the surge.** See below. Ships OFF pending the re-tune, because
      it costs 23% of mean speed.
- [ ] Re-derive the provisional values (the gates especially) to recover that speed.
- [ ] FR4 — `PlaningRedirectMaxAngle` per board (rocker).
- [~] FR5 — **partly WITHDRAWN 2026-09-23.** `wavePenetrationDrag` stays retired (coefficient 0).
      `waveMassThrust` and `waveMassFlowDrag` are back at **0.0001** — see "FR5 revisited" below.
- [ ] FR6 — preserve the nose-up authority that retiring `waveMassThrust` removes.
- [ ] FR7 — every part independently A/B-able with a documented disable value.
- [ ] Player-validated on device.

## Why the previous approach failed, in one paragraph

The surge was chased by clamping forces: first one term
([wave-mass-thrust-closing-speed.md](wave-mass-thrust-closing-speed.md) FR1'), then the summed
forward drive (FR6, built and measured). FR6 engaged exactly as designed — the three governed terms
were cut 45–70% — and the net forward force on surge ticks moved **12%**, because the forces excluded
to protect the sideways-block and nose-up roles grew to replace them. The ride got *spikier*. The
lesson recorded there: the compensation is **positional** — compressing drive changes where the board
sits on the wave, and the flow-driven forces are functions of exactly that, so a force-level governor
cannot close a loop that runs through the board's position.

This spec stops trying to close that loop and removes it instead.

## The principle

| job | right mechanism | why |
|---|---|---|
| add energy | **force** | forces accelerate cleanly |
| remove energy / resist motion | **damping** | a resisting *force* overshoots and reverses; damping asymptotes |
| change direction without changing speed | **velocity redirect** | a rotating *force* overshoots the same way |

The wave-mass family currently does all three jobs with forces. That is the defect. Measured
evidence that it is a defect and not a style preference: in M9, `dragBottom_wavePenetration` — a term
whose entire purpose is to resist the board crossing the face — went from **−2,177 (resisting) to
+12,507 (driving)**. A resistive force became a propulsive one.

## The architecture

| job | today | becomes |
|---|---|---|
| wave imparts momentum (crash push, board along/against the crash) | `waveMassThrust` + `waveMassFlowDrag`, absolute-flow forces | **damping toward the water velocity** (FR1) |
| resist penetrating the wave, nose-first or sideways | `wavePenetrationDrag`, a v² force | **the same damping** (FR1) |
| nose into the wave → nose up the face, speed preserved | nothing explicit; the planing redirect exists but is untested down the line | **planing redirect, per board** (FR4), running *before* the damping (FR2) |
| sideways push → forward, down the line | `waveMassThrust` | **carve grip** — already does this losslessly (M8) |
| energy in | `bottomSlopeThrust` / `waveSlopeGravity` | **unchanged** — a force, correctly shaped |

### The wave is still an energy source, and the ratchet is not bounded by FR1

An earlier draft of this spec claimed that after the redesign "gravity down the face is the only
energy source". **That is wrong, and the error matters enough to state plainly.**

The redirect is speed-preserving and adds nothing on its own. But it does not act alone:

1. The wave pushes the board along `n` — FR1's damping accelerates it toward the water's normal
   velocity. **This adds kinetic energy.**
2. The carve grip rotates that normal-direction velocity into down-the-line velocity, preserving
   magnitude.
3. The board's velocity along `n` is now ~0 again, so `v_rel·n` is back to ≈ −u_n, and the damping
   pushes it again.

Each cycle injects momentum along `n` and the redirect banks it as tangential speed. This is the
sail/iceboat effect — a lift device *can* exceed the flow speed, which is how a surfer generates
speed down the line. It is correct physics and it must be kept.

**But it is not self-limiting, and FR1's asymptote does not bound it.** FR1 bounds the board's speed
*along the wave normal*. It says nothing about tangential speed, and because the redirect keeps
emptying the normal component, `v_rel·n` stays at ≈ u_n however fast the board is travelling down the
line — so the injection rate never tapers.

What bounds it is **drag**: tangential drag power grows as v³ while the injection rate stays roughly
constant, so a terminal speed exists. That is a real equilibrium, the same one a sailboat has — but
it is an equilibrium that must be **measured and landed somewhere plausible** (AC7), not something
this architecture guarantees.

So, precisely, what the redesign fixes and does not:

- **Fixed** — the overshoot and sign-flip of resistive forces; the board being driven forward while
  outrunning the water along `n`; injection magnitude (unbounded flow² force → bounded damping).
- **Not fixed by construction** — the ratchet. It is weaker, because injection per unit time is now
  bounded instead of growing with flow², but the mechanism survives.

If terminal speed comes out too high, the levers are the **redirect rate**, the **damping
coefficient**, or **tangential drag**. Not a force clamp: that family was disproved by FR6.

## Requirements

### FR1 — damp the wave-normal component of the velocity **relative to the water**

Damping toward zero is damping in the world frame, which is wrong on a moving wave: a stationary
board with a wave arriving would be held still instead of being carried. Damp toward the water:

```
v_rel_n = (V − V_water) · n                       // n = horizontal wave normal
V      += n * v_rel_n * (FrDampMul(D, Dt) − 1)
```

Only the normal component of the *relative* velocity decays. Three behaviours must follow without
being special-cased:

1. **Board still, wave arriving** — `v_rel·n < 0`, the board is drawn toward moving *with* the wave.
   This is the crash push, asymptotic to the water's speed, incapable of overshoot.
2. **Board charging the face** — `v_rel·n > 0`, decays. The board is stopped relative to the *water*,
   not the world.
3. **Board and water moving together** — `v_rel·n ≈ 0`, no effect. No spurious braking during a
   normal ride. `wavePenetrationThreshold`'s 400 cm/s deadzone exists today only to fake this and is
   retired with the term.

The board asymptotes to the water's normal-direction velocity and **cannot pass it**. That is the
momentum-transfer bound FR6 tried and failed to impose with a governor; here it is structural, and
there is no force left to leak around it.

Precedent for damping toward a non-zero target: `PitchRightingRate`, already in the fork and
documented as "the attitude analog of the velocity redirects". This is its linear twin.

### FR2 — the damping runs **after** the planing redirect

The solver order today is `DampingZUp/Down` → `DampAsymmetricalLinear` → carve grip → wave-carry →
planing redirect → velocity ceilings. **Damping first, redirects second.** Placing FR1 with the
existing damping would remove the into-wave velocity and *then* offer the remainder to the redirect,
destroying exactly the speed the nose-up redirect exists to preserve.

Place FR1 after the planing redirect. The hull then turns what it can turn, speed-preserving, and the
damping only ever sees the into-wave velocity the hull **could not** turn.

**This ordering is the whole design, not an implementation detail.** It is what makes FR4's rocker
character emergent: a board that redirects hard leaves little residue and keeps its speed; a flat
board redirects little, leaves a large residue, and the damping takes it — which is pearling, arising
from the mechanism rather than tuned in.

### FR3 — anisotropic: the wave normal only

Damp along `n` and nothing else. Damping the down-line or flow-tangential component would drag the
board to the water's speed along the wave and make it impossible to outrun a section — the opposite
of surfing. The tangential plane is left to the existing local-axis damping and the redirects.

### FR4 — `PlaningRedirectMaxAngle` becomes per-board

Rocker is how well a hull turns oncoming flow instead of digging into it, and it is the reason a
shortboard handles a steep face that a flat funboard pearls on. Today the rate is global (2.0 rad/s),
so every board redirects identically and that character is absent.

**No new plumbing is needed.** `PlaningRedirectMaxAngle` is a float tunable on `USurfTuningSubsystem`,
and the board-profile loader resolves *any* float tunable by name — so adding it to
`Content/Boards/<id>.json` makes it per-board immediately. Shortboard high, funboard low.

### FR5 — retire the three forces

`waveMassThrust`, `waveMassFlowDrag` and `wavePenetrationDrag` are removed, their jobs taken by FR1
and the existing redirects. Retire the tunables with them rather than leaving dead coefficients
(`waveMassThrustCoefficient`, `waveMassFlowDragCoefficient`, `wavePenetrationCoefficient`,
`wavePenetrationThreshold`, `waveMassMinSlopeSin`, and the decouple knobs that exist only to strip
torque these forces should not have been producing).

Do this **last**, behind FR7's switches, so the A/B against the current build stays possible until
the replacement is validated.

#### FR5 revisited (2026-09-23) — two of the three come back, as a lip term

Never executed: the coefficients were defaulted to 0 (`d4dbb60c1`) but the code and the tunables
stayed. That turned out to be the right call. Owner, after a PC editor pass alongside the carve-grip
gate and the airborne force gating:

> Together with increasing the wave mass coefs, which gives the lip a bigger punch, I think this
> looks good. I set both waveMassDragCoef and waveMassThrustCoef to 0.0001, set them as defaults.

So `waveMassThrustCoefficient` and `waveMassFlowDragCoefficient` are **0.0001**, and
`wavePenetrationCoefficient` remains 0.

**This does not reopen the argument FR1 settled.** 1e-4 is three and a half decades below the 0.003
these were retired from. At that magnitude they are not the unbounded drive
[wave-mass-thrust-closing-speed.md](wave-mass-thrust-closing-speed.md) measured — they are the lip's
punch, and the damping and redirects still own the wave interaction. The "half a swap is worse than
either side" warning in `SurfTuningSubsystem.h` stands; only the from-values in that control set
moved.

**Do not retire the tunables.** This is the second time this family has been needed after being
written off. Leave the coefficients in place.

**Open, and the reason this was not fixed in the same pass:** neither force is contact-gated. Both
recompute `boardWideEffectiveH` locally — [FluidDynamics.cpp:264](../Source/GoneSurfing/FluidDynamics.cpp#L264),
[:311](../Source/GoneSurfing/FluidDynamics.cpp#L311), [:494](../Source/GoneSurfing/FluidDynamics.cpp#L494) —
from `baseHeight + boardWideWaterColumnAbove + boardWideSlopeSin * slopeHeight`, bypassing the
`waterContactGate` that `effectiveWaterHeight` now carries. Harmless while the coefficients were 0;
live again now, and their slope gate peaks exactly where the lip throws the board clear — the same
signature `lipImpact` had. See [airborne-force-gating.md](airborne-force-gating.md). Left alone on
purpose: the owner validated the feel **ungated**, so gating it changes what was validated.

### FR6 — restore the nose-up authority that FR5 removes

`waveMassThrust` is applied with `AddImpulseAtLocation` at the **actor** position, not the CoM, so on
fore/aft bottom actors it produces pitch — an unintended but real contribution to the nose-up role.
Retiring it removes that authority.

Restore it through the mechanism that properly owns the role: the fork's asymmetric pitch damping
(`AngularDampingYAwayExtra` / `AngularDampingYTowardReduction`) or `PitchRightingRate`. **Not** by
keeping a forward force for its side-effect torque.

### FR7 — every part independently switchable, with the disable value documented

Each of FR1–FR4 needs one value that reproduces the current behaviour exactly, so the pieces can be
A/B'd separately through `Saved/TuningOverrides.json` with no recompile. The house convention is a
comment naming the value (`yawFwdOffFaceFloor` 1.0, `forwardDragWettingGate` 0,
`propulsionCeilingWeights` 0).

The fork needs the water velocity, which it does not have today. Feed `WaveNormalX/Y`,
`WaterVelX/Y/Z`, a rate and a gate as `p.Chaos.Solver.*` CVars, exactly the way the project already
feeds `CarveHeadingX/Y` and `PlaningRedirectUpX/Y/Z` every tick from `ASurfboardUtils`.

### NFR1 — frame-rate independent

Use `FrDampMul` (`pow(1-D, Dt/RefDt)`), like every other damping in the fork. A per-frame decay would
make the wave interaction frame-rate dependent, and this project has already had one wrong conclusion
from a frame-rate confound.

### NFR2 — board-wide, not per-actor

FR1 acts on the rigid body's velocity in the solver, so it is inherently board-wide. Do not
reintroduce a per-actor variant: symmetric left/right actors sampling different wave normals is what
produced spurious yaw torque before (`specs/per-actor-vs-board-wide-sampling.md`).

## Deferred — generalise the planing redirect to the surface tangent plane

**Not part of this change. Raised by the player 2026-09-08 and recorded so it is not lost.**

The planing redirect currently rotates velocity toward `-waveSlopeDownVec` — specifically *up the
face*. That is a special case. What a hull physically does is rotate its velocity toward the **local
water surface tangent plane**, whichever way it is already heading:

- **down a steep face, nose first** — deflected to run *along* the face. This is the missing case,
  and it is what should stop a rockered board pearling on a steep drop.
- **up the face** — the behaviour that exists today.
- **flat water** — deflected to horizontal, i.e. the board planes instead of diving.

The board does not know whether it is on a wave or on flat ocean; it reacts to the surface it is on.
One mechanism should cover all three, with rocker (FR4) setting the rate.

Two things whoever picks this up should know before starting:

1. **It inherits the `PlaningRedirectOutwardScale` problem, it does not remove it.** That one-sided
   escape exists because the redirect was cancelling buoyancy's push when a buried nose tries to
   surface. A general tangent-plane redirect rotates outward velocity into the plane just as
   readily, so the escape is still needed.
2. **It converges with FR1 on a steep face.** FR1 damps along the *horizontal* wave normal; this
   redirect works on the *surface* normal. On flat water those are orthogonal and independent. On a
   steep face they overlap substantially, so the split between "what the redirect turned" and "what
   the damping removed" becomes sensitive to face steepness — exactly where it matters most. FR2's
   ordering (redirect first, damping takes the residue) still holds, but the residue is no longer
   obviously small.

## Deferred — retire the drag forces

**Not part of this change. Raised by the player 2026-09-08.**

The drag terms were designed to pull the board toward the water velocity, with damping as the
separate mechanism that slows it toward zero without overshooting. Once FR1 provides a
relative-to-water pull, the drags are largely doing a job something else now does better — the same
observation that produced this spec, applied to an older layer.

Measured mean forward contribution, all ticks (M2's budget):

| term | mean F_fwd | note |
|---|---|---|
| `dragBottom_forwardDrag` | −8,213 | the big one |
| `dragRail` | −2,618 | sideways axis, doubled by `SurfboardSidewaysDamping` 0.0975 |
| `dragRailFlow` | −158 | |
| `dragFin` | −69 | negligible |
| `dragTail` | **+18** | a *drag* term that on average pushes the board FORWARD — the same sign-flip pathology as `wavePenetrationDrag` |

**The constraint that gates this work.** FR3 confines FR1's damping to the wave normal so the board
can outrun the water down the line, which means **FR1 does not touch the tangential axis** — and the
tangential axis is what bounds the sail ratchet. AC7's terminal speed *is* the tangential balance.
`SurfboardForwardsDamping` must therefore keep owning that axis; retiring both it and the forward
drag would leave nothing holding the ratchet and speed would climb to `MaxVelocityX/Y` — the surge
back as a hard clamp instead of an equilibrium.

Suggested order, measuring between each:

1. `dragTail`, `dragFin` — near-zero contribution and one has the sign pathology. The cheapest test
   of the whole thesis.
2. `dragRail` — lean on `SurfboardSidewaysDamping`, already on the same axis.
3. **Measure AC7** before touching the forward axis.
4. `dragBottom_forwardDrag` — retires to `SurfboardForwardsDamping`. `forwardDragWettingGate`
   (commit `a49b76e49`) retires with it, and its measured side-effect (time-behind-crest
   20.6% → 35.2%) should unwind, which helps AC4.

**Known residual after step 4, and it is accepted.** `SurfboardForwardsDamping` pulls toward zero in
the *board* frame, while the drag it replaces pulled toward the *water* velocity. That is the same
world-frame approximation FR1 removes on the normal axis, left in place here deliberately. It is
second-order: down the line the board does 500–1400 cm/s against water doing ~50–330, so it is a
10–30% bias in the braking rate, not a sign error. It will bias terminal speed slightly low.

**The mechanism on this axis is settled — damping, not drag.** If AC7 lands terminal speed low, the
only thing in play is the `SurfboardForwardsDamping` coefficient (and its per-board values). Do not
reinstate a drag force, and do not extend FR1's relative-to-water formula onto the tangential axis;
either would reopen a decision that has been made.

**Do not bundle fin *lift* into this.** It is directional stability and anti-slip, not drag. The
carve grip may well duplicate it, but that is a separate question and must not ride along with a drag
retirement.

## M11 — first measurement of the swap

Trace `phone-2026-09-07-19-13-56`, shortboard, fixed 60 Hz, the player's hand-test board overlay held
constant. "Swap" = the three forces zeroed through their coefficients (FR7's switches) with the
damping on.

| | baseline | swap .03 | **swap .06** | swap .12 |
|---|---|---|---|---|
| p95 acceleration (the surge) | 1353 | 930 | **637 (−53%)** | 735 |
| time above 1000 cm/s | 1.9% | 0.0% | **0.0%** | 0.0% |
| speed SD | 317 | 265 | **250 (−21%)** | 237 |
| p90 \|accel\| | 545 | 450 | **388 (−29%)** | 346 |
| mean speed | 238 | 192 | **183 (−23%)** | 179 |
| time below 200 cm/s | 62.8% | 68.8% | **71.1%** | 72.1% |

**The surge is gone** — the first thing in this investigation to actually remove it (FR6 managed 12%
on the net force and made the ride spikier; this halves p95 acceleration and eliminates time above
1000 cm/s entirely). Rate 0.06 is the knee: 0.12 buys almost nothing more and stalls more.

**It costs 23% of mean speed**, which is expected — three propulsion terms were removed and nothing
re-tuned. **`slopeThrustCoefficient` is not the lever**: +41% on it moved mean speed 183 → 187 → 184,
flat. That points at the slope *gates*, not the coefficients, which matches the known finding that
`slopeThrustMinSlopeSin` 0.10 excludes most of the ride. Those gates are already on the provisional
list to re-derive, and M6's inflated `slopeSin` feeds them.

**Do not run the damping with the forces still on.** Measured at rate 0.15 with everything active:
RMS acceleration +80%, p99 +108%. The two mechanisms do the same job and double-handle.

### The damping axis is NOT the sampled wave normal

> **Verified visually 2026-09-08, and the wording below is corrected by it.** Drawn on the wave with
> `surf.debug.wavenormals`, the horizontal projections form a band along the crest in which red
> (along-crest) and green (cross-shore) **alternate sample to sample**. That is noise, not a
> systematic rotation — a wrong transform or swapped axis would give coherent regions of red. So the
> sampled normal is **unreliable**, not *rotated*, and the claim that `wavePenetrationDrag` resists
> along "a wrong axis" overstates it: it resists along a noisy one, right most of the time.
> The decision stands unchanged, for the stability reason rather than the correctness one.


Implementing this surfaced a defect in the term it replaces. `wavePenetrationDrag` uses
`waveNormalHoriz`, the per-tick sampled wave normal. Measured over a ride, that normal is:

- cross-shore dominant only **78%** of the time — it points **down the line** on the other 22%
- mean \|y\| 0.369 against \|x\| 0.830, with **22.5°** of tick-to-tick jitter at p90 and 180° flips

It carries the exporter's inflated Y component (M6). Damping along it would intermittently brake the
board down the line, which is exactly what FR3 forbids — **so FR1 uses `resolvedWaveBackDirection`
instead**, derived from the tile geometry by hill-climb rather than sampled. Measured after the
switch: **100% cross-shore, 0.1° jitter at p90.**

This means the existing `wavePenetrationDrag` has been resisting along a wrong axis 22% of the time.
Another reason FR1 should be an improvement on it, not merely a re-shaping.

## M12 — the slopeSin signal, and where the lost speed actually was

### M6's inflation does NOT reproduce along the ride

Step 2 was premised on M6: `slopeSin` inflated 1.59x median because the stored normals are noisy, and
noise inflates a MAGNITUDE (it cannot cancel the way a direction error can). A height-derived normal
was added behind `waveNormalHeightDelta` (central-difference the height field; 0 = stored normals)
with a `slopeprobe` flag logging both. Over 3954 samples of a real ride:

| | stored | height-derived |
|---|---|---|
| mean slopeSin | 0.1193 | **0.1400** |
| median | 0.0691 | 0.0944 |
| ticks above the 0.10 gate | 36% | **48%** |

**The stored slopeSin is 0.85x the derived — slightly DEFLATED, not inflated.** Ratio p10 0.27,
p50 0.86, p90 2.14: scattered both ways. The two normals differ by 4.5 deg at the median.

M6 measured one grid row at one frame against raw grid-spacing finite differences; the ride samples
through the bilinear interpolator, which smooths differently. **Do not carry M6's 1.59x forward** —
in situ the two agree within ~15% on the mean.

### Height-derived normals are still worth having, for smoothness

In the swap configuration they buy: p99 acceleration **-24%** (1530 -> 1156), RMS accel -15%,
churn -13%, and +2% mean speed. Not the speed fix, but a calmer ride from a cleaner signal.
(On the *unswapped* build they raise mean speed 238 -> 267 but also p99 accel 1559 -> 1976 and time
above 1000 cm/s 1.9% -> 2.7% — opening the gates feeds the forces that were still on.)

### The lever for the lost speed was the gate THRESHOLD

Three things were tested in order: `slopeThrustCoefficient` (+41% moved mean speed 183 -> 187 -> 184,
flat), the slopeSin *signal* (183 -> 187), and finally the *threshold* itself:

| | baseline | swap .06 | swap+h60 | **+ gate 0.05** |
|---|---|---|---|---|
| mean speed | 238 | 183 | 187 | **215 (-10%)** |
| p95 accel (surge) | 1353 | 637 | 706 | **598 (-56%)** |
| time above 1000 cm/s | 1.9% | 0.0% | 0.0% | **0.0%** |
| coefficient of variation | 133.4 | 136.4 | 130.7 | **125.8** |
| RMS accel | 352 | 302 | 256 | **291** |
| time below 200 cm/s | 62.8% | 71.1% | 69.6% | **66.0%** |

`slopeThrustMinSlopeSin` / `yawFwdSlopeGateMin` 0.10 -> 0.05 recovers most of the gap. 0.02 measures
identically to 0.05, so 0.05 is sufficient. Note the surge gets **better** as the speed comes back,
not worse — the gate opening feeds slope thrust, which is the correctly-shaped force.

### The candidate configuration, for a ride test

```json
{ "waveMassThrustCoefficient": 0, "waveMassFlowDragCoefficient": 0, "wavePenetrationCoefficient": 0,
  "waveNormalDampingRate": 0.06, "waveNormalHeightDelta": 60,
  "slopeThrustMinSlopeSin": 0.05, "yawFwdSlopeGateMin": 0.05 }
```

Surge more than halved, sawtooth calmer on every aggregate, speed within 10% of today, and the board
never exceeds 1000 cm/s. **Not ridden yet** — that is the gate on all of it.

## Acceptance criteria

**AC1 — the surge is gone, structurally.**
Given the trace `phone-2026-09-07-19-13-56` replayed on the shortboard at fixed 60 Hz,
then no tick has the board's along-`n` speed exceeding the water's along-`n` speed by more than the
redirect can account for, and time above 1000 cm/s falls below 1.0% (from 1.9–3.2%).
Scored on whole-ride aggregates — speed SD, RMS accel, p95 accel — never single-event maxima.

**AC2 — the crash push still works.**
Given a stationary board with a wave arriving,
then the board is carried, reaching the water's normal-direction speed asymptotically and never
exceeding it.

**AC3 — wave catch survives.** Every board still catches the wave and reaches planing within the same
step count, and `surfing-down-the-line` still completes.

**AC4 — the glide-through block is at least as good.**
Time-behind-crest and excursion duration no worse than today (20.6% / 0.41 s were the pre-wetting-gate
figures; 35.2% / 1.01 s the current ones — do not push them further). FR1 should *improve* this: it
resists correctly on a moving wave, where the v² force was measuring against the world frame.

**AC5 — nose-first still becomes nose-up, with speed preserved.**
Peak nose-down pitch rate and time to surface alignment within 15% of today (FR6), **and** the speed
retained through the redirect is measurably higher than today — that is the point of FR2's ordering.

**AC7 — the sail ratchet reaches a plausible terminal speed.** *(the criterion the architecture does
not guarantee — see "The wave is still an energy source")*
Given a sustained down-the-line ride with the board held on the face,
then tangential speed settles to a terminal value set by the drag balance rather than climbing for as
long as the ride lasts, and that terminal value is in the range a shortboard should reach on a 3.3 m
face — **not** the 1371 cm/s (49 km/h) the current build reaches.
Measure it directly: a long steady ride, tangential speed against time, looking for a plateau. If
there is no plateau, the ratchet is unbounded in practice and the redirect rate or tangential drag
has to take it — not a force clamp.

**AC6 — the boards differ, for a reason.**
Given the same input on shortboard and funboard with FR4's per-board values,
then the shortboard retains materially more speed through a steep-face entry and the funboard loses
more to the damping residue. If they behave the same, FR4 is not doing anything.

## Test cases

| # | setup | expectation |
|---|---|---|
| T9 | long sustained down-the-line ride, tangential speed vs time | AC7 — a plateau, not a climb. **Run this early**: it tests the one thing the architecture does not guarantee |
| T1 | all FR disable values set | force budget and ride stats match the current build — proves the escape hatch |
| T2 | FR1 only, board stationary, wave arriving | AC2 — carried, asymptotic, no overshoot past water speed |
| T3 | FR1 only, board charging the face at speed | stopped relative to the water; **T4 is the contrast** |
| T4 | FR1 + FR2 ordering, same charge | markedly more speed retained than T3 — isolates the ordering claim |
| T5 | FR1 with the damping placed *before* the redirect | should be visibly worse than T4. If it is not, FR2's premise is wrong |
| T6 | FR3 disabled (isotropic damping) | board is dragged to water speed down the line — confirms why FR3 exists |
| T7 | shortboard vs funboard, FR4 values set | AC6 |
| T8 | full stack vs current build, both traces | AC1, AC3, AC4, AC5 |

## Open questions

1. **The planing redirect has never been shown working.** M8 measured it as having *no detectable
   effect* on a down-the-line ride (−4% useful forward speed, inside replay noise). It is
   nose-submersion gated, so that is consistent with it being a drop-in mechanism — but this spec
   leans heavily on it, so **verify it does its job before building on it.** If it does not, FR2 and
   FR4 rest on nothing.
2. **Its gates were tuned against the force it is replacing.** `PlaningRedirectCrestFade` 600, the
   nose-submersion gate and the one-sided `PlaningRedirectOutwardScale` escape were all derived with
   `wavePenetrationDrag` present. Expect to re-derive them.
3. **The redirect is not free.** Rotating toward up-the-face preserves speed in the world frame but
   costs height, and gravity takes the speed back on the climb. Correct surfing, but the damping
   residue is not the only cost of a steep entry.
4. **Do the redirects compete?** Carve grip, wave-carry and planing redirect all rotate velocity and
   the fork notes the first two "find an equilibrium". Adding weight to the third may disturb it.
5. **Does `n` want to be the horizontal normal or the full 3D one?** `wavePenetrationDrag` uses the
   horizontal normal so that down-the-line motion has ~zero normal component. FR1 inherits that
   choice; a lip pitching over may want the vertical component too.
6. **Should the planing redirect target the surface tangent plane rather than up-the-face?** See
   "Deferred" above — the down-the-face case is currently unhandled, and generalising may change
   FR4's per-board values.
7. **The `slopeSin` inflation (M6) is still open** and still upstream of the gates. Deferred by the
   player as most likely a scaling issue; if it is ever corrected, this spec's tuning is re-measured,
   not carried over.

## Reproducing the measurements

Same harness as the superseded spec — see its "Reproducing the measurements" section for the runner
invocations, the `torque` / `crossing` / `propulsion` / `wavedump` flags, and the gotchas (a
`-ReplayTrace` run never calls Quit so the runner exits 2 on timeout; `surf.debug.actors` must be
non-empty for the actor-gated dumps; `Saved/BoardTuning/<id>.json` outranks the board profile and
must be checked before trusting any A/B).
