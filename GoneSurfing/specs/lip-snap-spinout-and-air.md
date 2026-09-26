# Spec: The snap at the lip — spin-out today, air when it should be

## Status
- [x] **Investigated 2026-09-18** on the owner's PC shortboard ride `Tests/InputTraces/phone-2026-09-18-10-15-54.csv`
      (replay tracks it; the spin-out reproduces). M1–M4 below.
- [x] **Not new**: the 2026-09-15 code spins out on the same input earlier and harder (M3). The
      owner's feel that "the redirect was lost recently" is not supported.
- [x] **Principle set** (owner, CLAUDE.md "Design principle"): the physics follows real surfing —
      including leaving the water when speed and lip allow — and scoring decides separately what it
      rewards. Airtime scores nothing (trick-scoring D2); air *physics* is not deferred.
- [x] **T1 BUILT 2026-09-18** — a per-sign yaw-rate ceiling in the fork (`p.Chaos.Solver.MaxAngularVelocityZPos/Neg`),
      fed by the project on the sign that turns the nose **toward the wave** only, at
      `YawRateCapMax × min(1, YawRateCapSpeedKnee / v)` (defaults 2.5 rad/s, 200 cm/s). M6–M8 below.
      A symmetric cap was measured and rejected: it blocks the hybrid's recorded 180°/s cutback (M7).
- [x] **T2 BUILT, in a different shape** — the fade itself must stay (a crest-riding board with the nose a
      few degrees over the back must keep sliding shoreward, M7), so the grip fade is **lifted while the
      board yaws toward the wave** (`CarveGripTurnUnfadeRate`, default 1.0 rad/s). M9.
- [x] T3 — **investigated, not built (M10, M11).** A synthetic steep-section snap (809 cm/s, slopeSin
      0.57, 50 cm from the crest) gives at most a ~20 cm hop at any yaw-cap knee: the wave-normal
      damping takes ~200 cm/s in the last 0.25 s before the lip. In the air, residual water forces are
      ~6 % of gravity (not treacle); no landing can ragdoll by itself (no vertical-speed trigger). The
      air needs a decision on the penetration block at the lip — owner's call, see M11.
- [x] T4 — regression, device-like harness (M8): hybrid 490 → 588, 4-snap shortboard 247 → 305,
      `hard_turn_towards_the_wave` unchanged (maxUW 0.76), funboard faster wherever comparable (AC3 note).
- [x] **T5 — player-validated on PC, 2026-09-18** (shortboard): "the board now skids less, and surfs up
      the wave in a much better way"; cutbacks "felt good as well". Shipped values kept (knee 200,
      un-fade 1.0). Android pass not done; knee 250 remains the "more turn" alternative if wanted.
      The PC ride `phone-2026-09-18-14-23-25` (mean 524, max 900, several 300°/s turns away from the
      wave) does not replay under the harness (board never gets going), so no headless read of it.

