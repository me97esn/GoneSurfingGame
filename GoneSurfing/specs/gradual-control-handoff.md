# Spec: Gradual control handoff — the first ride surfs with you

## Status

**HANDOFF ENTRY POINT.** Branch `gradual-control-to-user`, ~30 commits, **nothing pushed**. Built,
player-tested on device across several rounds, and not finished. Read "Where this stands" below
before touching anything.

- [x] Spec drafted 2026-08-25, both design decisions resolved (D1 ride time, D2 session-only now /
      persistent at release).
- [x] Phase 1 — assist controller: `SurfAssist.h/.cpp`, called from `ASurfboardPawn::Tick`.
- [x] Phase 2 — ride-time credit, fade schedule, FR9 storage seam (both modes written).
- [x] Phase 3 — badge (`AssistOverlay`), panel (`AssistPanel`, four modes), manual level.
- [x] Constants re-tuned from measurement, then again from player testing on device.
- [x] **Steering sign PROVEN 2026-08-28.** Ridden on device at `AssistSteerGain 4`: the guard carves
      the board back toward the band. `AssistSteerSign` stays **+1**. Note what this does and does
      not settle — see "Proving the steering direction" below.
- [ ] **Whether the SHIPPING authority is enough is still unproven.** The sign was demonstrated with
      the steering channel amplified ~4x into bang-bang. At `AssistSteerGain 1` the correction is a
      fraction of that, and whether it actually recovers a bad line at that strength is what
      `assist_guard_out_the_back` still has to answer.
- [ ] **Spec's own tests never written** — `assist_guard_out_the_back`, `assist_guard_into_the_flats`,
      `assist_neutral_midface`. See "Test cases".
- [ ] **At release:** flip `bPersistAssistCredit` (see FR9) and confirm credit survives a restart.

### Where this stands (2026-08-28)

Open items, roughly in the order they matter:

1. **`AssistMaxAuthority = 10` disables the clamp.** (Now the first thing worth doing: with the sign
   proven, authority is the remaining unknown on the steering channel.) Player-tuned and it feels right, but the player
   range is 0..1, so at 10 the clamp never binds and FR1's guarantee now rests on the PD gains rather
   than on a hard limit. **~0.4 should feel identical while restoring the backstop** — worth an A/B.
2. **The crest-scan fix moves player-validated physics.** `aaa4f2908` corrected a scan that chose its
   direction from ~0.02 cm of noise and went shoreward on 60% of ticks. `signedDistanceToCrest` also
   gates `PlaningRedirectCrestFade`, `CarveGripCrestFade` and `lipImpact`, so the "crest fades
   600/600" tuning was validated against the broken scan and may want revisiting.
   `CrestScanRobust = 0` restores the old scan exactly for an A/B.
3. **Success sound is wired but unassigned.** `ASurfboardPawn::AssistLevelDownSound`; the project has
   no celebratory audio. Plays as a UI sound so the card's pause does not silence it.
4. **Copy decision outstanding:** the level-down card no longer says "You've surfed N seconds on your
   own" (cut in `238b91d1a` to match the player's shorter wording). That line was the evidence behind
   the praise — decide whether the card is poorer without it.
5. **Confetti density unverified.** 130 pieces, never actually seen by the author: it fades ~4 s after
   the panel opens and every screenshot lands later than that.
6. **Snapshot baselines are stale** (pre-existing, not caused by this work) — see
   [[stale-baselines-block-regression-sweep]]. A REGRESSION verdict currently proves nothing.
7. `bShowTuningHUD` is still true — see [[tuning-hud-temporarily-enabled]].

### Gotchas this work paid for

Every one of these cost at least one wrong diagnosis. They are why the code has the shape it has.

- **Viewport Slate needs `FInputModeGameAndUI`.** Under `GameOnly` the viewport keeps mouse/touch
  capture and Slate widgets get nothing — the badge was visible but dead. Worse, the existing
  condition keyed on `bShowTuningHUD`, a dev flag reverted before release, so it would have shipped
  broken and only in the release build.
- **A full-screen Slate root must be `SelfHitTestInvisible`.** `SurfTuningHUD`'s root `SOverlay` was
  hit-testable across the whole screen at ZOrder 300 and swallowed every click below it, while its
  own buttons kept working — invisible from the inside.
- **Overlays run through a 2x `SDPIScaler`.** A font size is that many *logical* pixels in a 540-high
  space. "Font 11" was a third of what it needed to be; a card sized at desktop proportions pushed its
  own close button off the bottom of the phone.
- **The start screen pauses the world, so the pawn does not tick.** Any Tick-side watch for it
  closing can never fire.
- **`RestartLevel` deliberately skips the start screen**, so anything hung off it misses the restart
  path — which is the path that matters after earning a level drop.
- **`waveHeightAndNormal`'s return is not world Z** (~20 where the board rides at ~280). Every
  existing caller uses it relatively, so it had never mattered. See
  [[waveheight-return-not-world-z]].
- **`SDPG_Foreground` ignores `LifeTime`** and is flushed every frame, so foreground debug lines
  cannot survive a pause however long a lifetime is asked for.
- **C++ needs a repackage to reach the phone; tuning values change live.** Several rounds were spent
  discussing a fix the device could not have been running.

### How to work on this

- Anything under `Tuning|Assist` is live-editable in the tuning HUD, on device included.
  `AssistDisable = 1` takes the assist out of the loop before tuning anything else — credit is
  session-only, so every playtest otherwise opens at alpha 1.
- `AssistDrawBand = 1` draws the guard band on the wave face, coloured by the same
  `SurfAssist::BandError` the controller gates on.
