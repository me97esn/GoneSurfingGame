# Spec: Board selection — the board is the difficulty setting

## Status
- [x] Spec drafted (2026-08-31)
- [x] Board profile format + `USurfBoardSubsystem` (load, persist selection, apply). Both profiles
  parse and load (verified headless 2026-08-31).
- [x] Tuning baseline fix so board values never leak into `Saved/TuningOverrides.json`.
  **Verified against a run that actually applied a board**: 5 coefficients set, and the dev JSON
  still held only its one human-set key afterwards (AC2).
- [x] Visual mesh swap on a render-only component; physics body untouched. Verified from the swap's
  own diagnostics: `registered=1 visible=1 radius=43.0cm | physics mesh hidden=1`.
- [x] `SBoardOutlineGlyph` + `UBoardOutlineWidget` (UMG wrapper for the bottom-bar buttons)
- [x] `BoardPanel` dialog — what this board is, opened by tapping its outline
- [x] Two shipped profiles: `soft-top`, `shortboard` (tuning deltas are first guesses — dial in on
  device, they have never been ridden)
- [x] FR8 scripted-run gate. First cut FAILED: the `surf.autopilots` cvar arrives via `-ExecCmds` a
  frame after the pawn's BeginPlay, so a `::surf-straight` run applied the soft-top and retuned five
  coefficients under a baseline recorded on compiled defaults. Fixed with `FApp::IsUnattended()`,
  the same race-closer the Start screen uses; re-verified — profiles load, nothing is applied.
- [ ] Editor: repoint the Restart / Replay / Back visibility binding in `WBP_SurfboardControls`
  from `bStartScreenActive` to **`bRideUIBlocked`** (same polarity - show when false). Without it
  they draw straight through the board rack, over the screen the player is choosing on. One flag
  rather than two AND-ed, so the next modal hides them for free.
- [x] The picker is C++ (`BoardPanel`). The button that opens it is the UMG ride bar's, built
      on `UBoardOutlineWidget` so it draws the live board - the C++ pill that stood in for it
      (`BoardButtonOverlay`) is deleted, since two buttons for one job cannot stay aligned
  to *place* them - only the visibility repoint above.
- [ ] Second board mesh imported from `Content/Surfboards/OrangeSurfboard/orange_surfboard.fbx`
- [x] Eye-test: the swapped mesh on the wave. Both boards captured mid-ride and visibly different
  (2026-08-31) — the soft-top full and round-nosed, the shortboard narrower and flatter at
  `visualScale` 0.92 (bounds 43.0cm vs 39.6cm) — with the ASSIST badge full on one and empty on the
  other, so the per-board assist level shows too. Needed two supporting fixes, below.
- [x] `StartScreenSkip` tunable: drop straight into the ride. Also skips the first-play assist card,
  because honouring it for the menu alone just moves the click one screen later and the ride stays
  unreachable. Not PIE-only, unlike `AssistSkipFirstPlayCardInPIE` — the runs that need it most are
  standalone `-game` ones. Set in `Saved/TuningOverrides.json`, which is not shipped.
- [x] `Screenshot.ps1`: force the game window topmost before capture. `SetForegroundWindow` alone
  let an always-on-top window sit over it, and every shot came back with its left half black in the
  shape of that window — the script's own comment already said the window "has to actually be on
  top", it just was not made so.
