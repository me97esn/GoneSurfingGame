# Spec: Ride score — make the credit visible

## Status

**Built 2026-08-28, not yet ridden by a human.** Compiles clean, verified on a desktop `-Phone`
capture: counter renders top-centre at both states, credit accrues, best banks on a wipeout. Every
number in it is from a board with no player input.

**Moved to top-LEFT 2026-09-13.** The Restart/Replay bar had moved to the top of the screen and the
counter was pushed down a fifth of the screen to clear it - straight onto the surfer, whom the camera
keeps dead centre. It now sits in the top-left corner on the bar's row (`kScoreLeftPadding`), the one
place the board never is; the snap grows down-left into sky and the burst fans into the shoulder.
Verified on a `-Phone` capture with `surf.score.holdpeak 1` (pins the celebration at full snap +
burst mid-flight, for exactly this check): nothing crops, the bar is clear, the board is untouched.
Two bugs surfaced by that capture and fixed with it: the burst was centred by reading the number's
cached geometry back through `AbsoluteToLocal`, which on a desktop `-game` window resolved to the
bottom-RIGHT corner (shards flew out of the joystick ring) - it is layout arithmetic now; and the
counter never asked for a repaint while pulsing, so on a frozen score the cached paint hid the burst.

`RideScoreOverlay.h/.cpp` is the widget; `ASurfboardPawn::UpdateRideScore` / `AccrueRideCredit` are
the call sites; the store lives in `SurfAssist` beside the credit.

- [x] D1 — **RESOLVED 2026-08-28: both, split by moment.** In-ride = this ride, cards = total + best.
- [x] D2 — **RESOLVED 2026-08-28: speed multiplies the rate now, tricks deferred.** See FR2.
- [ ] D3 — does the counter show on a first ride, where it crawls. Deliberately left to the device
      eye-test rather than settled in the abstract.
- [x] D4 — **RESOLVED 2026-08-28: points. Seconds are internal and never displayed.**
- [x] FR1–FR5 — counter, rate feedback, ride-end display, threshold effect
- [x] FR6–FR7 — automated-run gate, storage through the existing seam
- [x] **FR5 revised from device feedback 2026-08-28.** First celebration read as a wobble; rebuilt as
      a snap + burst + caption, and the badge now steps down at the moment of earning rather than on
      a card next ride. See FR5.
- [x] **Celebration confirmed good on device 2026-08-29.** Player verdict: "works very well now,
      exactly how I wanted it."
- [x] **Both step cards removed** (2026-08-29). The celebration made them redundant; FR4's totals
      moved to the Settings card, which is now the only one a player can reach at will.
- [ ] **Timing a capture into the 2.4 s pulse is a lottery** — run-to-run jitter in handoff time is
      ~3 s, so six attempts caught only the tail. If this needs another visual pass, add a way to
      trigger the celebration on demand rather than trying to time it. Note the counter shows RIDE
      credit while the crossings are on TOTAL credit; getting those two confused is what made the
      first three attempts miss.
- [ ] **FR2's rate contrast still unvalidated by a player.**
- [ ] Regression sweep not run (test case 2). Baselines are stale anyway
      ([[stale-baselines-block-regression-sweep]]), so it proves less than it should.
- [ ] D5 — **revisit after the first playtest of the counter.** If a live counter plus a live badge
      makes a mid-ride assist change legible, the level-down card may become redundant and the
      handoff spec's FR6 latch may be worth reopening. Do not pre-build for this.

### Open after the build

1. **Reaching an assist-on ride headlessly needs `surf.assist.lastshown`.** A fresh process has
   `LastShownLevel = -1`, so `MaybeShowAssistCard` opens the FirstPlay card, which pauses the world —
   no capture could ever reach a ride with the assist on. Seed it to the current level to ride
   through; seed it *above* the current level to open the LevelDown card at startup. The recipe that
   works:
   `-unattended` + `surf.assist.force 1,surf.assist.credit 6.5,surf.assist.lastshown 4`, with the
   fast schedule (`AssistCreditFullSeconds` 5 / `Mid` 10 / `Zero` 20) in `TuningOverrides.json` so
   crossings arrive seconds apart rather than minutes.