- **`AssistSkipFirstPlayCardInPIE = 1` stops the "A helping hand" card pausing every PIE session.**
  The card blocks the world until dismissed, which on an editor that gets restarted many times a day
  is a click between every test run. It lives in the tuning subsystem rather than behind a CVar for
  one reason: `Saved/TuningOverrides.json` survives an editor restart, so it is set once and stays
  set — a CVar would have to be retyped each time the editor came back, which is the problem rather
  than the fix. **PIE only**, verified: a standalone `-game` run with the flag on still opens the
  card, so screenshots and headless runs cannot quietly be testing a different first experience.
- Console: `SurfAssistPanel` / `SurfAssistFirstPlay` / `SurfAssistLevelDown` open each card directly;
  `SurfAssistReset` re-arms a first run. In PIE use the **viewport** console, not the Output Log box.
  `surf.assist.credit 95` lands anywhere on the fade schedule without riding there.
- `surf.debug.flags 'assist'` logs the controller per tick, the crest scan, and why a card did or did
  not appear.
- **Measure before theorising.** Most of the wrong turns above came from reasoning about the code
  when a log line or a one-value A/B on device would have settled it in a minute.

### Proving the steering direction

The guard's sign is the oldest open item and it stayed open because the shipping controller is, by
design, hard to observe: the correction is small (FR1), it only speaks at the band edges (FR2), and
from the deck *"the assist caught me"* and *"the wave caught me"* look identical. Turning the knobs
that already existed did not fix that — `AssistMaxAuthority` is 10 and does not bind, so there was
nothing to turn up.

`AssistSteerGain` / `AssistTrimGain` (default **1.0** = the shipping controller, bit for bit) are
plain multipliers on each channel's correction, and they **scale the `AssistMaxAuthority` clamp with
them** so the amplified value is not silently capped. That deliberately suspends FR1's "assist can
never overpower a committed input" guarantee: they are a measuring instrument, not a feel knob.

Past a gain of ~2 the channel is **bang-bang**. Steering peaks around 0.3 of correction and 0.5 is
full deflection from centre, so the correction swamps the whole 0..1 weight range: while the guard is
speaking the board is pinned to full lean and player input cannot fight it. That is what makes the
answer unmissable, and it is why no separate "ignore player input" switch was needed — the player
still has the board to themselves *inside* the band, so they can steer into a bad line on purpose,
and the assist takes it over the instant they cross the edge.

The recipe, all live-editable in the tuning HUD (the slider spans 0..4× default, which for these is
already past saturation — no phone keyboard needed):

| Knob | Value | Why |
|------|-------|-----|
| `AssistSteerGain` | 4 (slider hard right) | steering goes bang-bang |
| `AssistTrimGain` | 0 | mute trim, or an amplified trim term nose-dives the ride before the steering question gets an answer |
| `AssistAlphaForce` | 1 | pin full authority rather than riding to it — credit is session-only |
| `AssistDrawBand` | 1 | so "wrong direction" and "wrong band" cannot be mistaken for each other |

Then ride out the back on purpose and watch what happens as the band edge is crossed:

- **carves hard back down the line** — sign is right. FR2's headline claim finally has a witness.
- **drives itself further out the back and off the wave within a second or two** — sign is wrong.
  Set `AssistSteerSign = -1` and repeat; the board should now recover.

**Result, 2026-08-28: the sign is correct.** Ridden on device at `AssistSteerGain 4`; the guard
carves back toward the band. `AssistSteerSign` stays +1 and the derivation in `EvaluateAssist` —
`sign(dot(boardLeft, faceDown))`, held through the degenerate straight-down-the-face pose — is
vindicated.

**Be precise about what that proved.** It proved the *direction*. It did not prove the *strength*:
the demonstration ran with the channel amplified into bang-bang, where the correction swamps the
whole weight range. At gain 1 the guard emits a fraction of that, and "does it recover a bad line at
shipping authority" is a different question with a different answer. That one still belongs to
`assist_guard_out_the_back`. Do not let a proven sign be mistaken for a proven guard.

Trim direction is a separate test on the same rig (`AssistSteerGain 0` / `AssistTrimGain 4`) and has
not been run.

Repeat with `AssistSteerGain = 0` / `AssistTrimGain = 4` for the trim channel, whose failure is a
nose-dive or a stall rather than a lost line.

**Put both gains back to 1 before any feel judgement.** Nothing about how the assist *should* feel
can be read off a bang-bang run — it only answers which way the correction points.
`surf.debug.flags 'assist'` now logs `gain=steer/trim` on every line, so an amplified capture cannot
later be misread as the shipping controller.

## Overview

A first-time player gets one wave to decide whether this game is worth a second wave. Right now
that wave begins with a **hard cut**: the intro plays on rails, and at `SurfboardPawn.cpp:2238`
`bPlayerControlsEnabled` flips false→true in a single frame. The weight write at
`SurfboardPawn.cpp:962` goes from 100% autopilot to 100% player between two ticks.

`ControlDelayAfterAutoPilot` looks like it softens this, but it does not: during that window the
board is *neither* assisted *nor* controlled. It is a dead pause, and it ends by handing a novice
full authority over a board that (see `specs/hard-carve-progressive-boost.md`,
`specs/weight-shift-control-responsiveness.md`) turns hard on a small weight shift.

This spec replaces the cut with a **fade**: an assist that has real authority on the first wave and
progressively less on each subsequent one, until the player is flying the board the game ships with.

## Objective

