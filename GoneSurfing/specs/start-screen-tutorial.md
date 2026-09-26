# Start screen + control tutorial

## Intent

Before the first wave, hold the player on a **Start screen** instead of dropping them
straight into the ride. Two buttons: **Start** and **Show instructions**. Instructions is a
3-step, one-per-screen control tutorial the player taps/swipes through, ending on a **Start**
button. (Originally 4 steps; the "You take control" card described the paddle-in wait, which
the skip-paddle state injection removed — see specs/skip-paddle-intro.md.)

Design was prototyped as a web mockup (feel/layout/copy) before the in-engine build. The
illustrations are hand-drawn and imported as textures (see "Swapping in art").

## Control copy (source of truth)

1. **Tilt sideways** — Rock the phone left or right to lean the board onto its edge and carve your turns.
   - *held upright:* **Steer sideways** — Turn the phone left and right like a steering wheel to carve your turns.
2. **Tilt forward & back** — Tip the phone forward to drop the nose for more speed; tilt it back to lift the nose and turn faster.
3. **Pump in & out** — Pump the phone toward you and back to power the board — each pump builds speed.

Step 1 has two wordings because it has two gestures; which one is shown follows how the phone is
being held right now. See "Sideways: two gestures, one card".

Every card is an action the player performs. The former step 4 (**You take control** — "The
surfer paddles in and pops up on its own...") was cut with the paddle phase; its
`instructions-controls-hand-over` texture is unreferenced.

Player-facing text avoids pitch/roll/yaw jargon deliberately.

**2026-09-22:** the cards are hidden by default (CVar) and the ride's instructions are the touch
controls' own captions — so the hub now draws the controls at rest over the menu, where they can be
read before START. See specs/two-screen-navigation.md FR1a. The cards still work behind
`-ShowInstructions`; the resting controls step aside while a card is up.

## Behaviour

- On `ASurfboardPawn::BeginPlay` (end), `MaybeShowStartScreen()` **pauses the world**
  (`SetGamePaused(true)`) and installs the overlay. The board, wave, and autopilot are frozen
  behind the menu, so the existing pop-up → control-handoff flow runs **exactly as normal, just
  deferred** to when Start is pressed. No changes to the autopilot timing itself.
- **Start** (menu button, or Start on the final tutorial card) → `OnStartScreenStart()`:
  uninstall overlay, restore `FInputModeGameOnly` + hide cursor, `SetGamePaused(false)`. The ride
  begins.
- **Show instructions** → step 0 of the tutorial. **Back** (‹) goes to the previous step, or back
  to the menu from step 0. **Skip** (✕) returns to the menu. **Next** advances; on the last step it
  becomes **Start**. Progress dots show position; the active dot is coral.
- **Restart drops straight into the ride** — it does NOT reshow the Start screen. `RestartLevel()`
  sets a module-static `GSkipStartScreenOnNextLoad` that survives the `OpenLevel` reload;
  `MaybeShowStartScreen()` consumes it and skips. (Restart is the R key / the Android Restart UI button.)
- **Back to instructions mid-game** — `ShowStartScreen()` (BlueprintCallable) reopens the overlay and
  re-pauses. Wire a "Back" UI button (below Restart/Replay) to it. Tapping Start resumes the ride.
- **Gameplay UI (Restart/Replay/Back) hides while the menu is up.** Those buttons live in a UMG HUD
  which renders on the PlayerScreen layer *above* this viewport Slate overlay, so the pawn exposes
  state the BP binds to: `bStartScreenActive` (BlueprintReadOnly) plus `OnStartScreenOpened` /
  `OnStartScreenClosed` (BlueprintAssignable). The HUD shows the buttons only when
  `bStartScreenActive == false`. Set/broadcast in `OpenStartOverlay` (opened) and
  `OnStartScreenStart` (closed).
- Navigation inputs: tap the card, swipe left/right (touch or mouse-drag), the Next/Back/Skip
  buttons, and the dots row. Swipe left = forward, right = back; a tap advances.

## Handoff cue

`RideCueOverlay.h/.cpp` is a small styled Slate overlay (centered upper-third, bold text + soft
shadow, DPIScaler 2× on Android, HitTestInvisible, ZOrder 150) fired by
`ASurfboardPawn::UpdatePlayerControlState`:

- **"…and surf!"** — the instant controls enable (`RideCue::PlayGo`, which self-installs the
  overlay), tinted gold, holds ~1s then fades out (`FCurveSequence`) and collapses. One-shot.
  Not an instruction — a *go signal* marking the exact frame control begins.

The steady **"Wait..."** waiting phase was removed with the paddle phase
(specs/skip-paddle-intro.md): the intro now starts at the cobra pose and the no-control window
is a ~2s beat that needs no explanation. `RideCue::ShowWaiting` remains in the API, unused.
Uninstalled in `EndPlay`.

## Suppressed during tests

The overlay would pause the world and starve the autopilot, corrupting snapshot/trace CSVs, so
`MaybeShowStartScreen()` no-ops when **any** of: `FApp::IsUnattended()` (every headless
`RunGameAndCollectLogs` launch passes `-unattended`), `bExternalWeightOverride` (–ReplayTrace
runs), `surf.autopilots` filter non-empty (snapshot / state-trigger test runs), or `WorldType`
is neither `Game` nor `PIE`. Same "is this a test run" gate as `UpdateFallDetection`.
Also gated by the editable `bShowStartScreen` UPROPERTY (default true).

The unattended check exists because the cvar check alone is a **race the headless runner
loses**: `surf.autopilots` arrives via `-ExecCmds` one frame after the pawn's BeginPlay gate
runs, so the menu installed anyway, paused the world, and the run hung to the 600s timeout
(observed 2026-08-18, first headless run with `bShowStartScreen` enabled in the umap).

## Implementation

- `StartTutorialOverlay.h/.cpp` — pure-C++ Slate, no UMG. `StartTutorial::Install(World, OnStart)` /
  `Uninstall(World)` keyed by `UWorld`, mirroring `WaveRadarHUD` / `ReplayOverlayHUD`
  (`AddViewportWidgetContent`, ZOrder 200, `SDPIScaler` 2× on Android). The `SStartTutorial`
  `SCompoundWidget` owns view state (menu vs tutorial, step index) and the swipe/tap handling.
- `SurfboardPawn`: `bShowStartScreen` UPROPERTY, `MaybeShowStartScreen()` (BeginPlay tail),
  `OnStartScreenStart()`, and `StartTutorial::Uninstall` in `EndPlay`.

## Background: a recorded wave loop

The menu's ground is a **pre-recorded loop of the wave**, not a live scene. 96 textures at 128×72
(`/Game/UI/StartScreen/T_StartBg_000` upward) cycled at 15 fps by the same Slate active timer that
drives the tutorial illustrations — half the capture's natural rate (stride 2 off a 60 fps wave
would replay at 30), because the wave reads calmer behind the menu at half speed. Active timers run
on real time, so they play through a world pause, which is the whole reason this works where the
live version could not.