2. **The earning/guarded contrast is one person's judgement on one screenshot.** The guarded ink was
   raised from alpha 0.55 to 0.75 after a `-Phone` capture showed it washing into bright water. It
   still has to survive the thing that matters: peripheral vision, on a phone, while surfing.
3. **D3 is still open and now testable** — does the counter crawling at 0.25x on a first-ever ride
   read as "I am earning nothing"?
4. **Speed-multiplier scale is a guess.** `AssistSpeedCreditRefLow` 1400 / `RefHigh` 2600 /
   `MaxMult` 2.0, live-editable under `Tuning|Assist`. A headless ride with no player input earned
   16.2 credit over ~31 s, which is the guard-active floor times roughly the full speed multiplier —
   consistent, but not evidence the range is right for a real ride.
5. **Credit keeps accruing after graduation, which the spec did not originally say.** `UpdateAssist`
   returns early at alpha 0 so a graduated ride stays bit-identical to an assist-free build; that
   would have frozen the counter at exactly the moment the assist stops supplying its own
   progression. The guard state is now recomputed there from `SurfAssist::BandError` — a pure read,
   no correction, no rate limiter, no weight touched — and credit accrues on it. See FR2.

## Overview

`specs/gradual-control-handoff.md` fades the assist out over the player's first minutes, driven by
**credit**: seconds of riding, accrued at 1.0× while the steering guard is silent and 0.25× while it
is correcting (FR6 there). Two things are missing around it, and one change fixes both.

**Nothing tells the player whether they are surfing well.** The assist steps down and they have no
idea what they did to earn it. Showing the seconds on the level-down card does not fix this: a number
delivered at the threshold explains the threshold, not the *behaviour*. The player needs to know
mid-ride, while they can still act on it.

**Nothing asks the player to keep riding.** You try the game, you ride a wave, and there is no reason
to start a second one. The assist fade is a progression, but it moves invisibly and takes minutes to
show.

The mechanism for the first is **already computed and thrown away**. The 1.0×/0.25× accrual rate is a
per-tick verdict on "is this player holding the line unaided", and it disappears into a float the
player never sees. A counter that visibly ticks fast when the guard is silent and crawls when it is
not is a continuous answer to *am I doing this right* — delivered at the moment it can still change
what they do.

## Objective

Make the assist fade **earned rather than administered**, and give a first-time player a reason to
start a second wave — without adding a second scoring system that competes with the assist for the
player's attention or contradicts it.

## Requirements

### FR1 — The counter IS the credit, not a number beside it

One quantity on screen, and it is the same one `SurfAssist` accrues and `AlphaFromCredit` reads.

A parallel arcade score is the obvious implementation and it re-creates the exact problem this spec
exists to fix: two numbers that disagree about why the board changed. The player watches the score,
the assist steps down on the *other* number, and the step-down is unexplained again — failure mode 2
of the handoff spec's Objective, reached by a different route.

Consequence to accept up front: whatever the counter rewards is what graduates the player. That is
the feature — it is what makes the fade legible — and it is the whole of D2's difficulty.

**The player sees points; the code keeps credit (D4).** Seconds are an internal unit and are never
shown — not in the ride, not on a card, not in the copy. The display is `credit × kScorePerCredit`,
a presentation constant and nothing more: it does not make the counter a second quantity, and the
schedule thresholds stay in credit units where `AlphaFromCredit` already reads them.

**10 points per credit-second** is the suggested constant. It ticks fast enough to feel alive (a
1/second counter reads as static), and it puts the FR6 schedule on round numbers a card can quote:
300 / 900 / 1800.

### FR2 — The RATE is the feedback, not the total

