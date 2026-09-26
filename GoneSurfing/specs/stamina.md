# Spec: Stamina — every ride is a run with an energy budget

## Status

**Built 2026-09-14/15, desktop-verified, not yet ridden on a device.** `Stamina.h/.cpp` is the pool
(pure, caller-owned `FState`, the `SurfAssist` shape); `StaminaBarOverlay.h/.cpp` the bar; the pawn
ticks `UpdateStamina` right after `UpdateFallDetection` and holds `StaminaState`; tunables under
`Tuning|Stamina` on `USurfTuningSubsystem`; the tired tuning layer is `USurfTuningSubsystem::SetTired`
+ `Saved/TiredTuning.json`; the rider's anim flag is `USurferAnimInstance::bTired`.

**FR3 was rewritten on 2026-09-15 before the first device ride.** The draft ended the ride when the
pool emptied. The owner: *"that's not how most games handle out of stamina. Normally they play a
heavy breathing animation while making the controls a lot slower"* — and then: *disable assist, play
a tired animation, enable specific tuning values for when the stamina is out.* So empty is now a
STATE the physics resolves, not an ending: assist off, a tired tuning layer on, the anim flag up,
slow recovery while resting. Verified on a desktop capture with a fast schedule
(`StaminaPassiveRideSeconds 12`, `StaminaRecoverySeconds 4`): tired at 12.0 s → layer ON with the
sample file's 2 values → assist off → 1.8 s of rest refills to the 30 % exit → layer OFF, assist
back → the budget resumes; eight clean cycles. A run-out no longer produces a card of its own — the
ride ends the way rides end, LOST THE WAVE or WIPEOUT, and the ride-over log line now also reports
recovery and tired time.

Decisions:

- [x] D1 — passive drain yes, `StaminaPassiveRideSeconds` 120 (owner 2026-09-22; was 90); 0 keeps the alternative one edit away.
- [x] D2 — **REPLACED 2026-09-15: running out does not end the ride at all.** See FR3 and D2.
- [x] D3 — one pool for every board; per-board only through the tuning stack, no code.
- [x] D4 — **REVISED 2026-09-15: slow recovery, but only while tired.** Owner: *"I agree with slow
      recovery."* Recovery only during the tired spell, and only while resting; see D4 for why a
      refill during normal riding would make a cruiser immortal.
- [x] D5 — thin ghost bar, top-centre, 200x8 logical px at 14 px from the top edge; coral under 20 %.
      Steps down to 54 px while the dev TUNE toggle is up (they share the strip).
- [x] D6 — **REVISED 2026-09-15: tired DOES change the feel, through a tuning layer, not a force
      branch.** `Saved/TiredTuning.json` is applied on top of the whole stack while tired and lifted
      on recovery. Which coefficients and by how much is UNKNOWN — the owner: *"I honestly have no
      idea which tunables should be adjusted when tired. This will have to be iterated on device."*
      The HUD loop for that is built: while tired, TUNE edits write to the tired file, not the
      board's. The two-entry sample the desktop check used (`MaxPumpForce` 80000, i.e. 0.4x;
      `WeightTorqueMagnitude` 2000, 0.5x) is only a way to see the layer flip in the log.
- [x] D7 — moot: no card of its own any more. `ERideEndKind::Spent` and OUT OF STEAM were removed.
- [x] FR1–FR8 implemented against the revised FR3. FR8 ended up as console aliases rather than a
      CVar: `SurfStamina <0..1>` writes the pool (0 tires the rider on the next tick, anything past
      the exit fraction recovers them), `SurfSpend` = `SurfStamina 0`.
- [x] **What tired does — REVISED AGAIN 2026-09-15: REVERSED assist.** The tired tuning layer failed
      its first device session: every value tried (`MaxPumpForce` down, `WeightTorqueMagnitude`
      down, …) made the board EASIER. This game's difficulty is over-responsiveness — a small weight
      shift over-turns — so a duller board is a more forgiving one. Owner: *"the one thing that makes
      surfing easier is assist, the one thing that can make surfing harder during out of stamina is
      reversed assist. If the assist actively steers the surfer away from the wave, that has to make
      it difficult without a hard kill."* Built: while tired `GetAssistTuning` flips `SteerSign`, alpha
      is forced to `StaminaTiredAssistAlpha` (1.0) so it bites on alpha-0 boards, trim is scaled by
      `StaminaTiredTrimGain` (0 = off, never reversed — reversed trim is a nose-dive), the steer push
      by `StaminaTiredSteerGain` (the difficulty knob), and the assist's steering-input rate limit is
      disabled while tired (a slower stick is an easier board — the same trap). Why it is hard but
      not a kill: the guard is a PD controller that engages only OUTSIDE the pocket band, so reversed
      it is positive feedback — nothing inside the band, every excursion amplified — while staying
      additive and clamped by `AssistMaxAuthority`, so the player's input always works. The tired
      tuning layer stays as an optional extra. Desktop-verified only that the mode engages; the
      un-steered board never leaves the band, so whether the push ends a real player's ride is the
      first device question. Side effect to know: trick-scoring FR5 discards turns the assist steered
      (alpha > 0, guard active), so a tired rider scores few or no turns — arguably right.