**The softness is the upscale, not a filter.** Frames are stored tiny and stretched to full screen;
there is no runtime blur and no `SBackgroundBlur`, so nothing can be unsupported on a device.

128×72 was chosen by comparing three levels side by side: at 192×108 the island's waterfall stays
legible but the GridLodActor seams start to show again; at 96×54 the foam becomes a suggestion rather
than a feature. 128×72 keeps the wave motion and the foam trail reading as water with every hard edge
gone, at ~2.2 MB of PNG for the whole loop. It also blurs away the island backdrop card's diagonal
edge, a pre-existing artefact that is visible in the live game.

Changing the softness is one downscale of `Saved/StartBg/` — the captured 1280×720 originals are kept
precisely so that neither a blur change nor a second opinion needs another capture run. Only a change
of *framing* does.

The loop is seamless by construction: it covers exactly one wave period (WaterController frames
886–1078, stride 2), so the last frame meets the first. If the frames are missing the overlay falls
back to the flat night panel and logs `StartTutorial: background loop N frame(s)`.

### Capturing the loop

`-StartScreenCapture` (see `ASurfboardPawn::TickStartScreenCapture`) takes over from the ride:

```
UnrealEditor.exe <project> Surfing_infinite_wave -game -windowed -ResX=1280 -ResY=720 -StartScreenCapture
```