The counter must visibly change speed. Guard silent → ticking fast. Guard correcting → crawling.

This is the requirement that answers the first gap, and it is nearly free: `bAssistGuardActive` is
already on the pawn and already gates the accrual. What is needed is that the *difference is legible
at a glance* — a change of speed alone is too subtle on a small number in peripheral vision while the
player is watching a wave. Pair it with a state change on the counter itself (colour, weight, a glow)
so the two regimes are distinguishable without reading the digits.

Deliberately not: a "GOOD!" / "BAD!" callout. The guard is a crude signal — it says the board left the
rideable band, which is *sometimes* the player setting up a turn rather than making a mistake. A rate
that slows is honest about being a rate. A word that says "wrong" is not.

#### Speed multiplies the rate (D2)

Board speed scales the accrual on top of the guard factor, so riding fast unassisted earns faster than
drifting unassisted. It is the one multiplier that does not change what graduation means: riding
faster is still surviving, only harder, and it opens no route to graduating without holding a line.

Scheduled the same way the trim target already is — a reference speed and a gain on the excess,
clamped — so it is data-driven on `USurfTuningSubsystem` and A/B-able through
`Saved/TuningOverrides.json` with no rebuild. Start conservative: at most ~2× at the top of the
board's realistic speed range. A large multiplier turns the fade into a speed-run and re-opens the
question D2 just closed.

**Naming consequence, worth knowing before it confuses someone.** Once a multiplier exists, credit is
no longer literally seconds — it is *effective* seconds. `CreditFullSeconds` / `CreditMidSeconds` /
`CreditZeroSeconds` keep their names because renaming them churns the tuning JSON and every override
anyone has saved, but they are thresholds in credit, not wall-clock. Nothing player-facing says
"seconds" anyway (D4), so this stays an internal wrinkle.

### FR3 — In the ride, the counter shows THIS RIDE, and a fall resets it

The number that creates the pull to ride again is the one you can lose. A total that only ever rises
cannot be failed at, so there is nothing to chase; the wipeout is what makes the next attempt worth
making.

Ride boundaries already exist — `BeginAssistRide` / `EndAssistRide`, with `TriggerFall` banking credit
explicitly (`SurfboardPawn.cpp:4250`). The in-ride value is the credit accrued since
`BeginAssistRide`, not `AssistState.CreditSeconds`, which carries the session total.

Only one focal number is affordable in the ride. The screen is 1200×540 logical after the 2×
`SDPIScaler`, with the Restart/Replay bar across the top-centre, the wave radar bottom-left and the
joystick rings bottom-left and bottom-right. The counter takes the top-left corner, on the bar's row:
the surfer is always at screen centre, so the centre column below the bar is not available. The cumulative total is already represented in-ride anyway, by the badge's emptying
bar.

### FR4 — Out of the ride, the cards show the TOTAL and the BEST ride

After a fall, and on the assist panel's `LevelDown` / `Graduated` cards, show accumulated total and
best single ride.

**As built, the post-fall surface is the counter itself**, not a new card: the ride's final score
stays on screen in the earning ink with `BEST nnn` beneath it. That costs no new widget and, more to
the point, no modal on every single wipeout — which would be the fastest way to make a game about
retrying feel like a game about dismissing dialogs.

**The cumulative totals live on the SETTINGS card** (revised 2026-08-29). They were on the level-down
and graduated cards; both are gone, so they moved to the one card a player can still reach at will by
tapping the badge. Without that move, removing the cards would have quietly dropped this requirement
rather than relocating it — the total would have been nowhere.

The stats are one line (`TOTAL 420   BEST RIDE 0`), not label-under-value. Stacked looked better and
pushed the card past its height cap into a scrollbar, measured on a `-Phone` capture. That card was
already sized to the last logical pixel — it once put its own close button off the bottom of the
phone — so anything added to it has to be paid for in height somewhere. That is where the total belongs: it is the quantity α actually keys on, and the
step-down card is the moment the player is being told what changed.