- [x] **Tired clips authored (owner, 2026-09-15) and driven from C++.** `Content/Surfer/SK_tired_fade_in`
      (stance → slump, once), `SK_tired_heavy_breathing` (one breath, starts and ends in the slump,
      looped), `SK_tired_fade_out` (slump → stance, once). `USurferAnimInstance::UpdateTiredClips`
      runs the machine None → FadeIn → Breathing → FadeOut → None and publishes `TiredSequence` /
      `TiredSequenceTime` / `TiredStanceBlendAlpha`, the pump's explicit-time pattern: interrupted
      fades mirror their time instead of restarting (assumes the two fades are each other's
      reverse), and the current breath is always finished before fading out. The three sequences
      are `EditDefaultsOnly` on the anim instance.
- [ ] **Editor step: wire the AnimBP** (`SK_Mannequin_AnimBlueprint`), inside the Surf state where
      the pump pair lives, BEFORE the pump blend and before the procedural lean bones:
      1. Class Defaults → Surfer|Tired: assign the three sequences.
      2. A **Blend Poses by Bool** (the plain two-way `Blend` node proved unfindable in the 5.4
         palette): Active Value = `TiredStanceBlendAlpha > 0.5`, True = a **Sequence Evaluator**
         (never a Player) with its Sequence pin exposed and bound to `TiredSequence`, Explicit Time
         bound to `TiredSequenceTime`, False = the stance pose, both Blend Times 0. Equivalent
         because the alpha only ever steps 0/1, and the fade-in's first frame and the fade-out's
         last frame are the stance, so a zero blend time cannot snap.
      3. Feed that into the existing pump blend's stance input, so a pump overrides tired.
      Not eye-tested until this is done; `StaminaForceTired 1` in TUNE is the way to look at it.
      Replays do not carry `bTired`, so a replayed ride never slumps — accepted for now.
- [ ] **`Saved/TiredTuning.json` is empty on device** until someone gets tired, opens TUNE and moves a
      slider. That is the intended loop; nothing is pre-seeded. **`StaminaForceTired` (Tuning|Stamina,
      2026-09-15)** makes it a one-flip loop: 1 = tired now and held there regardless of the pool,
      0 = normal; flip it in the TUNE panel mid-ride. Stamina's own knobs are never written into the
      tired file (`IsTiredLayerKey`), or the switch would save itself as a tired value.
- [ ] **First-cut rates are untested by a rider.** Tune from the ride-over log line
      (`Stamina: ride over after N s with X% left - spent passive/pump/turns; recovered R%; tired K
      time(s) for T s`). Expect the turn cost to need the most attention: derived on paper from the
      trick detector's thresholds.
- [x] **Does a tired cruiser actually lose the wave? NO, not on the foamie — fixed 2026-09-22.**
      Owner, playing the foamie: *"Even when the surfer gets tired, if I just keep on surfing
      straight without any big movements, I can easily continue until I have regenerated stamina.
      It should be a lot more difficult surfing forever on the foamie."* Two causes, both by
      construction: the reversed guard has the assist's dead zone, so a rider holding the pocket
      was never pushed at all; and recovery refilled the pool while they waited. The owner's two
      suggestions are built, both as `Tuning|Stamina` tunables the foamie profile sets
      (`Content/Boards/1-foamie.json`), defaults unchanged for every other board:
      - **`StaminaTiredGuardEverywhere` 1 — the reversed assist works in every part of the wave,
        not just the scoring part.** While tired the controller's band collapses onto the crest
        (`SurfAssist::FTuning::bGuardEverywhere`), so every point on the face reads "too far
        down" and the reversed guard pushes DOWN the face from everywhere on it, full strength
        from `AssistBandSoftness` below the crest. The score's pocket does not move: credit and
        trick-scoring FR5 read `FOutput::bOutsideBand`, the authored band, so tired changes what
        the board does and never what a second in the pocket is worth. First cut was a band
        narrowed about its centre line; that failed on the desktop because `signedDistanceToCrest`
        is a 50 cm scan — the "line" was a 50 cm bucket reading exactly 0, and the un-steered board
        sat in it, silent, for 60 % of a run.
      - **`StaminaRecoverySeconds` 0 — never regenerate stamina once tired.** Already the
        documented meaning of 0; tired is now for the rest of the ride on the foamie.
      Desktop-verified with `StaminaForceTired 1`, `-Board=foamie`, the `assist` debug flag and no
      hand on the controls: before, the reversed guard was silent or ≤0.06 weight units in the
      pocket and the board rode 100 s without leaving (65 s at gain 1, and gain 4 changed nothing
      because of the bucket); after, the push is 0.14 median / 0.26 p90 the whole time and the
      board LOST THE WAVE 12 s after handoff. A real player counters it by holding ~0.15–0.25 of
      lean for the rest of the ride — bounded, additive, `StaminaTiredSteerGain` is still the
      strength knob. **Device confirmation owed**: is it "a lot more difficult" or a kill?
