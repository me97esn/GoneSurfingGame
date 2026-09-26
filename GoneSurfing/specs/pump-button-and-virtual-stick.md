# Spec: Pump Button and Virtual Joystick

## Overview

Replace the two motion-based Android controls with on-screen touch controls: a **virtual joystick**
for weight shift (today: phone tilt, `specs/surfing-controls.md`) and a **held pump button** for
pumping (today: shaking the phone, `specs/pumping.md`). Tilt stays in the build behind a tuning
switch so the two schemes can be compared on device.

Nothing in the physics changes. Both controls write the same values the existing schemes write —
`AWeightDistribution::amountInFront` / `amountToTheRight`, and `ASurfboardPawn::PumpInput` — so the
force model, the pump impulse and the bottom-hydrofoil `pumpGate` are untouched.

## Motivation

Both motion controls fail for the same reason: **the player cannot watch the wave and perform the
gesture at the same time.**

- **Shaking to pump** is the worse of the two. A shake cannot be modulated — there is no half-pump
  and no "pump for two seconds" — and the phone's motion pollutes the tilt steering while it
  happens. It also demands rhythmic motor timing from a player who is trying to read a wave.
- **Tilting to steer** is better, but it has no fixed reference: neutral is wherever the phone
  happens to be held, it drifts over a ride, and a hard lean means physically rotating the screen
  away from the player's eye line.

A stick and a button also make the fore/aft weight ceiling
(`USurfTuningSubsystem::WeightMaxInFront`, added 2026-09-08) work properly rather than eating input
— see FR3.

## Objective

Two thumbs, both resting in the bottom corners, neither of which needs to be looked at:

- Left thumb steers, with a stable neutral that is wherever the thumb went down.
- Right thumb pumps, holding for as long as the pump should continue.

## Scope

**In scope:** the virtual joystick, the pump button, their layout and hit zones, the input-source
switch, the pump's weight oscillation and its interaction with steering and with the forward cap.

**Out of scope:** the pump animation (the author is producing it as part of this work); the physics
of pumping (unchanged, `specs/pumping.md`); PC controls (mouse/keyboard path unchanged); trick
scoring; the start-screen tutorial's copy, which will need rewriting once the scheme is settled but
should not be rewritten twice.

## Functional Requirements

### FR1: Input-source switch

A tuning value selects the Android weight-input scheme, live-switchable from the tuning HUD:

```
USurfTuningSubsystem::WeightInputSource   // 1 = virtual joystick (default), 0 = phone tilt
```

`>= 0.5` means joystick. Default **1**. The existing `AndroidWeightInputSource` UPROPERTY
(`Tilt` / `Stick`, where `Stick` means a physical gamepad) stays as it is; this is a third source
and the tuning value takes precedence on Android.

Switching mid-session must not strand state: leaving joystick mode releases the stick and
auto-centres; leaving tilt mode drops the tilt basis so the next entry recalibrates.

### FR2: Virtual joystick (weight shift)

- **Floating origin.** The stick's centre is wherever the thumb first touches inside its zone, not a
  fixed circle on the screen. This is what makes it usable without looking.
- Deflection from that origin maps to `CurrentWeightOffset` exactly as the physical stick does —
  X = sideways, Y = fore/aft — so everything downstream (assist, input trace, replay) is unchanged.
- Full deflection at **90 logical px** of travel; deadzone **8%** of that.
- **Release: the command latches on both axes** (`StickLatch`, default 1, 2026-09-14): lean and
  trim stay where the thumb left them, so a turn carries on and a lifted nose stays lifted after
  release; a *tap* on the stick (a touch that never travels past the deadzone from where it
  landed) re-centres both; and a touch-down onto a held command *picks it up* — the origin is
  placed so the thumb lands at the current offset, not at zero — so adjusting never drops it
  first. The knob draws deflected in the home ring while latched, so a held command is never
  invisible state. Set the tunable to 0 for the original everything-auto-centres behaviour over
  `WeightAutoCenterDuration` (0.3 s, AC1). Sideways-only latching was the first cut and was
  rejected on the phone the same day: two axes returning on different rules felt like two
  instruments. If testers over-lean with the latch on, the fix is a heading-command stick (Out of
  scope), not a tweak here.