It hides the board and rider (they'd drift and break the loop's seam), lets the camera settle onto
the foam for 90 ticks and then **freezes** that transform, steps the wave one `CaptureFrameStride`
at a time holding each frame 5 ticks so Niagara can consume the new white-water points, and writes
`Saved/StartBg/startbg_NNN.png`. Downscale those to `T_StartBg_NNN.png` and import.

**It must run unpaused** — that is the point. Niagara does not simulate in a paused world, so a
paused capture records water with no foam on it, which is exactly the dead end that killed the live
background.

### Keeping the type legible over it

A recorded wave is a far brighter ground than the flat panel it replaced, and the tutorial copy went
grey-on-bright. Three changes, in order of how much they do:

- **The tutorial cards carry an extra veil** (`C_ScrimSteps`, night at 30%) on top of the shared
  scrim, full-screen and shown only on that page. The start screen is a hero shot and stays bright;
  the cards are something the player has to *read*. It must be sized by the root overlay, not the
  900px content column, or it draws as a vertical band; and it is `HitTestInvisible` so it cannot
  swallow a tap meant for a button.
- **Body copy moved from `C_Slate` to `C_Foam`.** The muted tone fell below readable over sunlit
  water. Title still leads on size and weight, so the hierarchy survives losing the colour step.
- **Every light label carries a shadow** (`TextShadow` / `C_TextShadow`) — titles, body, counter and
  the wordmark. Cheaper than darkening the scrim further, which would flatten the background the
  whole feature exists to show.

`-ShowInstructions` opens the overlay straight on the tutorial cards. They are otherwise reachable
only by tapping, which a headless screenshot run cannot do — and they are the hardest part to keep
legible, so they are the part most worth capturing.

### Finding a framing: the sweep flags

The wave is drawn by a handful of `AGridLodActor`s and the joins between them read as hard lines at
glancing angles — the thing that actually decides this framing. Hunting it one build-and-run per
guess is painfully slow, so two sweep modes each write a 96-image contact sheet to
`Saved/StartBgSweep/` in a single run, wave held still:

- `-CaptureYawSweep` — `StartScreenCameraYawOffsetDeg` from −48° to +47°, one image per degree.
- `-CaptureZSweep` — `StartScreenCameraOffsetCm.Z` from 700 down to 35 cm.

Current values came from exactly that: yaw **−32°** cleared a gap between tiles at bottom-right, but
made the far mesh edge at top-left more pronounced; dropping Z to **140 cm** fixed both, because a
low camera lets nearer water occlude the far tile edges. Height is the stronger lever of the two —
reach for the Z sweep first when a seam shows up.

## Superseded: the live wave, blurred

`SBackgroundBlur` is **gone** — this section is kept only so the reasoning isn't rediscovered. The
live approach worked for the wave mesh but could never show white water, and the machinery below
(camera posing, wave-clock driving, tick opt-ins) now exists to serve the *capture* pass instead.

The `C_Scrim` border (`C_Night` at 52% alpha) and the wordmark's drop shadow survive: recorded water
is still brighter than the type in places.

Three things have to survive the `SetGamePaused(true)` for this to work, all set in
`OpenStartOverlay()` and handed back in `OnStartScreenStart()`:

1. **The camera has to compose at all.** `UpdateCameraTransform` only runs from `Tick`, and the
   pause lands in `BeginPlay` *before* the first `Tick` — so the camera used to sit at the pawn's
   spawn transform. (This is what made the start screen look like it used a "lowered" camera.) The
   pawn opts into `PrimaryActorTick.bTickEvenWhenPaused`, and `Tick` early-returns on
   `bStartScreenActive` after nothing but `UpdateCameraTransform` — the ride path must not advance
   while the player is still reading.

2. **The menu gets its own framing, not the ride's.** `PoseStartScreenCamera()` replaces
   `UpdateCameraTransform` while the menu is up: it stands the camera at
   `StartScreenCameraOffsetCm` **relative to the board** and looks back at it (plus
   `StartScreenLookAtRiseCm` above it). Borrowing the ride camera failed twice over — it sits
   **5 cm under the surface** at the start pose (measured: `clearance=-5`), rendering as the flat
   underside of the water mesh, and its fixed Beside yaw points along open water with the wave out
   of frame. Nudging lift and pitch on it was guesswork; a look-at keeps the subject in shot by
   construction, and the offset reads as a sentence ("25 m out to sea, 10 m down the line, 7 m up").

   Axes for tuning: **−X is toward the beach**, +Y is down the line, +Z is up. There is real land in
   the level — a tropical island, shoreward of the break — which only enters frame if the camera is
   seaward of the board looking back through it.

   Historical trap, if anyone reinstates an offset on top of the ride camera: it must go on
   **`CameraRoot`**, which `UpdateCameraTransform` re-*sets* each tick, not on `CameraComponent`,
   whose relative offset nothing resets — put it there and the camera climbs at offset × framerate
   per second (observed: Z past 1.7 million).
3. **The wave itself.** It animates by mesh-swapping — the `WaterController` blueprint advances
   `CurrentFrame`, and every `AGridLODActor` / `AMeshArrayActor` reads it to pick this frame's mesh.
   All three are ordinary actor `Tick`s, so a paused world freezes the water.
   `SetWaveAnimationTickWhilePaused(bool)` walks that chain and flips
   `PrimaryActorTick.bTickEvenWhenPaused`; the tick manager re-reads the flag per frame when
   queueing, so a runtime flip takes effect next frame. Deliberately **not** opted in: the board,
   the autopilot, and the `AFluidDynamics` samplers, which stay frozen behind the menu.

   **Ticking the chain is necessary but not sufficient** — the `WaterController` blueprint does not
   advance `CurrentFrame` through a pause even when it ticks (measured: pinned at 887 across ten
   seconds). `AdvanceWaveFrameWhilePaused` drives the clock from C++ instead, at
   `StartScreenWaveFps` (60), looping inside the grid's baked `StartFrame`..`EndFrame` range so the
   mesh lookup can't run off the end of the cached frames. `RestoreWaveFrameAfterStartScreen` puts
   the original frame back on Start, so the ride still begins at exactly the wave phase the
   autopilot's takeoff timing expects.

The radar is uninstalled while the menu is up and reinstalled on Start (skipped during a replay,
which hides it deliberately) — blurred, it read as an unexplained smudge.

### White water on the menu — UNRESOLVED

The wave animates on the menu but still carries **no foam**. Four separate blockers were found and
cleared; something inside the Niagara setup remains, and it is the open question here.

The engine gate was real: Niagara simulation for a world is driven by `FNiagaraWorldManager`'s own
tick functions, which stock `NiagaraWorldManager.cpp` registers with `bCanEverTick` /
`bStartWithTickEnabled` but **not `bTickEvenWhenPaused`**, and `TickFunctions` is private — so a
paused world means no Niagara at all, whatever individual components are set to. The fork now carries
an additive, off-by-default opt-in, `FNiagaraWorldManager::SetTickEvenWhenPaused(bool)`, called from
`SetWaveAnimationTickWhilePaused`. Verified working ("world manager found"), and **not sufficient**.

Where it stands, all measured in one run:

| check | value |
|---|---|
| Niagara opt-in | enabled, world manager found |
| foam points fed per frame | 192–388 |
| camera off-axis to cluster | 11–13° |
| camera to cluster | 36–40 m |
| foam actually rendered | none |

So the points reach the channel and the subject is in shot, but nothing spawns. Two concrete
suspects, both checkable in-editor faster than by black-box iteration:

- **Niagara scalability / significance culling** deactivating the systems
  (`fx.NiagaraAllowRuntimeScalabilityChanges 1`, `fx.Niagara.QualityLevel 3` in the log). Culling
  decisions may themselves depend on ticks or on the player camera.
- **Data Channel islands.** `AParticleSystemsController::BeginPlay` anchors `SearchParams.Location`
  once, at its own actor location (2030, 11020, 240), while the foam sits ~143 m away around
  (8100, −1900). A reader near the foam may be querying a different island than the writer wrote to.

Three other things were fixed along the way and are all genuinely needed *before* the Niagara side is
reached, so don't undo them when revisiting this:

1. `AParticleSystemsController::Tick` used to early-return on `bWorldPaused`, skipping
   `UpdateNiagaraParameters()`. It now skips only when the Start screen isn't driving the wave.
2. Niagara component ticks are opted in by `SetWaveAnimationTickWhilePaused` — inert for simulation
   today, load-bearing if the engine flag is ever flipped in the fork.
3. **Foam exists in only part of the wave animation.** Measured across the controller's full
   886..1078 range: zero points outside ~925..960, peaking at 939 (526 points).
   `StartScreenWaveFrameStart/End` pin the menu clock to that window.

Also worth knowing: foam culling is measured from `AParticleSystemsController::CameraActor` (an actor
set in the level — currently `StaticMeshActor_5`) with `MaxParticleDistanceFromCamera` = 25 m, **not**
from the render camera. Moving the menu camera changes where foam appears in frame, never which foam
survives — a wrong assumption that cost one iteration here.

The menu camera aims at the foam centroid (`bStartScreenLookAtFoam`, smoothed) rather than the board,
because the break sits well down the line from where the board waits. Keep that even if the foam
question is dropped: it is the better framing regardless.

If the foam is abandoned, the engine-fork addition can be reverted — it is additive and off by
default, so it is harmless to leave, but it buys nothing on its own.

Consequence to watch: the board is frozen while the water moves through it. At this camera height it
is not prominent, but if it looks wrong, hiding the board + surfer for the duration is a
`SetActorHiddenInGame` call in the same two places.

## Illustrations: two-frame flip

Each step's illustration is a **two-frame animation** — an `SWidgetSwitcher` holding frame A (index
0) and frame B (index 1). A single active timer (`TickFlip`, `FlipInterval` = 0.45s) toggles every
step between the two frames, so the art reads as motion (rock left↔right for roll, forward↔back for
pitch, in↔out for pump). The flip runs even while the game is paused behind the menu.

The art is hand-drawn and imported to `/Game/Images/` as `instructions-<gesture>-A/B`; the paths live
in `FrameTexturePath()`, the one place that names them. A step whose frame B is missing renders as a
single static image, and a step with no art at all falls back to `MakePlaceholderFrame` (a blue
marker parked left/right, so the flip is still visible). Flip speed is `FlipInterval`, except pump,
which flips faster to read as a quick pumping motion.

## Sideways: two gestures, one card

Sideways weight has **two** gestures, because it is fed by two orthogonal sensor channels that are
summed (`AdvanceTiltYawFusion`, specs/tilt-yaw-fusion.md):

| phone held | gesture | channel | why |
|---|---|---|---|
| near flat | rock it left/right | gravity roll | gravity resolves the roll directly |
| near upright | turn it left/right like a steering wheel | gyro-integrated yaw | gravity is blind to rotation about itself; the gyro covers exactly that null space |

