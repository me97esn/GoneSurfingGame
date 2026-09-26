# Spec: The yaw-hydrofoil forward drive ignores where the water comes from

Split out of [broken-wave-no-consequences.md](broken-wave-no-consequences.md) on 2026-09-16: that
spec's investigation found two distinct problems and this is the smaller, mechanical one. The other
— the physics has no broken-wave state, so whitewater neither shoves nor drags — stays there. Fix
them separately; the acceptance tests below are written so this one can be judged without the
other.

## Status
- [x] Cause isolated with tick-exact replay A/B (M8 in the parent spec, 2026-09-16): zeroing
      `yawHydrofoilCoefficient` on the tick before the event removes the wrong-way push completely;
      zeroing every other candidate does not.
- [x] FR3 measured 2026-09-16: whitewater has a 0.23–0.34 face in the data; slope gating cannot
      work; the whitewater-broadside case is the parent spec's (FR-A A2).
- [x] **FR1 implemented 2026-09-16, re-based 2026-09-17** — `calcWithWaveGate()` in
      `FluidDynamics.cpp`, `yawFwdWithWaveGateOff/On = −0.85/−0.55` in `SurfTuningSubsystem.h`;
      disable with On ≤ Off. **Gate on the board's velocity direction, not its nose** (see "The
      first version braked the turn").
- [x] **FR2 withdrawn 2026-09-17** — gating the slope thrust's fin-redirect was the braking the
      player reported; it is the drive through a turn. Ungated again.
- [x] **AC1 met** (20-51-12: ride identical, crest crossing identical, upstream push +207 → +42
      peak, board never leaves the face). **AC2 met** (player's turn-into-the-wave trace identical
      to gate-off through the turn). AC3 partial: rides identical on three traces; full aggregate
      sweep still to run.
- [x] **Player-validated on device 2026-09-17** (v2). v1 had failed: braked on turns.
- [x] **v3 2026-09-18** — a PC shortboard cutback ran the *velocity* up the face (d −0.63 → −0.96
      at 300 → 200 cm/s) and v2 stalled it 378 → 143. Knee moved to −1.0/−0.90 and the gate now
      fades out above `yawFwdWithWaveGateSpeedMax` 150 cm/s (over 100): it acts only on a slow board
      being driven into a crest, which is the case it exists for. Replays: 10-48-07 identical to
      gate-off on every tick; 20-51-12 push still +207 → +4. Owner's priority order recorded:
      surfing/tricks/speed first, slowing when leaving the wave second, no whitewater acceleration
      last.

## The defect

`FluidDynamics.cpp`, `=== YAW HYDROFOIL ===`:

```
yawThrustMag = coef · v²InBottomPlane · |sinSlip| · wettedForce · effectiveWaterHeight
yawFwdMag    = yawThrustMag · |sinSlip| · yawFwdSpeedAtten · yawFwdGate     // along forwardsUnit
```

`v² · sin²(slip)` along the nose, where `slip` is the angle between the board-wide relative water
velocity and `board.forwards`. It never asks whether the water arrives from ahead, beside or behind:

- **Sign-blind.** Flow from any direction but dead-ahead produces thrust along the nose, the way the
  nose points — including up the face and through the crest against the wave's travel.
- **Maximal broadside.** A board lying across the flow gets the most forward drive.
- **Self-amplifying upstream.** `v²` is the *relative* speed. A board that starts moving into the
  flow raises its own relative speed, which raises the drive. Measured (M8): relative cross-shore
  speed 89 → 321 cm/s and the term 56 k → 180 k in 0.2 s, on water that was only moving 40–120 cm/s.
  The "stopped board in fast water" picture was wrong; the board makes its own v².
- **The gates cannot help on the face.** `yawFwdGate = floor + (1−floor)·slopeGate·frontGate`
  answers "is there a rideable face here", which is true on the face whichever way the board points.
  Measured: setting the funboard's `yawFwdOffFaceFloor` 0.25 → 0 on the event tick changed nothing
  (M8 run H). Off the face the floor is the problem instead (FR3).