- **Neither axis self-centres while held** (since 2026-09-14). `StickForeAftReturnSeconds` still
  exists (default now **0**; > 0 makes the origin drift toward the thumb's Y so a push is a nudge
  that decays) — it was 0.6 s to stop a forgotten nose-down trim, but with the latch it snapped
  the nose back the moment the thumb stopped moving and read as the stick ignoring the player.
  The FR3 forward cap is what guards a forgotten nose-down now.
- The drawn stick is a visual echo of the touch, not a control that must be aimed at: it appears on
  touch-down, follows the thumb, and fades on release.

### FR3: The forward cap becomes range, not a wall

With the joystick, map the fore/aft axis so **full-forward stick equals `WeightMaxInFront`**, rather
than mapping to 1.0 and clamping there:

```
amountInFront = (offsetY >= 0) ? 0.5 + offsetY * (WeightMaxInFront - 0.5) * 2   // forward half
                              : 0.5 + offsetY * 0.5                             // back half
```

With `WeightMaxInFront` below 0.5 the forward half of the range compresses to nothing, which is
correct and deliberate — the board cannot be put nose-heavy — but the player is no longer spending
input travel on a region that does nothing. This is the fix for the dead forward half observed on
2026-09-08. The clamp itself stays as a final safety (the assist can still bias trim forward).

### FR4: Pump button - crouch on hold, drive on release

A pump is a crouch and a rise, not a rhythm. **Holding** the button crouches the surfer; **releasing**
extends the legs and drives the board down. All of the force belongs to the extension.

- **The moment of release is the skill.** Let go entering a turn, or at the bottom of the face, and
  the drive lands where the rail can use it. That is a decision the player makes, not a cadence the
  game imposes.
- **Charge scales the pump.** `PumpChargeSeconds` (0.5) is the hold that reaches a full crouch;
  releasing earlier gives a proportionally weaker pump, which is what makes a half-pump expressible
  at all. Below `PumpMinCharge` (0.15) nothing fires, so a thumb brushing the button cannot pump.
- **The extension is the force window.** `PumpReleaseSeconds` (0.35) shapes `PumpInput` as a half-sine
  across it, scaled by the stored charge. Zero while crouching — the same loading-phase-only
  asymmetry the accelerometer path had, except the player now chooses when the loading phase is.
- Everything downstream is unchanged: the downward impulse, the speed and slope attenuations, and
  the hydrofoil's `pumpGate` all read `PumpInput` exactly as before.

**Superseded 2026-09-10.** The first version was hold-to-pump: holding ran a free 1 Hz cycle. It was
wrong about the movement — a surfer stores energy by crouching and spends it by rising, and the
timing of that spend is the whole point — and it made holding the button strictly better than not,
which is why the flat-water runaway needed capping in the first place. Charge-and-release costs the
player a decision per pump and rewards it.

**Animation:** two clips, `Content/Surfer/SK_crouch_down` and `Content/Surfer/SK_rise_up_tall`,
blended inside the AnimBP's Surf state so the procedural lean downstream still applies. Both are
Sequence Evaluators (never Players - a Player keeps its own clock and drifts out of phase):

- crouch: `ExplicitTime = PumpCharge x crouchLength`
- rise:   `t0 = 1 - PumpReleaseFromCharge`, `ExplicitTime = (t0 + (1 - t0) * PumpReleasePhase) x riseLength`

`bPumpReleasing` gates between them, with a very short blend - that switch is the moment of release,
which is the thing the player is timing. `bPumping` gates the pair against the stance.

The rise clip should end at TALL, not settle back to the stance: the outer blend already returns to
the stance, and a settle inside the clip gets crushed into the force window.

**The return to the stance is the third movement (added 2026-09-10).** It is not a clip - it is the
outer blend standing the pump pair down - and on its own it was a snap. `bPumping` and
`bPumpReleasing` drop on the SAME tick the extension completes, so at the instant the outer blend
starts, the inner gate has already swapped to the crouch clip at time zero, which IS the stance: the
blend crossfades stance to stance while the rider teleports down from tall.

`USurferAnimInstance` now holds the rise clip pinned on its last, tall frame for
`PumpRecoverySeconds` (tuning subsystem, default 0.45) after the extension ends, with `bPumping`
already false so the outer blend runs across exactly that window. `bPumpSettling` reports it. The
settle is visual only and strictly after the force window - `PumpInput`, the attenuations and the
next pump's charge are the pawn's and see none of it - and a fresh stroke cancels it outright.

