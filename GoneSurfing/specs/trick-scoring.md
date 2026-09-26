# Spec: Trick scoring — turns count, not just survival

## Status

**Built and merged to main.** Spec drafted 2026-09-03; implemented 2026-09-11 (`ed354610a`, "Score
turns, and pay them over the seconds after the turn"), corrected on device 2026-09-12 (`84ec8c471`
no words on the counter, `38a43f8a2` FR5 read the wrong guard flag). `TrickScoring.h/.cpp` is the
detector + window (pure, caller-owned `FState`, the `SurfAssist` shape); the pawn ticks it beside
`AccrueRideCredit` and holds `TrickState`; `RideScore::PlayCelebration(World, bBigTurn)` is the
re-exposed pulse; the wipeout card and the ride list show `TrickScore::ScoreShown` with the board
multiplier. `surf.score.holdpeak 1` pins the celebration for captures. Still open: D5, D6 and the
big-turn bar — all device tuning, none blocking. **FR12 added 2026-09-17**: whitewater pays a
quarter via the foam-density `brokenAmount`; device calibration of the foam thresholds owed.
**D9 2026-09-18: the window paid a tenth of what the owner expected**, for two reasons found by
replaying a device ride — its base shared the survival term's out-of-band quarter, and the
first-cut sizes capped a chain near 4 credit-seconds. Fixed in one commit; the owner's target ride
still owes a device confirmation. **FR12 RETIRED the same day**: the score ignores whitewater
(`ScoreWhitewaterCreditRate` 1.0) — `brokenAmount` false-positives through every turn toward the
pocket and was docking the hard rides the score exists to reward; the owner had already retired the
same signal from the hydrofoil drive cut.

- [x] **D1 — RESOLVED 2026-09-03: void, there is no graduation.** Assist level is a fixed board
      property (a foamie is 4 / `Full` forever), so nothing reads credit to decide anything. Trick
      points are score and only score. This also killed a silent defect in the draft — see D1.
- [x] **D2 — RESOLVED 2026-09-03: airtime is out of scope.** Owner's ruling: the board *can* catch
      air today, but not realistically. Air physics gets fixed first, and the scoring is designed
      against the fixed behaviour rather than against the current one. Do not add a placeholder.
- [x] **D3 — RESOLVED 2026-09-11: one rule for every board, and difficulty pays instead.** No
      per-board thresholds. One set of thresholds, calibrated against the weakest board that should
      still score, plus a per-board multiplier read from the difficulty the rack card already shows
      (FR11). Owner: *"I don't think thresholds per board is necessary. I think possibility for
      scoring more for boards with higher difficulty makes sense."*
- [x] **D4 — RESOLVED 2026-09-03: reuse the level-up celebration, on big turns only.** Owner likes
      the effect and it is stranded now that no assist level ever drops. Two tiers (FR6): ordinary
      scored turns move the counter, big turns get the full snap + burst + caption.
- [x] FR1–FR11 implemented (`ed354610a`); FR5's flag fixed in `38a43f8a2`; FR6/FR10 amended per D7.
- [x] **FR9/FR10 added 2026-09-11: a turn is paid over the seconds AFTER it, not at its exit.**
      Owner's framing: *"making a hard turn and then fall is easy, making a hard turn and then
      surfing back into the non assist area is difficult, and doing multiple hard turns in a row is
      even more difficult."* A turn opens an earning window instead of handing over a lump.
- [x] **First-cut values measured 2026-09-11** from 89 s of real riding across 8 device traces — see
      "First-cut values". Enough to implement against; the owner tunes on device from there.
- [x] **D7 — RULED ON DEVICE 2026-09-12: no words, no chain badge. The number is the feedback.**
      First device session. The caption ("BIG TURN" / "AGAIN!" / "LINKED!") and the ×2 / ×3 chain
      badge under the counter both went: reading pulls the eyes off the wave, and it was not obvious
      what they meant. Every scored turn now snaps the number; a big one adds the burst. FR6 and
      FR10 are amended below.
- [x] **D8 — FOUND ON DEVICE 2026-09-12: FR5's discard threw away eleven of twelve turns.** On the
      graduated path (alpha 0 — every board past the funboard) `bAssistGuardActive` means "outside
      the crest band", not "the assist is steering", and a sharp turn always leaves the band. Fixed:
      the discard now requires the assist to have authority (alpha > 0). The big-turn bar is also
      down 0.80 → 0.65: of sixteen turns that survived the bug, one cleared 0.80. See D8.
- [ ] D5 — how much of a turn's value is paid at the exit vs collected in the window. Set to ~20/80
      by D9's sizes (lump 2.0 credit-s × grade; window ~7.6 for an isolated big turn); device
      confirmation of the *feel* still open.
- [ ] D6 — how long the window runs. 6 s after D9; the floor is now measurable in-game — the `tricks`
      dump carries `d2crest` and `band=in|OUT` — but has not been measured across a set of rides.
- [x] **D9 — FOUND ON A DEVICE RIDE 2026-09-18: a hard turn paid less per second than going straight.**
      The window's base carried the guard band's 0.25, and a qualifying turn always leaves the
      100–200 cm band, so a 3× chain ran on a quarter base (0.75× the straight rate). Plus the
      first-cut sizes made even an in-band chain worth ~4 credit-s. The window now pays on an
      un-guarded base (`TrickWindowGuardRate`, 1.0) and is sized 6 s / peak 4 / lump 2 / cap 6.
      Owner's calibration: cutback + two linked turns + 24 s more surfing ≥ 500–600 on the hybrid.
- [x] **D4's stale premise — RESOLVED in the build.** `RideScore::PlayLevelUp` had been deleted
      (`74fac206c`, 2026-09-04) but the animation survived; it is reachable again as
      `RideScore::PlayCelebration(World, bBigTurn)` — snap for every scored turn, burst for a big
      one, no caption (D7).
- [ ] Where the "big turn" bar sits is still a device question (0.65 after D8; one of sixteen turns
      cleared the old 0.80). Getting it wrong in the generous direction turns the celebration into
      an obstruction. See D4's closing risk.
- [x] **On-demand celebration state exists:** `surf.score.holdpeak 1` pins the snap + burst at
      their peak for layout captures (`RideScoreOverlay.cpp`), so verifying the visuals is no longer
      a timing lottery.

## Overview

The ride score today is **survival**. `SurfAssist::CreditEarned` is
`DeltaTime × guardRate × speedMultiplier` — time alive, quartered while the steering guard is
correcting, scaled ~1–2× by speed. Direction is not measured; it enters only sideways, through
`BandError` firing when the board leaves the pocket band around the crest.

That was the right first scoring rule and it is still the right floor: it teaches the one thing a
beginner must learn, which is to stay on the face and keep moving. But it is silent about everything
a surfer actually does on a wave. A player who has learned to hold the line has nothing left to get
better at, and the number stops meaning anything the moment survival is easy.

**Turns are the next thing to reward, and the signals are already computed.** Not roll — see the
gotchas, the board is roll-stiff and `worldRollSin` barely leaves 0.04 even in a hard turn. Yaw rate
and sideslip are what move, and between them they separate the two things a surfer would call
different tricks.

## Objective

Make the score respond to **how the board is being ridden**, not only how long it stayed up — and
make a turn worth what the player does *after* it, so the largest numbers go to the part that is
actually hard: coming out of a hard turn still surfing, and then doing it again. Without
letting trick points become a way to graduate off a board the player cannot hold, and without adding
a second permanent number to a 1200×540 screen that has no room for one.

## Requirements

### FR1 — A turn is an EVENT, scored once, not a per-tick rate