- [x] **The funboard was too easy when tired too — same pair, 2026-09-22.** Owner, after the
      foamie fix: *"The funboard is too easy to control when tired. The same changes done to the
      foamie to make it harder when tired should also be done to the funboard."* So
      `Content/Boards/2-funboard.json` now carries the identical two keys
      (`StaminaTiredGuardEverywhere` 1, `StaminaRecoverySeconds` 0) — still no per-board branch in
      the code. Nothing else needed changing: while tired the alpha is forced to
      `StaminaTiredAssistAlpha` above every other source, so the guard bites as hard on the
      funboard's `assistLevel` 2 as on the foamie's 4, and `StaminaTiredSteerGain` remains the one
      strength knob for both. The hybrid, fish and shortboard keep the defaults.
      Headless hands-off (`-Board=funboard`, `StaminaForceTired 1`, `surf.assist.force 1` — the
      runner is `-unattended`, which is the automated half of the gate and suppresses the guard
      without it): alpha 1.00 all ride, guard ON 95 % of ticks with `bandErr == d2crest` on 243 of
      257 logged ticks (the collapsed band), push 0.20 median / 0.26 p90 — the same shape as the
      foamie's 94 % and 0.14/0.22, a little stronger. **That run does NOT measure difficulty**: the
      un-steered funboard never holds the pocket, it drifts behind the wave immediately, so the
      edge-only control engages 83 % of the time anyway and rode 92 s against the treatment's 65 s.
      One trajectory, one run, and the mechanism is all it proves. **Device confirmation owed for
      the funboard as well**, with the same question: difficult, or a kill?
- [x] **The assist switch was switching off the difficulty too — fixed 2026-09-22.** The owner
      played the above in the editor and reported it unchanged: *"I forced tired, but the board
      still goes straight without any problems... and if I actively tries to turn, it is too easy
      as well."* The reversed guard had never run: `UpdateAssist` starts with
      `IsAssistSuppressed()`, which included `IsAssistEnabledThisSession()`, which is false
      whenever `AssistDisable` is set — and `AssistDisable` is 1 in this project's
      `Saved/TuningOverrides.json`, i.e. in every PC dev session (log line:
      `SurfAssist: input mode = ... assistEnabled=0`). **A switch that turns off the HELP was also
      turning off the HINDRANCE.** `IsAssistSuppressed` now splits the gate: the automated-run
      half (new `IsAssistRunAutomated`, so tests and traces are as untouched as ever) always
      suppresses, and the player-facing half (`AssistMode::Off`, `AssistDisable`,
      `surf.assist.enabled`) suppresses only while NOT tired. Switch the tired guard off with
      `StaminaEnabled` 0 or `StaminaTiredAssistAlpha` 0 — never with an assist switch. No score
      comes of it: `UpdateAssist` opens an assist ride only when the assist is genuinely enabled,
      so credit and trick scoring stay off exactly as before (verified: `credit=0.0s` throughout).
      Re-verified hands-off with `AssistDisable` still on: guard ON 94 % of ticks, push 0.14
      median / 0.22 p90, LOST THE WAVE 13 s after handoff.
- [x] **The tired tuning layer was making tired EASIER.** `Saved/TiredTuning.json` on the PC still
      held the two-entry sample from the 2026-09-15 desktop check (`MaxPumpForce` 80000,
      `WeightTorqueMagnitude` 2000 — half). Dulling the board is the failed experiment this spec
      already records twice, and it is the direct answer to "turning while tired is too easy":
      halving the weight torque makes the foamie *more* forgiving. Moved aside to
      `Saved/TiredTuning.sample-dulling-20260922.json` and the layer left empty, which is what a
      device has anyway. If the layer is ever used again, it can only make coefficients read
      smaller — so on this game it can only ever make tired easier. Prefer the guard.
- [ ] **`StaminaTiredSteerGain` headroom, measured 2026-09-22 (hands-off, foamie, forced tired).**
      1.0 (shipped) → push 0.14 median / 0.22 p90, a bounded fight the player can always out-lean;
      board lost the wave in 13 s. 2.0 → 0.37 median / 1.30 p90, i.e. SATURATED: past the 0..1
      weight range the board is pinned to full lean, the player's input cannot fight it, and the
      board wiped out (lost planing) in 34 s. So the usable range above the default is ~1.2–1.5,
      and 2 is a kill, not a difficulty. Dial it live in TUNE with `StaminaForceTired` 1.
- [ ] **Bar legibility on device.** Deliberately faint (white 0.85 on a 0.18 track); NFR1 says err
      quiet, so raise it only if being tired surprises people.
