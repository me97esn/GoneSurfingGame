# Spec: live control feedback in the tutorial

## Overview

While reading the tutorial cards, players **perform the gestures and wait for something to
happen**. Nothing does. Give each card a live board that responds to the real sensor input.

## Objective

Answer two questions the cards currently leave open:

1. *Did that register?* — the sensor is working and it saw me.
2. *What does it do to the board?* — the mapping from phone motion to board response, which is the
   actual content of the lesson and is currently taken on faith.

(2) is the reason the feedback shows the **board** rather than mirroring the phone. Mirroring only
answers (1); "tilt it back to lift the nose and turn faster" is a claim the player cannot check.

Observed in user testing, 2026-08-21.

## Requirements

### FR1 — A response glyph per step, viewed along the axis it teaches

A board silhouette drawn in Slate (no new art), under or within the illustration card. The **view
changes per step**, so the taught axis is always the one facing the player:

| # | Step | View | Responds by |
|---|------|------|-------------|
| 1 | sideways | from behind (tail-on) | rolling onto its edge |
| 2 | fore/aft | from the side | nose lifting / dropping |
| 3 | pump | from the side | launching forward out of frame |

A single fixed view would show two of the three responses edge-on, where the motion is invisible.
The side view is also what gives pump something to move along: a paused board cannot accelerate, so
**forward translation is how speed gets expressed**.

Only the axis being taught is live on each card. The others read as noise the player did not ask
for — and the *view* cannot decide this on its own, since fore/aft and pump share the side-on board.
Without an explicit per-card filter the pump card pitched to a gesture it says nothing about.

**The pump board flies.** First attempt nudged it forward with speed lines; on device the player
could not tell whether anything had happened at all, because they are busy pumping a phone and fine
detail is not available to them. It now launches out of frame and wraps round: each pump adds speed,
drag bleeds it off, so pumping in rhythm keeps it running — which is exactly what the card claims.
The widget clips, so leaving really looks like leaving.

**The board outlines are hand-drawn.** `Content/Images/surfboard_profile.svg` and
`surfboard_rear_view.svg`, converted to point lists by `Tools/svg2glyph.py` into the generated
`TutorialBoardGlyphShapes.inc`. **Redraw the SVG and re-run the script**; never hand-edit the `.inc`.

Kept as paths rather than imported as textures, which is what makes the rest work: they stay crisp
at any size, they **tint** to the accent colour for the success flash, and there is no asset to
import or keep in sync. The converter normalises the hull to span x −1..1, scales y by the same
factor, centres on the hull, and mirrors the profile so the nose points +x — the direction the pump
card travels.

It handles the subset Inkscape emits here (relative `m`/`c`/`z`). A path using arcs or quadratics
would need the parser extending. The **hull must keep the id `path1`**, since that is what the
normalisation is measured from; everything else in the file is drawn as-is.

(The first outlines were hand-authored in code — a fat lens with one fin that read as a leaf, then a
better guess at real proportions. Both are gone; this note is only here to explain why the code once
had bezier samplers in it.)

### FR2 — Driven by the ride's own math, not a re-derivation

The mapping must come from the same code the ride uses. A tutorial that teaches a subtly different
mapping is worse than no feedback, and a copy will drift the first time either side is tuned.

`UpdateTiltWeight` / `UpdatePumpInput` cannot be called as they stand: both are `#if PLATFORM_ANDROID`,
both read pawn state, both write to the pawn. Extract the pure mapping into a shared unit:

```cpp
struct FTiltBasis      { FVector NeutralGravity, ForwardAxis, RightAxis; };  // from Calibrate()
struct FTiltYawState   { float AngleDeg, RateBias, BiasAccum, BiasElapsed; bool bBiasReady; };
struct FTiltTuning     { float PitchForFull, RollForFull, DeadzoneDeg, YawGain, YawSign,
                               YawLeakSeconds, YawBiasSampleSeconds;
                         bool  bInvertPitch, bInvertRoll; };

FTiltBasis SurfTilt::Calibrate(const FVector& Gravity);
FVector2D  SurfTilt::WeightOffset(const FTiltBasis&, const FTiltTuning&, FTiltYawState& InOut,
                                  const FVector& Gravity, const FVector& RotationRate, float Dt);
float      SurfTilt::PumpFromAcceleration(const FVector& Gravity, const FVector& Acceleration,
                                          float& InOutLowPass, const FPumpTuning&, float Dt);
```

