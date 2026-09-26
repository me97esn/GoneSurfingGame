# Spec: Two-screen navigation — hub, ride, and the wipeout card

## Status

- [x] Spec drafted (2026-09-14) from a user-testing session
- [x] C++: hub buttons (Replay, Change board) on the start screen
- [x] C++: ride HUD reduced to Back (top-left) + score (top-right) + the two touch controls
- [x] C++: Back = end the ride and reload into the hub
- [x] C++: replay launched from the hub, exiting back to the hub
- [x] C++: board rack picks a board without restarting
- [x] C++: wipeout card (SURF AGAIN, plus the corner BACK) after a fall
- [x] C++: one BACK, one corner, on all five screens (`BackPill`, FR2a) - desktop-verified
      2026-09-16, label at x 22..58 / y 17..25 on ride, replay, ride list, wipeout card and rack
- [x] C++: the hub shows the controls at rest, greyed, like the wipeout card (FR1a) - desktop-verified
      2026-09-22; device check of TouchControlsRestArt/RestText owed
- [x] Desktop-validated 2026-09-14: every AC below walked with `Drive.ps1` (synthetic clicks at
      phone size) - hub, Start, Back, wipeout card, SURF AGAIN, rack pick, replay from the hub with
      Back mid-replay and with the end-of-replay list. `pop-up` snapshot A/B before vs after:
      max 15 cm over 641 rows (run jitter).
- [ ] Editor: stop spawning `WBP_SurfboardControls` from the `Surfing_infinite_wave` level Blueprint
      (the NFR2 safety net removes it at runtime and warns, once per load, until then)
- [ ] Device-validated with testers (FR1a still unseen on a phone: the two rest levels were judged on
      a desktop screenshot of the recorded loop, and both are in the TUNE panel to be dialled there)
- [ ] Follow-up: fall detection fires too seldom (see "Out of scope")

### Found while building

- **The end-of-replay list reopened itself over the reload.** Closing that list is how a hub replay
  returns to the hub, but `TickReplay` kept holding on the last frame through the fade and put the
  list straight back up, which paused the world, which froze the reload timer - a list whose BACK
  did nothing. `bReloadPending` now gates the reopen, the card and the Back pill.
- **Fall detection is not test-gated the way the overlays are.** It checks the autopilot *filter*,
  so an unfiltered `RunGameAndCollectLogs.bat` (no args, never auto-quits) can wipe out in its tail
  six minutes after the recording flushed. The card is gated on `-unattended` like the start screen
  so it cannot open there; the fall itself is pre-existing and harmless to the CSV.
- **The ride list's close needs to know whether a row press follows.** Picking a second ride from
  the reopened list closes the list first; a caller that reloads on close would throw the pick away.
  `RideListPanel::FHooks::OnClosed` now carries `bToPlay`.
- The dev-only TUNE badge moved from the top-right to the top-centre, out of the score's new corner.
  It overlaps the rack's title there; acceptable for a control the shipping build hides.

## Overview

User testing (2026-09-14) found the ride screen hard to read: five controls on one screen — Restart,
Replay, Change board, the weight joystick and the pump button — and no signal at all when a ride
ends. Every tester tapped START on the start screen unprompted, and not one discovered that Restart
could be tapped mid-ride or after a wipeout.

This spec reorganises the game into two screens with one job each, and gives a ride an ending.

```
                 ┌──────────────────────────────┐
                 │  HUB  (start screen)         │
                 │        START                 │◄──────────────────┐
                 │   REPLAY    CHANGE BOARD     │                   │
                 └──────────────┬───────────────┘                   │
                     Start      │           Replay ─► ride list ─► replay ─► (ends) ─► ride list
                                ▼                                   │              close
                 ┌──────────────────────────────┐                   │
                 │  RIDE                        │── Back ───────────┤
                 │  ‹ BACK              score   │                   │
                 │  (joystick)         (pump)   │                   │
                 └──────────────┬───────────────┘                   │
                      wipeout   │                                   │
                                ▼                                   │
                 ┌──────────────────────────────┐                   │
                 │  WIPEOUT card                │── BACK ───────────┘
                 │  score / best                │
                 │  SURF AGAIN      BACK        │── Surf again ─► RIDE (fresh wave)
                 └──────────────────────────────┘
```