- [ ] Test case 1 (headless byte-identity on vs off) not run against a fresh baseline. The gate is
      fall detection's plus `-unattended`, and a headless run confirmed nothing installs or drains.

### Gotchas found in the build

- `FSlateDrawElement::MakeBox` paints with the tint you pass and ignores the brush's own colour —
  that is `SImage`'s job. The track was drawn with `FLinearColor::White` as the tint and a
  0.18-alpha brush colour, came out solid white, and hid the fill for three captures. Pass the
  colour as the tint. A leaf widget's paint is also cached until it calls
  `Invalidate(EInvalidateWidgetReason::Paint)`.
- **Recovery whenever resting made the cruiser immortal.** First cut refilled while not pumping or
  turning; with recovery (1/25 s) faster than the passive drain (1/90 s) — which it must be for a
  tired rider to recover at all — an un-steered board netted positive and never tired. Recovery is
  confined to the tired spell. See D4.
- **The tuning subsystem outlives the level.** It lives on the game instance, so a tired layer left
  on at Back / a fall would tire the next ride from its first tick. `EndRide`, `EndPlay` and
  `RestoreRiderFromRagdoll` all lift it.
- **Tired borrows the assist's controller, so it inherited the assist's OFF switch** (2026-09-22).
  Every dev session on the PC runs with `AssistDisable` 1, so every attempt to feel the tired state
  in the editor felt nothing at all, and the feature read as "built and useless" for a week. When a
  difficulty mechanic is implemented by reusing a help mechanic, audit every gate the help has: the
  log line `assistEnabled=0` was sitting there the whole time. See `IsAssistRunAutomated`.

## Overview

The infinite wave never ends, and the foamie's assist makes wipeouts rare on purpose. Together they
mean a ride can go on indefinitely, and that breaks three things that already exist or are spec'd:

1. **The ride list and best-ride replay** (`best-ride-replay.md`). `MaxRecordingSeconds` is 120: a
   ride longer than two minutes is not fully replayable, and a twenty-minute ride would be a bad
   replay even if it were. "Best" today can mean "sat there longest".
2. **The score** (`ride-score-counter.md`, `trick-scoring.md`). Survival credit is
   `DeltaTime × guardRate × speedMultiplier` — unbounded in ride length. A leaderboard over an
   unbounded ride measures patience.
3. **Pumping costs nothing** (`pump-button-and-virtual-stick.md`, Resolved 2026-09-09): holding pump
   on flat water is strictly good, bounded only by speed attenuation. The ruling there was that the
   fix is a stamina pool that pumping and hard turns both draw from, "so speed and turning compete
   for the same resource", and that it wants its own spec. This is that spec.

What this is **not**: a fix for "there is no Restart button mid-ride". The ride screen keeps the
always-visible `‹ BACK` pill (`two-screen-navigation.md` FR3: Back ends the ride, Start is a fresh
wave). A stamina pool is a slow timer; a player who wants out *now* still taps Back. Stamina is the
mechanic that gives a ride a length and a budget, and — as a consequence — makes every ride, wipeout
or not, end on the card with SURF AGAIN on it.

### What it should feel like

A surfer has a finite amount of effort in them per wave. Standing on the board costs a little;
pumping costs a lot; throwing the board into a hard turn costs a lot. A ride is a decision about
where to spend it: pump early for speed, or save it for turns that score; string three hard turns
together and there is nothing left for the section after. When the pool is empty the rider is spent
— they straighten out and the ride is over. Not a punishment; the end of the run.

## Objective

Every ride ends. Pumps and hard turns are a spend, not free. A ride's score is bounded by the energy
budget rather than by boredom. The whole thing is legible: the player can see the pool, sees it drop
when they do something hard, and is never surprised by the ending.

## Requirements

### FR1 — The pool

One scalar per ride, `Stamina ∈ [0, 1]`, starting at 1.0 at ride start. Owned by the pawn in a
caller-owned state struct (same rule as `SurfAssist::FState` and `TrickScore::FState`: no globals, no
UObject inside the logic, everything arrives in an inputs struct — a per-ride pool is exactly the
kind of value that becomes a static by accident and leaks between rides).

Draining is **armed** the same way fall detection is (`UpdateFallDetection`): only once
`AmountPlaning > fallPlaningArmThreshold` with player controls live. Paddling, the pop-up autopilot,
and the intro cost nothing — the budget is for the ride.

### FR2 — Three drains, one pool

Per tick, `Stamina -= DeltaTime × (base + pump + turn)`, each term a rate in pool-per-second:

- **Base** — riding at all. `1 / StaminaPassiveRideSeconds`. See D1 for whether this is non-zero.
- **Pump** — `StaminaPumpCostPerSecond` while a pump stroke is running (`bPumpActive`, the gesture
  as the player makes it). *Revised 2026-09-15:* the draft integrated `PumpInputAttenuated` so a
  pump the board did not feel would not be charged — but that value is speed- and slope-attenuated
  to exactly zero most of the time and non-zero only in the 0.35 s release, so a tired rider pumping
  flat out counted as resting and recovered. Effort is the gesture; muscles spend whether or not
  the wave pays. A longer hold costs more, which matches the animation (a longer hold sinks lower).
