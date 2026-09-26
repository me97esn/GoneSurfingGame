# Spec: Replay your BEST ride, not just your last one

## Status

**Not started. Spec drafted 2026-09-03; reviewed against the code 2026-09-11 on branch
`replay-best-score`.** Nothing built.

That review found four things this spec assumed and the code does not do. One is a layout fact that
blocks D2's chosen placement (D6); three are properties of the existing replay, ride-end and trace
code that FR1's "the picker is the whole feature" quietly rests on (D7, D8, D9). They are open
decisions, not detail — each one changes what gets built.

**D10 then replaced the entry point entirely** (ruled 2026-09-11): rather than a second Replay
control on the board card, the *existing* Replay button opens a list of rides to watch. That closes
D6 without answering it, retires D2's geometry, and makes D7 mandatory rather than optional.

- [x] **D4 — RESOLVED 2026-09-03: no migration, no versioning, no reset ceremony.** Owner: there are
      no users yet, so a change to what a score means costs nothing. **This ruling has an expiry** —
      see D4.
- [x] **D1 — RESOLVED 2026-09-03: swap the recorded board in for the replay**, restoring the previous
      state after. Not the "selected board only" narrowing that was recommended; the owner took the
      option that keeps a best watchable regardless of what is being ridden.
- [x] **D2 — SUPERSEDED 2026-09-11 by D10.** Ruled 2026-09-03 as a full-width row on the board card
      (mockups and the tap-target argument:
      https://claude.ai/code/artifact/ec3cffcb-44c8-4b2b-978a-6927157132e3). The row does not fit
      (D6) and is no longer wanted (D10). What survives is its reasoning: a control must be its own
      tap target, and must not be drawn when there is nothing to watch.
- [x] **D3 — RESOLVED 2026-09-03: both picks are explicit records.** Best and latest are each a stored
      filename; neither is found by scanning the directory. This deletes a class of bug rather than
      documenting it — see D3.
- [x] **D5 — RESOLVED 2026-09-03 (provisionally): back to the rack, same card still expanded.**
      Owner's words were "let's try that first" — reopen it if leaving a replay feels like being put
      back in a menu. One call site, cheap to change. See D5.
- [x] **D10 — RULED 2026-09-11: one Replay control, and it asks which ride.** The existing ride-bar
      Replay button opens a list instead of playing the last ride immediately. Owner's call, and it
      dissolves D2/D6 rather than settling them. **Its list contents want one confirmation** — see
      D10.
- [x] **D6 — CLOSED 2026-09-11 by D10, not answered.** The finding stands and is why the row is
      gone: a device capture shows the card bottoms and the action row nearly touching, and the rack
      sits inside a scale box, so the cost of adding a row is not a clipped card, it is the whole
      screen shrinking. Nothing is added to the card now. See D6.
- [ ] **D7 — OPEN, and D10 made it mandatory.** `bReplayActive` is never cleared anywhere; playback
      holds on the final frame forever and the only way out is a level restart. A list you come back
      to needs a real exit, so the restart-only answer is no longer available — what is open is how
      the exit is built, not whether. See D7.
- [ ] **D8 — OPEN: "the ride ends" is five different events**, one of which fires mid-wave, and at
      the fall it fires *before* the trace file is closed. FR2/FR3's ordering needs one of them
      named. See D8.
- [x] **D9 — RESOLVED 2026-09-11: nothing younger than seven days is pruned.** Owner's ruling,
      after the review found that FR3 as drafted would delete developer traces. FR3 is rewritten
      around it; a residue remains for captures older than a week. See D9.
- [ ] FR1–FR9 unimplemented.

## Overview

Two pieces already exist and do not know about each other.

**Replay works.** `on-device-ride-replay.md` plays back the last ride on the phone: physics off, the
board's transform driven straight from the recorded `board_*` columns, the wave kept in step by
`wave_frame`. Crucially it is **kinematic playback, not re-simulation** — so a trace replays exactly
as it was ridden, no matter how much the physics has been retuned since. That is the fact that makes
this feature possible at all, and it is the opposite of `input-trace-replay.md`, which re-feeds input
through live physics and deliberately diverges.

**Best scores work.** `SurfAssist::LoadBestRide(BoardId)` / `SaveBestRide(BoardId, ...)` keep one
number per board, shown on the board card as "YOUR BEST SCORE".

So the player can watch their *most recent* ride, and can read a number describing their *best* one.
They cannot watch the ride the number is about.

**The playback work is already done.** `LoadTraceFile(const FString&)` is factored out of
`LoadLatestTrace()` (`SurfboardPawn.cpp`) and takes an arbitrary path; `LoadLatestTrace` is only a
newest-by-timestamp picker in front of it. This feature is a different picker, plus the bookkeeping
that makes the picked file still exist.

## Objective

Make the best score something a player can **watch**, not just read — and in doing so give
`Saved/InputTraces/` the retention policy it has never had.

Since D10 the objective has a second half that follows from the first: **Replay stops meaning "the
last one" and starts meaning "which one?"**. The feature is no longer a best-ride button bolted
beside a last-ride button; it is one control that knows about more than one ride.

## Requirements

### FR1 — Selecting the trace is the whole feature. Do not build a second replay path.

`LoadTraceFile` already accepts any path and `StartReplay` already drives the board from it. The
implementation is a different file-picker and the records that make the pick possible.

Stated as a requirement because the alternative is attractive and wrong: a "best ride replay" that
grows its own loader, its own parsing and its own playback ends with two replay paths that drift, and
the second one will be the one nobody tests. If the two need different behaviour, that difference
belongs inside the one path.

### FR2 — Score and trace are one record, written together or not at all

Per board, the best is `{ score, traceFileName }`, updated atomically at the moment a ride beats the
previous best.

Today the best is a bare float. Storing the filename separately, or deriving it by timestamp, gives
three ways to end up with a score whose trace is gone: a prune that ran between the two writes, a
crash between them, or a file deleted by the OS under storage pressure.

**Never offer a dead button.** If the best record's trace is missing or unreadable, the replay
affordance is not shown at all — the score still displays. Degrading to "replay the latest ride
instead" is forbidden: it silently shows the player a different ride than the one they asked for,
under a label that says it is their best.

### FR3 — An explicit retention policy, because there is none today

**Nothing prunes `Saved/InputTraces/`.** One CSV per ride, ~15 columns at ~60 Hz, kept forever. That
is a slow leak on the phone right now that nobody has hit; this feature makes at least one file
permanent, so the policy can no longer stay implicit.

Keep, per board and **forever**: the **best** trace, and the **latest** trace (the one the existing
Replay button plays). Both are named by stored records, not found by scanning — D3. Neither is ever
pruned, at any age.

Everything else is pruned **once it is at least seven days old**. A trace no record references but
that is younger than that is kept.

The rule is therefore: **delete a trace only if no record references it AND it is older than seven
days.** Its first half is checkable against the records at any moment, which a rule phrased over file
timestamps alone is not; its second half uses the file's modification time for the one thing a file
timestamp is actually reliable for — how old a file is, rather than which of two rides came first.

**The grace period is not slack, it is the development machine** (D9, ruled 2026-09-11). This
directory is shared: `-RecordIntro` rails captures land in it, and so does every trace recorded on PC
while tuning. "Delete anything unreferenced" deletes the capture a developer made ten minutes ago to
stage as a rails intro, on the next run, with no warning. A week is long enough that nothing in a
working session is at risk, and short enough that the leak this policy exists to stop stays bounded.

Pruning runs at a defined moment — after a ride ends and the best has been decided — never mid-ride,
and never against a file that is currently open for writing or currently being replayed.

**This is the requirement most likely to cause data loss if implemented casually.** A prune that runs
before the new best is committed deletes the ride it was about to preserve. Order is: end ride →
decide best → commit the record → prune what the record no longer references *and* the grace period
no longer protects.

### FR4 — The trace records which board rode it

A `# board=<id>` line in the metadata block written by `StartInputTrace`.

It is not there today — the filename is `<prefix>-<timestamp>.csv` and the metadata carries session,
platform and map only. Without it a trace cannot be attributed to a board, which FR2's per-board
record and FR3's per-board retention both need, and D1 cannot be implemented at all.

**This must land before any trace worth keeping is recorded.** Every trace already on a device is
unattributable and always will be; that is fine now (no users, D4) and would not be later.

### FR5 — The replay shows the board that was ridden, and gives it back afterwards

A best-ride replay draws the board named in the trace, whatever the player is currently on. The
board they had is restored when the replay ends **by any route** — completion, early exit, back-tap,
or anything else that leaves the replay.

The replay is a recording. Playing a foamie's trajectory under a shortboard's mesh is a
straightforwardly false depiction of something presented as the player's own past ride — and since
board choice changes how a ride looks, it is the kind of wrong a player reads as the replay being
broken.

Because playback is kinematic, the substitution is **presentational only**: mesh and profile for the
duration, no simulation, no tuning coefficients, no assist level — none of it is consulted, because
nothing is being simulated. Do not route this through the full profile-application path; it would do
work the replay cannot use. See D1.

**The restore is the risky half, not the swap.** Entering happens once, on one path. Leaving happens
on several, and a replay that returns the player to the wrong board is worse than the feature is
good.

### FR6 — One Replay control, and it asks which ride

**There is exactly one replay affordance in the game** (D10): the Replay button already on the ride
bar — `ReplayButton_1` in `WBP_SurfboardControls`, beside Restart, Choose Board, Camera and Back.
Pressing it opens a **ride list**; picking a row plays that ride. Nothing is added to the board card,
and the rack goes back to doing one job.

The list:

- **One row per watchable ride**, carrying the board it was ridden on, which ride it is (LAST /
  BEST), and its score — `SHORTBOARD · BEST · 148`. The row *is* the number and the ride in one
  object, which is what D2 wanted the board card to become and could not afford to.
- **The last ride is the first row.** "Watch what I just did" is the common case and it must stay
  the shortest path: top of the list, under the thumb, no scrolling and no reading.
- **Only rows that lead somewhere.** A board never ridden has no row. A record whose trace is
  missing or unreadable has no row (FR2) — not a greyed one. If there is nothing at all to watch,
  the Replay button itself is not drawn, so the list never opens empty.
- **It pauses the world**, like the rack, and by the same mechanism — only if nothing else already
  has.

Two constraints inherited from D2, which survive it:

- **The control is its own tap target.** Trivially true now that it is not inside another button;
  stated because the bug D2 was avoiding — a near miss doing something else — is what killed the
  on-card placement.
- **It is never a dead control.** Nothing is offered that cannot be played.

A ride list is also what makes one Replay button honest. The alternative this spec had arrived at was
two Replay controls differing only by *which* ride they play — the exact thing FR6 originally
rejected for the ride bar, avoided by moving the second one to another screen rather than by not
having it.

### FR7 — Inert in every automated run

No best written, no trace pruned, no replay offered, in any headless, unattended, filtered-autopilot
or trace-replay run.

Two specific hazards here, beyond the usual gate:

- **Test runs record traces.** `bRecordInputTrace` is enabled in the playable level, so an automated
  run produces CSVs like any other. A prune that runs in a test run can delete a real best.
- **Test runs enable player controls after handoff** ([[test-runs-enable-player-controls]]), so "this
  is a test" is not implied by anything the feature can observe locally. Gate on the `surf.autopilots`
  CVar plus `bExternalWeightOverride`, as every other controls-gated feature does.

### FR8 — Storage extends the existing seam

The `{score, traceFileName}` record extends `SurfAssist`'s `LoadBestRide` / `SaveBestRide` pattern —
same `[SurfAssist]` config section, same `IsPersistenceEnabled()` switch, and `ResetCredit()` clears
the filenames along with the scores. No `USaveGame`; `gradual-control-handoff.md` D2 ruled that out
and the ruling holds.

`ResetCredit()` clearing the records must also prune the traces they referenced, or "reset" leaves
orphans that nothing will ever delete.

### FR9 — A listed ride carries what its row shows

A row reads `SHORTBOARD · BEST · 148`, so board, kind and score must be recoverable for **every**
listed ride, after an app restart, without opening the trace.

Per board, therefore, **two records of the same shape** — the best and the latest —
`{ score, traceFileName, boardId, durationSeconds, recordedAt }`. FR2's atomicity rule applies to
both: a record is written whole or not at all.

This is a change in kind, not size. The latest trace is a *filename* in D3, because the only thing
anyone asked of it was "play it". A list asks it to describe itself before it is played, and the
score of the last ride is currently nowhere on disk at all — it lives in `AssistRideCreditSeconds`
until the app closes. Without this, a fresh launch can offer to replay your last ride but cannot say
what it was worth, and the row is half-blank for the entry the player reaches for most.

`boardId` is recorded in the trace as well (FR4). The record is what the list reads — the trace's
copy is what makes an orphaned trace still attributable, and what D1's board swap reads when the
replay actually starts.

### NFR3 — One layer owns the screen at a time

**The button bar is UMG. Everything this feature draws is C++ Slate.** When the two are interactive
at once, one of them stops responding to taps — which has cost this project real time more than
once, and is the single most likely way for the ride list to arrive broken.

The project already has the contract that prevents it. **The list joins it; it does not invent its
own.**

- **`ASurfboardPawn::bRideUIBlocked` is the flag every UMG widget binds its Visibility to.** One flag
  rather than a per-modal AND, so a new modal hides the whole bar for free instead of needing every
  widget rewired; and a plain `BlueprintReadOnly` property rather than a function, so a binding can
  be repointed without rebuilding the graph. The ride list sets it, exactly as
  [`OpenBoardPicker`](../Source/GoneSurfing/SurfboardPawn.cpp#L4940) does.
- **It is written at the transition, never polled from Tick.** These panels pause the world, so the
  pawn stops ticking the moment one opens and a polled flag could never turn true. The rack sets it
  before opening and clears it from its `OnClosed` hook; the list needs both halves, on every path
  out.
- **Slate modals install at ZOrder 260** — the modal tier the rack and the assist panel share, on
  the understanding that two of them are never up together. A ride list opened *from* the rack would
  break that assumption; opened from the ride bar, it does not.
- **`CanShowTouchControls()` gates on the same flag**, so the Slate joystick and pump stand down for
  the same reason the UMG bar does.

**The new hazard is the replay itself, and it is specific to D10.** A replay does *not* set
`bRideUIBlocked` today — the UMG bar deliberately stays up, because Replay and Restart are how a
player leaves a frozen final frame. D7 adds a Slate way out, and D5 brings the Slate list back
afterwards, so for the first time a Slate control and a live UMG bar would both want the same tap.

**The rule: during a replay, either the exit is the existing UMG bar and no Slate control is
interactive, or the bar is hidden through `bRideUIBlocked` and the exit is Slate. Never both.**
Whichever D7 chooses, it chooses for the whole replay — and the replay overlay itself stays
`HitTestInvisible` either way.

### NFR1 — Bounded storage

At any moment the device holds at most `(boards × 2)` **permanent** traces — a best and a latest
each — plus the one being written, plus whatever the last seven days of riding produced (FR3's grace
period). A week of idleness therefore drains back to the permanent set, which is the bound that
matters: the directory cannot grow without limit.

The transient term is real and deliberately unbounded-per-week: a heavy session week holds more
files than a quiet one. If that turns out to bite on a phone, **the lever is the age, not the rule**
— shorten the grace period, do not start pruning by count, which would put the developer captures
the grace period exists to protect back at risk.

### NFR2 — A replay is never confused with a live ride

Inherited from `replay-mode-clarity.md`, which already solved this: letterbox, REPLAY badge,
progress, end card. A best-ride replay uses the same treatment, and its badge should say which ride
it is — watching your best is a different thing from watching your last, and the screen should say
which one it is showing.

## Decisions

### D1 — Best trace's board vs the selected board — **RESOLVED 2026-09-03: swap it in**

Ruled by the project owner: temporarily apply the recorded board for the replay and restore the
previous state after. The narrower option — offer the replay only for the currently selected board —
was recommended and rejected.

The owner's call buys the thing the narrow option gave up: a best stays watchable regardless of what
is currently being ridden, which is what makes cross-board comparison ("watch my best on each") work
at all on a screen that exists for comparison.

**What "swap" has to mean here, precisely.** The replay is kinematic — the board's transform comes
straight from the trace and no physics runs on it. So the swap is a *presentation* substitution: the
recorded board's mesh and profile are applied for the duration, and nothing about tuning coefficients
or assist level matters, because nothing is being simulated. That is a much smaller operation than
`ApplyBoardProfile` in its normal role, and the distinction is worth keeping — a swap that ran the
full profile-application path would be doing work the replay cannot use, on a path where a
half-applied profile is a mess.

**Restore is the part that can fail.** Entering is one-way and easy; leaving happens on ride end, on
an early exit, on a back-tap, and on whatever the player does next. Every one of those paths has to
put the board back. A replay that leaves the player riding the wrong board is worse than the feature
is good.

**This puts pressure on D2, and it is the one thing the two rulings do not settle between them.**
With the row living on the expanded card (D2), reaching a board's best replay means tapping that
card, which selects that board. So in the common case the swap is a no-op and the player has changed
their next-wave board just by looking. The swap earns its keep in the case that motivated it — a
deferred mid-ride change ([`IsChangeDeferred`](../Source/GoneSurfing/BoardPanel.h)), where the
selected card is already not the board underneath the player. See D5's second paragraph; whether
"you must select a board to watch its best" is acceptable is the residue, and it is small — selection
is reversible and mid-ride it does not even take effect.

### D2 — Where the entry point lives — **SUPERSEDED 2026-09-11 by D10**

> **The placement below is not being built.** It does not fit (D6) and it is not wanted (D10): there
> is one Replay control now, on the ride bar, and it opens a list. Kept because its reasoning is the
> reason the list looks the way it does — a control must be its own tap target, and must never be
> offered when there is nothing behind it. Ruled 2026-09-03; superseded 2026-09-11.

Ruled by the project owner. Three placements were mocked at true scale — the score value as the
control, a full-width row beneath it, and the whole best-score row made pressable:
https://claude.ai/code/artifact/ec3cffcb-44c8-4b2b-978a-6927157132e3

**As built it is a row: `▶ WATCH YOUR BEST RIDE`, 280 × 30, under the best-score line**, in the same
rounded-pill shape as Restart, Replay and Back, so the shape says "button" before a word of it is
read.

The deciding argument is tap targets, not looks. **The whole card is already an `SButton` whose job
is "select this board"**, so anything tappable inside it is a button inside a button — and a near
miss does not do nothing, it switches boards. Putting the control on the score value gives a
~46 × 14 px target inside a 300 × 400 one, on a screen that has already been fixed twice for exactly
this class of bug (`a9665a9ea` made the card clickable at all; `60f6940f6` made the whole pill the
target rather than the drawing inside it). Making the existing best-score row pressable fails
differently: that row spans the card *specifically* so it reads as a summary line rather than a
fourth rating (`463cf6d03`), and giving it a border to look pressable turns it back into a box — at
which point it is the full-width row, only shorter and harder to hit.

**The ~42 px of card height it costs is paid for twice, by rules already in the spec:**

- **Only on the expanded card.** One card is expanded at a time and it is the one that already
  widened to 300 px for its description — the single card with room by construction. The other four
  stay 172 px with nothing new inside them and tap = select, unchanged.
- **Not drawn when there is nothing to watch.** A board never ridden shows "—", so FR2's
  never-offer-a-dead-button removes the row entirely, handing the height back on exactly the cards
  that do not need it.

So the row is on screen only when it is both meaningful and affordable.

Still to verify on a `-Phone` capture ([[screenshot-the-game-on-desktop]]): the card was once sized so
tightly it put its own close button off the bottom of the phone, so the 42 px has to be found in a
real capture, not assumed from this arithmetic.

### D3 — How the two surviving traces are picked — **RESOLVED 2026-09-03: both picks are explicit records**

Ruled by the project owner. Best and latest are each a **stored filename**; neither is discovered by
scanning the directory.

This is the option that deletes a bug rather than documenting one. `LoadLatestTrace` currently finds
the newest `*.csv` by file timestamp, which is only correct as long as "newest file" and "most recent
ride" are the same thing. The moment a best trace is preserved while newer ones are pruned, they come
apart, and the existing in-ride Replay button silently starts replaying the best ride instead of the
last one — with no error, no log, and a player who thinks the game is broken because Replay showed
them a ride they did not just do.

Making both explicit also means the retention rule in FR3 has something to be expressed *against*:
prune what the records do not reference. A rule phrased over records is checkable; a rule phrased
over timestamps has to re-derive what it is protecting every time it runs.

**Consequence for `LoadLatestTrace`:** it stops being a directory scan. Keep the scan only as a
fallback for a device that has traces but no records yet — and log when the fallback fires, because
after this feature ships that state means the records were lost.

### D4 — Score-format changes and stored bests — **RESOLVED 2026-09-03: nothing to migrate**

Ruled by the project owner: there are no users, so changes to the scoring system carry no migration
cost. Trick scoring (`trick-scoring.md`) can change what a score means without this feature needing
versioning, a reset ceremony, or comparability logic. Bests on a dev device are throwaway.

**This ruling expires at first release, and it will expire silently.** After that, a scoring change
makes every stored best a number produced by rules that no longer exist, sitting next to a replay of a
ride those rules scored — and nothing in the record will say so. The cheap insurance, if it is ever
wanted, is a format version in the record from the start; the decision here is deliberately *not* to
pay for it yet. Anyone reopening this should reopen it before shipping, not after.

### D5 — Where the player lands when a best-ride replay ends — **RESOLVED 2026-09-03: back where they left; RETARGETED 2026-09-11 by D10**

> **Retargeted, not reopened.** The ruling was "return the player to the screen they launched the
> replay from, with their place in it kept." That screen is now the ride list (D10), not the rack, so
> read "the rack, same card expanded" below as "the list, same row still highlighted". The reasoning
> transfers exactly — and it gets stronger, because a list is somewhere you come back to on purpose,
> where the rack was somewhere you happened to be.

Ruled by the project owner, explicitly as a first attempt: *"I think D5 should return back to the
card. Let's try that first."* Treat it as provisional — it is one call site (see below), and the
argument against it only becomes visible with a phone in hand.

The rack **pauses the world**, so tapping the row starts a sequence rather than an action: close the
rack, unpause, apply the recorded board (D1), load the trace, run the replay badged as BEST — and
then back to the rack, **with the same card still expanded**. Not merely "the rack reopens": the
player left from a specific card and the comparison they were in the middle of should resume where
they left it, not at whatever the rack considers its default.

The reasoning: the player came from a comparison screen, and the thing they most likely want next is
another board's best. Dropping them into a live wave ends that comparison at the moment they were
most engaged with it.

The argument against, kept because this is provisional: returning to a paused menu is one more tap
before surfing, for a player who only wanted to watch one ride and get back on the wave. That gets
stronger if D1's board swap turns out to feel heavy. **Watch for it on the first device session** —
if leaving the replay feels like being put back in a menu rather than returned to what you were
doing, this is the decision to reopen, and it is cheap to reopen because it is one destination.

**Ordering constraint this creates.** D1's restore must complete *before* the rack redraws, or the
card comes back showing the board the replay borrowed as the selected one — a wrong selection, in the
one place the player reads their selection from.

### D6 — There is no 42 px under a card — **CLOSED 2026-09-11 by D10, not answered**

> **The finding stands; the question went away.** D10 moved the entry point off the board card
> entirely, so nothing here needs ruling any more. The evidence is kept because it is *why* the row
> is gone, and because the two mechanisms below — a shared bottom block across all five cards, and a
> scale box that pays for overflow in point size — constrain anything anyone ever adds to this
> screen again. The options at the end were never ruled on; **option A was, in effect, chosen and
> then improved on**: D10 puts the control on the ride bar the player already uses, instead of a
> third button in the rack's action row.

D2 ruled the entry point a full-width row under the best-score line, and closed by saying the 42 px
it costs "has to be found in a real capture, not assumed from this arithmetic." It was not found.

![The rack on the phone: the expanded card's YOUR BEST line and the action row, with nothing between
them](images/board-rack-no-room-under-the-cards.png)

The expanded card runs from under the title to within a hair of the action row; `YOUR BEST 148` is
the last line on it, and RESUME WAVE / RESTART WITH FOAMIE sit 20 px below. **The arithmetic in D2
was wrong in two ways, and the second is the expensive one.**

**1. The expanded card does not pay for it — all five cards do.** The ratings block and YOUR BEST
hang off the bottom of every card behind a single `FillHeight(1)` spacer
([BoardPanel.cpp:439](../Source/GoneSurfing/BoardPanel.cpp#L439)), precisely so those four lines land
on one baseline across the rack; and cards are stretched to the tallest in the row. A row added under
YOUR BEST on the expanded card therefore lengthens that shared bottom block and grows **the row**,
not the expanded card's own slack. D2's "the single card with room by construction" does not hold:
the four narrow cards grow with it.

**2. The overflow does not clip, it shrinks.** The whole rack is inside an `SScaleBox` with
`StretchDirection::Both` ([BoardPanel.cpp:223](../Source/GoneSurfing/BoardPanel.cpp#L223)), and the
phone is already the binding screen — showing all five descriptions at once scaled the rack to ~0.6
and made the type unreadable, which is the entire reason only the selected card carries a
description. So 42 px of new content is not paid in white space. It is paid in point size, on every
card, on the screen the type is hardest to read on.

That is also why "just let the row get taller" is not an answer: the floor
([`MinDesiredHeight(462)`](../Source/GoneSurfing/BoardPanel.cpp#L251)), the 20 px gap and the 68 px
button row all sit inside the scaled block, so anything that grows any of them shrinks all of them.

**Options, for a ruling:**

- **A — A third button in the action row: `▶ WATCH FOAMIE'S BEST`, beside RESUME WAVE and RESTART
  WITH FOAMIE.** *Recommended.* It costs zero vertical space, and the horizontal room is plainly
  there in the capture. It is also the idiom this screen already uses: RESTART WITH FOAMIE acts on
  the expanded card and names it, which is exactly what this does. And it satisfies D2's real
  deciding argument — tap targets — better than the row ever did: a 68 px button outside the card
  cannot be a near-miss on the card. What it gives up is adjacency to the number it plays, which D2
  liked; the button naming the board recovers most of that. Watch one thing: FR2 removes it on a
  never-ridden board, and a three-button row recentres when it goes, moving the other two.
- **B — The row, but inside the card ABOVE the `FillHeight` spacer** (under the description). Spends
  the expanded card's own slack instead of the shared bottom block, so it is much cheaper than D2's
  placement. Not free: the foamie has the longest description and the least slack, and the foamie is
  the tallest-card case that sets the row height.
- **C — Make the YOUR BEST row itself pressable.** Already rejected in D2, on tap-target grounds and
  because that row spans the card specifically so it reads as a summary rather than a fourth rating.
  Nothing in the capture changes that; listed only so it is not re-proposed as "the cheap one".
- **D — Buy the height back** (shorter descriptions, tighter paddings). Rejected as a first move: it
  pays for a new control with the readability this screen was tuned twice to protect.

Whatever wins, **D5 is unaffected**: "the same card still expanded" is the rack's provisional
`PendingIndex`, which no placement here touches.

**A correction to D1 while this is open.** D1's closing paragraph worries that reaching a best replay
means tapping a card, "which selects that board", making the swap a no-op in the common case. It does
not: a card tap only moves `PendingIndex`, and
[only RESTART commits it](../Source/GoneSurfing/BoardPanel.cpp#L658). The swap is therefore always a
genuine cross-board substitution, D1's residue disappears, and D5's "the card shows the player's own
board as selected, not the borrowed one" falls out for free — RIDING NOW reads `GetActiveIndex`,
expansion reads `PendingIndex`.

### D7 — A replay has no end and no exit — **OPEN; D10 removed one of its two answers**

> **D10 makes the exit mandatory.** A list is a place you come back to — watch, return, watch
> another — so the "leaving means restarting the level" branch below would let a player watch exactly
> one ride per level load and then drop them onto a fresh wave. That is not a list, it is a menu that
> costs you your wave. What is open is now *how* the teardown is built, not whether.

FR5 requires the board to be restored "when the replay ends **by any route** — completion, early
exit, back-tap"; D5 requires the rack to come back. **None of those routes exists.**

`bReplayActive` is set true in [`EnterReplayMode`](../Source/GoneSurfing/SurfboardPawn.cpp#L3441) and
is **never set false anywhere in the codebase**. When playback reaches the last row,
[`TickReplay`](../Source/GoneSurfing/SurfboardPawn.cpp#L4014) sets `bReplayHolding` and freezes on
the final frame indefinitely; the end-card offers "Replay to watch again • Restart for a new ride".
The only way out is `RestartLevel()`, which reloads the map into a fresh ride.

So "the replay ends" has to be *built*: tearing replay mode down in place means unfreezing the mesh,
re-enabling the force pipeline (`SetForcePipelineTicking`), restoring controls and removing the
overlay — a teardown nobody has written, because nothing has ever left a replay. That is work FR1's
"the picker is the whole feature" does not cover, and it should be scoped before this is estimated.

**The ruling needed:** does this feature build a real exit (a Done/Back control that returns to the
rack without a level reload), or does "ends" mean the existing restart?

- If **restart** — *ruled out by D10, kept for the record*: FR5's restore would have come free (the
  board subsystem's active index was never changed, so BeginPlay re-applies the player's own board),
  and D5's return would have needed a flag surviving level travel, for which
  `GSkipStartScreenOnNextLoad` ([SurfboardPawn.cpp:697](../Source/GoneSurfing/SurfboardPawn.cpp#L697))
  is the precedent. The cost — watching a ride throws away the wave you were on — was tolerable for
  one replay and is not for a list.
- If **a real exit**, FR5's restore is the cheap half: D1's presentational swap is exactly
  [`SurfBoards::ApplyVisuals`](../Source/GoneSurfing/SurfboardPawn.cpp#L4802), called with the
  recorded profile and then called back with the player's. The expensive half is the teardown above
  — and it is precisely the path FR5 predicts will be got wrong.

### D8 — "The ride ends" is five events, one of them mid-wave — **OPEN**

FR2 commits the record "at the moment a ride beats the previous best" and FR3 orders it
*end ride → decide best → commit → prune*. Both read as though there were one ride-end. There are
five: [`EndAssistRide()`](../Source/GoneSurfing/SurfboardPawn.cpp#L4593) is called from `EndPlay`,
from `TriggerFall`, from `SetAssistMode`, from `ResetAssistCredit`, and from `UpdateAssist`
[whenever assist goes suppressed](../Source/GoneSurfing/SurfboardPawn.cpp#L4652) — with
`BeginAssistRide` re-latching when it un-suppresses. One wave can therefore end and begin several
"rides" while a single trace file stays open across all of them.

Two concrete consequences:

- **A suppression-triggered end would commit a record pointing at a file that is still growing.** The
  trace goes on accumulating the rest of the same wave, so the replay shows more than the ride that
  was scored — under a label that says it is the best.
- **At the fall, the best is decided while the file is open.** `TriggerFall` calls `EndAssistRide` at
  [line 5039](../Source/GoneSurfing/SurfboardPawn.cpp#L5039) and `StopInputTrace` only at
  [line 5132](../Source/GoneSurfing/SurfboardPawn.cpp#L5132). Committing there is fine; **pruning
  there is not**, and FR3's warning about open files applies on every platform, not only Android.

**Recommendation:** name the commit point explicitly rather than inheriting `EndAssistRide`'s
definition — commit on the fall and on `EndPlay`, the two events that also close the file, and let
the other three go on banking credit as they do today without touching the record. This changes when
a best can be set, so it is the owner's call.

### D9 — Pruning vs. developer traces — **RESOLVED 2026-09-11: nothing younger than a week is pruned**

Ruled by the project owner. FR3 is rewritten around it.

The problem the ruling fixes: `Saved/InputTraces/` is not the player's directory, it is everyone's.
`-RecordIntro` rails captures are written there by
[`StartInputTrace`](../Source/GoneSurfing/SurfboardPawn.cpp#L2845) like any other trace, and so is
every trace recorded on PC during tuning. FR3 as drafted — "delete any trace no record references"
— would have deleted the capture a developer made minutes earlier to stage as a rails intro, on the
next run, silently. The seven-day grace period means no trace is ever pruned during the session that
made it, or the week around it.

**The residue, stated so it is not a surprise:** a capture *older* than a week that was never staged
into Content is still unreferenced, and still gets deleted. If that bites, the cheap fix is to exempt
traces recorded with a non-empty `InputTraceSessionPrefix` — which is what the `-RecordIntro` and
tuning captures use and no player trace has — rather than to lengthen the period for everyone.


### D10 — One Replay, and it asks which ride — **RULED 2026-09-11: combine the two entry points**

Ruled by the project owner: *"We already have a button for Replay (last ride), the spec now suggests
adding another button for Replay (best ride). It should be possible to combine them together, so that
replay shows a list of rides you can replay."*

It is the right call, and it is worth being precise about what had gone wrong. FR6 rejected putting
the best-ride control on the ride bar because it "would sit beside the existing one differing only by
a word" — and then D2 satisfied that by moving the second control to *another screen*, which is
avoiding the collision rather than the duplication. Two controls whose only difference is which ride
they play are two controls too many wherever they are standing.

**The design.** The ride bar keeps the one Replay button it already has
(`ReplayButton_1` in `WBP_SurfboardControls`). It stops meaning "play the last ride" and starts
meaning "watch a ride", opening a list. FR6 carries the list's rules.

**What it buys:**

- **D6 and D2's geometry stop mattering.** Nothing is added to the board card, the 42 px is not
  needed, and the rack is a board picker again rather than a board picker with a replay feature
  bolted to it.
- **The number and the ride become one object.** D2's real argument for the card was that a screen
  which states a number and offers to play it explains itself. A row reading
  `SHORTBOARD · BEST · 148` does that more directly than a button under a score — and does it for
  every board at once, with no tapping through cards to compare.
- **It has somewhere to grow.** Best-per-board is five rows; D9's grace period means the last week's
  rides are on disk anyway, so "recent rides" is a section this list could gain later without any
  new retention policy. Not in scope now — noted so the shape is not designed against it.

**What it forces. None of this is free:**

1. **D7 becomes mandatory.** See its banner. A list without a working exit from replay is a list you
   can use once per level load.
2. **Every listed ride must describe itself** — FR9. The last ride's score is currently nowhere on
   disk, which nobody noticed while the only question asked of it was "play it".
3. **The common case costs one more tap.** Today Replay is one press and you are watching what you
   just did; now it is press, then the first row. Mitigated by making the last ride the first row
   and putting it under the thumb, and it buys the player something they could not do before — but
   if the extra tap grates on device, the lever is to make the first row big, not to bring the second
   button back.
4. **The button is Blueprint, the list is C++ Slate, and those two layers fight.** `ReplayButton_1`
   calls into the pawn; the pawn's `ReplayLastRide()` keeps working for the console alias and the
   `IA_Replay` key, and gains a sibling that opens the picker. Two separate hazards follow:
   - **Input.** A live UMG bar under a live Slate panel means one of them stops responding. NFR3 is
     the contract that prevents it, and the button that opens the list is the first thing that has
     to disappear when the list is up.
   - **Compilation.** If the Blueprint's call has to change,
     [[bp-signature-change-requires-node-refresh]] applies: Refresh Node + Compile in the widget, or
     the graph dies silently and the board free-falls.

**One thing to confirm: what is in the list.** Spec'd above as *the last ride, then each board's
best* — bounded, every row meaningful, and exactly the set FR3 already keeps forever. The alternative
is to list the last week's rides too (they exist, by D9), which turns a 6-row list into a scrolling
one full of 4-second wipeouts. Recommendation is to ship the curated list and see whether anyone
misses the rest.


## Acceptance criteria

```
GIVEN a board whose best ride was recorded several physics retunes ago
WHEN  the player replays it
THEN  it plays back exactly as it was ridden
```
```
GIVEN a ride that beats the board's previous best
WHEN  the ride ends
THEN  the score and the trace filename are committed as one record
AND   the previous best's trace is pruned only after that record is committed
```
```
GIVEN a best record whose trace file is missing or unreadable
WHEN  the ride list is opened
THEN  the board card still shows the score
AND   the list offers no row for that ride
AND   the latest ride is NOT substituted for it
```
```
GIVEN a player who has ridden many rides on several boards, none in the last seven days
WHEN  the traces directory is inspected
THEN  it holds at most two traces per board plus any in-progress recording
```
```
GIVEN a trace recorded today that no record references — a developer's `-RecordIntro` capture,
      or a ride that beat nothing
WHEN  pruning runs
THEN  it is still there
```
```
GIVEN the existing in-ride Replay button
WHEN  a best trace on disk is newer than the most recent ride's trace
THEN  Replay still plays the most recent RIDE, not the best one
```
```
GIVEN a player riding one board who replays their best ride on a different board
WHEN  the replay ends, by completion or by leaving it early
THEN  the replay showed the recorded board throughout
AND   the player is back on the board they had before it started
```
```
GIVEN a replay launched from the ride list
WHEN  the replay ends
THEN  the list is shown again with that same row still highlighted
AND   nothing anywhere shows the borrowed board as the player's own
```
```
GIVEN a fresh app launch, and a player who has ridden several boards
WHEN  the ride list is opened
THEN  the first row is the last ride
AND   every row states its board and its score
```
```
GIVEN the ride list is open over the UMG button bar
WHEN  the player taps anywhere on either
THEN  exactly one of them is interactive, and it is the one on top
```
```
GIVEN a ride watched from the list
WHEN  it finishes
THEN  the player can pick another ride without restarting the level
```
```
GIVEN a player who has never ridden anything
WHEN  the ride bar is drawn
THEN  the Replay button is not drawn at all
```
```
GIVEN a best-ride replay in progress
WHEN  it is on screen
THEN  it is unmistakably a replay, and identifiable as the BEST ride rather than the last
```
```
GIVEN any headless run (unattended, filtered autopilot, or trace replay)
WHEN  the run records a trace and ends
THEN  no best is written, nothing is pruned, and no existing best trace is deleted
```
```
GIVEN a player who taps "Reset"
WHEN  the reset completes
THEN  the best records are cleared AND the traces they referenced are deleted
```

## Test cases

1. **`best_replay_survives_retune`** — record a ride, change a physics coefficient materially,
   replay. The trajectory must be identical; this is the kinematic-playback guarantee and the whole
   premise. If it fails, the feature is not viable in the form specified.
2. **`best_record_atomicity`** — kill the process between the score write and the filename write (or
   simulate it) and assert the next launch offers no dead button.
3. **`prune_never_eats_the_new_best`** — a run of rides where each beats the last; assert the best
   trace always exists afterwards. The ordering bug FR3 names is easiest to catch by brute force.
4. **`replay_button_still_plays_latest`** — the D3 interaction: contrive a best file newer than the
   latest ride's file and assert the in-ride Replay button is unaffected.
5. **`prune_spares_the_last_week`** — a directory holding a referenced best, an unreferenced trace
   from today and an unreferenced trace back-dated eight days; assert only the last one goes. The
   back-dating is the test: the rule cannot be exercised by riding.
6. **`ride_list_describes_every_row`** — launch fresh (no in-memory state), with records for a
   best and a latest on two boards; assert each row names its board and score, that the last ride is
   first, and that a record whose file was deleted behind the game's back produces no row.
7. **`best_replay_inert_headless`** — full baseline sweep. Assert `Saved/InputTraces/` is not pruned
   and no best is written. Check `full-iso` dates first ([[stale-csvs-fake-regressions]]).
8. **`board_restored_after_replay`** — exercise every exit from a best-ride replay (completion, early
   exit, back-tap) and assert the board is restored each time. One path per test; the bug this
   catches lives in the path nobody remembered.
9. **Device check** — bests and traces live in the app's writable `Saved/` on Android. Verify pruning
   and reading both work on device, not just in the editor; file deletion semantics differ.

## Implementation details

Suggestions, not prescriptions.

- **`LoadTraceFile(const FString&)` already exists and already does this.** The picker is the new
  code. Resist growing a second loader (FR1).
- **Do not reuse `ResolveRailsTracePath`.** Its Content-first lookup exists because rails traces are
  staged into the packaged build; player traces live only in the app's writable `Saved/` and a
  Content lookup would be dead weight at best and a wrong hit at worst.
- The board id for FR4 goes in the metadata block in `StartInputTrace`, alongside `# session=`,
  `# platform=` and `# map=`. `LoadTraceFile` will need to surface it — the `#` block is currently
  skipped, not parsed.
- Pruning wants `IFileManager::Delete`, plus `IFileManager::GetTimeStamp` for FR3's seven-day test —
  the same call `LoadLatestTrace` uses today to pick the newest file. On Android a file still held
  open will not delete, so prune after the recorder has closed the ride's file, not at ride-end-tick
  (D8 shows the fall path ending the ride ~90 lines before it closes the file).
- Best records belong next to `LoadBestRide` / `SaveBestRide` in `SurfAssist`, sharing
  `IsPersistenceEnabled()` and cleared by `ResetCredit()`.
- The replay presentation is `replay-mode-clarity.md`'s existing overlay. It needs one more piece of
  state — which ride this is — not a new overlay.
- If this touches a `BlueprintCallable` signature, calling Blueprints need a manual Refresh Node +
  Compile or the whole graph dies silently and the board free-falls
  ([[bp-signature-change-requires-node-refresh]]).

## Related

- `specs/on-device-ride-replay.md` — the kinematic replay this reuses; read its comparison table first
- `specs/input-trace-replay.md` — the *other* replay, which re-simulates and diverges; not this one
- `specs/replay-mode-clarity.md` — the LIVE-vs-REPLAY presentation (NFR2)
- `specs/board-selection.md` — the board card and per-board best score (D2)
- `specs/trick-scoring.md` — what a score is about to mean; D4 rules that this costs nothing now
- `specs/ride-score-counter.md` — where best-per-board is stored today