Make waves 1-3 survivable and legible for someone who has never surfed a videogame board, **without
teaching them a control scheme that later stops working**. Two failure modes to avoid, both worse
than the current hard cut:

1. **Assist that steers for you.** The player learns "my input does nothing much, the board handles
   it", then at α=0 the board stops handling it. Everything they learned was about the wrong system.
2. **Assist that is invisible.** The game silently gets harder each ride. The player reads that as
   *"I am getting worse"* — the most demoralising possible outcome, and the exact thing this feature
   exists to prevent.

## Why the intro autopilot cannot be the thing that fades

The obvious implementation — keep `AStateTriggerAutoPilot` running past handoff and cross-fade its
weight commands out — does not work, and this is the central constraint on the design.

`FStateTriggerStep` (`StateTriggerAutoPilot.h:104`) is **open loop**: each step is a fixed
`weightRight`/`weightNose` pair plus trigger conditions for advancing. It is a recording of what one
good ride needed, not a policy. The moment the player perturbs the board off that trajectory the
canned values are wrong for the state the board is actually in, and blending them in **fights** the
player rather than helping. The rails intro is worse still — it is kinematic playback with forces
suppressed (`specs/deterministic-ride-handoff.md`), so there is nothing to blend into at all.

The thing that fades must therefore be a **closed-loop controller** that reads live wave-relative
state every tick and emits a *correction*. Such a controller degrades gracefully: at any α, from any
board state, its output is still the right direction to push.

## Requirements

### FR1 — Additive correction, never a lerp toward an autopilot target

```
finalWeight = clamp( playerWeight + alpha * correction, 0, 1 )
```

**Not** `lerp(assistWeight, playerWeight, alpha)`. The additive form guarantees that at *every* α the
player's input still moves the board in the direction they commanded — assist only biases the
result. The lerp form means that at α=1 the player's input does nothing, which is failure mode 1.

`correction` is clamped to `AssistMaxAuthority` (first guess **±0.15** in weight units, i.e. ±15% of
the full 0..1 range). Small, deliberately: carve gain is `lateralTurnCoefficient` × speed and a shift
of 0.1 is already a real turn (`memory: small weight shift → sharp turn`). The clamp is a hard
guarantee that assist can never overpower a committed player input.

### FR2 — Steering assist is a **guard band**, not a driver

The lateral channel (`amountToTheRight`) must be **deadbanded**: exactly zero correction while the
board is inside the rideable band, ramping up only as it leaves.

This is what resolves the tension between "steering is the fun part, give it to them immediately"
and "steering is what kills them". Their carve is never damped or corrected mid-face. The assist
only catches them at the two edges where a beginner's ride actually ends:

- **out the back** — drifting behind the crest and losing the wave
- **into the flats** — sliding too far down the face, stalling, wave rolls past

The band is defined on `ASharedCalculations::signedDistanceToCrest` (cm; positive = behind the crest,
negative = on the front face — `SharedCalculations.h:293`). Inside `[-BandFar, -BandNear]`: no
correction. Outside: correction ramps in over `BandSoftness` cm so there is no step at the edge.

**The deadband sits on the OUTPUT of the cascade (FR3), not on the outer loop's setpoint.** That is
the difference between a guard and a heading-hold controller, and it is easy to get wrong: with a
zero band error the outer loop naturally asks for "heading 0", and an inner loop acting on that
request holds the board pointed straight down the line through every carve the player attempts.
Inside the band the whole cascade is skipped, the steering correction is literally `0.0f`, and the D
term's state is cleared so it cannot kick on re-entry. Measured 2026-08-25: the first implementation
had this wrong and emitted ±0.03 of correction on supposedly-silent ticks.

One exception, and it belongs to stamina, not to this controller: while TIRED on a board with
`StaminaTiredGuardEverywhere` (the foamie), the controller runs on a band collapsed onto the crest
(`FTuning::bGuardEverywhere`), so the reversed guard pushes down the face from everywhere on it.
The authored band is still what the score reads (`FOutput::bOutsideBand`) and what `AssistDrawBand`
draws. See `stamina.md` (Status, 2026-09-22).

Because the guard only speaks at the edges, it reads to the player as *the wave keeping them on it* —
which is both truthful and the correct mental model.

### FR3 — Steering correction is a cascade (position → heading → weight), not position → weight

Position error must set a **target heading** relative to down-the-line; heading error then drives the
weight correction. Driving roll directly off position error is a classic under-damped line-follower,
and on a board this roll-stiff (`memory: surface-carve roll spring`) it will oscillate.

All axes derived at runtime from board and wave vectors. **Never hardcode board axes — the mesh is
rotated 90° and `board.forwards` is local +Y** (CLAUDE.md). Down-the-line is +Y in this project's
wave frame and the wave breaks toward −X (`memory: wave breaking direction`), but read it from
`ASharedCalculations::resolvedWaveBackDirection` / `waveNormal`, not from constants.

### FR4 — Trim assist is continuous, and fades on its own schedule

Fore/aft (`amountInFront`) gets a small continuous correction toward a trim target scheduled on speed
and `boardWideSlopeSin`. Unlike steering, trim has **no deadband**: fore/aft trim is invisible to a
beginner, they have no intuition for it, and getting it wrong produces a nose-dive
(`specs/nose-dive-bug.md`) or a stall — both of which end the ride with no legible cause.

Trim assist may fade faster than steering assist; it is the channel the player is least likely to
notice losing.

### FR5 — Rate-limit the player's STEERING while α is high; do **not** reduce their gain

A novice's phone tilt is jerky. The mitigation is a rate limit (max weight change per second) that
relaxes as α falls — **not** a reduced input gain.