## Objective

- The **hub** is where the player decides: start a wave, watch a ride, pick a board.
- The **ride** screen is two thumbs and one exit. Nothing else competes with the wave.
- A **wipeout** is a visible ending with the two things the player wants next: go again, or go back.

"Restart" disappears as a concept. Back → Start is the same two taps every other game uses, and the
wipeout card's SURF AGAIN is the one-tap retry where a retry is actually wanted.

## Functional Requirements

### FR1: Hub buttons

The start screen (`StartTutorialOverlay`, the menu view) shows, top to bottom:

1. **START** — primary, unchanged. Stays the single big control; testers tap it without reading.
2. **REPLAY** — secondary. Opens the ride list (`ASurfboardPawn::OpenRideList`) over the hub. Drawn
   only when `HasWatchableRides()` (best-ride-replay.md FR6: never a dead control).
3. **CHANGE BOARD** — secondary. Opens the board rack (`OpenBoardPicker`) over the hub. Drawn only
   when boards are installed (`GetBoardCount() > 0`). The label carries the current board so the
   choice is visible without opening the rack: `BOARD · SHORTBOARD`.

Both secondaries sit on one row under START, smaller than it. `SHOW INSTRUCTIONS` stays behind its
CVar, unchanged.

### FR1a: The hub shows the controls

Reported 2026-09-22: a first-time player opens the app to a hub with no description of how to play,
taps START, watches the surfer pop up with no idea what is expected, and then the joystick and the
pump button appear at the exact moment they are supposed to be used — no time to read either
caption. The captions *are* the game's instructions (the tutorial cards were hidden on 2026-09-09
on the grounds that the controls explain themselves), so they have to be readable before the ride,
not only after it.

The hub therefore draws the two touch controls the same way the wipeout card does (FR6): at rest,
deaf, greyed.

**The dim is two numbers, not one.** Fading the whole overlay evenly to 0.5 was the first build and
was not a signal at all — "it's too little difference between the disabled controls and the enabled.
At least to me it isn't obvious" (owner, 2026-09-22) — because a ghost control at half strength
still looks like a ghost control. So the **instrument** (rings, knob, pump glyph) drops to
`TouchControlsRestArt` = 0.22, far enough to read as a drawing of itself, while the **captions**
hold at `TouchControlsRestText` = 0.85: on the hub the words are the whole reason the controls are
shown, and dimming them would trade the one thing the screen is there to say for a signal about a
control nobody is using yet. What changes at the handoff is therefore the rings and the glyph coming
up solid, with the words steady across the cut. Both are tunables, for the same reason
`TouchControlsHint` is one — how obvious "not yet" reads is a judgement made on the glass over
moving water, not on a monitor.

Concretely:

- **Hub:** the controls are installed when the hub opens (explicitly from `OpenStartOverlay` —
  the hub pauses the world and the pawn does not tick), `TouchControls::SetInert` (deaf, at rest)
  and `SetDimmed` (the canvas at half opacity, since there is no scrim above them here). Their
  input pads are hit-test invisible while inert, so REPLAY and ABOUT — both inside the pads'
  zones — still take taps. The root `SConstraintCanvas` had to become `SelfHitTestInvisible` too:
  at Slate's default `Visible` a full-screen panel at ZOrder 250 blanketed the hub at 200 and START
  stopped taking taps.