- **Turn** — `StaminaTurnCostPerDegree × max(0, |headingRate| − StaminaTurnFreeRateDeg)`, where
  `headingRate` is the **smoothed wave-relative heading rate the trick detector already computes**
  (`TrickScore`, `HeadingRateSmoothingSeconds`). Not a new signal, not roll (the board is roll-stiff
  and a lean detector reads noise — `trick-scoring.md`, gotchas), and not a second smoother that
  could disagree with the first. Below the free rate — cruising, gentle line adjustments — turning
  is free. Above it the cost is proportional to how much of the turn is "hard".

The three are additive so that pumping *through* a turn is the most expensive thing in the game,
which is also the most speed-producing thing in the game.

### FR3 — Running out makes the rider TIRED; the physics decides the rest

*(Rewritten 2026-09-15. The draft ended the ride here; the owner ruled that is not how games treat
an empty stamina bar.)*

At `Stamina <= 0` the rider is **tired**. Nothing ends. Three things change, all at once:

- **The assist drops out.** `AssistAlpha` reads 0 while tired, above every dev override. The
  steering guard, the trim help, all of it — a tired rider has none.
- **A tired tuning layer goes on.** `USurfTuningSubsystem::SetTired(true)` applies
  `Saved/TiredTuning.json` on top of the whole four-layer stack — a fifth, transient layer, never
  written into a board overlay — and `SetTired(false)` lifts it, restoring exactly what each key
  read before. This is how tired changes the feel: weaker pumps, slower weight response, less turn
  authority, whatever the file says. No force anywhere has a "tired" branch; the coefficients
  simply read differently. **What the file should contain is unknown and is found on device**:
  while tired, TUNE-panel edits write to the tired file instead of the board's, so the loop is
  "get tired, open TUNE, move a slider, it sticks as a tired value". Reset while tired removes
  the key from the tired file.
- **The rider's anim flag goes up.** `USurferAnimInstance::bTired`, for the AnimBP to blend a
  heavy-breathing / slump clip over the surf stance — on top of the stance like the pump, never a
  link in the pop-up chain.

Then the physics decides. A tired rider either **rests and recovers** (D4: the pool refills while
tired and unexerted; tired clears at `StaminaTiredExitFraction`, 0.3, hysteresis) or **loses the
wave / falls** through the ordinary endings, which stay exactly as they are: LOST THE WAVE or
WIPEOUT on the card, ragdoll, score banked. There is no stamina card and no stamina ending.

**On the foamie and the funboard (2026-09-22) tired is harder and permanent**, because the
assist made everything else too easy: `StaminaTiredGuardEverywhere` 1 makes the reversed guard push
down the face from every point on it (not only outside the pocket), and `StaminaRecoverySeconds` 0
means the spell never ends — the ride does, by the same ordinary endings. Both are board-profile
values; the three boards above keep the defaults (edge-only reversed guard, slow recovery).

The tired layer is lifted wherever a ride can end or the pawn can go away (`EndRide`, `EndPlay`,
`RestoreRiderFromRagdoll`): the tuning subsystem lives on the game instance and survives the level
reload, so a layer left on would tire the next ride from its first tick.

### FR4 — Running out is never a surprise

The ending has to be earned in the player's eyes before it happens. Two rules:

- The bar (FR5) is visible from the moment draining arms, not only when it gets low. A timer that
  appears at 20 % reads as a bug.
- The last stretch is announced: under `StaminaLowFraction` (0.2) the bar changes state (brighter,
  warmer — the earning colour vocabulary `ride-score-counter.md` FR2 established), so the player
  gets a few seconds of "spend it or lose it" before the end.

No words on the bar. `trick-scoring.md` D7 ruled on device that reading pulls the eyes off the wave.

### FR5 — The bar

Thin horizontal bar, ghost tier (same translucency as the Back pill), **top-centre**, inside the top
18 % of the screen the touch zones leave free. Back has the top-left, score the top-right; the
centre of the top edge is the one horizontal strip still free, and the camera keeps the surfer dead
centre *vertically*, well below it. Pure-C++ Slate like `RideHudOverlay` and `RideScoreOverlay`; no
UMG (`umg-slate-input-overlap` — two live UI layers is the bug that keeps coming back). Installed
and removed with the Back pill (`WantsRideHud`); hidden while a modal is up.

The bar fills right-to-left as it drains (the full state is a full bar; the empty state is nothing),
and it does not animate beyond the low-state change — it is a glance, not a target (NFR1).

### FR6 — Test runs and replays are untouched

Same gates as fall detection and the wipeout card: no draining when
`surf.autopilots` is non-empty, when `bExternalWeightOverride` is set (`-ReplayTrace`), or when
`FApp::IsUnattended()`. A snapshot CSV or a trace-replay comparison cannot see stamina.
`test-runs-enable-player-controls` is exactly why: headless runs do enable controls after handoff,
and an un-gated pool would end the ride in the recording tail.