Both work everywhere — the gravity channel fades as the phone stands up, the gyro channel fades as it
lies down, and neither is ever switched off. So the card is not choosing a *correct* gesture, it is
showing the one that currently has the most authority.

**The card follows the phone, live.** Step 1's illustration and copy swap as the player rotates the
device, which teaches the dual gesture better than a caption could: turn the phone while reading and
the instruction changes with it.

### The pose signal

`|g.z| / |g|` from `GetInputMotionState` — device Z is the phone's **in-plane vertical** axis, so this
is 0 flat and 1 upright (measured on device: 0.00 flat, 1.00 upright, 0.92 reclined —
specs/sensor-probe.md). It is the *same* quantity `AdvanceTiltYawFusion` uses as `YawWeight`, which
is the point: the picture and the control law read one number, so they cannot drift apart.

Needs no calibration — the tilt basis isn't established until autopilot handoff, long after the menu,
and this classification doesn't use it.

Conditioning, in order:

- **Reject untrustworthy samples**: `|g| < 0.5` means no sensor (desktop) or a reading still
  converging. The probe capture shows `|g|` climbing 0.61 → 0.85 → 0.95 across three consecutive
  samples at the start of a step, so this is a real transient, not a hypothetical.
- **Low-pass** over `PoseSmoothSeconds` (0.35 s), seeded on the first good sample so an already-upright
  phone shows the right card immediately rather than fading in from flat.
- **Hysteresis**: vertical above `PoseVerticalEnter` (0.60), flat below `PoseVerticalExit` (0.40).
  Crossover is ~30° from flat; without the band, a phone parked near it flickers between the two cards.

### Why this works on a paused menu — and the trap it sits next to

The start screen pauses the world, so **the pawn never ticks** and `UpdateTiltWeight` never runs. The
sensors themselves are unaffected: `GetInputMotionState` keeps returning live values through a pause.
The overlay samples from its existing `TickFlip` **active timer**, which is real-time and pause-immune
— the same timer already driving the wave loop and the frame flips.

This is exactly the mistake the sensor probe made and paid for: v1 sampled from `ASurfboardPawn::Tick`
and produced six header-only CSVs across six device sessions before anyone noticed (specs/sensor-probe.md
FR4). **Anything on this overlay that wants sensor data must poll it from the active timer.**

### Forcing a pose

`surf.start.holdpose` — `-1` auto (default), `0` flat, `1` upright. Both cards are otherwise
unreachable without a device: desktop has no gravity, so auto always yields the flat card. Use it with
`-ShowInstructions` to capture or eyeball the upright variant in the editor.

### The swap has to be a transition, not a cut

First device build swapped art and copy on the same frame. Player verdict: **it reads as a glitch,
not as a response** — two unrelated things changing instantly at once looks like a rendering fault.

So the card **dips out and back**, swapping at the trough where nothing is on screen to jump:
`RenderOpacity` on the whole step page (illustration, title, body — the nav and dots stay put),
smoothstepped, `surf.start.posefade` seconds each way, default 0.18.

A dip rather than a crossfade, deliberately: a true crossfade needs both variants alive in an
overlay, which doubles the step page and the flip switchers for a difference the player would not
name. Revisit only if the dip still reads wrong on device.

Four details that matter:

- **Only the sideways card dips.** It is the only step with two variants, so it is the only one with
  anything to transition *to*. Running the dip on the other cards was actively harmful: the pitch
  gesture — tip the phone forward and back — *is* the motion the classifier reads, so performing the
  card's own gesture crossed the threshold, and the trough's `SeedPreviewBasis` snapped the live
  board back to level and re-captured neutral mid-gesture, all to land on the same picture again. Two
  things had to change. `StepHasPoseVariant` now requires the upright path to *differ* from the flat
  one, not merely to be non-null: `FrameTexturePath` ignores `bVertical` for the pitch and pump
  steps, so the old null check called every step a variant step, gave those two a pair of duplicate
  switcher slots, and enrolled them in the fade. And `AdvancePoseFade` applies the swap silently off
  a pose-variant card (art and copy, no fade, no re-seed), so the off-screen sideways card is still
  correct on return without disturbing the card the player is actually on.