Compiled **unconditionally** — no platform gating in the pure unit, so desktop type-checks it.
`AdvanceTiltYawFusion` already follows this rule and moves across as-is.

The pawn keeps its `FTiltYawState` and basis; the overlay keeps its own. Neither can disturb the
other, because the state is a parameter rather than a member.

### FR3 — Preview calibration, isolated from the ride's

The mapping is relative to a neutral pose. The ride's is captured at autopilot handoff, long after
the menu, so the tutorial needs its own — taken from the first good gravity sample (`|g| >= 0.5`)
after the cards open, and **discarded at Start**.

It must never write `NeutralGravity` / `TiltForwardAxis` / `TiltRightAxis` on the pawn. Corrupting
those silently changes where "neutral" is for the whole ride.

**Re-seed the preview neutral when the grip swaps.** A neutral captured flat is ~90 degrees off once
the player sits up, and the preview would peg at full deflection and stay there. The
[start-screen-tutorial](start-screen-tutorial.md) dip already provides the moment to do it: re-seed
at the trough, where nothing is on screen.

### FR4 — ~~The demo loop yields to live motion~~ WITHDRAWN (device, 2026-08-21)

The idea was that the A/B flip and the player's own hand would be two motions arguing, so the flip
should hold while the player performs the gesture and resume after ~1 s of stillness — giving a free
"watch, then your turn" beat.

**It failed on contact: a hand holding a phone is never still**, so the activity gate was
permanently held and the illustrations simply never animated. A frozen illustration is a worse
failure than a busy one. The flip now runs unconditionally, and the theory is recorded here so it is
not re-invented.

### The drawn board has to have weight

The glyph originally chased the commanded lean at 12 Hz — instantly, in human terms. **The real board
does not.** It has mass and the fork's angular damping, deliberately tuned so the board does not
pitch or roll quickly, so a weight shift takes real time to become an attitude. The tutorial was
therefore teaching a snappier board than the one being ridden — a fidelity bug, not a preference,
and exactly the kind of drift FR2 exists to prevent. Getting the *mapping* from shared code is not
enough if the *dynamics* are invented.

The attitude now follows a **second-order spring-damper** toward the commanded lean, not a faster or
slower lag. The character is the point: a board settles into a lean and rights itself; it does not
ease exponentially. `surf.start.boardhz` and `surf.start.boarddamp`.

Applied in the **offset domain**, so the lag is inside what success measures — the card cannot be
earned by a flick the drawn board never followed.

These can only be set by eye — by riding, then watching the glyph, and matching — so they live on
`USurfTuningSubsystem` under **Tuning|Tutorial** rather than on CVars: something adjusted by eye
needs to be adjustable *live, with a slider, while looking at the thing it changes*. The tuning HUD
enumerates the subsystem's float properties automatically, and sits at ZOrder 300, above the start
overlay, so the sliders work with the glyph on screen.

The same category carries the pump card's feel (`TutorialPumpLaunch`, `TutorialPumpDrag`), so the
whole tutorial is dialled in from one panel.

Tuned on device against the real board (2026-08-21): **damping 1.5**, **Hz 1.0**, **pump launch 50**.
Overdamped, so the board leans over and stays there instead of wobbling into place, which is what the
real one does under its angular damping — and Hz is what compensates for the slower approach that
buys, since damping above 1 removes the wobble at the cost of arriving later. Only `TutorialPumpDrag`
is still a first guess.

Note the HUD does **not** persist: values are session-only, so a number that works has to be brought
back here as a default.

### Three different things called "sensitivity"

Worth separating, because only one of them is the tutorial's to change — and none of them is what
"the board moves too fast" turned out to mean:

| What | Knob | Scope |
|---|---|---|
| How much phone tilt is full deflection | `TiltRoll/PitchDegreesForFullDeflection` on `USurfTuningSubsystem` | **The ride.** The preview reads it on purpose (FR2), so changing it moves both together — that is the feature, not a side effect |
| How far the *drawn* board swings for that deflection | `surf.start.tiltgain` | Tutorial only, purely visual |
| How far you must go for the card to count | `surf.start.successtilt` | Tutorial only |
| How *quickly* it gets there | `surf.start.boardhz` / `boarddamp` | Tutorial only — but should be matched to the ride, see above |