`StaminaEnabled` (tunable, default true) turns the whole thing off for development. With it off — or
in a gated run — the physics is bit-identical to a build without this feature: v1 touches no force
(D6).

### FR7 — Tunables

All under `Tuning|Stamina` on `USurfTuningSubsystem`, resolvable and live-editable through the
four-layer stack and `surf-live`, so the feel is dialled in with no rebuild:

```
Tuning|Stamina   StaminaEnabled                = true
Tuning|Stamina   StaminaPassiveRideSeconds     = 120    // base drain: full pool lasts this long cruising (0 = no base drain); 90 until 2026-09-22
Tuning|Stamina   StaminaPumpCostPerSecond      = 0.04   // pool/s at full pump: ~25 s of continuous pumping
Tuning|Stamina   StaminaTurnFreeRateDeg        = 35     // = TurnEntryRateDeg; turning below this is free
Tuning|Stamina   StaminaTurnCostPerDegree      = 0.0012 // pool per degree of heading change above the free rate
Tuning|Stamina   StaminaLowFraction            = 0.2    // FR4: the "spend it" state
Tuning|Stamina   StaminaRecoverySeconds        = 25     // D4: empty -> full in this long while tired and resting; 0 = never (foamie + funboard)
Tuning|Stamina   StaminaTiredExitFraction      = 0.3    // tired clears here (hysteresis)
Tuning|Stamina   StaminaTiredAssistAlpha       = 1.0    // reversed-guard strength floor while tired
Tuning|Stamina   StaminaTiredSteerGain         = 1.0    // the difficulty knob for the reversed push
Tuning|Stamina   StaminaTiredTrimGain          = 0.0    // trim help while tired; never negative
Tuning|Stamina   StaminaTiredGuardEverywhere   = 0      // 1 = reversed guard pushes down the face from everywhere, not only outside the pocket (foamie + funboard)
Tuning|Stamina   StaminaForceTired             = 0      // dev: 1 = tired now, pool frozen
```

Back-of-envelope with these numbers: a full-size hard turn (`GradeRefSweepDeg` 120°, held at ~80°/s
for 1.5 s) spends `(80−35) × 1.5 × 0.0012 ≈ 8 %` of the pool, so a dozen hard turns in a row is a
ride; a pump-then-turn combo spends ~12 %. Passive riding alone ends the ride at 90 s, which keeps
every ride inside `MaxRecordingSeconds`. **These are guesses** — the free rate is copied from the
turn detector's entry threshold so the two agree about what "hard" means, and everything else is a
first cut for the owner to tune on device.

### FR8 — On-demand states for verification

Build these before trying to eyeball anything (lesson from `ride-score-counter.md`: timing a capture
into a transient state failed six times out of six):

- `surf.stamina.set <0..1>` — write the pool directly. `0.15` for the low state, `0` for the ending
  and the card.
- Console alias `SurfSpend` — set the pool to 0 (the `SurfFall` pattern).

### NFR1 — It must not become the thing they watch

Same NFR as trick-scoring's NFR2. The bar exists so the ending is legible, not so the player manages
a meter. If device play shows eyes on the bar rather than the wave, make the bar quieter, not louder;
the low state is the one moment it is allowed to ask for attention.

### NFR2 — No physics change in v1

The pool reads inputs the physics already produces and writes nothing back into it. A ride with
stamina on and the pool above zero is bit-identical to one with it off. This is what makes the
feature safe to land on tuned boards: it can change *when* a ride ends without changing *how* it
rides. D6 is the door to changing that, deliberately, later.

## Decisions

### D1 — Passive drain: yes, slowly

The alternative is base drain 0 — only effort spends the pool, and a passive foamie ride is bounded
by boredom and Back. It is tempting because nobody is ever ended by "you did nothing", but it gives
up the one property the overview lists first: rides bounded in length, replayable, comparable. A
90 s cruise is long for this game (measured device rides run ~10–40 s of actual riding) and lands the
ride inside the recording cap. Recommended: non-zero, and tune the number rather than the principle.
`StaminaPassiveRideSeconds = 0` keeps the alternative one edit away.

### D2 — What running out looks like — **REPLACED 2026-09-15: nothing ends**

Two drafts, both wrong in the same direction. The first recommended a kick-out with the rider left
standing; a standing rider was tried on the stall ending the same day and read as "the ragdoll got
reset". The second ragdolled with an OUT OF STEAM card. The owner then reframed the question: an
empty stamina bar in most games is not an ending at all, it is heavy breathing and sluggish controls
— and here, *disable assist, play a tired animation, enable specific tuning values*. That is FR3
now. The ride still ends, but by the physics: a tired rider with no assist and a weakened board
loses the wave or falls, and the card names that. `ERideEndKind::Spent` and its title are gone.

