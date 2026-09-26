# Spec: A board in the air is not in the water — gating the force pipeline on contact

## Status
- [x] Spec drafted (2026-09-23), owner-agreed.
- [x] **BUILT** 2026-09-23 on branch `airborne-force-gating`. Gate signal and sites decided by
      measurement — see "Design (decided)". Two tunables, `airborneForceScale` (1 = today's
      behaviour bit for bit, 0 = the fix) and `airborneFadeDistance`, both live in the tuning HUD
      and via `surf-live`. **Default is `airborneForceScale = 0` (the fix) since 2026-09-23.**
- [x] AC1 measured and PASSING: over the window where every surface is clear of the water, the
      horizontal velocity went from +84.8% / +12.3 deg to **+0.7% / -1.0 deg**.
- [x] AC3, AC4 measured and passing. AC5 measured on the clean fixture: no green-face regression.
- [x] **Owner ride-tested on device and baked `airborneForceScale = 0` as the default**
      (2026-09-23). Verified *at 0*, not inferred from a pass on the inert build: the owner tuned
      the value to 0 live on the device and rode it. Every number below is still headless — the
      device pass is the evidence that it feels right, not a re-measurement of them.
- [ ] Three sites found ungated and deliberately NOT changed — see "Left ungated".

> **The root cause was not what this spec assumed.** The spec was written around
> `effectiveWaterHeight`'s baseline leaking into the 23 per-surface formulas. Gating that changed the
> airborne drive by ~4%: nearly every force family was *already* dead in the air. The dominant
> airborne force was **`lipImpact`**, at its cap. Read "What was actually driving it" before touching
> anything here.

## Problem

The board keeps being driven, turned and braked while it is clear of the water.

Measured on [replay-rails-until.md](replay-rails-until.md)'s fixture over
`phone-2026-09-22-20-26-02` (fish, `-RailsUntil=3.00`). From **t=3.99 to t=4.33** the board is
24–39 cm above the surface — `underW=0.00`, `submersion=-24..-39`, and both SC contact gates read
`0.000`. Gravity is vertical, so across that window the **horizontal** velocity vector cannot change
at all: not its magnitude, not its direction.

| build | horizontal speed | change | spurious turn |
|---|---|---:|---:|
| before the sideways-damping contact gate | 416 → 373 | −43 cm/s | **+34.8°** |
| after it (`SidewaysDampingAirScale=0`) | 525 → 969 | **+444 cm/s** | +12.2° |

Both rows should be zeroes. The damping gate ([carve-grip-via-redirect.md](carve-grip-via-redirect.md)
"Step 4 revisited") removed a brake that was partly *masking* a drive; with it gone the board
accelerates by nearly 4.5 m/s in 0.34 s of free flight.

## Where it comes from

`AFluidDynamics::setup()`:

```cpp
this->effectiveWaterHeight = FMath::Min(
    this->baseHeight + this->waterColumnAbove + this->slopeSin * this->slopeHeight,
    this->maxEffectiveWaterHeight);
```

`waterColumnAbove` goes to 0 when the actor leaves the water, but `baseHeight` and
`slopeSin * slopeHeight` do not — deliberately, so trough and non-submerged forces stay non-zero and
the board does not strand itself (CLAUDE.md, "Forces, surfaces, and what 'lift' means"). Every
per-surface formula multiplies by `effectiveWaterHeight`, so every one of them keeps producing force
in the air. And `slopeSin` is **0.55–0.59** at exactly the moment the board is launched off the lip,
which is the largest that term ever gets — the forces are at their strongest precisely where there
is no water.

`actorWetted` (per-actor, from `waterColumnAbove`) already exists and does go to ~0 in the air, but
it gates only some sites, not the `effectiveWaterHeight` product itself.

This is the same defect class as the sideways damping: a term whose gate answers "how much wave is
here" rather than "is the hull touching any of it".

## What was actually driving it