The last two are independent by construction: success is measured on the **offset**, not on the drawn
angle, so amplifying the drawing cannot quietly make a card easier or harder to earn. That was a real
trap — the first version derived the success test by dividing the drawn angle back out, which would
have coupled them the moment a gain existed.

### FR5 — Exaggerate, and show the deadzone

Real offsets are 0..1 with a soft deadzone (`TiltAngleDeadzoneDegrees`, 2 degrees) and full
deflection at 25 degrees. Mapped 1:1 to a small glyph, an ordinary gesture would barely move it.
The glyph amplifies — but the deadzone stays visible as genuine dead travel, because "nothing
happens until you commit" is part of the lesson.

## Hazard: the flat grip is where the tilt basis degenerates

Found while specifying this, and it constrains FR3.

`Calibrate` seeds the forward axis with device **+X, the screen normal**, and projects out gravity.
When the phone is flat, gravity *is* the screen normal, so the projection collapses:
`Fwd.Size() < 0.2` and the code already logs "near-degenerate ... phone is close to flat".

This is not a code defect — it is inherent. With the phone flat, gravity cannot say which way the
player is facing, so "right" is genuinely undefined. The existing calibration dismisses it as
unreachable *while surfing*, which is fair.

But the tutorial is not surfing, and **this is exactly the flat card's pose**: `|g.z| < 0.40` spans
from ~24 degrees off flat all the way to dead flat. So a preview neutral captured there yields a
roll axis pointing anywhere, and the sideways glyph — the first thing the player tries — would
respond to the wrong axis or not at all.

**Decision: suppress tilt feedback instead of fixing the basis** (2026-08-21). Reading the cards
with the phone laid flat is a rare way to hold it, and a device-frame fallback would mean inventing
a second basis construction — new code on the tilt path, for the pose nobody plays in.

So the glyph is **live only where the basis is trustworthy**, and says so rather than sitting frozen:

- The conditioning number is `|Fwd| = sqrt(1 - g.x^2)`, which — since `g.y ~ 0` in every measured
  pose — is `~|g.z|`, the **same number** as the grip signal and the fusion's `YawWeight`. Three
  uses, one quantity.
- Below `TiltFeedbackMinConditioning` (0.25, with a hysteresis band like the grip swap), the tilt
  glyph fades out. **Fading, not freezing** — a glyph stuck at rest is exactly the "looks like a
  bug" failure the grip-swap dip was added to fix.
- **Pump is exempt and always live.** `UpdatePumpInput` needs only gravity's direction and
  acceleration — no basis, nothing to degenerate. Card 3 responds in any grip, flat included.

**Known consequence, accepted:** the sideways card is shown below `|g.z| = 0.40` and the glyph needs
`0.25`, so cards 1-2 have a live glyph over only part of their own range. A player holding the phone
truly flat gets feedback on the pump card alone. This is tolerable because holding it flat while
reading is uncommon, and because holding it up to read — the common case — is both well-conditioned
*and* already past the grip swap into the steer card, where feedback works throughout.

Worth re-checking on device: if flat-ish reading turns out to be common after all, the fallback
basis comes back on the table.

## FR6 — A success beat when the gesture actually lands

The live board says *something is happening*. It does not say *you have got it*. Players need the
second one to know they can move on — otherwise the only signal that a card is finished is running
out of patience with it.

### What counts as success

Not "the glyph moved" — noise moves the glyph. The player has to demonstrate **control of the axis**:

| Card | Earned by |
|------|-----------|
| sideways | taking it past the threshold **both left and right** |
| fore/aft | nose down **and** nose up |
| pump | one pump strong enough to actually launch the board |

Both directions, deliberately. One-directional would be satisfiable by drifting, and going both ways
is the thing that proves the player found the axis rather than stumbled onto it.

**Latched.** Once earned it stays earned, including if the player goes back to a card. Nothing about
this is a test to be re-passed.

### It never gates anything

**Next always works and never waits.** No auto-advance either: yanking the card away mid-gesture
would punish the player for succeeding, and the card is also the only place the instruction is
written. Success is a reward for having done it, not a toll on the way out.

A consequence worth accepting: held flat, the tilt cards cannot be earned at all, because the glyph
is suppressed and there is nothing to measure. That is fine precisely *because* nothing gates —
the player just doesn't collect the mark.

### Progress has to be visible before it is complete