- **The visible pose trails the sensor** by one dip. `bPendingVertical` is what the sensor wants,
  `bVerticalPose` is what is drawn; they meet at the trough. Rotating back mid-dip therefore
  *cancels* the swap and fades the original back in, rather than queueing a second swap.
- **The first classification never dips.** It lands before the player has looked at the card, so it
  applies outright — otherwise the card flinches on open, and someone already holding the phone
  upright would watch the flat card fade away.
- **`surf.start.posefade 0` restores the instant swap**, which is also how the fade was verified:
  stretch it to 8 s and screenshot mid-dip.

### Layout

Step 1's switcher carries four slots — `[flat A, flat B, upright A, upright B]` — indexed as
`PoseBase + FrameBit`. Flattening it this way keeps one switcher per step, so `TickFlip` and
`FrameSwitchers` stay as they were.

The card is sized from frame A of the flat variant. Both variants are the same 2739×2000, so nothing
moves on a swap; a future variant with a different aspect would letterbox inside the existing box
(`SScaleBox` `ScaleToFit`) rather than resize the card, which is the preferable failure.

## Live control feedback

Players perform the gestures while reading the cards and expect a response. Giving each card a board
that reacts to the real sensor input is specced separately in
[tutorial-live-feedback.md](tutorial-live-feedback.md) — it needs the ride's tilt/pump math extracted
into a shared unit first, and it re-seeds its preview calibration on this page's grip-swap dip.

## Open items / not yet done

- **The dip duration is unjudged on device.** 0.18 s each way was chosen on a desktop capture, not
  on a phone. Tune with `surf.start.posefade` at runtime — no rebuild — and bake the number that
  feels right. (Auto-detection itself is player-verified: both cards appear for the right grip.)
- Nothing announces that the *other* sideways gesture exists. A player who reads the tutorial in one
  grip and then changes grip mid-ride has been told about one gesture only. A line of copy under
  step 1 would fix it, at the cost of a fourth text row on a short landscape screen — left out
  pending a look at the real card.
- Step transitions are an instant switch; the mockup's directional slide is not yet ported (would
  use an `FCurveSequence` / active timer in Slate).
- No "seen once" persistence — the menu shows every launch by design for now; add a `USaveGame`
  flag + auto-open-once if wanted later.
- On-device touch verification (swipe/tap) — interactive Slate touch input can't be checked headlessly.
- Visual tuning (spacing, sizes, exact colors) against the mockup.
- **Blurred wave background is verified on PC only** (16:9, captured frames). On Android:
  `SBackgroundBlur` costs a render-target copy, and if it is disabled or unsupported the fallback
  brush silently restores the old flat panel — which looks like "the feature didn't work". Confirm
  the blur is actually happening on device rather than assuming it from the code.
- The **`SHOW INSTRUCTIONS` button is now blue-on-blue-water** and separates much less than the coral
  `START`. It matches the in-game Replay pill by design, so changing it is a call to make against
  the whole button system, not here alone.
- `StartScreenCameraOffsetCm` was chosen from captured frames at 16:9; re-check the framing at phone
  aspect, where the vertical field of view differs.

## Status

**The SHOW INSTRUCTIONS button is hidden as of 2026-09-09** (`surf.start.instructionsbutton 0`).
Steering moved to an on-screen joystick and pumping to a button labelled PUMP
(`specs/pump-button-and-virtual-stick.md`), so the controls now explain themselves and a tutorial
standing in front of them reads as ceremony.

Nothing is deleted: the cards, their live board preview, the step navigation and `-ShowInstructions`
all still work, and the CVar puts the door back. Kept deliberately — the control scheme is still
being A/B'd against tilt, and tilt is the half that needs teaching.