The anti-slip *side* component (`antiSlipDir · cosSlip · antiSlipForceScale · yawThrustMag`) is a
separate output of the same term and is not in question here; `antiSlipForceScale` is 0 by default
anyway and the carve-grip redirect carries that role.

## Evidence

Trace `phone-2026-09-15-20-51-12` (PC, funboard), event at t = 17.7–18.2 s: a board resting in
front of an arriving crest, nose ≈ 30° off +X, is driven +X *into and over* the crest at 207 cm/s.
Force budget (`Fin` = into the wave) on the control replay:

```
t=17.7  bottomHydrofoilYaw  +56k    bottomSlopeThrust +20k
t=17.8  bottomHydrofoilYaw +139k    forwardDrag −15k
t=17.9  bottomHydrofoilYaw +180k    forwardDrag −26k
t=18.0  bottomHydrofoilYaw +115k    (crest reached)
```

Tick-exact A/B, one value flipped at t=17.2 ([replay-scheduled-overrides.md](replay-scheduled-overrides.md)),
`vx` (+ = into the wave):

| t | control | `slopeThrustFinRedirect`=0 | **`yawHydrofoilCoefficient`=0** | both | `waveNormalDampingRate`×10 | `yawFwdOffFaceFloor`=0 |
|---|---|---|---|---|---|---|
| 18.0 | +172 | +93 | **−3** | +4 | −44 | +169 |
| 18.2 | +207 | +182 | **−8** | −1 | −53 | +184 |
| 18.4 | +156 | +148 | −10 | −5 | +85 | +136 |
| x at 22 s | 4111 | 4055 | **3832** | 3831 | 3792 | 4048 |

The passive slope thrust's fin-redirect term (`slopeThrustFinRedirect · |slopeAlongLeft|`, also
sign-blind) is a minor initiator — it delays the push 0.2 s when removed and adds nothing once the
hydrofoil is off. Ten-times damping holds the board on the face and then loses at the crest, where
its slope gate closes while the drive is still at full strength.

Also: the pre-damping force set (`waveMassThrust`/`waveMassFlowDrag`/`wavePenetration` restored)
pushes the same board *through* the crest five times harder in the like-for-like window at
t=10–12 (vx +264 vs +47). It is not a fix and not a control.

## Requirements

### FR1 — forward drive only while the board moves with the wave

**Not** a gate on the relative flow. Measured on the control replay (`crossing` log, t=0–9 healthy
ride vs t=17.6–18.1 push): the relative flow is from ahead in *both* — `−dot(flowDir, fwd)` 0.87–1.00
on the ride and 0.88–0.97 at the push, because a board moving upstream makes its own head-on flow.
An ahead-gate would leave the push intact. The sampled water-velocity vector is no reference either:
it points against the nose on 60 % of ride ticks (median dot −0.29; ~110 cm/s, off-registered grid).

**Not** a gate on the nose either — see "The first version braked the turn" below. A top turn and
a punch-through share a nose orientation; they differ in where the board is *going*. So the gate
reads the **board's velocity against the wave's travel axis**:

```
shoreward = −resolvedWaveBackDirection (tile geometry — (0.891, 0.454) on this level, NOT −X)
dVel  = dot(velocityHoriz / |v|, shoreward)      dNose = dot(fwdHoriz, shoreward)
d     = lerp(dNose, dVel, clamp((|v| − 50) / 100))   // nose stands in below ~150 cm/s
withWaveGate = SmoothStep(yawFwdWithWaveGateOff −0.85, yawFwdWithWaveGateOn −0.55, d)
```

Measured `d` (velocity, nose-blended) over three traces, two boards:

| window | d p5 / p50 / p95 |
|---|---|
| 20-51-12 healthy ride t=0–9, speed > 400 | −0.30 / +0.01 / +0.22 |
| 20-57-25 riding the face t=0–1.2 | −0.01 / +0.08 / +0.49 |
| **10-48-07 turning into the wave t=1.5–2.6** | **−0.40 / −0.25 / −0.18** |
| 20-51-12 turning up over the back t=10–11 | −0.51 / −0.37 / −0.11 |
| 20-57-25 whitewater bore push t=5.6–6.1 | −0.65 / −0.60 / −0.58 |
| 20-51-12 the upstream push t=17.6–18.1 | −1.00 / −0.99 / −0.97 |