Reduced gain makes the board feel dead, which is its own quit-in-30-seconds failure, and it lies
about how responsive the board will be later. A rate limit preserves full authority and full travel;
it only refuses to get there in one frame. Framerate-independent
(`specs/framerate-independent-angular-damping.md`).

**Steering only. The fore/aft axis is not rate-limited at all.** Pumping lives there and a pump is
fast by definition: at 1.5 units/s the limiter allows 0.375 units per 250 ms half-stroke, shaving the
stroke going out *and* coming back. On device it removed the pitch oscillation from pumping entirely
(2026-08-26 — confirmed by lifting this one value, after `TrimP = 0` had ruled the trim term out).

Gating it on a pump-detection threshold was the first fix and is the wrong shape: it leaves a cliff
where a gentle pump is still eaten, and it makes a core mechanic depend on a magic number. Nothing is
lost by dropping the limit outright — the jerky-input problem FR5 exists for is a *steering* problem
(this board over-carves on a small lateral shift), and the fore/aft failure modes are what the
continuous trim term and `MaxAuthority` already cover.

### FR6 — α is a function of accumulated *unassisted ride time*, held constant within a ride

The authority ceiling α is driven by **how many seconds the player has spent actually surfing**, not
by how many waves they have started. Ride count charges a player who wipes out instantly three times
in a row for three rides' worth of graduation while they have learned nothing, and under-credits the
player who rode one wave for thirty seconds — which is the clearest evidence of competence the game
can get. (Decided 2026-08-25; supersedes the ride-count schedule this spec was drafted with.)

Ride time keeps the two properties that made ride count preferable to a competence score: it is
**monotone** (seconds only accumulate, so the assist can never come back) and it is **explainable in
one sentence** ("you have surfed two minutes").

#### Credit accrues faster when the guard is silent

At α=1 the guard band is part of what is keeping the player up, so raw seconds would credit the
player for time the *assist* earned. Weight the accrual by what the steering guard (FR2) is doing —
a quantity the controller already computes, so this costs nothing:

| Board state while riding | Credit rate |
|--------------------------|-------------|
| Inside the band, steering correction exactly 0 — holding the line unaided | 1.0× |
| Guard actively correcting — being shepherded | 0.25× |

Full credit is then literally "seconds spent surfing unaided". The 0.25× rather than 0× is a
deliberate backstop: a player who never quite holds it alone still graduates eventually, four times
slower, rather than being pinned at α=1 forever.

Trim assist (FR4) is continuous and so cannot gate anything — credit keys on the **steering** guard
only.

#### Schedule

Data-driven on `USurfTuningSubsystem` so it can be A/B'd through `Saved/TuningOverrides.json` with no
recompile (`memory: tuning-override JSON A/B`):

| Accumulated credit | α |
|--------------------|---|
| 0–30 s   | 1.0        |
| 30–90 s  | 1.0 → 0.4  |
| 90–180 s | 0.4 → 0.0  |
| 180 s+   | 0.0        |

#### Constant within a ride

Credit accrues continuously, so α *could* fall mid-ride. It must not. α is evaluated once at handoff
and held for the whole ride.

A player halfway through their first good forty-second wave must not have the board change feel under
them at second thirty — that is a fall they cannot attribute to anything they did, which is failure
mode 2 from the Objective in miniature. The within-ride variation they need is already supplied by
the guard band engaging and disengaging as they wander, and a ride with one α rather than a ramp is
far easier to test.

What accrues during a ride is credit; what changes between rides is α.

**This latch may be worth reopening once `specs/ride-score-counter.md` lands.** Its rationale above
rests on the change being *invisible* — unattributable, so the player blames themselves. A live
score counter crossing a visible threshold, with the badge stepping down at the same instant, removes
that assumption: the player watches themselves earn it. It would also make the `LevelDown` card
redundant, since nothing would be left to explain on the next ride. See D5 of that spec for what
still argues against it (reward timing, and a moving α inside every trace comparison) and for the
cheaper middle ground of showing the *earned* level on the badge while the board stays latched.
Decide it on a ride, not on this paragraph.

#### What counts as ride time

Time from control handoff to ride end (fall, wave lost, or level restart). Excludes the intro and
rails playback, excludes anything before handoff, excludes paused world (start screen, tutorial), and
excludes every run gated off by FR8.

### FR7 — The assist is named, shown, and switchable

Non-negotiable, and the requirement most likely to be dropped for time. It must not be.

- The player is told, once, that the first few waves are guided. **On the start screen, under the
  buttons — not in the ride.** Both options were on the table here and the handoff cue was tried
  first; on device (2026-08-26) it landed on top of the "...and surf!" cue, two long lines at the one
  moment the player should be watching the wave rather than reading. Pre-wave, reading costs nothing,
  and the line is silent for a mid-schedule player who already knows.
- While α > 0 there is a persistent indicator that assist is active, with a strength bar that visibly
  empties ride over ride. **It is up through the intro** — paddle, cobra and pop-up — not just once
  the player has the board. That is dead time with no control and nothing to do but look around,
  which is when they actually read it (player observation, 2026-08-26); by handoff they already know
  what it means, so the "...and surf!" moment stays free of anything to read. Pre-handoff it shows
  the strength the ride is about to start with, which is what `BeginAssistRide` then latches.

  This is the one part of the FR8 gate the badge does not share. `IsAssistEnabledThisSession()`
  covers mode, the kill switch and the automated-run gates; `IsAssistSuppressed()` adds the
  pre-handoff / rails / fallen checks that hold the *controller* back. Hidden behind the start
  screen, which is a modal. Enough that when it goes away, the player attributes the change to the game
  and not to themselves.

  **Quiet is not the same as invisible.** The first sizing (font 11, ink alpha 0.62) could not be read
  on a phone without taking a screenshot and zooming in. The overlays run through a 2x `SDPIScaler`,
  so a font size is that many *logical* pixels in a 540-high space, sitting next to UMG buttons whose
  text is ~27 — a "small" number here is far smaller than it looks in the source. Sized to sit just
  under those buttons.