**Best ride** is what makes a second attempt specific — "beat 340" is a goal, "keep surfing" is not.
Stored on the same seam as the credit (FR7).

### FR5 — The threshold effect fires ON the counter, and on the badge, at the moment it is earned

When accumulated credit crosses a schedule threshold, the counter itself does something — flashes,
levels up, pops. The `LevelDown` panel already explains the change in words; this draws the causal
line those words assert. The card is the explanation; the counter's reaction is the cause.

Ordering matters and is easy to get wrong. The crossing happens **mid-ride**, but α does not move
until the next handoff (handoff spec FR6, "constant within a ride"). So the counter celebrates
*earning* the step-down when it happens, and the card explains the *board changing* on the next ride.
Two moments, two messages, and neither may claim to be the other — a counter that flashes "assist
reduced!" mid-ride would be lying, because the board is unchanged for the rest of that wave.

#### Revised after device testing, 2026-08-28

Three defects found while verifying the rework, all of them invisible in code review:

1. **The peak was cropped off the top of the screen.** The number's box is ~70 logical px tall sitting
   near y=0, so scaling 2.35x about its centre put the top third off-screen. Part of "the numbers were
   just a bit bigger" was the big part being cut away. Peak, top padding and pivot are now one
   arithmetic package — 1.8x, 26 px down, pivot below centre so it grows into empty screen.
2. **The caption's shadow did not fade with it.** Slate does not scale a text block's shadow by the
   text's own alpha, so a fading caption with a fixed 0.8 black shadow becomes a grey-black smear.
   The shadow is an attribute now.
3. **A scaled `WhiteBrush` `SImage` is a rectangle.** The burst was a solid white box expanding to
   ~480 px over the wave, which reads as a rendering glitch. It is painted as radial shards in
   `OnPaint` now, the same way the assist panel draws its confetti.

The first build got this half wrong, in two ways, and the player verdict was blunt: *"the numbers were
just a bit bigger, then a moment later they were smaller again."*