Two ways to wire the duration, and they differ:

- Leave the outer Blend-by-bool on `bPumping` and set its stance-side **Blend Time** to match
  `PumpRecoverySeconds`. The settle guarantees the blend has a tall pose to leave from; the AnimBP
  still owns the timing, so the two values have to be kept in step by hand.
- Or swap that node for a Blend taking `PumpStanceBlendAlpha` as its alpha (1 = pump pair, 0 =
  stance, smoothstepped down across the settle). Then the AnimBP has no opinion about timing and
  `PumpRecoverySeconds` alone decides it - tunable on device from `Saved/TuningOverrides.json` with
  no recompile. This is the one to prefer.

### FR5: Pumping does not touch the weight at all

A pump produces the downward impulse and nothing else. It does not shift the rider's weight fore or
aft, and the forward ceiling is therefore irrelevant to it.

**Tried and removed 2026-09-09.** The first implementation swung the weight — amplitude 0.35 around
the player's trim — so that the board visibly pumped. That swing turned out to be a drive path in
its own right: rocking fore and aft at 1 Hz works the bottom hydrofoil through board pitch, which is
where nearly all of the board's drive comes from, and none of the pump's attenuations reached it. On
the 2026-09-09 PC trace a held pump on flat water behind the wave ran the board to 1570 cm/s, above
the ~1200 it reaches riding down the line, while the impulse path contributed nothing (tapered to
zero above 600 cm/s). Tapering the swing by the same factor fixed the speed but left a control input
whose only purpose was to be seen.

Feedback for pumping belongs in the **pump animation**, which moves the rider without touching the
physics. That animation landed 2026-09-09 and the debug sphere that stood in for it has been
deleted.

A second effect died with the swing, worth remembering if anything like it is proposed again: with a
tail-heavy trim the swing's trough clipped at 0 while its peak moved freely, so each cycle carried a
net forward bias rather than being a symmetric rock.

### FR5b: The stroke always fires; only its effect is attenuated

Pressing the button always starts a stroke and always produces feedback, everywhere, at every speed.
What the attenuations govern is how much the stroke *achieves*, never whether it happens.

`AWeightDistribution` therefore carries two channels:

- `PumpInput` — the force signal. Attenuated in place by speed and slope, frequently to exactly zero
  (81% of live samples on the 2026-09-09 log). Read by the impulse and the hydrofoil `pumpGate`.
- `bPumpActive` + `PumpStrokePhase` — the feedback signal, which no attenuation touches. Read by
  `USurferAnimInstance` as `bPumping` / `PumpPhase`, and through them by the pump clip.

Two rules follow, and both were bugs before they were rules:

1. **A fresh press restarts the cycle from phase 0**, so the loading pulse is immediate. A press
   landing while a released stroke was still finishing used to resume mid-cycle, sometimes in the
   silent recovery half, and the button felt ignored.
2. **Feedback runs across the whole cycle**, not only the loading half. `PumpInput` is silent through
   recovery by design; a stroke indicator that follows it blinks out for half of every second and
   reads as dropped input.

The rider therefore pumps visibly even where the pump achieves nothing, which is deliberate: the
stroke is what the player did. Whether it *worked* is not yet conveyed anywhere in the fiction now
that the sphere is gone — a full-commitment versus token-effort distinction in the animation would
be the natural home for it.

### FR6: Pumping does not touch sideways steering

`PumpLateralAttenuation` scales the player's commanded sideways shift toward centre while pumping.
It defaults to **1.0**, which is no scaling at all — the knob is inert by default and should stay
that way:

```
amountToTheRight = 0.5 + (commanded - 0.5) * PumpLateralAttenuation
```

Ramped in and out over ~0.2 s so entering and leaving a pump is not a step.

**Settled 2026-09-09, after going the wrong way twice.** The first version kept half the steering
authority; the second removed it entirely on the belief that a surfer either pumps or turns. Both
are wrong. Pumping while rolling onto a rail is not merely possible, it is a preferred technique:
you turn the board onto its rail while driving your weight down through it. The two are one
movement, so the game must not separate them at all — not even by reducing sensitivity.

The mechanism survives as a knob at 1.0 (inert) so the coupling can be A/B'd, not because it is
expected to move. The ramp and the ordering relative to the assist still apply if it ever does.

### FR7: Layout and hit zones