- **An assist panel**, reached by tapping the badge (which pauses the ride) and shown automatically
  at two moments. `AssistPanel.h/.cpp`, one widget in three modes:
  - **FirstPlay** — on the way out of the start screen, before the first wave. Explains in plain
    language what assist does and, as importantly, what it does *not* do ("it never steers for you").
  - ~~**LevelDown**~~ and ~~**Graduated**~~ — **both removed, 2026-08-28/29.** The ride score counter
    celebrates a step at the moment it is EARNED, with the badge draining on the same frame, so a
    card next ride could only re-announce something the player had watched happen — in a dialog
    blocking the wave they wanted to surf. FR7's "told before the wave" is still met, and met
    earlier: at the crossing, a full ride before the board changes. The success sound moved to the
    celebration; the Slate confetti went with the cards (it is in git). See
    `specs/ride-score-counter.md` FR5. Confetti, the total time surfed unaided, and a plain
    statement of what changes. This is the moment that stops the fade reading as "the game got harder
    for no reason", which is failure mode 2 in the Objective. Slate confetti, not Niagara: the panel
    pauses the world and no particle system simulates while paused
    ([[niagara-frozen-while-paused]]).
  - **Settings** — the badge tap.
- A manual level (Off / Light / Medium / Strong / Full, or Auto). **A hand-picked level PINS α and
  stops the fade** until the player chooses Auto again — once they have made a choice the board must
  stop changing under them, which is the whole point of offering it. Stored through the same FR9 seam
  as the credit, so it persists on exactly the same schedule.

Device testing (2026-08-27) is what forced this: a status word and a strength bar explain nothing on
their own. A player who has not read the code cannot tell what "ASSIST" is doing to their board, and
has no way to want more or less of it.
- **Reset assist** — puts accumulated credit back to zero. Cheap to add next to the toggle, and it is
  the control the author actually needs while passing the phone between testers: it re-arms a fresh
  first-run without force-quitting the app. Keep it once credit persists (FR9), where it becomes the
  only way back to a first-run state.

### FR8 — Inert in every automated run

The assist changes the weight written to `AWeightDistribution` and would therefore shift every
snapshot baseline and corrupt every trace-replay comparison. It must be fully inert when **any** of:

- `FApp::IsUnattended()`
- `bExternalWeightOverride` (trace replay)
- `SurfDebug::CVarAutopilots` non-empty (filtered test run)
- `bPlayerControlsEnabled == false` (pre-handoff — the intro autopilot owns weight then)
- the rails intro is driving (`SurfRails::AreForcesSuppressed()`)

Same gate shape as `UpdateFallDetection` (`SurfboardPawn.cpp:469`) — reuse it, do not re-derive it.
`surf.assist.force 1` re-enables the assist inside a filtered run, for the tests below only.

### FR9 — Credit storage is one seam, and both storage modes ship from day one

Credit is **session-only for now** (D2, decided 2026-08-25): it resets on app launch, because the
phone gets handed to a fresh tester and every tester must start at α=1.0. At release it will need to
persist. Build for that switch now rather than leaving a TODO.

Every read and write of accumulated credit goes through exactly two functions:

```cpp
namespace SurfAssist
{
    float LoadCredit();          // 0.0 on a fresh session, or the persisted value
    void  SaveCredit(float);     // no-op in session mode, GConfig write in persistent mode
}
```

Nothing else in the codebase touches the stored value. Both implementations exist from the start,
selected by a single config bool (`bPersistAssistCredit`, default **false**), so shipping means
flipping a flag whose other branch has already been exercised — not writing new code under release
pressure. `GConfig` + `GameUserSettings.ini` is enough; this feature must not drag in a save system
(D2 option (c)).

The same seam carries the FR7 assist-off toggle, which needs persistence on exactly the same
schedule.

**Consequence for the author's own testing:** session-only means every playtest starts at α=1.0 on
an assisted board, which will quietly mislead any physics tuning done in those sessions. `surf.assist.alpha 0`
and the FR7 toggle are the escape hatches — use one of them whenever the session is for tuning rather
than for a first-run test.

### NFR1 — Cost and determinism

Two PD controllers and a clamp per tick; no allocation, no scans (`signedDistanceToCrest` is already
computed by `ASharedCalculations`). All smoothing DeltaTime-correct, so a 30 fps phone and a 120 fps
desktop behave identically.

### NFR2 — Nothing new in the physics layer

The assist writes only `AWeightDistribution::amountToTheRight` / `amountInFront` — the same two
scalars the player writes. It adds **no forces**, touches no coefficient, and is not a second
propulsion path. If the assist can achieve something the player cannot, the assist is wrong.

## Decisions

### D1 — What drives the fade — **RESOLVED 2026-08-25: accumulated ride time**

Ruled on by the project owner. The two options this spec was drafted against were **ride count** and
**competence score**; the third option, **ride time**, beats both and is now FR6.

Ride count is unfair in the case that matters most: a player who wipes out instantly three times
running has learned nothing, but is charged for three rides and handed a harder board. Ride time
charges them for what they actually did — a few seconds. Symmetrically, one thirty-second wave is the
strongest competence evidence available, and ride count under-credits it as "one".