### D3 — One pool for every board

`trick-scoring.md` D3: no per-board thresholds; difficulty pays through the multiplier the rack card
already shows. Same here — one set of numbers, and the per-board tuning stack
(`Saved/BoardTuning/<id>.json`, board profile layer) can override any of them later without code if
a foamie should have a bigger pool. Do not pre-build a per-board multiplier.

That is exactly how the foamie got its harder tired state on 2026-09-22: two keys in
`Content/Boards/1-foamie.json` (`StaminaRecoverySeconds` 0, `StaminaTiredGuardEverywhere` 1), no
per-board branch anywhere in the code. The owner rode the funboard the same day and found it the
same — too easy to control when tired — so the identical pair went into
`Content/Boards/2-funboard.json`. The tired guard forces its own alpha
(`StaminaTiredAssistAlpha`) over whatever the board rides at, so it bites as hard on the funboard's
`assistLevel` 2 as on the foamie's 4.

### D4 — Recovery — **REVISED 2026-09-15: slow, and only while tired**

The draft said no regeneration, because regen makes ride length unpredictable and reintroduces the
unbounded ride whenever it exceeds the base drain. The owner chose slow recovery (with FR3's tired
state, a pool that never refills would be a one-way slide). The draft's objection then bit exactly
as written: the first cut refilled whenever the rider was not pumping or turning, and since recovery
(1/25 s) has to beat the passive drain (1/90 s) for anyone to ever recover, an un-steered board
netted positive and never tired — the immortal cruiser, in the first desktop run.

So recovery runs **only during the tired spell**, and only while resting (no pump, no hard turn
that tick). Riding always costs; being tired is a breath you take (~10 s from empty to the 0.3
exit at the defaults, the passive drain still running underneath); then the budget resumes. A
cruiser tires every ~90 s and, with the assist off during the spell, is expected to lose the wave
— which is the ride-length bound, now enforced by the physics rather than by a timer. Whether it
actually does is the open item in Status.

**Resolved for the foamie 2026-09-22: it did not.** With the assist's dead zone the reversed guard
never touched a rider holding the pocket, and recovery then refilled the pool under them — the
immortal cruiser again, one board up. The owner's ruling for the foamie: *never regenerate stamina
once you get tired* — `StaminaRecoverySeconds` 0 in its profile, so the spell lasts the rest of the
ride. The funboard got the same pair on the same day, for the same reason. The three boards above
keep slow recovery; whether they need the same is a device question.

### D6 — Does tired change the feel — **REVISED 2026-09-15: yes, through a tuning layer**

The draft kept full strength until empty and deferred any tapering as a physics change. FR3 now
changes the feel while tired, but the mechanism keeps the draft's point: no force gets a tired
branch, no multiplier is threaded through the physics. The tired layer is ordinary tuning values,
applied and lifted as a set. `forces-are-not-the-default-tool` is untouched — nothing here adds
energy; the layer can only make coefficients read smaller. Its contents are a device question the
owner has explicitly left open.

## Acceptance criteria

```
GIVEN a live ride on any board with StaminaEnabled
WHEN  the board first exceeds fallPlaningArmThreshold with controls live
THEN  the bar appears full at top-centre and begins to drain

GIVEN a ride with no pump and no turn above StaminaTurnFreeRateDeg
WHEN  StaminaPassiveRideSeconds have elapsed since arming
THEN  the rider is tired: AssistAlpha reads 0, the tired tuning layer is on (log: "tired layer ON"),
      USurferAnimInstance::bTired is true, the bar is empty and coral - and the ride is still live

GIVEN a tired rider
WHEN  they neither pump nor turn hard for long enough for the pool to reach StaminaTiredExitFraction
THEN  the rider recovers: assist back, layer lifted with every key restored, bTired false, and the
      passive drain resumes from the exit fraction

GIVEN a tired rider on the foamie or the funboard (StaminaTiredGuardEverywhere 1,
      StaminaRecoverySeconds 0)
WHEN  they hold the pocket and rest
THEN  the pool never refills, the reversed guard keeps pushing down the face (assist log:
      guard=ON with bandErr = d2crest everywhere on the face, steer > 0.1 at gain 1), an
      un-steered board loses the wave within ~15 s, and the credit rate in the log still reads
      1.00x inside the authored band

GIVEN a tired rider
WHEN  the board loses planing or rolls past the fall threshold
THEN  the ordinary ending runs (LOST THE WAVE / WIPEOUT, ragdoll, card) and the tired layer is
      lifted before the level reloads

GIVEN the pool at 0.5
WHEN  the player holds full pump for 5 s
THEN  the pool reads ~0.30 (0.5 − 5 × 0.04) and the bar visibly dropped during the pump

GIVEN the pool above StaminaLowFraction
WHEN  it crosses below
THEN  the bar changes to its low state within one frame and stays there until the end

GIVEN a headless run (surf.autopilots non-empty), a -ReplayTrace run, or -unattended
WHEN  the ride runs past every drain that would empty the pool
THEN  the pool never moves, no bar is installed, no card opens, and the snapshot / trace CSV is
      byte-identical to a run with StaminaEnabled=false

GIVEN a wipeout before the pool empties
THEN  the wipeout path runs exactly as today; stamina neither delays nor alters it

GIVEN Back tapped mid-ride at any pool level
THEN  FR3 of two-screen-navigation runs unchanged; the pool is simply discarded with the level
```