Points are awarded on a **completed turn**: an entry, a sustained arc, an exit. The turn is GRADED
from the arc as a whole, at the exit — and then paid out over the seconds that follow it rather than
handed over there and then (FR9). The grading is the event; the payment is a window.

This is the requirement that makes the feature un-farmable, and it is the one most likely to be
"simplified" away during implementation. Integrating `|yawRate|` per tick looks equivalent and is
not: high-frequency wiggle sums to a large number without a turn ever happening, so the optimal
strategy becomes shaking the board — which is both ugly and the exact opposite of what the score
should teach. An event with a minimum duration and a minimum total heading change cannot be farmed
that way, because a wiggle never completes one.

A turn event opens when signed yaw rate exceeds an entry threshold, stays open while the sign holds,
and closes when the rate falls below an exit threshold or the sign flips. It **scores only if** it
accumulated at least a minimum total heading change over at least a minimum duration; otherwise it is
discarded silently. Hysteresis between entry and exit thresholds is required, or a turn held near the
threshold chatters into a dozen events.

### FR2 — Heading is measured against the WAVE, not the world

A board riding a straight line across a steep face must score zero turn.

Measured naively — the rate of change of `board.forwards` in world space — it does not: the face is
sloped, the board is banked into it, and simply traversing produces a world-space heading change with
no turn in it. The frame that makes a straight line read as straight is the wave's, and it already
exists: `SurfAssist::FOutput::HeadingSin` is the board's heading relative to the down-line direction,
computed every tick from `resolvedWaveBackDirection` and `BoardForward`. A turn is a change in
*that*.

The exact projection is left to implementation. The requirement is the property: straight-line riding
on a steep face scores nothing, and a bottom turn — which swings wave-relative heading hard — scores
a lot.

### FR3 — Trick points are score, and score is now only score

`ScoreShown = (ScoreFromCredit(credit) + TrickPoints) × BoardDifficultyMultiplier` (FR11). Trick
points feed nothing else, because there is nothing else left to feed.

**Graduation is gone** (owner, 2026-09-03): assist level is a fixed property of the board and stays
there forever — a foamie is level 4, `Full`, for the life of the board. Nothing reads accumulated
credit to decide how much help the player gets any more.

That does not merely resolve D1 in favour of the split — it **dissolves the question**. Credit was
only ever load-bearing because `AlphaFromCredit` read it; with no board reading it, credit is not
"graduation fuel" that trick points must be kept away from. It is just the survival term of a score,
sitting next to the trick term. The constraint this spec was written to respect —
`ride-score-counter.md` D2's *"opens a way to graduate off a board they cannot actually hold"* — has
no force left at all, because there is no graduating.

So the design is simpler than D1 framed it. One score, two contributions:

| term | what it rewards | where it comes from |
|---|---|---|
| survival | staying on the face, fast | `ScoreFromCredit`, unchanged |
| tricks | turning hard and cleanly, and still surfing afterwards | this spec |

**Still one number on screen.** The player sees a single total; the code keeps two accumulators
behind it.

**Do not build on `AlphaFromCredit`.** The credit→alpha schedule (`CreditFullSeconds` / `Mid` /
`Zero`) is now dead machinery for any board with a fixed level — `ApplyBoardProfile` calls
`SaveManualLevel(Profile->AssistLevel)` and the manual level short-circuits the schedule. It still
compiles and still runs; it just decides nothing. Anything in this feature that keys off "assist
level changed" is keying off an event that will never fire.

### FR4 — The turn's QUALITY is scored, not just its size

One detector, one event type, graded — not a "carve detector" and a separate "hard turn detector".
The same arc is scored on three axes, all available per tick:

| axis | signal | what it means |
|---|---|---|
| size | total wave-relative heading change over the arc | how far the board came around |
| bite | `1 - abs(cosYawAngleOfAttackLeftN)` averaged over the arc | rail engaged vs pivoting across the flow |
| drive | exit speed / entry speed | speed held through the arc vs bled |

`cosYawAngleOfAttackLeftN` on `ASharedCalculations` is the normalized dot of board-left with the unit
projected relative-water-velocity: 0 when the flow runs along the board, ±1 when it runs straight
across it. It is exactly sideslip, it is already normalized to [-1,1], and it is already computed
every tick for the fin terms.

That decomposition is why "hard turns" and "carving turns" should not be two features. They are the
same event with different sideslip:

- **Carve** — big heading change, low sideslip, speed held. The board went where its nose pointed.
- **Snap / slide** — big heading change, high sideslip, speed bled. The board pivoted across the flow.

Both should score. The carve should score **more**, because it is harder and because it is the one
that leaves the player in a position to do something next. A slide that scores as well as a carve
teaches the player to throw the tail out and stall, which is a worse ride.

### FR5 — The assist may not earn trick points on the player's behalf — but α is NOT the test

A turn the assist drove does not score. **The test is `bAssistGuardActive` over the arc, not the
board's assist level.**

The problem being solved is real: at high authority the assist steers the board into the pocket, that
correction registers as a wave-relative heading change, and the player is awarded a carve they did
not make. The obvious fix — scale trick points by `(1 - α)` — is **wrong now, and would have been a
silent defect.** With a fixed per-board assist, α is no longer "how much this player still needs
help"; it is a *property of the board they chose*. A foamie is permanently α = 1, so `(1 - α)`
scoring means **the beginner board can never score a single trick, forever** — the scoring feature
simply would not exist on the board most likely to be a new player's first.

`bAssistGuardActive` has none of that problem, because it is per-tick and behavioural: it is true
exactly while the assist is correcting, and false while the player is driving. A foamie rider who
comes round hard with the guard silent turned the board themselves, and gets paid for it at any α.

So: discard an arc whose guard was active for most of its duration; apply no α scaling anywhere.

The residue this leaves — that a high-α board is rate-limited (`RateLimitAtFullAssist` = 1.5) and so
turns differently from a shortboard — is real, but it is a *calibration* problem and belongs in D3's
per-board thresholds, not in a permanent points tax on one board.

### FR6 — Two tiers of feedback, and the celebration is the rare one

The counter stays the single permanent figure, top-centre. Everything else is transient, and it comes
in two tiers:

- **Every scored turn** — the counter snaps and settles. No burst. *(Amended 2026-09-12: the
  original "light acknowledgement" was the number merely changing, which was not visible enough to
  register as the game noticing. It gets the snap now.)*