Every ride and turn tick (285, min −0.55) reads 1.0; the upstream push reads 0; the bore push
~0.9 (the parent spec's `broken` signal owns that one). The low-speed nose blend is what catches a
board at rest in front of an arriving crest — its velocity is noise, its nose points upstream.
Multiplied into `yawFwdGate` (floor included — the off-face floor is a beginner assist for leaving
the pocket, not for driving up through the crest); the anti-slip side component is untouched.
Disable value for A/B: On ≤ Off → gate ≡ 1.

Physically: the hydrofoil converts the wave's flow past the hull into drive; a board moving
against the wave's travel is not being carried by it and should get nothing from this term.

The takeoff is unaffected by construction: the intro is kinematic (rails, forces suppressed until
handoff — CLAUDE.md "Autopilots"), so a board waiting for a wave never runs this term.

#### The first version braked the turn (2026-09-17)

v1 (42b9b6d0c) gated on the **nose** (`dNose`, knee −0.70/−0.45) and also gated the slope
thrust's fin-redirect (FR2). Player-tested on the phone: the board braked hard every time it turned
into the wave (`phone-2026-09-17-10-48-07`, hybrid: 782 → 265 cm/s on a normal top turn, three
times in 11 s). Replayed with the force budget, gate on vs off, first turn t=1.4–2.7 (F along the
nose):

| | gate on (v1) | gate off |
|---|---|---|
| `bottomSlopeThrust` (fin-redirect) | 2.5–5.5 k | **50–115 k** |
| `bottomHydrofoilYaw` | 12–33 k | 36–115 k |
| speed at t=2.0 / 2.4 / 2.6 | 445 / 231 / 168 | 673 / 616 / 535 |

The fin-redirect is the drive *through a turn* — its own comment says it exists to "remove the
stall when the nose points up-face" — and v1 gated exactly that. And a turn's nose (−0.57…−0.73)
is indistinguishable from the punch-through's (−0.55…−0.72). v2 gates the velocity instead and
leaves the fin-redirect alone: the player's turn replays identical to gate-off on every tick
(673 / 616 / 535), while the upstream push still drops from +207 to +42 peak.

### FR2 — ~~the fin-redirect gets the same gate~~ withdrawn
`redirectDrive = slopeThrustFinRedirect · |slopeAlongLeft|` is sign-blind in the same way, but it
is the drive through a turn (50–115 kN on the player's turn) and gating it was the braking the
player reported. Ungated. It is ~10 % of the upstream push, which FR1 handles.

### FR3 — (measured, moved) no forward drive in whitewater
Player-observed and recorded (`phone-2026-09-16-20-57-25`, hybrid, tilt): broadside in the
whitewater ~2000 cm behind the peel, the board goes 198 → 458 cm/s along its nose with no input.
Replayed with `-ReplayUseWeights` (tracks the phone to t≈5, reproduces the surge at 5.7–6.0):

```
  t    distCrest  slopeSin  nose·(−X)   bottomHydrofoilYaw Fin   |v|
 5.3     −250      0.017     +0.22            347               220
 5.5     −250      0.081     −0.07          2,380               198
 5.7     −150      0.175     −0.39         63,126               215
 5.9      −50      0.323     −0.56        161,078               409
 6.0      −50      0.337     −0.58        108,579               458
```

**The whitewater is not flat in the data.** A face of `slopeSin` 0.23–0.34 arrives — as steep as the
face ridden at t=0–1 (0.25–0.38). A bore front is steep; the height loop re-forms it at every Y
(parent spec M3) and nothing tells the physics it is a bore. So the slope gate is wide open, a
0.15 threshold would still be open, and the per-board `yawFwdOffFaceFloor` is irrelevant here.
**Slope gating cannot separate whitewater from a face. This case is the parent spec's, FR-A A2
(face-keyed propulsion × (1 − broken)).** It is not solved by this spec and is not FR1's job:
on the phone the nose was at 97–112° (nose·(−X) = +0.12…+0.37), inside FR1's keep zone by design
— nose-along-the-line broadside to the flow is what surfing down the line looks like. (In the
replay the nose had drifted seaward, −0.4…−0.6, so FR1 *would* have cut the replayed surge; that
is a divergence artifact, not evidence.)

What this spec still owns for whitewater: nothing. The term that delivers the push is the same
(`bottomHydrofoilYaw` 2 k → 161 k in 0.4 s, slope thrust 11–42 k behind it), so the parent spec's
`broken` multiplier lands on exactly the two terms FR1/FR2 gate.

## AC1 result (v2, 2026-09-17)

Replay of `phone-2026-09-15-20-51-12` with FR1 v2 on from the start, against the control and the
`yawHydrofoilCoefficient=0` reference (M8 run E):

| t | control vx | **FR1 v2 vx** | yaw-off vx |
|---|---|---|---|
| 17.8 | +66 | +42 | +14 |
| 18.0 | +172 | +34 | −3 |
| 18.2 | +207 | +6 | −8 |
| 18.4 | +156 | −39 | −10 |
| x at 18.4 | 4017 | **3806** | 3905 |

- t=0–9.5 identical to the control to the cm (mean 625 vs 625); the t=10–12 crest crossing is
  also identical to the control (v1 had removed it; v2 does not — the board's velocity there is
  along the line).
- The push is 80 % removed rather than 100 %: the nose blend fades in over 50–150 cm/s, so the
  drifting board keeps a little drive until it is nearly stopped. It never leaves the face
  (x 3806 vs 4017).

AC2 (`phone-2026-09-17-10-48-07`, hybrid, `-ReplayUseWeights`): the turn into the wave at
t=1.4–3.0 is identical to gate-off on every tick. The replay diverges from the phone after ~3 s
(turn direction), so the later two turns are not comparable — device validation covers them.

## Acceptance criteria

- **AC1** — replay `phone-2026-09-15-20-51-12`, FR1 on from the start: t=0–12 identical to the
  control; at t=17.8–18.4 peak vx < 50 and x < 3950 (control +207 / 4017). **Met (v2).**
- **AC2** — replay `phone-2026-09-17-10-48-07` (hybrid, `-ReplayUseWeights`): the turn into
  the wave at t=1.4–3.0 within replay noise of gate-off. **Met (v2).** Plus `phone-2026-09-16-20-57-25`
  t=0–5.5 unchanged (met in v1, unchanged in v2).
- **AC3** — whole-ride aggregates on both boards (mean/median down-the-line speed, time in pocket,
  count of no-input ≥300 cm/s gains in <0.5 s, turn-surge metrics from
  `shortboard-turn-surge-sawtooth`): the gain count goes to zero behind the peel and is unchanged on
  the face; mean speed within replay noise. Never a single-event maximum.
- **AC4** — player-validated on device, funboard and shortboard.

## Files

- `Source/GoneSurfing/FluidDynamics.cpp` — `=== YAW HYDROFOIL ===`; `=== PASSIVE SLOPE THRUST ===`
  (`redirectDrive`).
- `Source/GoneSurfing/SurfTuningSubsystem.h` — `yawHydrofoilCoefficient`, `yawFwdSlopeGateMin/Width`,
  `yawFwdOffFaceFloor`, `slopeThrustFinRedirect`; **`yawFwdWithWaveGateOff/On`** (the gate).
- `Source/GoneSurfing/FluidDynamics.cpp` — `calcWithWaveGate()`; applied in `=== YAW HYDROFOIL ===`
  only (`yawFwdGate *=`); `thrust` log prints `withWave:`.
- `Tests/InputTraces/phone-2026-09-17-10-48-07.csv` — the braking trace (AC2).
- `Content/Boards/*.json` — per-board `yawFwdOffFaceFloor`.
- Related: [bottom-yaw-hydrofoil.md](bottom-yaw-hydrofoil.md),
  [flat-water-propulsion-audit.md](flat-water-propulsion-audit.md),
  [yaw-thrust-speed-attenuation.md](yaw-thrust-speed-attenuation.md),
  [passive-slope-thrust.md](passive-slope-thrust.md),
  [replay-scheduled-overrides.md](replay-scheduled-overrides.md).