- **Instruction cards** (`-ShowInstructions` / the CVar'd button): the controls step aside; the
  cards draw their own illustrations where the rings go. The overlay reports the view switch through
  `FHooks::OnInstructionsView`, because nothing ticks under the hub to notice.
- **Modals over the hub** (rack, ride list, about): no special case — their 0.90 scrims sit above
  the controls and take them out with everything else.
- **Intro** (Start → pop-up → handoff): the controls stay on screen at rest, still dimmed, with
  their pads live so a thumb that lands during the pop-up is tracked as before. They used to draw
  nothing here, which is what made them "appear" at the handoff.
- **Handoff:** the dim lifts — the instrument snaps to full, the captions barely move. Coming up
  solid is the signal that control has arrived, alongside the existing "…and surf!" cue.
- **Wipeout card:** unchanged — inert, full opacity, dimmed by the card's own scrim. The dim is
  not applied there (a second dose would take the captions below readable).

Desktop-verified 2026-09-22 with `Drive.ps1` at phone size: hub (rings faint, captions fully
readable), START still taps, intro dimmed, handoff full, card unchanged. **Open on the device:**
whether 0.22 keeps the controls recognisable as controls over a bright wave, or whether the hub
reads as two captions floating over water. The pair of captures is published at
https://claude.ai/artifact/AfoaAASgKjaonh5uRYtoUw.

### FR2: Ride HUD

While a ride is live the screen carries exactly:

- **‹ BACK** — top-**left**. Reading order runs left to right, so the way out sits where the eye
  starts. Ghost tier (translucent pill), small, inside the top 18 % of the screen that the touch
  zones leave free (`kStickZone` / `kPumpZone` start at 0.18). No confirmation: a ride is cheap and
  a modal is one more thing that pauses the world.
- **Score** — top-**right** (moved from top-left to make room for Back). It is a glance, not a
  target, so the far corner is fine. The burst and snap geometry stays anchored to the number.
- The joystick and the pump button, unchanged.

The UMG bar `WBP_SurfboardControls` (Restart, Replay, Choose board, Camera, Back) goes away
entirely — see "Editor step" and NFR2.

Back is a pure-C++ Slate overlay (`RideHudOverlay`), installed whenever the ride screen owns the
display: not the start screen, no modal up. It also shows **during a replay**, where it is the exit
(FR4) — the replay overlay is HitTestInvisible so the two coexist.

### FR2a: BACK never moves

**Every** screen with a way out draws the same pill, from `BackPill::MakeAnchored`, in the same
corner, at the same size — ride, replay, ride list, wipeout card, board rack. Measured at phone size
(1200×540) the label lands at x 22..58, y 17..25 on all five.

This is a rule, not a default, because the five drifted apart once already. The ride and the replay
shared the corner pill; the ride list, the wipeout card and the board rack each centred a BACK of
their own further down the screen. Two of those three open **on a timer over a live ride** —
`TickReplay` puts the ride list up a beat after a replay freezes, `UpdateWipeoutCard` puts the card
up a beat after a fall — so the exit moved from the corner to screen centre at exactly the moment a
thumb was already travelling towards the corner. Reported from play, 2026-09-16.

What was paired with BACK keeps the centre and loses its partner: the wipeout card is now
`SURF AGAIN` alone, the rack is `RIDE THE …` alone. The rack's BACK used to carry the primary
emphasis while the pick was unchanged; that emphasis now lives only on the confirming button, which
is the only button left in the row.

Mechanically: the pill goes in the **last** slot of the panel's root `SOverlay`, so it draws over the
scrim, and **outside** any `SScaleBox`, so it is not scaled with the panel's content. Every caller
uses the same DPI scale (2.0 on Android, 1.0 elsewhere), which is what makes "the same corner" mean
the same pixels rather than merely the same anchor.

### FR3: Back ends the ride

Back reloads the level and shows the hub. It is `RestartLevel` without `GSkipStartScreenOnNextLoad`.

- **Not** the current `ShowStartScreen`, which pauses the live ride and draws the menu over it —
  tapping START would then resume the abandoned wave. In the new structure START must always mean a
  fresh wave, no exceptions.
- The ride is filed as it is today: `EndPlay` closes the trace and calls `CommitRideRecords`, so a
  ride ended by Back still appears in the ride list as LAST RIDE and can become a best.
- The reload is invisible behind the hub: its background is a recorded loop, not the live world.

### FR4: Replay from the hub

Tapping a row in the ride list while the hub is up:

1. Uninstalls the start overlay and unpauses the world (the replay is driven from `Tick`).
2. Runs the existing kinematic replay (`StartReplayOfRecord` → `EnterReplayMode`), which freezes the
   force pipeline and drives the board and rider from the trace. **The intro autopilot must be
   frozen too** (`AStateTriggerAutoPilot` added to `SetForcePipelineTicking`): the hub is
   pre-intro, and an autopilot left ticking would watch the replayed board's attitude, advance its
   steps and push Paddle/Cobra/PopUp onto a rider the replay is already posing.
3. When the replay ends, the existing hold + end-card + ride-list reopen (best-ride-replay.md D5)
   runs unchanged. Closing that list — or tapping Back at any point during the replay — **reloads
   into the hub** (FR3's path). Not `LeaveReplay`'s world-restore: that restores a mid-ride world,
   and there is no mid-ride world here to restore; a reload is the state the hub is defined as.

Every replay now starts from the hub, so `LeaveReplay`'s restore path is reached only by the
`ReplaySurf` console alias. It stays for that.

### FR5: Board rack picks, it does not restart

The rack is now only ever opened from the hub, before a wave. Its commit button becomes a pick:

- `RIDE THE SHORTBOARD` (accent when the pick differs from the active board) → `SelectBoard(Index)`
  and close. `SelectBoard` already applies immediately between waves, and the hub is between waves.
- `BACK` (was `RESUME WAVE` — there is no wave to resume) → close, nothing applied.
- The chip on the active card reads `YOUR BOARD` (was `RIDING NOW`).

The `RestartWithBoard` hook and the pawn function stay for Blueprint callers but nothing in the
rack calls them.

### FR6: Wipeout card

`TriggerFall` today ragdolls the rider, hides the joystick, freezes the score — and changes nothing
else. The camera keeps following the board, so a first-time tester cannot tell the ride is over.

After a fall, once `kWipeoutCardDelaySeconds` (1.2 s) have passed — the same reasoning as
`kReplayHoldBeforeListSeconds`: the beat is what makes the stop read as intended, and a card that
slams up over the fall takes that away — a modal card (`WipeoutPanel`, ZOrder 260, the rack's tier)
shows:

- **WIPEOUT** title — or **LOST THE WAVE** when the ride ended by the board stalling rather than
  the rider falling (2026-09-14: the two endings are different things and the card names which;
  `ASurfboardPawn::ERideEndKind`, `surfer-fall-ragdoll.md`). The rider ragdolls either way.
- This ride's score, and `BEST nnn` beneath it when there is one (the counter's own ride-over
  surface, moved onto the card; the counter hides while the card is up).
- **SURF AGAIN** — primary, coral. `RestartLevel()`: straight into a fresh wave, hub skipped.
- **BACK** — quiet. FR3's reload into the hub.

The world is **not** paused behind the card: the surfer tumbling in the white water behind it is the
wipeout, and the card should sit on top of it, not freeze it.

The joystick and the pump button **stay on screen under the card**, inert and dimmed by its scrim
(2026-09-14, `TouchControls::SetInert`). Their captions are the ride screen's only instructions,
and nobody reads them while surfing; the end card is the first moment the player has time to, and
pulling the controls the instant the ride ended took the words with them. They take no input there
- the scrim is hit-testable above them, and the canvas ignores its pads while inert. No Replay on the card (v1): one home
for Replay outweighs one saved tap, and the hub is one BACK away. Revisit if testers ask.

### FR7: Test runs are untouched

Every gate that keeps overlays out of scripted runs stays: `MaybeShowStartScreen`'s unattended /
`surf.autopilots` / `bExternalWeightOverride` checks, `WantsTouchControls`' test gate, and fall
detection's. The ride HUD and the wipeout card install only where the start screen would have, so a
snapshot CSV cannot see any of this.

## Non-Functional Requirements

- **NFR1 — one UI layer at a time.** Every modal (rack, ride list, wipeout card) sets
  `bRideUIBlocked` when it opens and clears it when it closes, and the ride HUD binds to the same
  flag. Two live layers means one silently stops responding (umg-slate-input-overlap).
- **NFR2 — no UMG left standing.** With the bar deleted from the level Blueprint there is no UMG in
  the game. As a safety net against the editor step being missed, the pawn removes any live
  `UUserWidget` whose class name begins `WBP_SurfboardControls` on its first tick, with a warning in
  the log — so the failure is loud and one-layer, not silent and two-layer.
- **NFR3 — plain language.** Button copy says what happens: SURF AGAIN, BACK, RIDE THE SHORTBOARD.
  No "restart", "resume", "level".
- **NFR4 — Android layout.** Judge Back's size and the score's right padding on a `-Phone`
  screenshot; the bar the score used to share a row with is gone, so the score can sit at the
  same top padding it has today.

## Acceptance Criteria

**AC1 — Hub.** GIVEN the app has just launched WHEN the start screen shows THEN it carries START,
and REPLAY / BOARD · <name> beneath it only when rides / boards exist.

**AC2 — Ride HUD.** GIVEN a live ride THEN the only controls on screen are ‹ BACK (top-left), the
score (top-right), the joystick and the pump button; no UMG bar is present.

**AC3 — Back.** GIVEN a live ride WHEN ‹ BACK is tapped THEN the level reloads and the hub shows,
the ride is listed as LAST RIDE, and tapping START begins a fresh wave (not the abandoned one).

**AC4 — Replay from hub.** GIVEN the hub WHEN REPLAY → a row is tapped THEN the replay plays from the
row's trace with the rider standing (pop-up chain from the recorded rows), the intro autopilot does
not advance, and when it ends the ride list reopens on that row; closing the list returns to the
hub with the same board selected as before.

**AC5 — Board pick.** GIVEN the hub WHEN BOARD → a different card → RIDE THE <name> is tapped THEN
the hub's board label updates, no reload happens, and START rides that board.

**AC6 — Wipeout.** GIVEN a live ride WHEN the rider falls THEN after ~1.2 s a WIPEOUT card shows the
ride's score and best, the world keeps moving behind it, SURF AGAIN starts a fresh wave with the
hub skipped, and BACK returns to the hub.

**AC7 — Tests.** GIVEN `RunGameAndCollectLogs.bat` THEN no hub button, ride HUD or wipeout card is
installed and every snapshot CSV compares as before.

## Implementation Notes

- `StartTutorial::Install(World, OnStart)` grows a hooks struct: `OnStart`, `OnReplay`,
  `OnChangeBoard`, `HasRides()`, `BoardLabel()` (empty = hide the board button).
- `ASurfboardPawn::ReturnToHub()` (BlueprintCallable, "Back") = `ReloadLevel(/*bSkipStartScreen*/false)`;
  `RestartLevel()` = `ReloadLevel(true)`.
- `PlayRideListEntry` while `bStartScreenActive`: uninstall the start overlay, unpause, clear
  `bStartScreenActive`, then the normal replay entry. The ride-list `OnClosed` hook calls
  `ReturnToHub()` when `bReplayActive`.
- `RideHudOverlay.h/.cpp`: `Install(World, OnBack)` / `Uninstall`; ZOrder 250 with the touch
  controls; root `SelfHitTestInvisible`, only the pill hit-testable.
- `WipeoutPanel.h/.cpp`: `Open(World, Score, Best, Hooks{SurfAgain, Back})` / `Close`; palette and
  pill styles shared with the tutorial's `MakeButton` tiers. Opened from `Tick` once
  `FallElapsedSeconds >= kWipeoutCardDelaySeconds`.
- `RideScoreOverlay`: column `HAlign_Right` with `kScoreRightPadding`; burst centre becomes
  `LocalSize.X - kScoreRightPadding - ColumnWidth/2`.
- `SetForcePipelineTicking` also disables `AStateTriggerAutoPilot`.

## Editor step

Open the `Surfing_infinite_wave` level Blueprint and delete the `Create Widget WBP_SurfboardControls`
+ `Add to Viewport` nodes. Until that is done the NFR2 safety net removes the widget at runtime and
logs a warning every launch.

## Out of scope

- **Fall detection tuning.** Testers routinely kept surfing through what should have been a wipeout
  (`fallRollThresholdDeg` 90°, `fallPlaningStopThreshold` 0.05 are rarely reached). Now that the
  wipeout card is the ride's only ending, the triggers must fire at the right moments — a
  dedicated follow-up against traces from the testing sessions.
- **Joystick latch experiment** (sideways lean held on release) — independent of navigation,
  its own small change.
- Replay on the wipeout card; a pause menu; any mid-ride restart shortcut.