Measured with the `torque` force budget over the airborne window, which is what CLAUDE.md says to
read first and what this spec should have reached for before proposing three designs.

Already zero in the air, before any change here:

| family | what already gated it |
|---|---|
| bottom/rail/fin/tail hydrofoil up-thrust, forward thrust, yaw hydrofoil, slope thrust | `wettedForce = actorWetted x compensation`, and `actorWetted` reads 0.000 in the air |
| bottom Bernoulli lift | `actorWetted` |
| bottom forward drag | `forwardDragWettingGate` (the one term a previous pass fixed this way) |
| buoyancy | `calcAmountUnderWater` returns 0 once the component is above the surface |
| wave-normal damping, planing redirect, sideways damping | contact-gated in the fork feed; all read `contactGate = 0.000` in the window |
| wave-mass penetration | retired to 0 by default (d4dbb60c1) |
| ~~wave-mass thrust / flow drag~~ | ~~retired to 0~~ — **no longer true, see below** |

> **Re-opened 2026-09-23.** `waveMassThrustCoefficient` and `waveMassFlowDragCoefficient` are back
> at 0.0001 (owner, for the lip's punch — see
> [wave-interaction-damping-and-redirect.md](wave-interaction-damping-and-redirect.md) "FR5
> revisited"). They were listed here as already-zero, so they were never gated, and **they bypass the
> `waterContactGate` this spec added**: all three sites recompute `boardWideEffectiveH` locally from
> `baseHeight + boardWideWaterColumnAbove + boardWideSlopeSin * slopeHeight`
> ([FluidDynamics.cpp:264](../Source/GoneSurfing/FluidDynamics.cpp#L264),
> [:311](../Source/GoneSurfing/FluidDynamics.cpp#L311),
> [:494](../Source/GoneSurfing/FluidDynamics.cpp#L494)). Their slope gate peaks exactly where the lip
> throws the board clear — the same signature `lipImpact` had.
>
> **Gated 2026-09-23** at the owner's call ("better gate them as well, I suspect it might bite me
> further on otherwise"). All three sites now multiply by `waterContactGate`. Safe to do without
> re-validating the feel: `waterContactGate = Lerp(1, airborneForceScale, clearSmooth)`, so at the
> shipped `airborneForceScale = 1` it is **exactly** 1 and the multiply is bit-identical. Measured
> on the fixture, `airborneForceScale` 1 vs 0: min down-line 194 both, peak slip 54 deg both, mean
> speed 409 vs 408 — the lip punch survives the gate.

So the force budget over the window contained essentially **one** term:

```
TORQUE [SC_2] cat=lipImpact   n=1  F[fwd/left/up]=(-74107, 165892, 11951)   magF ~ 200 000
TORQUE [SC_1] cat=TOTAL            magF = 99 994 ... 100 000     <- pegged at lipImpactMaxForce
```

`lipImpact` ([FluidDynamics.cpp:637](../Source/GoneSurfing/FluidDynamics.cpp#L637)) synthesises the
falling lip slamming the nose. Its three gates are `crestGate` (distance to crest), `slopeGate`
(board-wide `slopeSin`) and `speedGate` (the probed jet speed), and its direction is the jet's own
velocity. At the instant the lip throws the board clear, **all three read full** — the board is ~0 cm
from the crest, on a 0.55 face, beside fast water — so the term pushes the airborne board along the
jet at its cap for the whole flight. Not one of its gates asks whether the hull is touching water.

That is the spec's own thesis, just one term further in than the spec looked: a gate that answers
"how much wave is here" instead of "is the hull touching any of it".

## Design (decided)

**A signed clearance per surface, gating the force.** `waterColumnAbove` clamps the above-water side
to 0, so nothing downstream could tell "just breaking the surface" from "a metre in the air". The new
`surfaceClearance = actorZ - waveZ` keeps that sign, and `waterContactGate` smoothsteps 1 -> 
`airborneForceScale` as the surface rises `airborneFadeDistance` (10 cm) clear.

The property that makes this safe is that **a submerged actor gates to exactly 1**. Option 1 in the
original draft was rejected for removing the trough baseline `baseHeight` exists to provide; keying
on the signed clearance instead of on `actorWetted` keeps that baseline untouched for anything in the
water, so AC2 holds by construction and `airborneForceScale = 1` reproduces today bit for bit. This
was confirmed, not assumed: control and fix runs are **bit-identical up to the last contact tick** and
diverge on the first airborne one.

Applied at five places, all in `AFluidDynamics`:

1. `effectiveWaterHeight` itself — one multiply covering all 23 sites (option 1, refined).
2. `finLiftForce` — commented "fins are fully submerged by construction", true of a board on the
   wave, false of one above it, and it carries a forward drive component.
3. `waveSlopeGravityForce` — the propulsion that drives planing. Gated only by `AmountPlaning`
   (pure board speed; read a flat 0.75 right through the window) and wave steepness.
4. `lateralTurnForce` — the carve. Its note rules out `amountUnderWater` to avoid distorting the
   force's distribution *along the board's length*, which is an argument about which submerged actor
   contributes how much, not about a board with no water under it.
5. `lipImpact` — the term that actually mattered.

### Left ungated (found, deliberately not changed)

- **`pitchAlignTorque`** ([SharedCalculations.cpp:1006](../Source/GoneSurfing/SharedCalculations.cpp#L1006))
  — gated on slope and `AreForcesSuppressed()`, not on contact, so it still rotates an airborne
  board. It is a torque, so it does not violate AC1; it changes the attitude the board lands in.
  `ASharedCalculations` has no per-actor clearance, so this needs its own board-wide signal.
- **`ASurfboardPawn::Turn`** ([SurfboardPawn.cpp:2561](../Source/GoneSurfing/SurfboardPawn.cpp#L2561))
  — a 10 000 N sideways force at a forward offset, raw `AddForceAtLocation` on the mesh with no
  contact gate, so it would steer an airborne board. It is guarded by `bPlayerControlsEnabled`, which
  covers the intro, so it is only live once the rider has control. Small and unmeasured here (a
  replayed phone trace supplies no turn input), and worth a gate only if `IA_Turn` is actually mapped
  in the shipping input config — the weight shift is the real steering channel.
- **`ASurfboardPawn::PaddleForward`** ([SurfboardPawn.cpp:2481](../Source/GoneSurfing/SurfboardPawn.cpp#L2481))
  is **not worth gating: the board never paddles.** Owner, 2026-09-23: paddling and the wave catch are
  kinematic and expected to stay that way for the foreseeable future ([[takeoff-is-kinematic]],
  [deterministic-ride-handoff.md](deterministic-ride-handoff.md)). The force is dead in practice.
  Do not treat it as an airborne leak, and do not add a paddle-driven AC to this spec.
- **`AmountPlaning` itself** is the reason so many gates failed at once: it is pure board speed
  ([[planing-is-speed-based]]) and read 0.75 throughout a 40 cm-high flight. Gating it would reach
  much further than this spec (it also feeds the fork's planing redirect) and is not attempted.

## Acceptance criteria

- **AC1 — Free flight is free.** Over any window where every contact gate reads 0, the horizontal
  velocity vector changes by < 2% in magnitude and < 2° in direction. This is a physical invariant,
  so one run proves or disproves it.
- **AC2 — The trough still works.** `surf-straight` and a flat-water snapshot show no new stranding:
  the board does not lose its ride where `waterColumnAbove` is small but the hull is still wet.
- **AC3 — The launch is unchanged.** The board leaves the lip at the same speed and angle it does
  today; only what happens after it leaves changes. Compare the tick of last contact.
- **AC4 — The landing is continuous.** No velocity step on re-entry beyond what the impulse at
  contact justifies.
- **AC5 — No ride regression, measured PER WAVE ZONE.** The 30 s clean fixture
  (`phone-2026-09-18-08-41-02`, hybrid, `-RailsUntil=2.00`) holds its green-face slip and speed.
  **Do not use a whole-ride aggregate.** That ride is 49% whitewater, 39% flat and 13% pocket, and
  pooling them inverted the verdict on the carve-grip gate (see that spec's "The clean-ride numbers
  were measuring the wrong water"). Split on the `CROSSING` line's `geo=` and report the green face
  separately; the airborne term will mostly fire near the lip, so the pocket slice is the one that
  matters. Reference green-face numbers on the current build: slip mean 9.7, p90 21.1, speed 610.
- **AC7 — A new force family is gated when it is introduced.** The wave-mass thrust and flow drag
  were listed here as "already zero" and so were never gated; when the owner brought them back at
  1e-4 they were an open leak. Any force added or revived must either read `effectiveWaterHeight`
  (gated) or multiply by `waterContactGate` itself. The three that recompute `boardWideEffectiveH`
  locally are the pattern to copy.
- **AC6 — Landing position is not this spec's business.** Owner, 2026-09-23: the board finishing on
  the wrong side of the wave "feels like the responsibility of the wave push, not the carve
  redirect". Same applies here. Do not tune the airborne gate to control where the board lands
  relative to the crest.

## Measured result

Fixture: `phone-2026-09-22-20-26-02` (fish, `-RailsUntil=3.00`), plus the two carve-grip overrides the
airborne event now requires — see "The fixture needs two overrides" below. Control and fix are the
same binary, one `airborneForceScale` apart, so they ride identical physics to the tick the board
leaves the water.

Window = every one of the ~20 surfaces clear of the water (the definition the Problem section used):

| | control (`=1`) | fix (`=0`) | AC1 |
|---|---:|---:|---|
| horizontal speed | 524 -> 969 cm/s (**+84.8%**) | 409 -> 412 cm/s (**+0.7%**) | < 2% — **PASS** |
| heading | **+12.3 deg** | **-1.0 deg** | < 2 deg — **PASS** |

The control row reproduces the Problem section's table (525 -> 969, +12.2 deg) to within rounding,
which is what says the fixture and the measurement are the same ones.

Over the looser whole above-surface excursion the spurious drive falls from **+542 to +34 cm/s** and
the spurious turn from **+19.8 to -1.5 deg**. Peak clearance drops 40 cm -> 25 cm, because in the
control a good part of that height was *powered* rather than thrown.

- **AC2 — the trough still works.** True by construction (a submerged actor gates to exactly 1), and
  the clean-fixture run shows no stranding.
- **AC3 — the launch is unchanged.** Stronger than the AC asks: the two runs are bit-identical
  through frame 235, the last tick with contact (`submersion=+7`, `vH=443` in both), and first differ
  on frame 236, the first tick clear. The launch is not merely similar, it is the same ride.
- **AC4 — the landing is continuous.** Re-entry steps of +8, +16, -1 and -0 cm/s. The fix run has
  **zero** ticks with |dvH| > 60 cm/s; the control has two.
- **AC5 — no ride regression, per zone.** Clean fixture `phone-2026-09-18-08-41-02` (hybrid,
  `-RailsUntil=2.00`), shipping config, green face (pocket + shoulder) reported separately as the AC
  requires: speed 498 -> 494 cm/s (-0.8%), slip mean 12.0 -> 12.3 deg, p90 32.6 -> 35.3 deg.
  Whitewater 170 -> 164. No meaningful green-face cost. (The `flat` slice's slip moves a lot,
  18.6 -> 56.9 deg, but the board is doing 9-12 cm/s there — slip angle at a standstill is noise, not
  a regression. That ride is 66% whitewater and 26% flat, which is why the AC forbids pooling them.)
- **AC6 — landing position** was not tuned for, per the owner's note.

Caveats, stated plainly: all headless, one board (fish) for AC1 and one (hybrid) for AC5, and the AC5
numbers do not match the reference numbers quoted in this spec's AC5 (slip mean 9.7, p90 21.1, speed
610) — those predate the build and are not reproducible from it, so control-vs-fix on the same binary
is the only comparison claimed. Nothing has been ridden on a device.

## The fixture needs two overrides

**The fixture as this spec originally wrote it no longer contains an air.** `4486b42bf` baked
`CarveGripContactBlend` and `CarveGripTurnUnfadeSymmetric` from 0 to 1, *after* the measurement in the
Problem section was taken at `1fa3ba629`. Those change the heading, so the ride diverges and the lip
launch stops happening: on HEAD defaults the same trace's largest excursion is **4 cm for 0.13 s**,
against 24-40 cm for 0.42 s. To reproduce the airborne event, put both back to 0:

```json
{"AssistDisable":1,"StartScreenSkip":1,"AssistSkipFirstPlayCardInPIE":1,
 "CarveGripContactBlend":0,"CarveGripTurnUnfadeSymmetric":0}
```

Two further traps found on top of the five already listed:

6. **`surf.debug.actors` matches `GetActorLabel()`, which the boards spawned by `-BoardInTests` do
   not carry.** A label filter like `bottom_middle` silently prints nothing — it is a real label in
   the editor level, just not on the spawned test board. `SharedCalculations` matches because the
   generated object name contains it. The `contact` flag is therefore flag-only, not label-gated.
7. **"Both SC contact gates read 0.000" is not a reliable airborne test.** It holds when the board
   is properly clear (in the main window all ~20 surfaces were 24-71 cm up and `contactGate` was
   0.000), but near the surface it disagrees with the board's own sampler: measured `contactGate`
   0.95-1.00 while the SC actor sat 8-14 cm *above* the surface. That is not necessarily a bug —
   `amountUnderWater` is written by the buoyancy CORNER actors at their own positions, which can
   still be wet — but it means the gate cannot be used to *delimit* an airborne window. Use the
   signed `submersion`, or the new per-actor `surfaceClearance`, which is what "Finding the airborne
   window" below should key on.

## Shipped ON — `airborneForceScale = 0`, device-verified

It defaulted to **1.0** — the A/B *control* value — while it waited for a feel pass, so for a while
everything here was wired, measured and switchable with the switch off. **Baked at 0 on 2026-09-23
after the owner tuned it to 0 live on the device and rode it**, alongside the carve-grip gate knobs
and the wave-mass lip term. The device pass covers this value, not just the build it shipped in.

`1` still restores the old ungated behaviour bit for bit, so the A/B survives.

Headless it was nearly free on the fixture — min down-line 194 vs 194, peak slip 54 vs 54 deg, mean
speed 409 vs 408 — which is why the device pass was the deciding evidence rather than these numbers.

Worth knowing when reading the numbers below: with the carve-grip gate and the wave-mass lip term
in, the board is barely thrown clear on this trace any more — the fully-airborne window is **0.10 s**
against the 0.42 s this spec was written from, and the residual leak over it is +11 cm/s ungated
against the +542 cm/s originally measured. The hole is much smaller than it was; it is still a hole.

## Handoff — how to reproduce the measurement

Everything below was established 2026-09-23 and is the part that is expensive to rediscover.

### The command

```powershell
$tr  = "<repo>\GoneSurfing\Saved\InputTracesromphone\phone-2026-09-22-20-26-02.csv"
$env:TEST_MAP   = "Surfing_infinite_wave"
$env:EXTRA_ARGS = "-ReplayTrace=$tr -ReplayUseWeights -RailsTrace=$tr -RailsUntil=3.00 " +
                  "-usefixedtimestep -fps=60 -BoardInTests -Board=fish"
& "<repo>\RunGameAndCollectLogs.ps1" -Argument "crossing,damping:SharedCalculations,SurfboardUtils" -TimeoutSeconds 420
```

Trajectory lands in `Saved/Tests/latest/phone-2026-09-22-20-26-02.csv`; its `t` is **trace time**.

### Five traps, each of which cost a run here

1. **`-ReplayUseWeights` is mandatory.** Phone traces have all-zero `tilt_*`/`stick_*` columns (the
   on-screen joystick never populates them), so a plain `-ReplayTrace` is a **silent no-input ride**
   that glides down the line and looks plausible. Nothing in the log says so.
2. **`-RailsTrace` must point at the SAME trace** as `-ReplayTrace`, or the fixture seats the board
   from one ride and feeds it another's input.
3. **Keep the `RunGameAndCollectLogs` argument to two colon-parts** (`flags:actors`). A third part
   sets the autopilot filter, which disables the intro autopilot and leaves the board parked with
   controls already enabled.
4. **Do not end the turn while a run is in its early game phase.** The `Stop` hook kills
   `UnrealEditor.exe` and the run produces no trajectory CSV, silently.
5. **`BuildAndLaunch.bat` exits 1 on a successful build** and prints "Editor not launched. Fix
   errors above and re-run." Check for `Link [x64] UnrealEditor-GoneSurfing.dll` and the DLL's
   timestamp instead of trusting the exit code.

### Finding the airborne window

- `crossing` on `SharedCalculations` prints, per tick: `underW=`, `submersion=` (negative = the board
  pokes above the surface), `slopeSin=`, `geo=<zone>`, `broken=`, plus `bVel=` and `fwdH=` so slip
  and speed come off the same line as the zone.
- `damping` on `SurfboardUtils` prints `frontW=`, `backW=` and the derived `contactGate=`. **Airborne
  = both of those at 0.000.** On this trace that is a 0.45 s window.
- Log frames map to trace time as `t = (frame - releaseFrame)/60 + releaseT`. Get `releaseFrame` and
  `releaseT` from the `Rails: -RailsUntil=... releasing on row N at trace t=...` and
  `HANDOFF DELTA` lines — on this trace, frame 180 and t=2.992.
- Read **one** SC actor (`SharedCalculationsBP_C_1`) or every tick counts twice.

### The sites

- `AFluidDynamics::setup()` — [FluidDynamics.cpp:2207](../Source/GoneSurfing/FluidDynamics.cpp#L2207)
  assigns `effectiveWaterHeight`; **23 references** in that file multiply by it. `actorWetted` is
  computed immediately below it from `waterColumnAbove / wettedTransitionDistance` (60 cm).
- The impulse choke points already gated by `SurfRails::AreForcesSuppressed()` — option 3's insertion
  points: [FluidDynamics.cpp:1946](../Source/GoneSurfing/FluidDynamics.cpp#L1946),
  [:2046](../Source/GoneSurfing/FluidDynamics.cpp#L2046),
  [:2093](../Source/GoneSurfing/FluidDynamics.cpp#L2093),
  [Buoyancy.cpp:82](../Source/GoneSurfing/Buoyancy.cpp#L82),
  [SharedCalculations.cpp:1006](../Source/GoneSurfing/SharedCalculations.cpp#L1006).
- Follow the house pattern for the knob: a 0..1 scalar where **one specific value reproduces today's
  behaviour bit for bit**, so the A/B survives (`SidewaysDampingAirScale` is the model), registered
  in `SurfTuningSubsystem.cpp`'s category table so it is live in the tuning HUD and via `surf-live`.

### Where this came from

Commit `1fa3ba629` ("Sideways damping stops braking a board that has left the water") added the
contact gate that removed the masking brake and made the airborne drive visible;
`4486b42bf` is the head of that arc. Branch `release-fixes-2026-09-22`.

## Related

- [replay-rails-until.md](replay-rails-until.md) — the fixture; AC1 is only measurable because the
  board can be put back at the same state twice.
- [carve-grip-via-redirect.md](carve-grip-via-redirect.md) — "Step 4 revisited", the sideways-damping
  gate that exposed this.
- [per-actor-wetting.md](per-actor-wetting.md) — `actorWetted`, the signal option 1 would use.