**Branch:** `wave-crash-2` (all of this week's work; `main` is behind it). **Working tree
discipline:** one session at a time, or separate worktrees — two sessions on one tree cost an
hour on 2026-09-18.

## The report

Owner, PC, shortboard: *"the board still slows down a lot from time to time"* — and, on the
comparison request, *"I feel that the lost redirect of velocity is something rather new."*

## What happens (M1)

`phone-2026-09-18-10-15-54`, replayed with `crossing` + `torque` + `foam` + `thrust` on:

```
  t   |  v   nose  heading  slip(nose−vel)  underW  distCrest | input wf  wr
 5.10 |  644   109    118       −9          0.69    −100     | 0.41 0.30   ← weight goes hard left
 5.40 |  758   107    121      −14          1.00    −150     | 0.34 0.05
 5.60 |  660    88    114      −26          0.95    −200     | 0.31 0.02
 5.80 |  436    63    103      −39          0.99    −150     | 0.31 0.01
 6.00 |  200    39     94      −55          1.00    −150     | 0.34 0.06
 6.20 |  103    26    126      −99          0.99    −100     | 0.46 0.56   ← released; too late
```

- At 758 cm/s, 1–1.5 m in front of the crest, hull fully under, water flowing shoreward at
  300–535 cm/s, the player puts the weight hard left (a snap). The **nose swings 113° → 39° in
  0.7 s (~105°/s)** while the velocity heading only follows 121° → 94°. Slip grows 8° → 55° → 99°.
- Speed collapses 758 → 200 in 0.6 s while **every force along the nose is positive**
  (+7 k … +97 k). No force brakes it; the fork's sideways damping stops a board skidding at
  40–55° of slip, as it is designed to. A spin-out.
- None of this week's gates were involved: `withWave` 1.00, `fwdGate` 1.00, the whitewater
  drive cut's surround read 0.00 (and `BrokenDriveCut` is 0 anyway).
- The milder slowdown at t=1.5–2.0 (738 → 448) is the same shape at half-left weight: nose
  144° → 100°, slip stays under 10°, the redirect keeps up, the board recovers to 600.

## Why the redirect does not keep up (M2)

Two things, both old:

1. **The carve-grip redirect is faded out at the lip.** `CarveGripCrestFade = 600`:
   `gripCrestGate = SmoothStep(0, 600, crestDistFront)` (`SurfboardUtils.cpp`), so at 100–150 cm
   from the crest the grip that should bend the velocity toward the nose runs at **~16 %**. Set on
   2026-07-22 (player-validated then) to kill the bottom-turn punch-through
   (`pitch-righting-and-redirect-escape.md`, memory `bottom-turn-crest-punchthrough`).
2. **The turn authority scales with speed.** `lateralTurnCoefficient` (5400 on the shortboard) ×
   speed: at 750 cm/s a full lean commands ~105°/s of yaw, more than any redirect can bend 750 cm/s
   of momentum through. Same finding as memory `small-weight-shift-sharp-turn` ("fix the gain,
   not righting").

## Tick-exact A/B at t=5.0 (M4)

One value flipped on the tick before the snap ([replay-scheduled-overrides.md](replay-scheduled-overrides.md)):

| t | control: v / nose / slip | `CarveGripCrestFade`=0 | `CarveGripRate` 4→8 (still faded) | `lateralTurnCoefficient` 5400→2700 |
|---|---|---|---|---|
| 5.4 | 758 / 107° / −14° | 765 / 108° / −10° | 759 / 107° / −13° | 764 / 110° / −13° |
| 5.8 | 436 / 63° / −39° | 524 / 69° / −24° | 453 / 64° / −36° | 553 / 79° / −32° |
| 6.0 | 200 / 39° / −55° | 337 / 46° / −22° | 232 / 41° / −48° | **446 / 66° / −36°** |
| 6.4 | 60 / 19° / −159° | 130 / 28° / −11° | 35 / 20° / −138° | 251 / 57° / −43° |
| 7.0 | 57 / 19° / 174° | 122 / 44° / −3° | 58 / 19° / 177° | **289 / 65° / −13°** |
| mean speed t=5–8 | 234 | 293 | 236 | **371** |

- Full grip at the lip removes the *skid* (slip ≤ 24°, back to 0) but the board still loses most
  of its speed: the velocity now follows a nose pointed 30–45° up the face, so it climbs and stalls.
  Doubling the grip rate while it stays faded does nothing — it is the fade, not the rate.
- Halving the turn authority leaves the nose at 57–66°, slip bounded, and the board exits the snap
  still riding at 250–290: a turn that scrubs speed instead of a spin-out.

## Comparison with the 2026-09-15 code (M3)

Branch `compare-2026-09-15` (ae6e7dc02 + the replayer carried over), same trace:

| t | today's build: v / nose / slip | 09-15 build: v / nose / slip |
|---|---|---|
| 5.4 | 758 / 107° / −14° | 500 / 86° / −31° |
| 5.8 | 436 / 63° / −39° | **209 / 55° / −73°** |
| 6.0 | 200 / 39° / −55° | 70 / 33° / −162° |
| mean speed, ride | 443 | 392 (phone 495) |

The old code loses grip 0.2 s sooner, reaches 73° of slip, holds more slip through the earlier
turns (−37…−39° at t=2.5–3.0 vs −6…−9°), and rides 50 cm/s slower overall. The spin-out is old.

## What "lossless" would look like — the air (M5)

g = 1000 cm/s² here (`DefaultGravityZ`; the budget's gravity line is mass × 1000). The data's
face tops out at `slopeSin` 0.5–0.65 (30–40°) near the crest; there is no vertical lip in the
export. Lossless along the face: `h = (v·sinθ)² / 2g`, airborne `≈ 2·v·sinθ / g`.

| face angle at the crest | at 750 cm/s: height over the crest / airborne | after the 90 cm climb to the lip |
|---|---|---|
| 20° | 33 cm / 0.5 s | ~25 cm |
| 30° | 70 cm / 0.75 s | ~50 cm |
| 40° (data's steepest) | 116 cm / 0.96 s | ~80 cm |
| 45° (a real lip) | 140 cm / 1.06 s | — |

Speed enters squared: 500 cm/s gives a quarter of this, 1000 cm/s roughly double. So a realistic
redirect at riding speed gives a modest hop — half a metre to a metre, under a second, landing
5–7 m down the line. That is the target behaviour, not something to guard against.

## M6 — the yaw cap, tick-exact at t=5.0 (2026-09-18)

Cap flipped on the tick before the snap, `YawRateCapMax` 2.5, knee swept. Window t=5–8; slip counted
only while riding (v > 100). Control: mean 247, max slip 175°, v@7 65.

| knee | cap at 750 cm/s | mean v | max slip | v@7.0 |
|---|---|---|---|---|
| 400 | 76°/s | 264 | 134° | 13 — still a spin-out |
| 300 | 57°/s | 357 | 58° | 250 |
| 250 | 69°/s | 366 | 45° | 253 |
| **200** | **38°/s** | **410 / 399** (two runs) | **39 / 41°** | 370 / 307 |
| 150 | 29°/s | 393 | 43° | 240 |

- The fix is the *ceiling*, not the coefficient: knee 400 still spins out, and the M4 ÷2 row is beaten
  by knee 200 (mean 410 vs 371, slip 39 vs 43).
- The cap's sign is verified on the log (`surf.debug.flags yawcap`): during the snap Wz is negative and
  the cap sits on −Z; `resolvedWaveBackDirection` is (0.89, 0.45) on this map, not +X.
- `Max × knee` is a lateral-acceleration bound (v·ω ≤ 500 cm/s² = 0.5 g). Below the knee the flat
  2.5 rad/s is never reached by a slow board.

Where the speed goes with the skid gone: with the grip fade at 0 too (both fades 0, knee 400) the
velocity follows the nose (slip ≤ 25°) and speed still drops 764 → 384 in 0.6 s while z climbs only 50 cm.
Flipping `waveNormalDampingRate` 0 on the same tick keeps 640 at the crest — and the board goes over the
back and keeps going (distCrest +950 by t=8). So the sink is the **wave-normal damping**, doing its
penetration-block job; the data's lip has no up-and-over flow to launch off (water velocity at the crest
reads ~−100 cm/s, the velocity grid sits shoreward of the height grid). Left alone.

## M7 — why the cap is one-sided and why the fade stays

`phone-2026-09-17-10-48-07` (hybrid), whole ride, overrides from handoff:

| variant | ride mean (control 490) |
|---|---|
| symmetric cap 2.5/200 | 322 — stalls from t=6 |
| `CarveGripCrestFade` 0 (planing fade kept) | 215 |
| `CarveGripCrestFade` 300 | 288 |
| both fades 0 | 215 |
| both fades 0 + symmetric cap | 259 |
| **one-sided cap 2.5/200** | **531** |
| **one-sided cap + `CarveGripTurnUnfadeRate` 1.0** | **588** |

- t=3–4.5 the hybrid does a **cutback**: nose 135° → −176°, ~180°/s at 750 cm/s — and rides out clean
  (slip ≤ 25°, exits at 512). The old down-the-line velocity is damped off sideways and the retained
  cross-shore component moves *with* the water. The snap is the mirror case: the retained component
  points *into* the flow and the wave-normal damping takes it too. A speed-only cap stops the cutback at
  146°, the next input reverses it, the board drives over the back. Hence: cap yaw **toward** the wave
  only (sign of `nose × resolvedWaveBackDirection`).
- t=1.5–2.3 the hybrid rides *along* the crest (distCrest 0) with the nose 5–10° over the back. With the
  grip un-faded at the lip the velocity follows that nose over the back (distCrest → +950) and the ride
  stalls; with the fade the velocity keeps a shoreward drift and the board stays on the face. That is
  the punch-through the fade was set for, seen on a real ride. So the fade cannot simply be narrowed.

## M8 — the harness: the headless takeoff is physics, the device's is not

`Rails: suppressed (test/replay run)` — in replays the 3.3 s takeoff before the handoff is driven by the
state-trigger autopilot on live physics, so a **baked** tunable acts during the takeoff and changes the
handoff state (cap baked: handoff at 633 cm/s, 50 cm nearer the crest; un-fade baked: 318 cm/s and
*behind* the crest — ride mean 62). On the device the takeoff is kinematic and the handoff is stamped.
The device-like measurement is therefore: **defaults off in `Saved/TuningOverrides.json`, flipped on at
`-ReplayOverridesAt=0.0`** (trace t=0 = the handoff). Two pure controls are bit-identical (247/453
twice), so this form is not noise. Every number in M7, M9 and the AC section is in this form; the
"baked, whole ride" numbers (255 / 62) are the headless takeoff artefact, not the ride.

Also fixed on the way: `-ReplayOverrides=a=1,b=2` only ever applied `a` — `FParse::Value` stops at the
first comma by default. Multi-override A/Bs before 2026-09-18 applied the first entry only.

## M9 — lifting the fade while yawing toward the wave

`gripCrestGate += (1 − gripCrestGate) × min(1, yawTowardWave / CarveGripTurnUnfadeRate)`. A loaded rail
in a turn grips; a board riding steady along the lip (yaw ≈ 0) keeps today's fade. Device-like harness,
cap 2.5/200 on in every row:

| trace | control | cap only | cap + un-fade 1.0 | cap + un-fade 0.5 |
|---|---|---|---|---|
| spin-out trace, t=5–8 mean / max slip / v@7 | 247 / 175° / 65 | 254 / 85° / 80 | **656 / 18° / 536** | 638 / 12° / 539 |
| spin-out trace, ride mean | 453 | 434 | **607** | 606 |
| hybrid, ride mean | 490 | 531 | **588** | 267 (t=0 run, takeoff artefact — not re-run) |
| 4-snap shortboard, ride mean | 247 | 248 (cap never engaged) | **305** | — |
| `hard_turn_towards_the_wave`, mean / maxUW | 626 / 0.76 | 628 / 0.76 | 630 / 0.76 | — |

- The cap alone only halves the skid (slip 175 → 85°); it is the un-fade that makes the momentum follow
  the nose. Neither alone works: un-fade alone (no cap) = 299 / 23° / 86.
- On the spin-out trace the rides diverge at t=2.55 (the milder t=1.5–2.0 slowdown no longer happens),
  so by t=5 the board points further down the line and the same hard-left becomes a 30°/s carve holding
  530–800 cm/s on the face (distCrest −250…−100). Faster ride, same inputs — but a *different* ride;
  the aggregate is the claim, not the event (`ab-video-invalid-when-trajectory-diverges`).
- The funboard (`phone-2026-09-15-20-51-12`, 41 s) diverges at t=6.55 and is faster or equal in every
  2 s window to t=18 (601/616/498/270/182/127 vs 595/690/460/231/114/72), both rides stall 18–32 s, and
  the control's re-catch at 32–42 s (450–580) is an input recorded for a wave phase the candidate is no
  longer in (263 → 53). Aggregate 278 vs 328 — a divergence artefact, not a like-for-like slowdown.

## M10 — the air, as far as this trace shows it

The snap happens on a slopeSin 0.26–0.33 (15–19°) section, not a ≥ 35° one, so AC5 is not testable here.
Across all variants the board leaves the water for 0.15–0.3 s at the crest with vz +90…+150 (5–11 cm
over the launch point — ballistically consistent with the 400–540 cm/s it has there), pitches +13…+19°
nose-up through the hop, roll returns to ~0, re-enters at −100…−220 cm/s and rides on; no ragdoll seen.
Nothing reads as "treacle" at this scale. What a 1 m air does on landing is untested.

## M11 — the steep-section snap, synthesised (T3)

`Tests/InputTraces/synth-steep-snap-from-10-15-54.csv`: the recorded ride with its own t=5.05–6.30
snap weights copied onto t=0.45, where the board is at 809 cm/s on a slopeSin 0.57 (35°) section 50 cm
from the crest — the AC5 scenario. Device-like harness, un-fade 1.0 in every row but the control.

| knee | what happens | speed at the crest | climb | dry | vz max |
|---|---|---|---|---|---|
| — (today) | spin-out: nose 147° → 28°, slip 123°, stopped by t=2 | — | 87 | 0.1 s | +177 |
| 200 (shipped) | a 38°/s carve *along* the face, 550–690 cm/s, no climb to speak of | 600 | 76 | 0.2 s | +175 |
| 300 | as 200, slip to 37° | 590 | 70 | 0.1 s | +179 |
| **400** | **the snap**: nose 147° → 65°, climbs 103 cm to the crest | 554 → 346 → 249 | 103 | 0.15 s | +214 |
| 600 | as 400, slip 46°, no hop | — | 99 | none | +180 |

- The shipped knee turns a steep-section snap into a carve along the face; knee 400 lets the nose
  go up the face — and the ride then dies behind the crest (ride mean 202) because the recorded input
  is neutral after the snap. What the *player* would do there is the device question (T5).
- Even with the snap delivered, the air is ~20 cm (vz +214 → 23 cm ballistic). The board loses
  554 → 346 in the 0.25 s before the lip while slip is only 26–37°: the **wave-normal damping**
  toward the water's velocity (the penetration block, `waveNormalDampingRate` 0.06) — the same sink
  as M6. The crest water here reads −318…−422 cm/s shoreward with no upward component; the board
  heading into it closes at 500+ cm/s relative and the damping removes that. So on this data a lip
  snap cannot become an air without one of:
  1. fading the wave-normal damping near the crest (the lip is thin; there is no water mass to block
     penetration) — which is the glide-through behind the crest that damping was built to stop
     (`wave-interaction-damping-and-redirect.md`; owner's [[ride-priority-order]] puts "slowing when
     leaving the wave" second, airs on the face first);
  2. an upward water velocity at the lip in the data (`wave-data-velocity-registration.md`).
  Neither is built here; it is the owner's call.
- **Owner's call (2026-09-18): nothing more for the air now.** Not deferred as a principle — the
  physics may leave the water whenever a situation makes it realistic — but not prioritised either:
  no work is justified to *make* airs happen. The PC rides after T1/T2 looked realistic without one.
  M11 is a record, not a task.
- **In the air** (front SC dry, 24 ticks across the runs): gravity 96.7 k; buoyancy 10 k (the tail is
  still wet); `waveSlopeGravity` 3.1 k, `dragRail` 2.3 k, `bottomHydrofoilYaw` 0.5 k, `bottomSlopeThrust`
  0.35 k — the `effectiveWaterHeight` baseline keeps them alive off the water — ≈ 6 % of gravity in
  total. Not treacle at this scale. The fork's linear damping and ceilings are not water-gated
  (`p.Chaos.Solver.AmountUnderWater` is fed but unused); the pitch-righting servo and the
  directional pitch extras are contact-gated; base `AngularDampingX/Y/Z` act in flight. In a 0.2 s hop
  none of it is visible: pitch +13…+19° nose-up through the hop, roll back to ~0.
- **Landing**: re-entry vz −209…−222 in every run, buoyancy peaks ≈ board weight, the ride continues.
  `UpdateFallDetection` ends a ride only on |roll| > 90° or `AmountPlaning` < 0.05 (velocity-only,
  so a landing at speed never trips it); there is no vertical-speed trigger, and the lip impact
  (`lip-impact.md`, F = jet² at the nose) did not fire in any of these snaps. A 1 m air lands at
  ~−450 cm/s; whether that should cost anything is undecided, and there is nothing to decide it on
  until an air exists.

## Tasks

### T1 — Cap the commanded yaw rate at speed
**Goal:** a full lean at 750 cm/s asks for a yaw rate the grip can follow; slow turns unchanged.
- Where: the lateral turn torque in `FluidDynamics.cpp` (`lateralTurn*` categories in the
  budget; `lateralTurnCoefficient`, `lateralTurnHardCarveBoost`, per board in `Content/Boards/*.json`).
  Read `specs/hard-carve-progressive-boost.md` and memory `small-weight-shift-sharp-turn` first —
  the additive commanded-lean carve is deliberate and player-validated; do not remove it.
- Shape: not a lower coefficient (M4's ÷2 also softens slow turns). Prefer a **yaw-rate ceiling
  that falls with speed** — e.g. a max commanded yaw rate `ω_max(v)` such that at 750 cm/s the nose
  cannot outrun what `CarveGripRate` (4 rad/s × gate) can bend — or attenuate the turn torque above
  a speed the way `yawThrustAtten` does for the drive. Tunable, disable value = current behaviour.
- Measure with the harness on `phone-2026-09-18-10-15-54` at t=5.0 first (should beat the ÷2 row:
  mean t=5–8 ≥ 371, slip ≤ 40°), then whole-ride on `phone-2026-09-18-09-51-17` (the other
  shortboard ride, four snaps) and `phone-2026-09-17-10-48-07` (hybrid turns — must stay identical
  to the cm on the face).

### T2 — Narrow the carve-grip crest fade
**Goal:** the redirect works at the lip so a snap carries momentum up the face instead of skidding.
- `CarveGripCrestFade` 600 → measure 300 / 150 / 0 with the harness (M4 has 0: skid gone, speed
  still lost because T1 is not in — do T1 first, then this).
- The fade was set to stop the **bottom-turn punch-through** (memory
  `bottom-turn-crest-punchthrough`: crest fades 600/600 player-validated; the righting servo was a
  negative result). Re-run that case: `hard_turn_towards_the_wave` / `top-turn` autopilots and the
  glide-through metrics in `specs/wave-interaction-damping-and-redirect.md` AC4 (time-behind-crest,
  excursion duration). `PlaningRedirectCrestFade` is the sibling knob; leave it unless the data
  says otherwise.
- Expected new behaviour with T1+T2: a snap at the lip at speed goes *up* — and at the steepest
  sections, over. That is correct (M5). Which leads to:

### T3 — Make the air plausible
**Goal:** when the board leaves the water it behaves like a board in the air and lands like one.
- What happens today when `amountUnderWater` → 0 for all actors: which forces still apply
  (the `SurfRails`/`AreForcesSuppressed` machinery is for the intro, not this), what the fork's
  damping does with no water under it (the per-axis local damping and velocity ceilings still
  act — measure whether that reads as "air" or as "treacle"), and whether the pitch righting /
  angular damping hold an attitude in flight.
- Landing: `lip-impact.md` and the buoyancy on re-entry; a 1 m drop at 750 cm/s should not
  ragdoll every time, nor should it be free. Measure the vertical speed at re-entry on the T2
  replays and decide.
- No scoring. Airtime is deferred *for scoring* (trick-scoring D2); do not wire it into
  `TrickScoring`.

### T4 — Regression
- Both shortboard traces above; the hybrid turn trace; `phone-2026-09-15-20-51-12` (funboard, the
  upstream-push case — `vx` peak at t=18 must stay < 50); the bottom-turn punch-through set.
- Whole-ride aggregates, not single events (`ab-video-invalid-when-trajectory-diverges`): mean and
  median speed, time in pocket, count of no-input ≥300 cm/s gains, the turn-surge sawtooth metrics
  (`shortboard-turn-surge-sawtooth`).

### T5 — Device
The owner tests turns into the wave on the shortboard and hybrid; the spin-out should become a
scrubbed turn, and a hard snap at a steep section should get air.

## Acceptance criteria
- **AC1** ✅ — `phone-2026-09-18-10-15-54`, t=5–8: slip ≤ 40° throughout, mean speed ≥ 371 (the ÷2
  row) and the board still riding (> 200 cm/s) at t=7. *Measured 18° / 656 / 536 (M9), device-like
  harness; the ride diverges at t=2.55 so the event is a carve, not the recorded snap.*
- **AC2** ✅ (relaxed by the owner to "no slower") — `phone-2026-09-17-10-48-07` (hybrid): 588 vs 490,
  the cutback rides out as in the control (M7).
- **AC3** ✅ with a caveat — `phone-2026-09-15-20-51-12`: identical to t=6.55 (not 12 — the un-fade
  engages on a toward-wave yaw there); `vx` peak at t=17.8–18.4 = +10 (< 50); faster in every
  comparable window; the 32–42 s re-catch is a divergence artefact (M9).
- **AC4** ✅ — `hard_turn_towards_the_wave` 630 / maxUW 0.76 / climb 86 vs 626 / 0.76 / 88. The `bottom-turn`
  and `top-turn` autopilots no longer exist in the saved map.
- **AC5** — withdrawn by the owner (2026-09-18, M11): on this data a lip snap gives a ~20 cm hop and
  nothing more is justified to make it an air. The synthetic trace stays as the test if that changes.
- **AC6** ✅ on PC (2026-09-18): snap into the wave and cutbacks both validated by the owner. Android
  not yet.

## Harness

```
# control, with everything logged
$env:TEST_MAP="Surfing_infinite_wave"
$env:EXTRA_ARGS="-ReplayTrace=phone-2026-09-18-10-15-54.csv -ReplayUseWeights -usefixedtimestep -fps=60 -BoardInTests -Board=shortboard"
RunGameAndCollectLogs.ps1 -Argument "torque,crossing,foam,thrust:SharedCalculations,bottom_left_middle-back" -TimeoutSeconds 300

# one value flipped on the tick before the snap
$env:EXTRA_ARGS="... -ReplayOverridesAt=5.0 -ReplayOverrides=CarveGripCrestFade=300"

# DEVICE-LIKE whole ride (M8): the tunables OFF for the headless physics takeoff, ON from the handoff.
#   Saved/TuningOverrides.json: {"AssistDisable":1,"YawRateCapMax":0,"CarveGripTurnUnfadeRate":0}
$env:EXTRA_ARGS="... -ReplayOverridesAt=0.0 -ReplayOverrides=YawRateCapMax=2.5,CarveGripTurnUnfadeRate=1.0"

# the cap's sign, value and the lifted grip gate, per tick
RunGameAndCollectLogs.ps1 -Argument "crossing,yawcap:SharedCalculations,SurfboardUtils"
```

`Saved/TuningOverrides.json` needs `"AssistDisable":1` for PC (mouse) traces. Trace t=0 is the
`Handoff observed` log line. Slip = angle between `fwdH` and `bVel` in the `CROSSING` line.
To replay against old code: branch at the commit, carry the replayer over
(`git checkout 070e12681 -- Source/GoneSurfing/InputReplayAutoPilot.* Source/GoneSurfing/SurfTuningSubsystem.*`),
commit on the throwaway branch, run, come back — `compare-2026-09-15` is one such branch.

## Files
- Fork `Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp` —
  `MaxAngularVelocityZPos/Neg` CVars and the per-sign clamp after the angular damping (T1).
- `Source/GoneSurfing/SurfboardUtils.cpp` — the fork feed: toward-wave sign, `gripCrestGate` + the
  turn un-fade (T2), the yaw-cap feed (T1), `yawcap` debug line; `PlaningRedirect*`, damping, ceilings.
- `Source/GoneSurfing/SurfTuningSubsystem.h` — `YawRateCapMax`, `YawRateCapSpeedKnee`,
  `CarveGripTurnUnfadeRate` (new); `CarveGripCrestFade`, `CarveGripRate`, `lateralTurnCoefficient`,
  `lateralTurnHardCarveBoost`, `PlaningRedirectCrestFade` (unchanged).
- `Source/GoneSurfing/InputReplayAutoPilot.cpp` — multi-entry `-ReplayOverrides` parse fix.
- `Source/GoneSurfing/FluidDynamics.cpp` — lateral turn, `=== YAW HYDROFOIL ===`, slope thrust (untouched).
- `Content/Boards/*.json` — per-board `lateralTurnCoefficient` / `lateralTurnHardCarveBoost`.
- Traces: `Tests/InputTraces/phone-2026-09-18-10-15-54.csv` (the spin-out),
  `synth-steep-snap-from-10-15-54.csv` (its snap moved onto the 35° section at t=0.45 — the AC5 case),
  `phone-2026-09-18-09-51-17.csv` (four snaps), `phone-2026-09-17-10-48-07.csv` (hybrid turns),
  `phone-2026-09-15-20-51-12.csv` (upstream push).
- Related specs: `carve-grip-via-redirect.md`, `planing-redirect.md`,
  `pitch-righting-and-redirect-escape.md`, `hard-carve-progressive-boost.md`,
  `wave-interaction-damping-and-redirect.md`, `yaw-hydrofoil-flow-direction.md`,
  `replay-scheduled-overrides.md`, `trick-scoring.md` (D2).