First cut said nothing until the whole two-way gesture was done. On device that meant **tilting one
way correctly produced no acknowledgement at all**, so the player had no reason to believe the first
half had counted, and no way to tell a correct tilt from a wobble.

So the card reports each stage:

- **Direction markers** under the board — a chevron per direction, lit once that side has been
  reached. An unlit chevron still points, so it reads as an instruction rather than a dead dot.
- **The caption is the coach**, not a label: `LEAN IT BOTH WAYS` → `NOW THE OTHER WAY` → `GOT IT`.

### The layers, and what each one is for

1. **A flourish at the moment it lands** — the board flashes to the accent colour and pops in scale.
   Feels like an answer; forgotten a second later on its own.
2. **A nudge toward the exit** — Next pulses once. Without it, success landed and then nothing
   suggested what to do with it, which on device read as *"I must still have something left to do"*.
3. **A permanent mark on the progress dots** — earned steps read accent-coloured, so the row becomes
   a quiet score of what has actually been performed. Too quiet on its own to notice landing.

### Retry, and why the mark is on the dots and not the card

Returning to a card **clears its attempt** — markers, caption and flourish all reset — so the
gesture can always be performed again. A player who doesn't know what they did needs something to
try; a card frozen on `GOT IT` gives them nothing, and a board that still moves under a finished
label reads as unfinished business rather than as an achievement.

The permanent record lives on the **dots** instead, which is why they are the layer that never
resets. Practising again costs nothing and takes nothing away.

### Pump is earned by the thing you can see

Not by an internal speed reading — the first cut wanted `GlyphSpeed >= 1.10`, which real pumping
never reached, because pumps are short pulses and only the loading phase counts. The board would fly
and the card would still never be earned. It is now earned when **the board leaves the frame**,
with only a small speed floor so a slow creep to the wrap point doesn't count.

Which makes how fast it flies part of the *success* design, not just the look. Two corrections after
the first device run, where a launch took far too many pumps:

- **Acceleration is high (14 travel-units/s²).** A pump is a pulse, not a hold — only the loading
  phase registers, so one committed pump is maybe a third of a second of input. The gain has to be
  large for that to amount to a launch.
- **It wraps at 2.3, not 1.6.** The board spans x −1..1 and the widget clips at 1.15, so it is not
  actually *gone* until travel passes 2.15. At 1.6 its tail was still on screen and the wrap read as
  a teleport rather than as leaving — which undercuts the whole criterion, since "it left" was
  supposed to be the thing the player sees.

`surf.start.pumpspeed` scales the launch at runtime — one knob, because faster/slower is the only
axis this ever comes back on.

## Acceptance Criteria

### AC1: the ride is unchanged — MET (2026-08-21)
The extraction is behaviour-preserving. `surf-straight` compares OK against baseline, and a device
ride confirms the feel is identical. This is the criterion that mattered most — FR2 put shipping
control code under a refactor for the benefit of a menu.

Note what the desktop half could and could not show: `UpdateTiltWeight` is `#if PLATFORM_ANDROID`,
so no desktop run ever calls `ComputeWeight`. The snapshot proved no collateral damage; only the
device ride could prove the mapping.

### AC2: each glyph responds on the taught axis
On device: rocking the phone rolls the step-1 board; tipping it pitches the step-2 board; a pump
surges the step-3 board forward. Neither of the other two axes moves the glyph.

### AC3: the flat grip degrades visibly, not silently
Laid flat, the tilt glyph **fades out** rather than sitting inert, and the pump glyph keeps
responding. Coming back up brings it back without a jump.

### AC4: the deadzone is visible
A small wobble moves nothing; committing to the gesture moves the glyph. Both are legible.

### AC5: it fits at phone proportions
Checked at `Screenshot.ps1 -Phone` (1200x540). The card is **already tight** there — the current
three-line body sits close to the nav row — so the glyph cannot simply be a new row without the
illustration envelope giving up height. See Open questions.

## Open questions

1. **Where does the glyph go?** Being compared as layouts before any of this is built — see
   "Placement candidates".

The flat-grip fallback question is closed: not building it (see the hazard section). The ride's own
calibration is left exactly as it is.

## Placement candidates

At phone proportions the card has **width to spare and no height at all** — the body text already
sits close to the nav row. That inverts the obvious answer.