Numbers are in the phone's real logical space, **1200 × 540** (see the `screenshot-game` skill; note
`-Phone` renders 1200×540 *pixels*, which the DPI curve expands — judge fit at true logical size).

| Control | Visual | Touch zone |
|---|---|---|
| Weight joystick | drawn at touch point, ~90 px radius | left 40% of the screen, full height below the top 20% |
| Pump button | ~110 px diameter, centre ~100 px from the right edge and ~95 px from the bottom | bottom-right ~260 × 230 px region |

- **Hit zones are much larger than the drawn controls**, extending into the screen corners. A thumb
  landing anywhere in the region works. This is the single most important detail for eyes-off play.
- **A thumb that is already down when the controls appear works too.** Slate only tells a widget
  about a finger through its touch-started event; a thumb resting on the start screen while the
  other taps Start, or one that lands during the first (slow) frame after Start, never sends one to
  the pads, and the stick then did nothing until it was lifted and pressed again - about one ride in
  five on device (2026-09-12). The pads now also poll Slate's pointer table every game tick and adopt
  any finger that is down inside a free pad and not captured by a live widget, with the thumb's
  current position as zero. The same poll drops a finger Slate no longer lists, so a lost touch-end
  can never leave the board steering on its own.
- Zones must exclude the rects of any live UI buttons (the HUD's Restart / Replay / Back, the replay
  overlay's bottom bar) so a touch never does two things at once.
- The bottom-left corner is free as of 2026-09-09 — the wave radar was switched off
  (`specs/wave-radar.md`), which removes the only collision with the conventional layout.
- `bMirrorTouchControls` (default false) swaps the two sides for left-handed players. Cheap now,
  expensive later.

### FR8: Haptics

A short vibration on each pump stroke. This is the direct answer to "I cannot watch the screen while
pumping": it returns the rhythm to the player through the thumb, the one channel not competing with
the wave for their attention. Off switch: `PumpHaptics` (default on).

## Non-Functional Requirements

- **NFR1:** No change to any force, coefficient or physics path. The controls write the same three
  values the existing schemes write.
- **NFR2:** The input trace keeps recording what the player commanded, before the cap and before the
  pump oscillation, so old traces stay comparable and replays reproduce the recorded ride.
- **NFR3:** Touch controls must not appear, and must consume no input, during the start screen, an
  autopilot intro, or a replay.
- **NFR4:** Headless test runs enable player controls after autopilot handoff. The touch UI must be
  gated so it cannot install or write weight during a test run, or it will corrupt snapshot CSVs
  (see `specs/run-game-and-collect-logs.md`).
- **NFR5:** Slate only, consistent with the other overlays in this module. No UMG dependency.

## Acceptance Criteria

### AC1: Joystick steers
Given the joystick scheme, when the player drags the left thumb fully right, then
`amountToTheRight` reaches 1.0. On release: with `StickLatch` 0 it returns to 0.5 within 0.3 s;
with 1 (default) it stays at 1.0 until a tap on the stick, which returns it to 0.5 within 0.3 s.
The same holds for `amountInFront` on a fore/aft drag. (Latch verified on desktop 2026-09-14 via
`Drive.ps1 -Steps "drag 144 270 224 270; …; click 144 270"`.)

### AC2: Floating origin
Given no touch, when the player touches down anywhere in the left zone, then the commanded offset is
(0, 0) at that point regardless of where on the screen it was — no jump.

### AC3: Forward travel is live
Given `WeightMaxInFront = 0.45`, when the player pushes the stick fully forward, then
`amountInFront` reaches 0.45, and intermediate positions produce intermediate values. No part of the
forward range is inert.

### AC4: Crouch stores, release drives
Given the ride is live, when the button is held, then `PumpInput` stays at zero and the surfer
crouches; when it is released, then `PumpInput` runs a single half-sine over `PumpReleaseSeconds`
scaled by the charge held, and the board gains speed once per release rather than continuously.
A hold shorter than `PumpMinCharge` produces nothing at all.

### AC5: Pumping is visible without being a force
Given a held pump, when observed from the ride camera, then the rider's animation shows the stroke
while `amountInFront` is unchanged by it. Until the animation exists, the debug sphere swelling once
per stroke is the stand-in. A held pump on flat water must not exceed the speed the board reaches
riding down the line.

### AC6: Pumping leaves the turn alone
Given a sustained lean, when the player starts pumping, then `amountToTheRight` is unchanged and the
board keeps turning at the same rate. Rolling onto a rail and pumping at the same time must feel
like one movement, because that is what it is.

### AC7: Scheme switch
Given `WeightInputSource = 0`, when the ride starts, then tilt drives weight and no touch controls
are drawn; setting it to 1 mid-session switches to the joystick without a restart and without
leaving stale weight applied.

### AC8: No interference
Given a headless test run, when the autopilot hands off, then no touch control is installed and no
snapshot CSV changes relative to its baseline.

## Test Cases

1. **Flat water, held pump, 5 s** — speed at t=5 s exceeds the no-pump control run. Compare against
   the accelerometer path at the same `MaxPumpForce`.
2. **Pump through a carve** — sideways deflection held at 1.0 while pumping: yaw rate is roughly
   half the non-pumping rate, and non-zero.
3. **Forward-stick sweep** — sweep the fore/aft axis end to end and log `amountInFront`: monotonic,
   no flat regions, top value equals `WeightMaxInFront`.
4. **Thumb-landing sweep** — touch down at 10 points across the left zone; commanded offset starts
   at (0, 0) every time.
5. **Zone exclusion** — touch each live UI button; it actuates and no weight is commanded.
6. **Scheme A/B on device** — same wave, tilt vs joystick, judged on whole-ride aggregates rather
   than a single event (see the note on single-event maxima in `specs/per-board-tuning.md`).

## Tuning Values

New, all in `USurfTuningSubsystem`:

```
Tuning|Weight   WeightInputSource        = 1.0    // 1 = joystick, 0 = tilt
Tuning|Weight   StickFullDeflectionPx    = 90.0
Tuning|Weight   StickDeadzoneFrac        = 0.08
Tuning|Weight   StickForeAftReturnSeconds    = 0.6    // fore/aft self-centres while held; 0 = hold trim
Tuning|Pump     PumpChargeSeconds        = 0.5
Tuning|Pump     PumpReleaseSeconds       = 0.35
Tuning|Pump     PumpMinCharge            = 0.15
Tuning|Pump     PumpLateralAttenuation   = 1.0    // inert: pumping does not affect steering
Tuning|Pump     PumpHaptics              = 1.0
```

## Resolved

- **Pumping should cost something** (2026-09-09). Holding pump is otherwise strictly good on flat
  water, bounded only by the existing speed attenuation. The intended direction is a **stamina
  pool** that every hard movement draws from — pumping and hard turns — rather than a
  stronger speed taper, so speed and turning compete for the same resource. **Deliberately not part
  of this work**; it wants its own spec.
- **Fore/aft self-centres while held; sideways does not** (2026-09-09). See FR2.
- **One stick and a button, never two sticks** (2026-09-09). A phone in landscape offers two thumbs
  and hold-to-pump cannot share a thumb with steering, so the question was never "one stick or two"
  — it was what the second thumb does, and pumping claimed it.
- **A sideways-only stick was considered and not taken** (2026-09-09). After the two decisions above,
  fore/aft is a decaying nudge inside a band capped below centre, which is thin justification for a
  whole axis; one-dimensional sticks are also markedly easier to use without looking (no diagonal,
  no accidental fore/aft mid-carve, the thumb slides along a line). Kept 2D because the nudge is
  exactly what the drop and stalling for the pocket need, and the pump cannot substitute — its
  oscillation is symmetric and averages to no trim change. **Revisit if** on-device play shows
  players rarely using fore/aft deliberately, or if accidental fore/aft during hard carves survives
  the self-centring.
- **Diagonal coupling** is a known property of the 2D stick: a hard carve is pushed diagonally
  whether the player means it or not, so every hard turn carries an unintended fore/aft component.
  `StickForeAftReturnSeconds` cleans it up on its own — a second, unplanned argument for the
  self-centring decision above.
- **Tilt is kept, not deleted** (2026-09-09). It survives as a player option, selected today by the
  `WeightInputSource` tuning value and eventually by an in-game settings page (follow-up, not this
  work). The tutorial then has to teach whichever scheme is selected rather than assuming tilt.

## Open Questions

1. **Should pumping onto a rail be MORE effective than pumping flat?** If driving weight down
   through a board that is already rolled onto its rail is the preferred technique, the game
   currently under-rewards it: the pump impulse is the same either way, and only whatever the carve
   redirect does with the extra downward motion distinguishes them. Rewarding the combination would
   make the better technique the faster one. Measure what the existing physics already gives before
   adding anything — the redirect may cover it.
2. **How much authority does a decaying fore/aft nudge leave?** With the axis self-centring over
   0.6 s, a sustained tail-weighted trim is no longer expressible by the player at all — only
   the assist and the physics decide where the board settles. If that proves too little control, the
   lever is `StickForeAftReturnSeconds` (0 restores a held trim), not the ceiling.

## Status

Implemented 2026-09-09 in `TouchControlsOverlay.*` plus the pawn wiring. Compiles and installs;
the controls draw and the pump button renders in a true-logical-size capture. **Nothing below is
device-verified** — no thumb has touched it yet.

- [x] `WeightInputSource` tuning value + scheme switch (tilt stands down when the joystick is up)
- [x] Virtual joystick widget (floating origin, oversized zones, mirror, fore/aft self-centring)
- [x] Fore/aft range remap against `WeightMaxInFront` (FR3) — joystick only; tilt keeps the clamp,
      since its neutral is wherever the phone is held
- [x] Pump button widget + hold-to-pump cycle driving `PumpInput`, finishing the stroke on release
- [x] Pump weight oscillation — built, then removed (FR5): it was an un-attenuated drive path.
      Visible feedback is the animation's job; the debug sphere stands in meanwhile.
- [x] Sideways attenuation while pumping, ramped (FR6)
- [x] Lifecycle gating: no touch UI during the start screen, replay, board picker, a fall, or a
      headless test run (NFR3/NFR4)
- [ ] **Phase-lock the pump cycle to the board's pitch response** (FR4). Currently a fixed-frequency
      cycle at `PumpHz`. A held pump is therefore in time only by luck, which is the part of FR4 that
      makes the rhythm forgiving — worth doing once the fixed version has been felt.
- [ ] **Haptics** (FR8). Left out deliberately: phone vibration needs an Android-only JNI call and
      this machine can only build Win64, so it could not be compiled or tested here.
- [x] Pump animation, phase-locked to the physics stroke: a Sequence Evaluator inside the AnimBP's
      Surf state, sampled at `PumpPhase x` clip length, blended over the stance on `bPumping` so the
      procedural lean downstream still applies. A Sequence Player will NOT work here - it keeps its
      own clock, so it plays at the authored rate, drifts out of phase, and never restarts.
- [ ] On-device A/B against tilt
- [ ] Tutorial copy rewritten to teach the selected scheme (not just tilt)
- [ ] In-game settings page exposing the control scheme — follow-up, not this work

### The Slate controls are a testing scaffold

The C++ Slate widget exists to answer one question quickly: does a button-and-stick scheme beat
tilt and shake on the water? It is drawn in flat rings because that was the cheapest thing that
could be played, not because it is the intended look. **If the scheme proves out, the pump button
should be rebuilt in UMG** so it carries the game's visual design like the rest of the interface.

The migration is not symmetric, and the button is the easy half:

- **The pump button** is a plain press-and-release. UMG can own the whole thing — it needs to set
  the pawn's held flag on pressed and clear it on released, and `ASurfboardPawn::UpdateHeldPump`
  keeps running the cycle from that flag alone. Nothing else moves.
- **The joystick** is harder, because its floating origin means it has no fixed rect: the control
  materialises wherever the thumb lands, anywhere in a zone covering most of the left half of the
  screen. That is a custom hit-testing widget in any framework, so it may be right to leave it in
  Slate and dress it, while the button becomes UMG.

Either way the input layer stays put: `TouchControls::FState` and the pump cycle are the contract,
and the drawing is what changes.

### Verifying it on desktop

`surf.input.touchui 1` shows the controls in PIE and lets the mouse drive them; `2` additionally
bypasses the ride-state gates so the layout can be screenshotted before the ride starts. Judge the
layout at the device's true logical size, not with `-Phone`: `ApplicationScale=2.0` under
`[/Script/Engine.UserInterfaceSettings]`, captured at `-ResX 1200 -ResY 540`, then revert the ini.

**Known layout issue:** in the pre-ride state the pump button sits under the UMG bottom bar
(Back / Shortboard / Replay / Restart). The overlay is at ZOrder 80, below everything that wants
clicks, so the bar still works — but whether the two ever share the screen during a live ride
needs a device check. If they do, the button moves up.