## Implementation notes (suggestions, not prescriptions)

- **Logic module**: `Stamina.h/.cpp` in the `SurfAssist` / `TrickScore` shape — `FTuning`,
  `FInputs { DeltaTime, PumpInputAttenuated, HeadingRateDeg, bArmed }`, `FState { Pool, bArmed,
  bTired }`, one `Step(...)`. Pure, no world. Testable headlessly by feeding it numbers.
- **Where it ticks**: next to `UpdateFallDetection` in the pawn's tick, after the trick detector has
  produced this tick's smoothed heading rate. It shares the arming condition; consider hoisting
  the arm check so fall and stamina cannot disagree about whether the ride is underway.
- **The ride-end split** (D2): `TriggerFall` today does three things — ends the ride (`bFallen`,
  `EndAssistRide`, input cut, trace close), ragdolls the rider, and hands the wipeout card its beat.
  Extract the first and third into an `EndRide(EReason)` that `TriggerFall` calls before its ragdoll
  and that stamina calls alone. `WipeoutPanel::Open` grows a title parameter (or a reason enum) for
  D7. Everything that today reads `bFallen` as "the ride is over" — `UpdateWipeoutCard`, the score
  counter's ride-over state, `WantsRideHud`, the joystick's visibility — must read the ride-over
  state, not the fell state. Grep for `bFallen` and decide each site: several of them mean "over",
  some mean "ragdolling".
- **Bar widget**: sibling of `RideHudOverlay` — `StaminaBarOverlay`, one `SBox` with a fill, ghost
  brush, low-state brush. Installed/uninstalled in `UpdateRideHud` alongside the pill. Hidden by the
  same conditions that hide the score counter under a modal.
- **The heading rate**: `TrickScore::FState` already holds the smoothed wave-relative rate. Expose
  it read-only rather than recomputing; if the detector is not ticking (assist off, no wave), the
  turn drain is 0 and only base and pump apply.
- **Records**: `FRideRecord` does not need an end reason for v1 — the list shows duration and score,
  and a 90 s ride with no fall is self-explanatory. Add one only if the list ever wants to label it.

## Test cases

1. **Headless identity.** `RunGameAndCollectLogs.bat` with the standard snapshot autopilots, once
   with `StaminaEnabled` true and once false in `Saved/TuningOverrides.json`. Every CSV in
   `Saved/Tests/latest/` byte-identical between the two runs (`surf-telemetry compare`, and check the
   `full-iso` dates first — `stale-csvs-fake-regressions`).
2. **Passive ending.** Desktop `-game`, `Screenshot.ps1`-style, ride straight with no input after
   handoff, `StaminaPassiveRideSeconds 20` in overrides: the card opens at ~21.2 s after arming
   (20 + the 1.2 s beat) with the non-wipeout title; the ride list afterwards shows the ride.
3. **Pump cost.** `surf.stamina.set 0.5`, hold pump 5 s via the touch overlay or `TestPumpInput 1`,
   read the pool from the log / `surf-live get_live`: ~0.30 ± the attenuation ramp.
4. **Turn cost.** From a device trace with known turn events (`trick-scoring.md` first-cut traces),
   replay with stamina on and log the per-turn spend; a 120° turn at ~80°/s spends ~8 %. Adjust
   `StaminaTurnCostPerDegree` until the biggest measured turn spends what the owner wants it to.
5. **Low state + card visuals.** `surf.stamina.set 0.15` → `-Phone` capture of the bar's low state;
   `SurfSpend` → capture of the card after the beat. Judge layout at the device's real logical size
   (`screenshot-phone-flag-wrong-layout-space`).
6. **Wipeout precedence.** `SurfFall` with the pool at 0.05: the wipeout card, not the stamina card.
7. **Back discards.** Back at pool 0.3, then Start: the new ride's bar starts full.

## Open questions

- Does the pump *charge* (crouch) or the *release* cost? Integrating `PumpInputAttenuated` charges
  whatever the physics consumed, which is the honest answer, but the player may read the cost as
  landing on the wrong half of the gesture. Device question.
- Should the last-20 % state have a haptic tick? `PumpHaptics` exists; a single pulse at the crossing
  is cheap. Deferred until the bar alone has been judged.
- Whether a stamina ending should score a small bonus for "spent it all" versus a wipeout — a
  finished run is the harder outcome (`trick-scoring.md` FR9's framing: surfing *out* of the hard
  thing is the achievement). Not for v1; note it so the scoring spec can claim it.