Ride time also keeps what made ride count preferable to a competence score: it is monotone (so the
assist can never return) and it is legible in one sentence. It *is* a competence measure — survival
duration — but the crudest one available, and that crudeness is the point.

The refinement that makes it hold up is in FR6: credit accrues at 1.0× only while the steering guard
is silent, and 0.25× while the guard is doing the work. Without it the player graduates on seconds
the assist earned for them.

### D2 — Does assist survive the first session — **RESOLVED 2026-08-25: (a) now, (b) at release**

Ruled on by the project owner. **Session-only for now**, and the reason is the one that outranks the
returning-player argument: the phone is handed to fresh testers, and every tester must get a genuine
first run. Persisted credit would mean the second tester of the day inherits the first tester's
graduation and never sees the feature being tested.

That reason expires at release, when the phone stops being a test rig and starts being one player's
device. **(b) `GConfig` is the release target** — one float in `GameUserSettings.ini`, no asset, no
serialisation versioning, and no `USaveGame` dependency (option (c) stays rejected: this feature must
not be the thing that drags in a save system).

FR9 is how the spec carries that: both storage modes are written now and selected by one config bool
defaulting to session-only, so releasing is a flag flip rather than a code change. The FR7 **Reset
assist** control exists partly for the same testing workflow — it re-arms a first run without a
force-quit, and it stays useful once credit persists.

## Calibration, measured 2026-08-25

The constants this spec was drafted with were guesses. Headless run on `Boards_on_flat_water` with
`surf.assist.force 1` and `surf.debug.flags 'assist'`, ~35 s of riding. What it showed:

### The board rides far closer to the crest than assumed

`signedDistanceToCrest` during a normal ride spans **−450 to −50 cm**, clustered −250 to −50, and is
quantised in 50 cm steps (the wave-height grid resolution). The drafted band of `[-1200, -250]` was
therefore wrong at both ends: `BandFar` could never trigger, and `BandNear` classified an ordinary
ride as "out the back" on **100% of ticks** — which also meant credit accrued at 0.25× permanently, so
nobody would ever have graduated.

Retuned: `BandNear` 250 → **40**, `BandFar` 1200 → **500**, `BandSoftness` 400 → **150**. The guard
now sits silent ~74% of a normal ride.

The 50 cm quantisation is worth remembering — the position loop has roughly 9 distinct values to work
with across the whole rideable strip, so there is no point tuning the band to finer than that.

### The D term was bang-bang, not damping

At `HeadingD` 0.15 on a raw derivative, half of all samples sat at ±`MaxAuthority` with a median of
~0 — the correction was flipping sign every tick, not damping anything. Heading is noisy on a board
this roll-stiff, and differentiating noise amplifies it.

Fixed with a low-pass on the rate (`HeadingRateSmoothingSeconds` = 0.15, time-constant form so it is
framerate-independent) and `HeadingD` 0.15 → **0.03**. Steer correction spread went from
`p25 −0.042 / p75 +0.140` to `p25 −0.032 / p75 +0.017`.

### Trim was biased near its clamp

A riding `boardWideSlopeSin` never exceeds ~0.31, and `TrimSlopeGain` 0.35 put the trim target within a
whisker of `TrimTargetMin` for an ordinary ride. Trim is continuous, so that bias applies the whole
time. Halved to **0.20**.

### The guard leaked (bug, fixed)

See FR2. Silent-guard ticks were emitting ±0.03 of steering correction, because the deadband was on
the outer loop's setpoint rather than the cascade's output. Post-fix, every `guard=silent` tick emits
exactly `0.000` — which is the first acceptance criterion, and it now holds by construction rather
than by tuning.

### Seeing the band: `AssistDrawBand`

Playtesting the guard has a diagnosis problem: from the deck, "the band is in the wrong place" and
"the controller is misbehaving" look identical. So the band draws itself.

`AssistDrawBand` in the tuning HUD (the only switch reachable on a phone — no console there) draws
the two edges of the no-assist band as lines on the wave face, running down-the-line, with the board
between them. **Green = the band is holding. Red = the edge the board has breached**, coloured per
edge so it is visible *which* way the ride is going wrong. A short vertical tick marks the board.

Two rules keep the picture honest, and both were mistakes made first:

- The colouring calls `SurfAssist::BandError`, the same free function the controller gates on — not a
  reimplementation of it. A drawn band that disagreed with the enforced band would send you hunting
  for a controller bug that was really a geometry bug, or the reverse.
- Only the ONE SharedCalculations the assist reads draws it (`bAssistBandReference`, set by the
  pawn). A board has a front and a back SC, each hill-climbing its own crest; drawing from both puts
  two overlays at two slightly different estimates on screen, which reads as a bug in the band.

Drawn at the wave-data height minus 40 cm (the display meshes sit that far below the data — the same
bias the spray waterline uses) plus 20 cm back up, in `SDPG_Foreground`. Exactly on the mesh it is
z-fought and half-swallowed; in the foreground group it stays readable when the board is behind the
face, which is precisely when it matters.

**Persistence, and why it is not simply a lifetime.** The band has to survive a pause — that is when
a screenshot gets taken. Three engine facts constrain this, all learned the hard way:

1. `SDPG_Foreground` routes to `UWorld::ForegroundLineBatcher`, which **short-circuits the `LifeTime`
   argument entirely** (`DrawDebugHelpers.cpp` `GetDebugLineBatcher`), and that batcher is `Flush()`ed
   every rendered frame in `UGameViewportClient::Draw`. Foreground debug lines live exactly one frame
   no matter what lifetime is asked for. A first attempt spent three rounds "tuning" a lifetime that
   was being ignored.