- **A big turn only** — the snap plus the burst: `RideScore::PlayLevelUp`'s animation, reused
  (owner's call, 2026-09-03; it is the effect they want kept). **No caption** — see D7.

**The tiering is the requirement, not the decoration.** That effect currently fires perhaps three
times in a player's whole history, which is most of why it reads as an event. Fire it on every turn
and it becomes wallpaper within one ride — and worse, it covers the wave for 2.4 seconds each time,
which is the failure NFR2 names. So "hard enough" from the owner's message is a real, separate,
*high* bar sitting above the FR1 minimum that merely makes a turn score at all. Two thresholds, tuned
independently.

Reuse it rather than rediscovering the four lessons already baked into it: a fast snap with a long
settle rather than a symmetric swell, more than one element moving, a pivot below centre so the peak
does not crop off the top of the screen, an alpha-tracking shadow attribute, and a burst painted as
radial shards rather than a scaled `WhiteBrush` rectangle.

Two things the reuse needs that it does not have today:

- ~~**The caption must become a parameter.**~~ **Superseded by D7: there is no caption.** It was
  made a parameter and shipped as "BIG TURN" / "AGAIN!" / "LINKED!", and the first device session
  cut it. The lesson the caption left behind — a fading text's shadow must follow its alpha — is
  kept in the code comments in case words ever return.
- **A retrigger rule.** The settle runs 2.4 s and a good rider will link big turns faster than that.
  Restarting the animation mid-settle reads as the wobble the effect was rebuilt to stop being.
  Either the second trigger restarts cleanly from the snap, or a cooldown suppresses it — decide on a
  device, and note the celebration is timing-sensitive to capture ([[ride-score-counter]] status,
  open item 1: run-to-run jitter made six attempts catch only the tail. Add an on-demand trigger
  before trying to screenshot this one.)

**No language at all, since D7.** The rule that governed the words while they existed —
[[plain-language-not-physics-jargon]], never "yaw", "sideslip", or a degree count — still governs
anything else this surface ever says.

### FR7 — Inert in every automated run

Same gate as the counter and the assist (`IsAssistEnabledThisSession()` / `IsAssistSuppressed()`),
for the same reason, plus one specific to this feature: **the autopilot turns the board**. A scripted
snapshot run would generate turn events, and anything that writes a best score from one leaks between
runs and corrupts stored state.

Note [[test-runs-enable-player-controls]] — headless tests do enable player controls after handoff,
so "this is a test" is not implied by anything the detector can observe locally. Gate on the
`surf.autopilots` CVar, as everything else does.

### FR8 — Storage extends the existing per-board seam

Best ride is already per board — `SurfAssist::LoadBestRide(BoardId)` / `SaveBestRide(BoardId, ...)`.
A ride's trick points fold into the same per-board best; no new storage mechanism and no `USaveGame`,
which `gradual-control-handoff.md` D2 already ruled out and that ruling still holds.

### FR9 — A turn is paid over the seconds that FOLLOW it, not at its exit

A scored turn does not hand over a lump when it ends. It opens an **earning window**: for a few
seconds afterwards the ride earns at a multiplier, and the player collects the turn's value by
continuing to surf. Fall, and the window closes with the ride having paid almost nothing.

This is the requirement that decides what the score is *about*. FR1–FR4 measure the arc, and a lump
at the exit would say the hard turn is the achievement. It is not — it is the cheap half. Owner,
2026-09-11: *"making a hard turn and then fall is easy, making a hard turn and then surfing back into
the non assist area is difficult, and doing multiple hard turns in a row is even more difficult."*
The window is what makes the number say the same thing.

**The "non-assist area" is not a new signal to build.** It is the guard band that already exists, and
the credit rate is already multiplicative on exactly it: `CreditEarned` is
`DeltaTime × GuardRate × SpeedCreditMultiplier`, where `GuardRate` is `AssistedCreditRate` (0.25)
while the steering guard is correcting and 1.0 while it is silent — that is, full rate inside the
band and a quarter outside it. The window is one more factor on that product, and three properties
fall out of it with nothing new measured:

- ~~A player who blows out of the band after a turn collects the window at a quarter rate while the
  assist is talking them back in, and at full rate from the moment they are back inside. **Recovery
  is paid as it happens, in proportion to how fast they manage it.**~~ **Withdrawn by D9
  (2026-09-18).** The band is 100–200 cm from the crest, and a turn that qualifies (60°+) leaves it
  every time, so this factor did not price *recovery* — it priced *turning*. The window's base now
  carries `TrickWindowGuardRate` (1.0) where the survival base carries 0.25. Recovery is still paid,
  by the survival term: full rate back in the pocket against a quarter outside it. Nothing about
  the pocket got cheaper; the turn stopped being taxed.
- Speed is already a factor, so a turn that bleeds all its drive pays less through its own window
  than one that keeps it. FR4's "drive" axis gets a second, automatic vote.
- A fall ends the ride, `bRideActive` goes false, accrual stops, and the turn is worth whatever was
  collected before the board went down.

**Do not multiply the credit accumulator.** This is the trap in this requirement, and it fails
silently. FR3's identity — the survival term is identical to a build with this feature absent — is an
acceptance criterion, and `AssistState.CreditSeconds` also feeds the per-board best and the (dead)
`AlphaFromCredit`. Compute the base the way the ride already does and route only the *excess* into
the trick accumulator:

```cpp
const float Base   = SurfAssist::CreditEarned(DeltaTime, bGuardActive, Speed, T, Broken);  // -> credit, unchanged
TWindow.AssistedCreditRate = TrickWindowGuardRate;                                        // D9: 1.0
const float WBase  = SurfAssist::CreditEarned(DeltaTime, bGuardActive, Speed, TWindow, Broken);
const float Bonus  = WBase * (WindowMultiplier - 1.0f);                                    // -> trick points ONLY
```

Same speed multiplier, same whitewater factor, same zero when no ride is active — and the survival
term never learns this feature exists. The only factor the two bases do not share is the
out-of-band rate (D9).

**The multiplier is sized by the turn and decays back to 1 across the window.** A big clean carve
opens a larger and longer window than a scrappy qualifying one; FR4's grade is already that number.
Decay rather than a cliff, so there is no moment where the score stops paying for no reason the
player can see.

**What this must not become is a recovery bonus.** Paying a lump for re-entering the band is the
obvious-looking version, and it inverts the whole feature: a clean carve held in the pocket never
leaves the band, so it would collect nothing, while a sloppy turn that nearly lost the wave collects
the prize. As specified, the clean turn takes its whole window from the first tick *and* its
survival term at full rate, so it always out-earns the blowout. Keep it that way.

### FR10 — Linked turns compound, up to a ceiling

A turn that scores while a window is open does not replace it — it **refreshes the window and steps
the multiplier up**, so three linked turns are worth materially more than three isolated ones. That
is the owner's third case, and it is the top of the skill ladder this score is trying to describe.

- **The guard speaking does not break the chain.** It only pays less while it speaks (FR9). Breaking
  on `bGuardActive` would zero the score for exactly the recovery this feature exists to reward — and
  an implementer will reach for that break, because FR5 uses the same flag for the opposite purpose
  one requirement earlier. **FR5's test runs during the arc and asks "did the assist steer this
  turn". FR9's runs after the exit and asks "is the player back in position yet".** One signal, two
  questions; only FR5's discards anything.
- **A fall breaks the chain**, and so does the window running out with no new turn in it.
- **The ceiling is not optional.** The multiplier caps, and the cap is load-bearing because the
  payment loop is already triple: `lateralTurnHardCarveBoost` pays a hard turn in speed
  ([[hard-carve-progressive-boost]]), speed multiplies the credit rate, and the window now multiplies
  it again. Uncapped compounding on top of a loop that already feeds itself is how a score stops
  being about surfing.
- FR1's event gate is what keeps this un-farmable at the entrance: only a completed arc with a real
  heading change opens or extends a window, so a chain cannot be assembled out of wiggle.

**The player must be able to see that a window is open**, or it teaches nothing. A change of earning
*rate* alone is too subtle to read in peripheral vision while watching a wave —
`ride-score-counter.md` FR2 established that when it gave the counter a colour for `bEarningFast`
rather than trusting the tick rate to speak. Reuse that channel for the window. ~~Show the chain
step (×2, ×3) beside the counter while a chain is above one.~~ **Cut by D7 (2026-09-12):** the badge
was built, sat under the counter, and was the first thing to go on device — a second figure to
decode while surfing. What tells the player a chain is alive is that the number keeps snapping as
they link turns, and the colour the window already drives. Transient, no second permanent figure,
and the burst stays rare, for the big turn.

### FR11 — The board's difficulty multiplies the score, and thresholds do not move

Every board is scored by the same rule — one entry threshold, one minimum heading change, one minimum
duration, one big-turn bar — and the board then multiplies what that rule produced. A harder board is
worth more per wave; it is not graded on an easier curve.

**The multiplier is already authored and already on screen.** `FSurfBoardProfile::RatingDifficulty`
is a 1–5 rating in `Content/Boards/*.json`, drawn as pips on the rack card, and described in
`SurfBoards.h` as *"the axis the player chooses along"*. Reading it costs no new field, no new
authoring step, and — the part that matters — no new concept for the player: the number that pays is
the number they read before they picked.

| board | assist | turning | difficulty |
|---|---|---|---|
| foamie | 4 | 1 | 1 |
| funboard | 2 | 3 | 2 |
| fish | 0 | 3 | 3 |
| hybrid | 0 | 4 | 4 |
| shortboard | 0 | 5 | 5 |

**It scales the whole ride score, survival and tricks alike** (owner, 2026-09-11). Staying on a
shortboard is harder than staying on a foamie, so the survival term has the same claim on the
multiplier that the trick term does — and one rule the player can state in a sentence ("the
shortboard pays double") beats two terms that scale differently for reasons only the code knows.

**The floor is 1.0 and nothing may take it below.** This is FR5's lesson restated in the one place it
could come back: a board-dependent factor that reaches 0 deletes the whole feature for whichever
board sits at the bottom — and that board is the foamie, which is the one a new player meets first.
Difficulty 1 pays 1×. It never pays nothing.

**Store raw, multiply where the number is shown.** The per-board best is stored in credit (FR8). Bake
the multiplier into the stored value and re-tuning it silently invalidates every best already
recorded, leaving one board's history denominated in two different currencies with nothing marking
the change. Apply it at presentation and at the moment a best is compared, so both sides of that
comparison are always in the same units.

**Fixed thresholds put a calibration duty where there was none.** This is the cost of D3's ruling and
it is the thing most likely to go wrong quietly:

- `RatingDifficulty` tracks `RatingTurning` almost exactly — `SurfBoards.h` says outright that
  difficulty *"falls out of turning minus assist"*. So the high-difficulty board is also the board
  that turns most readily, and it clears a fixed threshold most easily. The multiplier is therefore
  multiplying an advantage the board already has, not correcting for a disadvantage.
- Which means the thresholds must be set against **the weakest board that should still score** — a
  foamie's honest hard turn, guard silent, has to clear the bar. Calibrate them on a shortboard and
  the foamie scores nothing, the multiplier multiplies zero, and FR5's defect is back by another
  route: the beginner board is one where this feature does not exist.
- A shortboard should out-score a foamie by the authored, visible multiplier — not by an invisible
  threshold the player never sees and cannot reason about. Both together is double-counting.

The multiplier's shape (linear in the rating, or steeper at the top) belongs on
`USurfTuningSubsystem` like everything else here. A straight `1 + k(difficulty - 1)` with `k = 0.25`
puts the foamie at 1× and the shortboard at 2×, which is the range to start arguing from.

**One promise this newly makes.** `SurfBoards.h` already calls the ratings *"a PROMISE the physics
has to keep"*. Difficulty is now also a promise the scoring keeps, which raises the cost of an
inaccurate rating: a board rated harder than it rides pays out more than it earned, and the rack card
becomes the place to farm points rather than the place to choose a board.

### FR12 — ~~Whitewater pays a quarter~~ RETIRED 2026-09-18: the score ignores whitewater

**Retired by the owner, 2026-09-18.** *"I have noticed on multiple occasions that the scoring is low
because of faulty white water control. I have abandoned checking if the board is in the white water
to determine if the hydrofoil should propel forwards or not, because it's too often a false positive
… The same problem exists with scoring less in the white water: the scores are too low for hard
tricks because the code thinks the board is in the white water. It's better to ignore white water
when scoring."*

The measurement behind that is already on record (`broken-wave-no-consequences.md` M13, memory
`a2-drive-cut-fires-on-clean-face`): `brokenAmount` read **1.0 for 1.5 s on a clean face** because
a turn pointed the nose at the lip's foam 6 m ahead. The foam cloud is a thin band along the broken
lip, so a turn *up into the lip* reads more foam than a whitewater sit; no shape rule on the point
cloud separates them. The signal says "broken water NEAR the board", never "board IN broken water".
And the moment it false-positives — mid-turn toward the pocket — is exactly when FR9's window is
paying, so at 0.25 it took three quarters off the hardest rides. The D9 replay of the owner's
cutback ride shows it: `foam=1.00` from t≈4.7 s to 9.5 s across the second and third turns.

What changed: `ScoreWhitewaterCreditRate` defaults to **1.0** (below, the value this section
already named as the A/B control). The `WaveRate` factor stays in `CreditEarned` and `brokenAmount`
is still fed through, so a signal that actually means *in broken water* — peel position from the
tile phase (broken-wave A4.3), or a speed gate — can price the foam again by lowering one number.
`brokenAmount` is not that signal, and nothing in the score should be keyed on it until one exists.
The `tricks` debug dump still prints `foam=` so the false positive stays visible.

The original requirement follows, for the record of why it was wanted and how it was wired.

---

Owner, after the hydrofoil gate landed: *"the score points score as much on an unbroken wave as it
does in white water. It is preferable to surf in the unbroken part of the wave."* True, and for a
structural reason: the guard band that already prices "out of the pocket" at `AssistedCreditRate`
is the **cross-shore** distance to the crest, and behind the peel the board sits at the same
cross-shore distance. Nothing in the credit product knows the wave there is broken.

The signal exists now: `ASharedCalculations::brokenAmount`, 0..1 from the sim's whitewater point
cloud counted within `FoamBrokenRadius` of the board (specs/broken-wave-no-consequences.md A4/M11 —
the only quantity in the data that means *broken*; slope, water column and water speed all read a
bore like a face). One more factor on the same product:

```cpp
GuardRate = bGuardActive ? AssistedCreditRate : 1
WaveRate  = lerp(1, WhitewaterCreditRate, brokenAmount)      // ScoreWhitewaterCreditRate, 0.25
credit   += DeltaTime * GuardRate * WaveRate * SpeedMult
```

Because FR9's window pays `Base × (mult − 1)` on that same `Base`, a turn thrown in the foam pays a
quarter through its window too, with nothing scored twice. The counter's rate — FR2 of
`ride-score-counter.md`, the feedback the player actually reads — drops on entering the foam and
recovers on leaving it, so the preference for the unbroken wave is shown, not told.

- 0.25 = the same price as being shepherded by the assist: out of the pocket, either way.
- 1.0 is the A/B control (whitewater scores like the face).
- This deliberately changes the survival term (the trick-scoring FR3 identity was about *that*
  feature's presence, not this one), so per-board bests and the graduation credit now also prefer
  the unbroken wave. Intended.
- Missing SC (tests, autopilot rides): `brokenAmount` reads 0 → face rate. FR7's inertness is
  unaffected.
- The signal was re-based the same day (broken-wave spec M12): foam *ahead of the motion* within
  600 cm (60/160) or dense foam all round within 300 cm (420/520). The first all-round-only
  thresholds (150/400) would have docked the score on ~8 % of face ticks in the pocket, where the
  board rides beside the lip's foam. Measured after the change: `brokenAmount` ≤ 0.08 on three face
  rides.
- **Device calibration still owed**: the four foam counts were read on PC; the cache holds
  camera-culled particles and the Android budget is lower. `surf.debug.flags 'foam'` on
  `SharedCalculations` logs both counts and the resulting `brokenAmount` per tick.

### NFR1 — No new physics, no second state of record

The detector reads `cosYawAngleOfAttackLeftN`, `board.forwards`, board velocity and the assist's
existing heading terms. It computes nothing the sim does not already compute, applies no force, and
must not become a second opinion about what the board is doing.

### NFR2 — It must not become the thing they watch

Inherited unchanged from `ride-score-counter.md` NFR1. A player reading a trick callout instead of
watching the lip has been made worse at surfing by a feature meant to reward surfing.

## Decisions

### D1 — Score and graduation fuel: one quantity or two — **RESOLVED 2026-09-03: the question is void**

Ruled by the project owner: *"Assist has no graduation anymore. So yes, visual points only. The
assist will be 4 for a foamie, and stay at 4 forever."*

The three options this was framed as — trick multipliers on credit, a cosmetic parallel score, or a
deliberate split — were all attempts to protect graduation from being gamed. **There is no
graduation to protect.** Assist level is a fixed board property; nothing reads accumulated credit to
decide anything. Trick points are score, score is score, and `ride-score-counter.md`'s FR1 (one
number, and it IS the credit) is not a constraint that survives its own premise.

Worth stating plainly because it is the kind of thing that gets re-argued: **credit is no longer a
mechanism, it is a term.** It is the survival contribution to a score, and this spec adds a second
contribution beside it.

**One thing this ruling did catch, and it is why it was worth asking rather than assuming.** The
draft FR5 scaled trick points by `(1 - α)` to stop the assist earning points on the player's behalf.
Under graduation that was fine — α fell as the player improved. Under a fixed per-board α it is a
silent defect: a foamie is `Full` forever, so `(1 - α)` = 0 and the beginner board could never score
a trick. FR5 now tests the per-tick guard instead. See FR5.

### D2 — Airtime — **RESOLVED 2026-09-03: deferred until air physics is fixed**

Ruled by the project owner. The board can catch air today but does not do it realistically; the
physics gets fixed first and the scoring is designed against the fixed behaviour.

Recording the analysis so it is not redone. Two signals were considered and both are traps:

- **`amountUnderWater` is the wrong instrument.** It is a soft ~1 m ramp, and the board rides 30–50 cm
  submerged in normal riding ([[planing-depth-equilibrium-fix]]), so in a ride it sits mid-range as a
  *depth* reading. Airtime lives at the extreme tail of that ramp, where nothing has been calibrated.
  Per-actor `AFluidDynamics::actorWetted` is a better read with the same defect.
- **The 40 cm render offset is fatal here specifically.** The display mesh sits ~40 cm below the
  wave-height data the physics uses ([[wave-render-meshes-lowered-40cm]]). Air scored off physics
  depth begins and ends at a visibly wrong moment — on the one trick whose entire definition is what
  the player sees leave the water.

When it is picked up, the honest signal is **ballistic, not depth**: the board is airborne when
nothing is pushing on it — total hydrodynamic plus buoyancy contribution near zero, board
acceleration near gravity. That is immune to both the ramp calibration and the render offset.


> **Clarified 2026-09-18 (owner):** the deferral is for *scoring* only. Air physics is not deferred —
> realistic surfing is the goal, and if the realistic outcome of a snap at the lip at speed is air,
> that is what the physics should do; it just scores nothing extra. See CLAUDE.md "Design principle".
### D3 — Do thresholds scale per board — **RESOLVED 2026-09-11: no. One rule, and difficulty pays.**

Ruled by the project owner: *"I don't think thresholds per board is necessary. I think possibility
for scoring more for boards with higher difficulty makes sense."*

So the lever is not the bar, it is the payout. One set of thresholds for all five boards, and the
board multiplies the score it produced (FR11) — applied to the whole ride score, survival and tricks
alike (owner, same day). A harder board is worth more per wave rather than being marked on an easier
curve, which is the honest version of what board choice already means.

**This is better than per-board thresholds for a reason worth recording, because the draft had it
backwards.** Per-board thresholds would have hidden the consequence of board choice inside numbers
the player never sees: two boards would both read "hard turn" while quietly meaning different turns.
A multiplier states the same consequence out loud, on a card the player reads *before* they commit —
and it reuses a rating that is already authored, already drawn as pips, and already described in
`SurfBoards.h` as the axis the player chooses along.

**What this ruling costs, and where it will go wrong if it goes wrong.** It moves the whole
board-fairness burden onto the calibration of a single threshold, and the correlation in the data
works against it: `RatingDifficulty` tracks `RatingTurning` almost exactly — 1/1, 2/3, 3/3, 4/4, 5/5
— because difficulty *"falls out of turning minus assist"*. The shortboard therefore clears any fixed
bar most easily **and** collects the largest multiplier, so the two effects compound rather than
cancel.

That is acceptable, and it is close to what the ruling intends. What is not acceptable is the tail of
it: set the bar where a shortboard's turns live and the foamie clears it almost never, the multiplier
multiplies zero, and the beginner board becomes one where trick scoring does not exist — which is
**exactly the defect D1 caught in the draft FR5**, arriving by a different route. The guard against it
is a calibration rule rather than a mechanism, and it is stated in FR11: *the thresholds are set
against the weakest board that should still score.* A foamie's honest hard turn with the guard silent
must clear them. Test 11 exists to keep that true.

The old framing — "does the shortboard own every leaderboard" — also overstated the stakes, because
there is no shared leaderboard: FR8 stores the best per board, deliberately, so that a global best
cannot be owned forever by whichever board scores most easily. The multiplier is therefore not about
fairness between competing scores. It is about giving a player on a foamie a visible reason to pick
up something harder.

### D4 — What a scored turn looks like — **RESOLVED 2026-09-03: reuse the level-up celebration, for big turns only**

Ruled by the project owner: *"we previously had a celebratory effect on the score when the assist
level decreased. The assist level no longer decreases, but I like the look of that effect. I'm
thinking we could use the same effect when the user makes a hard enough turn?"*

Yes — and it is better than reuse-for-its-own-sake, because that effect is now **stranded**.
`RideScore::PlayLevelUp` has exactly one call site (`AccrueRideCredit`, `SurfboardPawn.cpp:3915`),
guarded by the earned assist level dropping — which a fixed per-board assist never does. The code is
live, the trigger is dead, and a piece of hard-won, player-validated presentation ("works very well
now, exactly how I wanted it", 2026-08-29) sits in the build with nothing left to announce. A big
turn is a genuine celebration moment and it wants exactly this shape: an event with a cause the
player can point at.

Whether the old call site is then deleted or left standing is an implementation call, not a spec
one — but it must not stay the *only* thing that decides the caption, or FR6's caption parameter has
nowhere to come from.

The open question was never whether to have a callout. It was **how often it may fire**, and the
answer is what FR6 now specifies: a high bar, separate from and above the bar that makes a turn score
at all. What survives to be settled on a device is only the two mechanics FR6 lists — the caption
parameter and the retrigger rule — plus the `-Phone` check that the burst does not cover the lip
([[screenshot-the-game-on-desktop]]).

The risk to watch, and it is the one that would undo the whole thing: this effect earns its impact
from rarity. If the "hard enough" bar is set where a competent rider clears it twice a wave, the
celebration stops being a celebration and starts being an obstruction — and the evidence for where
that line sits cannot come from anywhere but riding it.

### D5 — How much of a turn is paid at the exit vs collected in the window — **OPEN**

Pure window is the honest reading of the owner's framing: the turn on its own is the easy half, so on
its own it should be worth very little. The risk is legibility — a turn that pays *nothing* at the
moment it happens may not read as having scored at all, and FR6's first tier is precisely "the
counter moves on every scored turn". A change of earning rate is the thing `ride-score-counter.md`
FR2 already found too subtle to notice while watching a wave.

The likely answer is a small lump for legibility with the bulk in the window, and the split is the
knob. Put both on `USurfTuningSubsystem` and settle it on a device
([[tuning-overrides-json-ab]]) — riding it is the only way to find out whether a pure-window turn
feels unrewarded at the exit or correctly deferred.

### D6 — How long the window runs — **OPEN, but its floor is measurable**

Both directions fail in a way worth naming before anyone picks a number:

- **Too short** and the recovery from a big turn does not fit inside it — which punishes the exact
  case FR9 exists to reward, and does it invisibly.
- **Too long** and the window is open for most of the ride. A multiplier that is always on is not a
  multiplier, it is the baseline with extra steps, and it takes FR10's chain down with it: refreshing
  something that never expired rewards nothing.

The floor does not need a device. Measure it off the traces already on disk (`hard_turn`,
`top-turn`): time from a scored turn's exit to the board being back inside the guard band
(`BandError` returning to 0). The window must be at least that, or recovery is unpayable. The ceiling
is a judgement about how often a competent rider lands turns, and that one does need riding.

### D9 — The window paid a tenth of what it should, and taxed the turn it was paying for — **FOUND AND FIXED 2026-09-18**

**The ride.** Phone trace `phone-2026-09-18-08-41-02` (hybrid, tilt): a hard cutback at t≈3.5 s,
two linked turns by t≈7.6 s, then ~24 s more riding at 400–800 cm/s until the board lost planing at
t=30. On the phone it scored **156**. The owner's straight rides on the same board score 300+, and
their calibration for this one: *"at least 500–600"*.

**What the replay showed** (`-ReplayTrace … -ReplayUseWeights -Board=hybrid`, `surf.assist.force 1`,
`surf.assist.alpha 0`, flags `tricks` and `crossing`; deterministic across two runs, 211 both — the
replay diverges from the device after t≈8, but the three turns reproduce):

| | measured |
|---|---|
| turns scored | 86° (grade 0.52), 114° (0.84, BIG), 75° (0.34) — chained to the 3.0× cap |
| trick credit, whole chain | **2.1 credit-s = 37 points** on the hybrid |
| survival | 10 of 30 s at full rate; the rest at 0.25 |
| window base during the chain | `0.0042`/tick (= 0.25 × 1/60), dipping to `0.0010` in foam — never `0.0167` |
| `distToCrest` after the first turn | −250 → −350 cm: below the 100–200 cm band for the whole window |

Two causes, both structural rather than tuning:

1. **The window's base carried the guard factor.** FR9 argued that made "recovery" pay as it
   happened. But a qualifying turn sweeps 60°+ and the band is one metre wide, so every real turn
   leaves it, and the window then multiplies 0.25: at the 3× cap the player earned **0.75× the
   straight-line rate** during the celebration. Turning was priced as a loss.
2. **The first-cut sizes could not reach the owner's number even in band.** 4 s × (2.5−1)/2 at
   full grade ≈ 3 credit-s per isolated turn; a capped 3-chain ≈ 4 credit-s ≈ 70 points on the
   hybrid, against 525 for 30 s straight. No setting of the band factor alone gets a chain past a
   tenth of a ride.

**The fix.** (a) `TrickWindowGuardRate` (new, 1.0): the window's base is built by the same
`CreditEarned` call as the survival base with only the out-of-band rate swapped — speed and
whitewater still dock it; 0.25 restores the old behaviour for A/B. (b) Sizes: window **6 s**, peak
**4.0**, lump **2.0**, cap **6.0**. Arithmetic on the ride above with the phone's timeline (turns at
3.5 / 5.2 / 7.6 s, window open until 13.6 s, base rate 1/s since speed < 1400 cm/s):

| term | credit-s | points ×1.75 |
|---|---|---|
| survival (unchanged: 3.5 s in band + 24 s at 0.25) | ~9 | ~160 |
| exit lumps 2.0 × (0.52 + 0.84 + 0.34) | 3.4 | 60 |
| window: peaks 2.56 → 5.08 → 6.0 (capped), decaying, 3.5–13.6 s | ~25 | ~440 |
| **ride** | **~37** | **~660** |

A straight 30 s ride in band is unchanged at 525; an isolated big turn (grade 0.84) is now
1.7 + 7.6 ≈ 9.3 credit-s ≈ 160 points on the hybrid — roughly fifteen seconds of straight riding.
The compounding warning in FR10 still stands: two big turns reach the cap, and the cap is what
keeps a fourth turn from paying more than the ride.

**What this did not touch, and should be looked at separately.** The survival term paid this
ride's 24 s of post-turn surfing at a quarter because the board sat 250–350 cm below the crest — the
guard band is the *assist's* steering band (`AssistBandNear/Far`, 100/200 cm, tuned for where to
nudge), reused as the score's definition of "the pocket". On boards with no assist that is its only
job. Whether 3 m down the face at 700 cm/s is a quarter-rate ride is `ride-score-counter.md`'s call,
not this spec's.

**Owed.** The owner rides the same line on the device and reads the number; if it lands far from
500–600, the knobs are the four sizes and `TrickWindowGuardRate`, all in `Saved/TuningOverrides.json`.

### D7 — What the counter says when a turn scores — **RULED ON DEVICE 2026-09-12: nothing**

The owner's words, from the first session riding with trick scoring on: *"I don't like the extra
text when a turn scores more. Neither the x2, x3 or BIG TURN is good. It takes focus from surfing to
read the text, and it's not obvious what it means. I would much rather have the celebratory animation
from before, which just animates the numbers. A user doesn't have to know exactly the number his
points increases or that he did a BIG TURN. He already knows he did something good, and just an
indication that the scoring system noticed and rewards this *somehow* is enough."*

So the surface has one channel: **the number itself.** Every scored turn snaps it; a big one adds the
burst; a chain is felt as the snaps coming faster and the earning colour staying lit. No caption, no
badge, nothing to read.

Two things this settles beyond the words:

- **FR6's first tier was under-built.** "The counter moves" meant the figure ticking up by its exit
  lump, and on device that did not register as an acknowledgement at all. A turn that scores has to
  *visibly* do something to the number, so the snap now fires on every scored turn and the burst is
  what the big tier adds.
- **The tiering survives.** The burst still fires only on a big turn, for the reason FR6 gives: it
  covers the wave for a moment, and on every turn it would be wallpaper. The snap alone does not
  cover anything, which is why it can afford to be common.

The design rule this leaves for the counter, stated so it is not rediscovered: **anything drawn here
is felt in peripheral vision or it is not drawn.** A player watching a wave does not read.


### D8 — The guard flag means two things, and FR5 read the wrong one — **FOUND AND FIXED 2026-09-12**

Reported as "the celebration happens too seldom", pointing at a 144-degree snap around 7 s into the
shortboard's best ride. It was not seldom; it was not happening. That ride scored **one** turn
in-game. The same ride's recorded yaw, run through an offline port of the same detector with the
same thresholds, scores **twelve** — the 7 s snap among them, at the top of the grade range.

The difference is the one input the trace does not carry: `bGuardActive`. FR5 discards an arc whose
guard was active for more than half its duration, because the assist may not earn trick points on the
player's behalf. Under the controller (alpha > 0) that flag is "the assist is correcting right now",
which is what FR5 means. But on the graduated path — alpha 0, which every board past the funboard is
authored at — `UpdateAssist` recomputes it as `BandError != 0`: "the board is outside the crest
band", kept alive purely so the survival credit pays a quarter rate out of the pocket
([SurfboardPawn.cpp:5269](../Source/GoneSurfing/SurfboardPawn.cpp#L5269)). The assist steers nothing
there. And a sharp turn on a shortboard *always* leaves the band, so the guard read active for most
of every arc and FR5 threw the turn away as the assist's.

**Fix:** the discard requires authority — `bGuardActive && AssistAlpha > 0`. The window payment is
untouched: `BaseCredit` already carries the quarter rate, and out of the pocket *should* pay less.
Only the discard needed the assist to actually be steering.

**And the bar was too high anyway.** The sixteen turns that survived the bug across the session
graded 0.32–0.86, median ~0.55, and exactly one cleared `BigTurnGrade = 0.80`. The rider's own pick
for "sharp enough to celebrate" would have graded ~0.78. Lowered to **0.65**, at which roughly the
top third of scored turns burst — rare, but not absent. Live-tunable (`TrickBigTurnGrade`).

**Test that would have caught it:** replay a real shortboard trace through `TrickScore::Tick` with
`bGuardActive` forced false and compare the turn count with the in-game log for the same ride. The
offline detector used here lives in the session scratchpad; it should become a proper test.


## Acceptance criteria

```
GIVEN a board riding a straight line across a steep wave face
WHEN  it holds that line for ten seconds
THEN  no turn event is scored
```
```
GIVEN a player rapidly wiggling the board left and right without completing an arc
WHEN  the wiggling continues for ten seconds
THEN  no turn event is scored
AND   the trick total is unchanged
```
```
GIVEN two turns through the same total heading change at the same entry speed
WHEN  one holds low sideslip and exits near entry speed, and the other slides and bleeds speed
THEN  both score
AND   the carve scores materially higher than the slide
```
```
GIVEN a ride where the steering guard is correcting through a turn
WHEN  the turn completes
THEN  it scores nothing
```
```
GIVEN a foamie, permanently at assist level 4 (Full)
WHEN  the player turns it hard with the steering guard silent
THEN  the turn scores
AND   it scores on the same terms a low-assist board would earn
```
```
GIVEN any ride
WHEN  trick points are awarded
THEN  the credit accumulator is unchanged by them
AND   the survival term of the score is identical to a build with this feature absent
```
```
GIVEN any headless run (unattended, filtered autopilot, or trace replay)
WHEN  the autopilot turns the board
THEN  no callout is shown, no trick points are stored, and every existing baseline compares clean
```
```
GIVEN a ride containing several ordinary scored turns and one big one
WHEN  the ride is played back
THEN  the counter moves on every scored turn
AND   the burst fires only on the big one
AND   nothing is written on screen for either
```
```
GIVEN two big turns linked closer together than the celebration's settle time
WHEN  the second one fires
THEN  the effect reads as a second distinct event, not as the digits wobbling
```
```
GIVEN a ride that scores tricks
WHEN  the player wipes out
THEN  the ride total including tricks is banked against THAT board's best, not a global best
```
```
GIVEN a scored hard turn
WHEN  the board falls immediately after the exit
THEN  that turn's total contribution is a small fraction of the same turn ridden out
```
```
GIVEN the same scored turn, at the same grade, three times over
WHEN  one is followed by holding the pocket with the guard silent, one by a blowout the player
      recovers from with the guard correcting, and one by an immediate fall
THEN  all three score differently, in that order, highest first
AND   the clean in-pocket turn out-earns the recovered blowout — recovery is paid, but never more
      than never needing to recover
```
```
GIVEN three qualifying turns, each landed inside the previous one's window
WHEN  the chain ends
THEN  the total exceeds three isolated turns of the same grade
AND   the multiplier never exceeded its cap, however long the chain runs
```
```
GIVEN a turn whose window is spent entirely outside the guard band
WHEN  the window expires without the player getting back inside it
THEN  the chain is intact for a turn landed before it expires
AND   the window paid at the assisted rate throughout, not zero
```
```
GIVEN any ride containing windows, chains and falls
WHEN  the ride ends
THEN  the credit accumulator is identical to a build with this feature absent
AND   the per-board best stored for a ride with no scored turns is unchanged
```
```
GIVEN the same ride, tick for tick, on a foamie (difficulty 1) and on a shortboard (difficulty 5)
WHEN  both rides end
THEN  the shortboard ride shows the higher score
AND   the difference is exactly the difficulty multiplier, with no threshold having moved
```
```
GIVEN a foamie, the lowest-difficulty board in the rack
WHEN  the player makes an honest hard turn with the steering guard silent
THEN  it clears the fixed thresholds and scores
AND   its multiplier is 1.0 — never 0, and never a board-dependent factor that can reach 0
```
```
GIVEN a stored per-board best recorded before the difficulty multiplier was re-tuned
WHEN  the multiplier changes and the same board is ridden again
THEN  the displayed best and the displayed ride score are in the same units
AND   no stored value needed rewriting
```

## Test cases

1. **`trick_straight_line_scores_zero`** — replay an existing straight-riding trace; assert zero turn
   events. The FR2 regression test, and the one most likely to fail first.
2. **`trick_no_wiggle_farming`** — a synthesized input trace of alternating hard weight shifts at
   ~3 Hz ([[input-trace-replay]]); assert zero scored events.
3. **`trick_carve_beats_slide`** — the existing `hard_turn` and `top-turn` traces both contain real
   turns; score both and assert the ordering matches a human's judgement of which was the better
   turn. This is the calibration case, and it needs the owner to watch the two replays and say which
   was better *before* the numbers are read.
4. **`trick_inert_headless`** — full baseline sweep at defaults. Check `full-iso` dates first
   ([[stale-csvs-fake-regressions]]); baselines are already stale
   ([[stale-baselines-block-regression-sweep]]), so this proves less than it should.
5. **Device eye-test** — ride and ask, unprompted, what earned the points. If the answer is not
   roughly "turning hard without sliding out", FR4's grading has failed.
6. **Screenshot at `-Phone`** — the callout must not collide with the counter (top-centre), the wave
   radar (bottom-left) or the Restart/Replay bar, and must not cover the lip.
7. **`trick_window_pays_after_exit`** — replay one trace twice, truncating the second run a few ticks
   after a scored turn's exit. The truncated run must score close to the exit lump and far below the
   full one. This is the FR9 regression test and the one that catches a lump-at-exit "simplification".
8. **`trick_chain_beats_isolated`** — score a trace containing linked turns against the sum of the
   same turns scored in isolation; assert the chain is worth more, and assert the cap holds by
   synthesising a longer chain than any real ride contains.
9. **`trick_credit_unchanged`** — run any existing trace with the feature on and off and diff the
   credit column. Not the score column: the FR3 identity is about `CreditSeconds` specifically, and
   this is the test that catches the FR9 trap (multiplying credit instead of the trick term) which
   nothing else would — the score looks right either way.
10. **D6 floor measurement, before any tuning** — from `hard_turn` and `top-turn`, the distribution of
    (turn exit → `BandError` back to 0). No device needed, and it bounds the window from below.
11. **`trick_foamie_clears_the_bar`** — score a foamie trace containing turns the owner judges to be
    real ones; assert they clear the fixed thresholds. **This is the guard on D3's ruling**, and the
    single test that catches the way it fails: thresholds calibrated on a shortboard leave the
    multiplier multiplying zero, and nothing else in this suite would notice.

## First-cut values

Measured 2026-09-11 from **89 s of actual riding across 8 device traces**
(`Saved/Tests/latest/phone-2026-09-*.csv`), gated to speed > 200 cm/s and `planing` > 0.30. The gate
is not optional: a stalled or wiped-out board spins freely, and ungated it reports 390° "turns" with
the exit speed at 8% of entry. Everything below is a starting point to ride against, not a result.

**The two open numbers interact, so they are chosen together.** D6's "too long" failure — the window
open so much of the ride that the multiplier becomes the baseline — is a function of how easily a turn
qualifies under FR1, not of the window length alone:

| qualification | turns / riding min | 1.5 s | 2.5 s | 4.0 s | 6.0 s |
|---|---|---|---|---|---|
| entry 25°/s, sweep 40° | 14.8 | 34% | 47% | 65% | 86% |
| entry 30°/s, sweep 50° | 9.4 | 22% | 34% | 47% | 59% |
| **entry 35°/s, sweep 60°** | 4.0 | 9% | 14% | **18%** | 23% |
| entry 45°/s, sweep 80° | 2.0 | 5% | 7% | 8% | 8% |
| entry 60°/s, sweep 100° | 0.7 | 2% | 3% | 4% | 4% |

*(cells are the share of riding time the earning window would sit open)*

### FR1 — what qualifies as a turn

| | value | why |
|---|---|---|
| entry rate | 35 °/s | ~4 qualifying turns per riding minute in the traces |
| exit rate | 14 °/s | 0.4× entry — hysteresis wide enough that a held turn does not chatter |
| min duration | 0.35 s | median measured turn runs 1.0 s; this only excludes twitches |
| min sweep | 60° | median measured turn sweeps 49°, so this is deliberately above the middle |

### D6 — the window

| | first cut (2026-09-11) | after D9 (2026-09-18) |
|---|---|---|
| length | 4.0 s | **6.0 s** |
| peak multiplier | 2.5× at full FR4 grade, scaled down by grade | **4.0×** |
| decay | linear to 1.0 across the window | same |
| base outside the band | the survival term's 0.25 | **1.0** (`TrickWindowGuardRate`) |

At the FR1 values above the 4 s window was open **~18% of the ride** — a distinct state rather than
the baseline, which is what D6 asks for. At 6 s expect ~27% at the same turn rate; still a state, but
re-check if the sweep threshold is ever lowered.

**Which knob to turn first.** If the window feels too rare on device, drop min sweep 60° → 50°: that
roughly doubles the turn rate to ~9/min and takes the window to ~47% of the ride. Do **not** go below
about 40° at a 4 s window — at entry 25 °/s and 40° the window is open 65% of the ride and the
multiplier has become the baseline with extra steps, which is exactly the failure D6 names.

### FR10 — the chain

Exit-to-exit gaps between qualifying turns, measured at a slightly looser bar (entry 30°/s, sweep
50°) so there were enough events to see: **1.2, 2.0, 3.3, 3.4, 3.4, 4.6, 6.0, 8.7 s**. A 4 s window
links about half of those, which is the intended shape — a chain should be something the player
reaches for, not the default state. A 6 s window links all but the last. Cap the chain at ~~**3×**~~
**6×** (D9: with peak 4.0 two big turns reach it).

### FR11 / D3 — the board multiplier

`1 + 0.25 × (RatingDifficulty - 1)`:

| board | difficulty | multiplier |
|---|---|---|
| foamie | 1 | 1.00× |
| funboard | 2 | 1.25× |
| fish | 3 | 1.50× |
| hybrid | 4 | 1.75× |
| shortboard | 5 | 2.00× |

**Watch the compounding before raising the top end.** The shortboard also clears the fixed thresholds
more readily than a foamie (FR11's calibration trap), so its real advantage is this multiplier
*times* its natural turn-rate advantage, and only the first of those is visible on the card. If 2.00×
turns out to be too much, `k = 0.15` puts the shortboard at 1.60× with the foamie unchanged at 1.00×.

### D5 — still open, but a number to start from

20% of a turn's graded value paid at the exit, 80% through the window. Enough for the counter to move
visibly on every scored turn (FR6 tier one) without making the turn worth much on its own. D9's
sizes realise this: lump 2.0 × grade against a window worth ~7.6 credit-s for an isolated big turn.

### What these numbers do NOT establish

- **The floor D6 actually specified was not measurable.** The trace CSVs carry
  `t,gameSeconds,frame,x,y,z,vx,vy,vz,roll,pitch,yaw,step,slopeSin,planing,underwater` — no crest
  position, no `signedDistanceToCrest`, no band. So "time from turn exit until the board is back
  inside the band" could not be computed. **These figures bound the window from the "too long" side
  only.** The recovery-fits-inside floor still needs either a band column added to the recorder or a
  device session. Add the column — it is one field, and it makes this measurable forever after.
- **Yaw here is world yaw, not wave-relative.** FR2 requires the heading be measured against the
  wave, which will read differently on a curved face — a traverse that contributes nothing
  wave-relative still moves world yaw. Expect to re-check the sweep threshold once FR2 lands; if
  anything the real qualifying bar wants to be *lower* than 60°.
- **The sample is one board, and a rider not trying to link turns.** The traces predate the pump
  button and the touch-control work. Treat the turn rate as a floor on what a practised player
  produces, not a typical value.

## Implementation details

Suggestions, not prescriptions.

- **Never read roll to detect a turn.** `worldRollSin` stays around 0.04 (~2.7°) even through a hard
  turn — the board is roll-stiff, measured twice ([[small-weight-shift-sharp-turn]],
  [[surface-carve-roll-spring]]). A lean-angle detector reads noise. Yaw rate and sideslip are the
  signals that move.
- **`board.forwards` is local +Y**, not +X — the mesh is rotated 90°. Read the axis off the actor;
  never hardcode it. `SurfAssist::FInputs` already documents this and already receives `BoardForward`
  and `BoardLeft` per tick, so the detector can sit alongside it and be fed the same way rather than
  resolving axes itself.
- Yaw rate needs smoothing and must be framerate-independent — `SurfAssist::FState` already keeps
  `SmoothedHeadingRate` with `HeadingRateSmoothingSeconds`. Reuse it rather than adding a second
  smoother that disagrees with the first. See also `framerate-independent-angular-damping.md`.
- **Watch for a reward loop with the physics.** `lateralTurnHardCarveBoost` already pays a hard turn
  in speed ([[hard-carve-progressive-boost]]), and speed already multiplies credit. Adding trick
  points makes a hard turn pay three times. That may well be correct — a good turn *should* be worth
  a lot — but it means the trick coefficient has to be tuned against the loop, not in isolation.
- **The window state belongs in the same caller-owned struct as everything else here** — remaining
  seconds, current multiplier, chain step. `SurfAssist.h`'s two rules (no UObject, no globals; all
  mutable state in a caller-owned `FState` the pawn owns) are what make the detector testable without
  a world, and a window timer is exactly the kind of thing that ends up a static by accident and then
  leaks between rides and between PIE runs.
- **The window is applied where credit is already accrued**, `ASurfboardPawn::AccrueRideCredit` —
  it already early-outs on `!bRideActive`, which is the fall case for free, and it already has
  `DeltaTime`, `bGuardActive` and speed in hand. Add the trick accumulator beside
  `AssistRideCreditSeconds` and leave `AssistState.CreditSeconds` alone (FR9).
- All thresholds and weights on `USurfTuningSubsystem` so they are A/B-able through
  `Saved/TuningOverrides.json` with no rebuild. A launch-time `-ExecCmds` CVar will not stick — the
  subsystem pushes every tick ([[tuning-overrides-json-ab]]).
- The detector belongs in `SurfAssist.cpp`'s style: free functions over a small state struct, no
  UObject, testable without a world. The overlay belongs beside `RideScoreOverlay`, root
  `SelfHitTestInvisible` or it eats every tap below it.
- If this touches a `BlueprintCallable` signature, calling Blueprints need a manual Refresh Node +
  Compile or the whole graph dies silently and the board free-falls
  ([[bp-signature-change-requires-node-refresh]]).

## Related

- `specs/ride-score-counter.md` — the counter this extends; its FR1 and D2 are what D1 here reopens
- `specs/gradual-control-handoff.md` — the assist, the credit, and the automated-run gate
- `specs/board-selection.md` — per-board best scores and per-board tuning overlays (D3)
- `specs/fin-force-normalization.md` — where `cosYawAngleOfAttackLeftN` came from and what it means
- `specs/hard-carve-progressive-boost.md` — the physics that already pays for a hard turn
- `specs/input-trace-replay.md` — the rig the detector tests run on