| | Placement | Costs | Verdict |
|---|---|---|---|
| A | New row under the body | Height, which there isn't | Rejected — everything squeezes and the glyph still ends up small |
| B | Inside the illustration card, beside the drawing | Widens the card | **Chosen** (2026-08-21) |
| C | Beside the card, in its own panel | A second panel | Rejected once B was drawn fairly — see below |
| D | Same slot as the illustration, swapping on motion | Nothing | Rejected — removes the instruction art exactly when the player is following it |

**B, decided from mockups at 1200x540.** The comparison turned on a mistake in the first round: B
was drawn with a small glyph and C with a large one, and that was an arbitrary choice, not a property
of either placement. The card can simply grow sideways into the same empty water C was going to use.

Given equal glyph size, B keeps everything C offered and adds:

- **A controlled ground.** A dark board on the card's white reads cleanly at any size. C's sits over
  moving water and needs a scrim panel to survive it — a panel to place, tune and fade.
- **One frame, so demo and response read as cause and effect**, and the eye never leaves the
  reading column to find the response.

What B obliges:

- **The drawing gets smaller** to make room. It is a simple sketch and survives it, but this is the
  real cost and it should be checked on the phone rather than assumed.
- **FR3's suppression has to fade the glyph half of a shared card** without disturbing the drawing
  beside it, where C would have faded one self-contained panel. Fade the glyph and its caption, keep
  the card and divider still.

## Related Files

- [start-screen-tutorial.md](start-screen-tutorial.md) — the cards this feeds, and the grip swap
  whose dip FR3 re-seeds on
- [tilt-yaw-fusion.md](tilt-yaw-fusion.md) — the fusion moving into the shared unit
- [sensor-probe.md](sensor-probe.md) — the measured device frame the FR3 fallback rests on
- [pumping.md](pumping.md) — the pump signal
- `SurfboardPawn.cpp` — `UpdateTiltWeight`, `AdvanceTiltYawFusion`, `UpdatePumpInput`, calibration
- `StartTutorialOverlay.cpp` — the cards, and the active timer that already reads motion

## Status

- [x] Extract the shared tilt/pump math (FR2), ride behaviour unchanged (AC1) — device-verified
- [x] Preview calibration, isolated from the ride's, re-seeded at the grip-swap trough (FR3)
- [x] The glyph, both views (FR1), exaggeration + deadzone (FR5)
- [x] Per-card axis filter — fore/aft and pump share the side-on board, so the view alone cannot
      decide which gesture a card responds to
- [x] ~~Loop yields to live motion~~ — withdrawn on device, see FR4
- [x] Conditioning gate + fade (the flat-grip decision)
- [x] **Device pass 1 (2026-08-21): directions correct** on all three axes — pitch, yaw and roll all
      move the glyph the right way. Three defects found and fixed, all recorded above: the demo never
      animated (FR4), the pump was indistinguishable from nothing happening, and the board outline
      read as a leaf rather than a surfboard.
- [x] **Device pass 2 (2026-08-21): all three instruction cards approved.** Redrawn outlines, the
      flying pump board and the always-on flip all pass.
- [x] The success beat (FR6) — detection, flourish, caption and dots, all three confirmed in capture
      with `surf.start.fakesweep` driving both directions
- [ ] **Device pass 3.** Open: whether the success threshold asks for a sensible amount of gesture
      (too low and it lands by accident, too high and it feels withheld); whether the flourish is
      long enough to notice and short enough not to nag; and, still from pass 1, whether the tilt
      exaggeration reads right and the conditioning gate fires at a sensible angle.

### Capture aids added for this

All exist because desktop cannot produce these states — see the `screenshot-game` skill.

| CVar | For |
|---|---|
| `surf.start.fakeresponse <-1..1>` | drive the glyph with no sensor |
| `surf.start.fakesweep <hz>` | sweep it back and forth — a *held* value only ever satisfies one direction, so without this the both-ways success condition is unreachable on desktop |
| `surf.start.step <n>` | open `-ShowInstructions` on a chosen card; the later ones need a tap |
| `surf.start.successflash <s>` | flourish length; stretch it to catch it in a capture |
| `surf.start.posefade <s>` | grip-swap dip length, same trick |

### Layout note

The first two attempts sized the card from the *available* height (once directly, once via an
`SScaleBox`) and both produced a card far smaller than the layout study — the body area's reported
height is not the generous space it appears to be on screen. Fixed internal sizes, centred, was the
answer. If the card needs resizing, change `ArtW` / `GlyphW` in `BuildStep`, not the envelope.