2. `SDPG_World` with `LifeTime > 0` goes to `PersistentLineBatcher`, whose lines expire by DeltaTime
   in `TickComponent` — so they survive a pause. Cost: they are depth-tested, hence the lift above
   the surface.
3. But redrawing every frame with a lifetime stacks copies — ~18 at 0.3s/60fps — and because the
   board moves they *fan* rather than overlap. With any line thickness they merge into screen-filling
   wedges.

So the redraw **cadence is tied to the lifetime**: redraw only as the previous copy is about to
expire (`lifetime * 0.8`). At most two copies exist, the band stays crisp, and winding
`AssistDrawBandSeconds` up for a screenshot buys persistence *without* multiplying copies, because
the cadence stretches with it. Cost: the band updates at ~1/lifetime Hz and lags the board by up to
one lifetime, so keep it short while riding.

**The edges are drawn at the BOARD's Z, not at a sampled wave height.** `waveHeightAndNormal`'s
return value is not directly comparable to world Z: measured 2026-08-26, it reads ~20 at the edge
centres while the board is riding at Z~280. Lines placed at the sampled height sat ~2.8 m under the
water and were depth-tested away — invisible, paused or not, while the board marker (drawn from the
actor's own transform) rendered fine. The crest scan above never caught this because it only
*compares* samples to find a peak; this drawing was the first consumer of the absolute value.

Board height is also the honest choice: the band is a horizontal-plane concept, the edges sit at most
a few metres from the board, and that is where the eye already is.

Known gap: the `d=` text readout uses `DrawDebugString` and did not appear in a `-game` capture. The
line colours carry the signal — treat the text as an editor-only extra. `surf.debug.flags 'assist'`
also logs the drawn geometry (`AssistBandDraw`), which is what found the Z bug.

### The crest scan was picking the wrong wave (fixed 2026-08-26, needs a ride test)

Reported from play: the band starts correct, then jumps somewhere far from the wave. Two conditions
co-occurred — crossing a GridLODActor seam, and leftover whitewater shoreward while the wave being
ridden had not yet broken.

Measured over a 12k-sample run, the cause is neither, exactly. `ASharedCalculations`' crest scan was
a **hill-climb that commits to a direction on one +/-50cm comparison** and then walks that way until
height stops rising, up to 30 m. Two profiles from the same run:

```
GOOD  dir=+1  d=-50    h0=17.5 hMinus=16.5 hPlus=20.0   | ... 0:18  300:20  600:19 ...
BAD   dir=-1  d=1000   h0=15.5 hMinus=15.5 hPlus=15.5   | ... 0:15  300:20  600:19 ...
```

Same height profile, opposite answer. In the bad case the three seed samples agree to within ~0.02cm
— a locally flat trough, which is exactly what a seam crossing or a between-waves position produces
— so **numerical noise chose the direction**. It then walked 10 m shoreward to a whitewater bump of
height 17.5 while the real face sat at height 20, 300 cm seaward. It set off shoreward on **60% of
all ticks**.

Fixed by sweeping symmetrically outward and taking the nearest *significant* peak: no noisy direction
commitment, and a 0.5 cm improvement floor so a marginally-higher ripple 10 m away cannot steal the
crest. Range bounded to 1200 cm (was 3000) — a crest that far off is not the wave being ridden.
Shoreward picks fell from 7224 to 4083 of ~12k.

**This is not assist-only, and it changes ride feel.** `signedDistanceToCrest` also gates
`PlaningRedirectCrestFade`, `CarveGripCrestFade` (`SurfboardUtils`) and the `lipImpact` crest gate
(`FluidDynamics`). Those read `crestDistFront = -signedDistanceToCrest`, so a spurious `d = +1000`
made `SmoothStep(0, 600, -1000)` = 0 and slammed the gates shut. The player-validated "crest fades
600/600" was therefore tuned against a scan that reported *behind the crest* 60% of the time.
`CrestScanRobust = 0` restores the legacy scan exactly, for a one-value A/B.

Headless runs cannot settle this: most of a headless ride is spent drifting on near-flat water where
there is no crest to find. It needs a real ride.

### Still open

- `BandFar` = 500 is under-observed: the measured ride never went deeper than −450, so the "into the
  flats" edge has not actually been exercised. `assist_guard_into_the_flats` is the test that will
  settle it.
- The steer correction still reaches the ±0.15 clamp during genuine edge events. That is the guard
  working at its authority ceiling, which is intended, but whether it is *enough* authority to
  actually recover a bad line is exactly what `assist_guard_out_the_back` has to prove.
- None of this has been ridden by a human. Every number above is from a board with no player input.

## Acceptance criteria

```
GIVEN a first ride with assist at alpha = 1.0
WHEN  the player holds a steady line mid-face inside the guard band
THEN  the assist correction is exactly 0.0 on both axes
AND   the board's response to their input is identical to an unassisted ride
```
```
GIVEN a first ride with assist at alpha = 1.0
WHEN  the player steers steadily toward the back of the wave
THEN  the correction ramps in as signedDistanceToCrest crosses -BandNear
AND   the board settles back inside the band without crossing the crest
AND   the correction never exceeds AssistMaxAuthority
```
```
GIVEN a ride with assist at alpha = 1.0
WHEN  the player commits a full-deflection turn and holds it
THEN  the board still completes the turn
      (assist biases the line; it must never veto a committed input)
```
```
GIVEN alpha = 0.0 (graduated on credit, or the settings toggle set to off)
WHEN  the same input is replayed
THEN  the resulting trajectory matches an assist-free build within snapshot-test thresholds
```
```
GIVEN any headless run (unattended, filtered autopilot, or trace replay)
WHEN  the ride executes
THEN  the assist writes nothing, and every existing baseline compares clean
```
```
GIVEN a player who has just graduated to alpha = 0.0
WHEN  they start that ride
THEN  they were told the assist has ended — before the wave, not after they fell
```
Met since 2026-08-29 by the in-ride celebration rather than by a card: the player is told at the
moment they earn the step, one ride before the board changes. Note what this criterion does NOT
say — that they cannot miss it. A modal could not be missed; a two-second celebration can be.
```
GIVEN a ride in which the steering guard is correcting for the whole duration
WHEN  the ride ends after N seconds
THEN  accumulated credit rose by 0.25 * N, not N
```
```
GIVEN a ride long enough that accumulated credit crosses a schedule threshold
WHEN  it crosses mid-ride
THEN  alpha does not change until the next handoff
```
```
GIVEN a player who wipes out three times within two seconds of each handoff
WHEN  they start their fourth ride
THEN  alpha is still ~1.0
      (this is the case ride count got wrong and is why D1 was reopened)
```
```
GIVEN bPersistAssistCredit = false (today's default)
WHEN  the app is restarted
THEN  accumulated credit is 0 and the next ride runs at alpha = 1.0
```
```
GIVEN bPersistAssistCredit = true (the release setting)
WHEN  the app is restarted
THEN  accumulated credit is what it was, to within a second
AND   no code outside SurfAssist::LoadCredit / SaveCredit had to change to get there
```
```
GIVEN a tester has graduated to alpha = 0.0 mid-session
WHEN  the next tester taps "Reset assist"
THEN  credit is 0 and the following ride runs at alpha = 1.0, with no app restart
```

## Test cases

The interesting property — *does the guard actually catch a bad line?* — is testable on the existing
trace-replay rig (`specs/input-trace-replay.md`), which is why FR8 keeps a force flag.

1. **`assist_guard_out_the_back`** — record or hand-author an input trace that steers steadily toward
   the back of the wave until the ride is lost. Replay twice: `surf.assist.force 0` and
   `surf.assist.force 1`. Assert that assist-off crosses the crest and assist-on keeps
   `signedDistanceToCrest` inside the band for the whole ride. This is the spec's headline claim; it
   must be a test, not an eyeball.
2. **`assist_guard_into_the_flats`** — same shape, steering down the face into a stall.
3. **`assist_neutral_midface`** — replay a known-good ride (an existing baseline trace) with
   `surf.assist.force 1`. Assert the trajectory is **unchanged** within snapshot thresholds: a good
   line must never touch the deadband. This catches a mis-signed or mis-scaled correction, which
   would otherwise surface only as "the board feels a bit odd".
4. **Regression sweep** — the full existing baseline set with assist at defaults (i.e. off, per FR8).
   Every CSV compares clean. Check `full-iso` dates first (`memory: stale CSVs look like regressions`).
5. **Device eye-test** — a person who has never played rides waves 1-4, and is asked afterwards,
   unprompted, whether anything changed between wave 1 and wave 4. If they noticed nothing, FR7 has
   failed. If they say "it got harder and I don't know why", FR7 has failed worse.

## Implementation details

Suggestions, not prescriptions.

- New `SurfAssist.h/.cpp` namespace alongside `SurfTilt` / `SurfDebug` / `SurfRails`, so
  `ASurfboardPawn` gains a call site rather than 300 lines. **Give free functions in the anonymous
  namespace file-unique names** — duplicates across .cpp files break the Android unity build
  (`memory: android unity-build anon-namespace collision`).
- One call site in `ASurfboardPawn::Tick`, between the auto-center block and the
  `WeightDistribution->amountToTheRight/amountInFront` write (`SurfboardPawn.cpp:955-970`). The
  player's `CurrentWeightOffset` goes in, the corrected pair comes out. Nothing else moves.
- Wave state comes from the `ASharedCalculations` the pawn already resolves —
  `ResolveSharedCalcForFall()` / `SharedCalculationsForCamera` (`SurfboardPawn.h:299,1073`). Rename
  the resolver if it ends up serving two features.
- All constants on `USurfTuningSubsystem` next to the other ride coefficients, so the whole feel is
  dialled in through `Saved/TuningOverrides.json` without a rebuild.
- CVars: `surf.assist.enabled`, `surf.assist.alpha` (−1 = follow schedule, else force),
  `surf.assist.force` (the FR8 override for tests), `surf.assist.credit` (set accumulated credit in
  seconds — lets a test land on any point of the FR6 schedule without riding there). Debug category
  `assist` for `SurfDebug::ShouldDebug`, logging band error, heading error, both corrections, live α,
  and the current credit accrual rate.

## Related

- `specs/deterministic-ride-handoff.md` — the rails intro this hands off *from*
- `specs/start-screen-tutorial.md`, `specs/tutorial-live-feedback.md` — what the player is told before the wave
- `specs/weight-shift-control-responsiveness.md`, `specs/hard-carve-progressive-boost.md` — why a small correction is a big turn
- `specs/nose-dive-bug.md` — the trim failure FR4 exists to catch
- `specs/wave-crossing-deceleration.md` — origin of `signedDistanceToCrest`
- `specs/input-trace-replay.md` — the rig the guard tests run on
- `specs/ride-score-counter.md` — makes FR6's credit visible as a live counter, so the fade reads as
  earned rather than administered