**A symmetric swell is not a celebration, it is a wobble.** A milestone needs a SNAP — an attack fast
enough to register as an event — then a long settle so the eye is caught by the hit and then has time
to read what happened. It also needs **more than one element to move**: a number changing size on its
own is ambiguous. As built the crossing now fires a fast snap to ~2.3x with a cubic settle over 2.4 s,
an expanding burst behind the digits, a plain-language caption ("LESS HELP NEEDED", or "ON YOUR OWN
NOW" at graduation), and the assist badge flashing as it loses a segment — all on the same frame.

**The badge steps down when the step is EARNED, not on a card next ride.** This is the substantive
change. Mid-ride the badge now shows `GetEarnedAssistAlpha()` — the level accumulated credit has
actually earned — rather than the latched `RideAlpha` the board is flying. Showing the latched value
was more literally accurate and it was the wrong call: the step-down had no moment, and the first the
player heard of it was a card at the start of the *next* ride, by which point they had done nothing
recent to connect it to. Immediate feedback beats postponed.

The consequence is that **both step cards are gone.** `LevelDown` went first, suppressed for
intermediate steps by calling `SaveLastShownLevel` at the crossing. `Graduated` followed on 2026-08-29
once the celebration was confirmed good on device — the player's call, and the right one: a card can
only re-announce something they already watched happen, in a dialog blocking the wave they want to
surf.

That does not drop FR7 of the handoff spec, which requires a graduating player be told *before the
wave, not after they fell*. They are told at the moment they EARN it — one full ride before the board
actually changes — by a caption reading "ON YOUR OWN NOW", the badge draining to empty, and the
success sound, which moved here from the card. Earlier than the card managed, and without a modal.

What is genuinely lost is **guaranteed delivery**: a modal cannot be missed, a two-second in-ride
celebration can be, if the player happens to be looking at the wave rather than the counter. That is
the trade accepted, and it is the thing to watch for on a fresh tester who graduates.

**α is still latched.** The badge leads the board by at most one ride, and the gap is in the player's
favour: they are told they need less help while still being given it. Whether the board should follow
immediately is D5, and it is now a smaller question than it was — the feedback half is already
immediate, so only the difficulty half is still postponed.

### FR6 — Inert in every automated run

Same gate as the assist itself (`IsAssistEnabledThisSession()` / `IsAssistSuppressed()`, handoff spec
FR8), and for the same reason. An overlay that only reads pawn state is harmless, but anything that
*writes* — banking a best, flushing `GConfig` — corrupts stored state and leaks between runs. Note
[[test-runs-enable-player-controls]]: headless tests do enable player controls after handoff, so "it
is a test" is not implied by anything the counter can observe locally.

### FR7 — Storage goes through the existing seam, not a new one

Best ride and lifetime total extend `SurfAssist::LoadCredit` / `SaveCredit`'s pattern — same
`[SurfAssist]` config section, same session-vs-persistent switch on `IsPersistenceEnabled()`, same
`ResetCredit()` clearing them.

Not a second storage mechanism, and not a `USaveGame`: the handoff spec's D2 rejected dragging in a
save system for this feature and that ruling covers the counter. `ResetCredit()` must clear best and
total as well, or "Reset assist" leaves a graduated-looking scoreboard sitting on a first-run board.

### NFR1 — It must not become the thing they watch

The counter sits in peripheral vision and is legible at a glance. If a player is reading digits
instead of watching the wave it has failed — the same failure the handoff spec found when it put
explanatory text on the "...and surf!" cue.

### NFR2 — No new physics, no second state of record

The counter reads `AssistCreditSeconds` and `bAssistGuardActive` off the pawn. It computes nothing the
controller does not already compute, and it must not become a second definition of "how well is this
going".

## Decisions

### D1 — Reset per ride, accumulate to a total, or both — **RESOLVED 2026-08-28: both, separated by when**

Ruled on by the project owner. **In the ride: this ride, resetting on a fall. On the cards: total and
best.**

The two numbers do different jobs at different moments, so the question is not which to keep but where
each belongs. The per-ride number carries the stake and the moment-to-moment rate signal (FR2, FR3).
The total is what the assist schedule actually reads, and what a step-down has to be explained in
terms of (FR4). Showing both in the ride splits attention on a screen with no room for it; showing
only the total removes the stake; showing only the ride loses the connection to the fade.

The ruling came with a caveat that became D5: if this works, the cards may not need to exist at all.

### D2 — What a point is, and may tricks accelerate graduation — **RESOLVED 2026-08-28: speed now, tricks later**

Ruled on by the project owner. The next iteration wanted points for sharp turns, airtime, and more for
riding unassisted at speed. Given FR1 none of those can be a separate score, so they have to be
**multipliers on the accrual rate of the same counter** — one number, still monotone, still "this is
why the assist stepped down".

The tension is with the handoff spec's D1, which chose crude survival duration *and said the crudeness
was the point*: monotone, hard to fake, explainable in one sentence. A rate multiplier changes what
graduation means, so the two candidates are not equivalent:

- **Speed multiplier — shipping now.** Riding faster unassisted is still surviving, only harder. It
  opens no route to graduating without holding a line, so it sharpens D1's measure rather than
  replacing it. Specified in FR2.
- **Trick multiplier — deferred.** A player spamming sharp turns is demonstrating *something*, but not
  necessarily the thing the assist is training wheels for, and it opens a way to graduate off a board
  they cannot actually hold. Revisit once the counter exists and the scale can be measured rather than
  guessed.

### D3 — Does the counter show on the very first ride — **OPEN**

At α = 1 the guard is doing much of the work, so the counter crawls at 0.25×. On a player's first ever
wave that risks reading as *"I am earning nothing"* — demoralising, and aimed at exactly the player
this whole feature exists to keep.

For: the crawl is honest, it is what gives the fast rate meaning later, and the 0.25× floor means it
still moves. Against: the first ride has no baseline to compare against, so "slow" is unreadable — it
is simply *the* speed.

Options: show it from ride one; hold it until the first level-down; or show it but suppress the rate
distinction on ride one. Settle it on the device eye-test, not in the abstract.

### D4 — Displayed unit: seconds or points — **RESOLVED 2026-08-28: points**

Ruled on by the project owner. **Seconds are internal only and are never displayed** — not in the
ride, not on a card, not in the copy.

Seconds would have tied the counter to the assist's own language, but they tick once a second, which
is slow enough to read as static, and D2's speed multiplier makes "seconds" a lie the moment it lands.
Points tick visibly, read as a score, and survive any multiplier D2 later adds.

The cost is that the counter is no longer self-explanatory from its unit, and the level-down card can
never say "you surfed N seconds on your own". That line was already cut (`238b91d1a`) for being too
wordy, so the cost is close to zero — but it does mean the *card copy* now carries the whole burden of
saying what the number is. See open item 4 of the handoff spec's status: the praise line was cut and
whether the card is poorer without it is still undecided. This decision makes that call harder to
defer.

### D5 — Does α step down mid-ride once the counter makes it attributable — **OPEN, revisit after playtest**

Raised by the project owner while ruling on D1, and it is the more interesting question of the two.

The handoff spec's FR6 latches α for the whole ride, and its stated reason is that *"a board that
changes feel at second 30 of a good wave produces a fall the player cannot attribute to anything they
did"*. **This spec weakens that reason.** That rationale assumed the change was invisible. With a live
counter crossing a visible threshold and the badge stepping down at the same instant, the change is
attributable — the player watched themselves earn it. And if the change lands mid-ride, the card at
the start of the *next* ride has nothing left to explain, so `LevelDown` becomes redundant and the
whole two-moment split of FR5 collapses into one.

That is a genuinely cleaner design if it holds. Two things could stop it holding, and neither can be
settled from the armchair:

1. **Reward timing.** Attribution fixes *"why did that happen"*, not *"I was surfing well and the
   board got harder for it"*. The fall it provokes arrives at the exact moment the player earned
   something — the worst possible place to put a difficulty step, and arguably worse than the gap
   this spec exists to close.
2. **Testability.** A ride with one α is far easier to reason about and to snapshot. A α that moves
   mid-ride puts a changing parameter inside every trace comparison.

**A middle ground worth trying first, because it costs almost nothing and breaks no requirement:**
keep α latched, but let the badge show the *earned* level immediately — visibly distinct from the
level the board is currently flying, e.g. a ghosted segment. There is precedent in the code:
`UpdateAssistBadge` already shows `GetPendingAssistAlpha()` before a ride begins, which is exactly
"the strength you are about to get". Extending that display into the ride gives the live feedback and
makes the card redundant *without* changing the board under the player.

**Do not pre-build for any of this.** Ship FR1–FR7 as specified, play it, then decide. The decision
needs a ride, not an argument.

## Acceptance criteria

```
GIVEN a ride with the steering guard silent
WHEN  the player holds the line for ten seconds
THEN  the in-ride counter rises at the full rate
AND   the state is distinguishable from the guarded state without reading the digits
```
```
GIVEN a ride with the steering guard correcting continuously
WHEN  the ride runs for ten seconds
THEN  the counter rises at a quarter of the full rate
AND   the credit banked at ride end matches what SurfAssist accrued, exactly
```
```
GIVEN two rides of equal duration with the guard silent throughout
WHEN  one is ridden near the top of the board's speed range and the other barely planing
THEN  the faster ride ends with a materially higher score
AND   the ratio is at most the configured ceiling (~2x), not unbounded
```
```
GIVEN any player-facing surface: the in-ride counter, either card, or any copy
WHEN  the score is shown
THEN  it is shown in points
AND   no seconds value appears anywhere the player can see
```
```
GIVEN a ride in progress with a non-zero in-ride counter
WHEN  the player wipes out
THEN  the in-ride counter resets to zero for the next ride
AND   the total and, if beaten, the best are updated on the ride-end card
```
```
GIVEN accumulated credit that crosses a schedule threshold mid-ride
WHEN  it crosses
THEN  the counter plays its effect at that moment
AND   nothing on screen claims the board has changed, because it has not until the next handoff
```
```
GIVEN a player who taps "Reset assist"
WHEN  the next ride starts
THEN  total, best and credit are all zero
```
```
GIVEN any headless run (unattended, filtered autopilot, or trace replay)
WHEN  the ride executes
THEN  no counter is shown, nothing is stored, and every existing baseline compares clean
```
```
GIVEN bPersistAssistCredit = false (today's default)
WHEN  the app is restarted
THEN  total and best are zero, on the same schedule as the credit
```

## Test cases

1. **`score_matches_credit`** — replay a known trace with `surf.assist.force 1`; assert the counter's
   final value equals `AssistState.CreditSeconds` to within a tick. The counter must never become a
   second, drifting estimate of the same thing.
2. **`score_inert_headless`** — the full baseline sweep at defaults. No overlay, no writes, every CSV
   clean. Check `full-iso` dates first ([[stale-csvs-fake-regressions]]), and note the baselines are
   already stale ([[stale-baselines-block-regression-sweep]]).
3. **Threshold-effect timing** — `surf.assist.credit` set just below a threshold, then ride across it.
   Assert the effect fires mid-ride and α does not move until the next handoff.
4. **Device eye-test** — a person who has never played rides three waves and is asked afterwards,
   unprompted, what the number was for. If they cannot say *"how long I surfed on my own"*, FR1 has
   failed. If they say the assist stepped down "for no reason", the whole spec has failed.
5. **Screenshot at `-Phone`** — 1200×540 logical. The counter must not collide with the surfer
   (screen centre), the wave radar (bottom-left) or the Restart/Replay bar (top-centre), and must
   not crop off the left or top edge at the peak of the snap
   ([[screenshot-the-game-on-desktop]]).

## Implementation details

Suggestions, not prescriptions.

- Everything needed is already on `ASurfboardPawn`: `AssistCreditSeconds` (session total),
  `AssistState.CreditSeconds`, `bAssistGuardActive`, and the ride boundaries `BeginAssistRide` /
  `EndAssistRide`. The in-ride value is credit-now minus credit-at-`BeginAssistRide`.
- A Slate overlay alongside `AssistOverlay` / `RideCueOverlay`, same shape: a namespace with
  `Show`/`Hide`/`Uninstall`, no UObject. **Root must be `SelfHitTestInvisible`** or it eats every tap
  below it — see the handoff spec's gotchas.
- **Sizes are logical pixels through a 2× `SDPIScaler`** in a 540-high space. Judge every size on a
  `-Phone` screenshot, never in the editor viewport.
- Ride-end presentation can reuse `AssistPanel` rather than growing a new card type, if a fifth mode
  is cheaper than a new widget.
- The threshold-crossing test is `SurfAssist::LevelForAlpha(AlphaFromCredit(...))` changing — the same
  single definition of "what level am I on" that the badge and the card already share. Do not
  re-derive it; they disagreed once before, and the card announced a step the badge had not made.
- Best/total accessors belong next to `LoadCredit`/`SaveCredit` in `SurfAssist`, sharing
  `IsPersistenceEnabled()` and cleared by `ResetCredit()`.

## Related

- `specs/gradual-control-handoff.md` — the assist this makes legible: FR6 is the credit, FR7 the badge
  and cards, FR8 the automated-run gate, FR9 the storage seam
- `specs/start-screen-tutorial.md` — where the player is first told what the assist is
- `specs/input-trace-replay.md` — the rig the counter tests run on