- [x] Material swap verified. It had never actually been exercised — both profiles pointed at
  `M_Surfboard`, so the two screenshots differed only in mesh. Shortboard moved to `M_Basic_blue`
  (placeholder until the orange board's own textures land) and the boards now read white vs solid
  blue.
- [x] **Tuning measured, not assumed** (AC1). Same autopilot
  (`surfing_down_the_line_then_sharp_turn_right`), same board, only the angular damping changed —
  shipped values vs all three axes at 1.0:

  | | roll | pitch | yaw |
  |---|---|---|---|
  | mean rotation rate, shipped | 4.1 °/s | 5.4 °/s | 3.6 °/s |
  | mean rotation rate, damping 1.0 | 0.6 °/s | 0.6 °/s | 0.5 °/s |
  | ratio | **0.15×** | **0.10×** | **0.14×** |

  Total yaw travel collapsed 111.4° → 25.1° (0.23×): the board very nearly stops turning. Peak
  rates fall less (p99 0.5–0.8×) than mean rates, which is the expected signature of damping — it
  resists *sustained* rotation while a hard torque still produces a spike.
- [x] `-BoardInTests` / `-Board=<id>` command-line switches, which is what made that measurement
  possible: autopilots only run in scripted runs, and FR8 bars boards from those. Command line, not
  a CVar — `-ExecCmds` arrives a frame after the board is applied. `-BoardInTests` logs a warning so
  a baseline can never be approved from such a run by accident.
- [ ] Player-validated on phone: the two boards feel different, and the choice reads as real
- [x] Assist UI retired (2026-09-02). `AssistOverlay` (the badge) and `AssistPanel` (settings and
  first-play card) deleted, with their call sites, exec aliases and the card scheduling. Done ahead
  of board validation rather than after it, because two competing assist UIs blocked both the merge
  and the corner the board button needs.

  The *mechanism* is untouched, as this spec always said: the closed-loop controller stays, and
  `GetPendingAssistAlpha` already preferred the manual level a board pins, so alpha was coming from
  the board and not the credit schedule before any of this was removed.

  What went with it: the tappable badge and its strength bar, the settings panel, the first-play
  card, and the badge pulse on a level step (the ride-score counter carries that alone now). The
  panel also held the score/best-ride summary, which has no home at present — see below.
- [x] Score found its home (2026-09-02): **best ride is now per board**, shown on that board's card
  as `YOUR BEST`, and a board never ridden shows a dash rather than a zero.

  A single global best was the wrong shape for this feature. It is owned for ever by whichever board
  scores most easily, so once the foamie sets it a shortboard run can never touch it - the number
  stops being a target and quietly punishes moving to a harder board, which is the opposite of what
  board selection is for. Per board, each card carries its own record and an untried board reads as
  an invitation.

  This is also what the spec always said would happen to the ride score once the fade retired: it
  stops driving a hidden alpha and becomes the thing that suggests the next board. Per-board bests
  are exactly the signal an unlock ladder would read.

  The ride screen needed no new widget - `RideScore::Show` already reveals a best when the ride ends,
  which is the moment it matters, and it now reads the current board's record.

  The lifetime total that also lived in the assist panel was **dropped**, not rehoused: it only ever
  goes up, so it is neither a target nor a record.

## Motivation

User testing of the gradual control handoff ([gradual-control-handoff.md](gradual-control-handoff.md))
found three UX failures and one much more interesting result.

The failures, all of them problems with the assist being **a system the player manages**:

1. Players who read every instruction still did not understand what the assist did, or that the
   badge was tappable.
2. The fade thresholds are unguessable — no way to set them without being too eager or too slow.
3. The level-down is easy to miss even when celebrated.

The result that matters more:

- Riding with full assist is **fun**. The player stays on the wave and gets a long ride.
- But it does not feel like being *better* than you are. It keeps you in the right place while
  making sharp turns harder.
- Full assist **feels like surfing a foamie**. Easy to go straight, hard to manoeuvre. No assist
  feels like a shortboard.

That last observation is the whole spec. The assist was already producing a *board*, not a helper —
players just had no name for it. Give it the name and every one of the three failures dissolves:

- Nothing to explain. "The beginner board is forgiving" is true of real surfboards, so the player
  arrives already knowing it. No info card.
- Nothing to threshold. The player changes board when they want to, which is the only schedule that
  is ever correct.
- Nothing to miss. The board is visibly different, all ride.

And it turns a limitation into a feature: the player who wants to throw the board around **chooses**
the responsive one, rather than waiting for a hidden meter to let them.

## What this replaces

The assist *mechanism* survives untouched — it is good, and it does real work ("it helps you stay in
the correct place"). What is retired is the assist as a **player-facing system**:

| Retired | Why |
|---|---|
| The credit → alpha fade schedule | A board's assist is fixed. A fading board is not a board. |
| `CreditFullSeconds` / `CreditMidSeconds` / `CreditZeroSeconds` / `AlphaAtMid` / `AssistedCreditRate` | Only the fade reads them. |
| The assist badge's strength bar draining | Nothing drains any more. |
| The first-play assist card | The board explains itself. |
| The level-down card + celebration | There is no level-down. |
| The tappable badge → assist panel | Replaced by the board buttons → board panel. |

**The ride score is not retired.** It already measures unassisted ride quality, which is exactly the
right signal for "you are ready for a shorter board". It stops driving a hidden alpha and starts
being the thing that suggests (or unlocks) the next board. See
[ride-score-counter.md](ride-score-counter.md).

Retirement happens **last**, only after two boards are player-validated. Until then both systems
coexist and the profiles simply pin an assist level.

## Design

### D1. A board is a JSON file

One file per board in `Content/Boards/<id>.json`, shipped with the game. This mirrors
`Content/InputTraces/` and `Saved/TuningOverrides.json`: text, diffable, editable on the device
without a rebuild or an editor round-trip, which is how every physics decision in this project has
actually been made.

```jsonc
{
  "id": "soft-top",
  "displayName": "Soft-top",
  "tagline": "Holds its line. Hard to lose.",
  "description": "A big, floaty beginner board...",   // the dialog copy
  "assistLevel": 4,                                    // 0..kAssistLevelCount-1, fixed
  "visualMesh": "/Game/3dModels/surfboard1.surfboard1",
  "visualMaterial": "/Game/Materials/M_Surfboard.M_Surfboard",
  "visualScale": 1.0,
  "surferDeckOffsetZ": 0.0,
  "outline": { "width": 0.30, "widePointFore": 0.0, "noseRadius": 0.55, "tailWidth": 0.62 },
  "tuning": { "AngularDampingZ": 0.22, "lateralTurnCoefficient": 3000.0 }
}
```

`tuning` keys are `USurfTuningSubsystem` property names — the same namespace
`Saved/TuningOverrides.json` uses, validated the same way, with unknown keys logged and skipped.

**Why not a `UDataAsset`.** It would need editor authoring for every tweak, and it would put the
physics numbers somewhere a device A/B cannot reach. The one thing an asset buys — a mesh picker —
is not worth losing the tuning loop over; the mesh is a path string, exactly like `RailsIntroTrace`.

### D2b. Four layers, and where a phone can write

Tuning arrives in four layers, each a sparse diff against the one beneath, most specific winning:

| # | layer | file | written by |
|---|---|---|---|
| 1 | compiled defaults | the code | a rebuild |
| 2 | board profile | `Content/Boards/<n>-<id>.json` | hand, on PC |
| 3 | global overrides | `Saved/TuningOverrides.json` | hand |
| 4 | board overlay | `Saved/BoardTuning/<id>.json` | **the in-game HUD** |

The constraint that forces layer 4: `Content/Boards` is staged **into the pak**, so on the phone a
board profile is READ-ONLY. Per-board tuning cannot live beside the profile even in principle;
`Saved/` is the only writable location on device.

And the reason it must be per-board rather than the one global file: the HUD used to write layer 3,
so tuning `AngularDampingZ` while riding the shortboard put that value on the *foamie* too the
moment the player switched. Two boards could not be tuned in one session without contaminating each
other — which is most of what a five-board game needs to do.

So **the HUD always writes the active board's overlay**. Layer 3 stays for genuinely cross-cutting
things, which is what already lives there: `StartScreenSkip`, `AssistSkipFirstPlayCardInPIE` — dev
switches, not feel. With no board installed the HUD falls back to layer 3, which is the behaviour
this project had before boards existed.

The HUD shows which board it is editing. With four layers and a value that looks identical whichever
one set it, that is not decoration: it is otherwise entirely possible to dial in the foamie while
believing you are on the shortboard.

**Settling values.** An overlay is exactly the diff not yet committed to a board's identity, so
`Tools/PromoteBoardTuning.ps1 -Board <id>` merges it into the profile and archives it — the board now
ships those numbers and the overlay starts empty for the next round. `-FromDevice` pulls it off the
phone first, using the same `adb shell find` discovery as `Tests/PullInputTraces.ps1`.

### D2. Precedence, and the trap it avoids

Order of application, whenever the active board changes:

```
compiled defaults  →  the board's "tuning" block  →  Saved/TuningOverrides.json
```

The dev JSON stays **global** and applies on top of whatever board is active — that is the right
semantic for a cross-cutting experiment. To tune one board, edit that board's file.

**The trap.** `USurfTuningSubsystem::SaveToDisk` is a *sparse* write: it stores every property whose
current value differs from the compiled default. If a board profile simply wrote its values into the
live subsystem, then the next HUD edit would mark the subsystem dirty, and the debounced save would
bake **the whole board profile** into `Saved/TuningOverrides.json` as if a human had dialled it in.
Those values would then survive a board switch and silently pin the shortboard to the soft-top's numbers —
a bug that would present as "the boards stopped feeling different" long after the cause.

Fix: the subsystem keeps an **effective baseline** — `Defaults` overlaid with the active board's
tuning block. `SaveToDisk` and `ResetToDefault` compare against the baseline rather than the raw
default. So the JSON only ever contains what a human changed *relative to the board they were
riding*, and a board's own numbers can never leak into it.

### D3. Physics geometry is board-independent, by construction

This is the constraint that makes the whole feature cheap, and it is not negotiable:

- **The collision body never changes.** `SurfboardActor`'s existing `StaticMeshComponent` keeps its
  convex hull (`Convex18DOP`), keeps `bOverrideMass` / `MassInKgOverride`, and stays the component
  every `AFluidDynamics` actor pushes impulses into. Same mass, same inertia distribution.
- **The ~20 samplers never move.** No per-board rig, no per-board re-tune of sampler positions.
- **Mass stays fixed across boards.** Mass rescales the effect of every force coefficient at once, so
  varying it would invalidate the shared baseline the boards are A/B'd against. "Heavier" is
  expressed through the damping values, where the change is visible.

What varies is coefficients, assist level, and appearance. That is enough: the two feels this spec is
built on were produced *by coefficient-and-assist differences alone*, on one physical board.

### D4. The visual swap

A second, render-only `UStaticMeshComponent` is attached to the physics component at runtime:

- collision `NoCollision`, no physics, no mass contribution
- the physics component's own rendering is switched off with `SetVisibility(false)` — which does not
  propagate to children, so the surfer skeletal mesh riding on it is unaffected
- the profile sets its static mesh, material and uniform scale

Uniform for every board including the default, so there is no "board 1 is special" path to be
surprised by later.

**Length is capped at ±10 %.** The samplers stay put, so a visual mesh much longer than the physics
board would have nose and tail poking past where any force is sampled, and spray spawning inboard of
the tips. Within ±10 % this is invisible. So the boards differ in **outline, colour and graphics —
not in literal length**, and `visualScale` exists to bring an imported mesh into that band. Deck
thickness is likewise held equal (`surferDeckOffsetZ` is there for small corrections, not for a
visibly thicker deck) so the surfer's feet stay planted.

### D5. When a switch takes effect

- **Not riding** (start screen, or after a wipeout — which is when the bottom bar is up anyway):
  applies immediately. The player taps an outline, reads the dialog, closes it, and the board under
  the surfer *is already the new one* before they press Start. That immediacy is most of what makes
  the choice feel real.
- **Mid-ride**: deferred to the next ride, and the dialog says so. Same rule as `SetAssistMode`
  ("takes effect on the next ride, so the board never changes feel mid-wave") — swapping tuning
  under a planing board is a discontinuity with no honest physical reading.

The selection itself is immediate and persisted either way.

### D6. The UI: an outline is the button

On the main screen, each board is **only its outline** — no text, no badge, sitting on the bottom bar
beside Restart / Replay. Tapping one does two things at once: it **selects that board** and **opens
its dialog**. So the differences are readable, but reading them is never a prerequisite: a player who
taps and dismisses has still changed board, and a player who wants to know what they just picked has
it right there.

The outline is **drawn, not imported** — same reasoning as `STutorialBoardGlyph`: it stays vector,
tints with state, needs no texture asset, and can be shaped from the profile. It is generated
parametrically from the four `outline` numbers, so a new board gets a distinct silhouette by editing
JSON. A shortboard is narrow with a pointed nose and its wide point aft; a soft-top is wide with a
round nose and its wide point centred — recognisable at bottom-bar size on a phone.

Since the bottom bar is UMG, `SBoardOutlineGlyph` ships with a `UBoardOutlineWidget` wrapper so the
buttons can be built in the existing widget blueprint.

The dialog (`BoardPanel`) follows `AssistPanel`'s shape exactly — a namespaced Slate overlay, hooks
passed as `TFunction`s so the widget never touches actors, pausing the world only if nothing else
already has. Copy is **plain language, no physics jargon**: what the board does, not what its
coefficients are.

### D7. Two decisions that look like defects

Both settled 2026-09-03, both verified in a real mid-ride open (`-BoardPicker -BoardPickerDelay=14`,
logged as `riding=1 controls=1`). Recorded because either could be "fixed" later by someone who
assumed it was an oversight.

**The surfer is not visible behind the rack, and that is fine.** The scrim is 0.90, so about a tenth
of the scene reads through - spray, water, the radar all show. The surfer is hidden because the cards
sit centre-screen exactly where they are, not because of the scrim. Deliberately left: the rack is a
screen for comparing boards, and the ride behind it is not information the player needs while doing
that. Do not move the rack off-centre or thin the scrim to reveal them.

**Selecting mid-ride changes nothing until the next wave.** Not the mesh, not the material, not the
scale, not the tuning, not the assist. The card chip says `NEXT WAVE` instead of `RIDING NOW`, which
is the only honest thing it can say. Swapping tuning under a planing board is a discontinuity with no
physical reading; swapping only the MESH would be worse still, since the player would then be looking
at a shortboard that is behaving like a foamie. Between waves - start screen, or after a wipeout - the
switch applies immediately, so the board has already changed by the time the rack closes.

## Functional requirements

- **FR1** Board profiles load from `Content/Boards/*.json` at game-instance start. A malformed or
  missing file is logged and skipped, never fatal; if no profile loads at all the game runs exactly
  as it does today (compiled defaults, no board applied).
- **FR2** The active board persists across level reloads and app launches.
- **FR3** Applying a board sets, in order: tuning baseline (defaults → board → dev JSON), assist
  level, visual mesh + material + scale, surfer deck offset.
- **FR4** A board's tuning values can never be written into `Saved/TuningOverrides.json` (D2).
- **FR5** The collision body, mass, inertia and sampler positions are identical for every board (D3).
- **FR6** `SelectBoard(i)` selects and opens the dialog. It applies immediately when not riding, and
  defers to the next ride when riding (D5).
- **FR7** Each board draws a distinct outline from its profile, usable inside UMG.
- **FR8** Nothing in this feature runs during snapshot/replay test runs — the same
  `surf.autopilots` gate the assist and fall detection already use — so the baselines keep meaning
  what they meant. Tests always run the compiled defaults, no board applied.

## Acceptance criteria

- **AC1** Given two profiles differing only in `tuning`, when the player switches boards and rides,
  then the ride measurably differs (yaw rate under the same commanded lean).
- **AC2** Given a board is active and the player edits a coefficient in the tuning HUD, when the JSON
  is written, then it contains **only** that coefficient — no board values.
- **AC3** Given a board switch, when the board is applied, then mass, inertia and every
  `AFluidDynamics` sampler world-offset are unchanged from before the switch.
- **AC4** Given the player is on the start screen, when they tap a board outline and close the
  dialog, then the visible board has changed before the ride starts.
- **AC5** Given the snapshot suite runs, then results are identical with the feature present and no
  board selected (FR8) — the existing baselines still pass.
- **AC6** Given the rails intro ([deterministic-ride-handoff.md](deterministic-ride-handoff.md)),
  when any board is active, then the intro is bit-identical — forces are suppressed on rails, so
  board tuning cannot reach the wave catch.

## The one open physics question

The rails intro makes the **catch** board-independent by construction (AC6). The **stamp** at handoff
is not: it hands the board a velocity, and a board tuned with less slope-thrust may not sustain it.

This has been proven across *assist levels* — assist changes weight input, not coefficients, and both
ends of it have been ridden from this same intro. It has **not** been proven across *coefficient
sets*, because there has only ever been one.

Cheap test, no code: put a candidate shortboard's numbers in `Saved/TuningOverrides.json`, ride from
the rails intro, and see whether the board holds its line after the stamp or falls off it. If it
stalls, either stamp a per-board handoff velocity, or accept that a shortboard is genuinely harder
straight after the drop — which is arguably correct.

## Test cases

| Input | Expected |
|---|---|
| No `Content/Boards/` directory | Game runs on compiled defaults; one log line; no crash |
| `foamie.json` with a stale tuning key | Key logged and skipped; the rest of the profile applies |
| Switch foamie → shortboard → foamie | Tuning returns exactly to foamie's values (idempotent) |
| HUD edit, then board switch, then HUD read | The edit survives (dev JSON re-applies last) |
| `visualMesh` path that does not resolve | Physics mesh stays visible; warning logged; board still applies its tuning |
| Test run with `surf.autopilots` set | No board applied; defaults intact (FR8) |

## Gotchas

- `SetVisibility(false)` does **not** propagate to child components by default — which is what keeps
  the surfer visible when the physics mesh is hidden. Do not add `bPropagateToChildren`.
- `USurfTuningSubsystem` is a **game-instance** subsystem: it survives the level reload that a
  restart performs. Board application must therefore be idempotent and must reset to the baseline
  first, not accumulate.
- The board's mesh is referenced by path string and soft-loaded. A renamed asset fails at runtime,
  not at compile time — hence the "physics mesh stays visible" fallback.
- Import `orange_surfboard.fbx` with **no collision** (the visual component must not contribute one).
