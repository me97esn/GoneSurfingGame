# Barrel-glide-through bug — investigation notes

> **RESOLVED (2026-06-28) for the sharp-turn case** via [planing-redirect.md](planing-redirect.md): instead
> of adding a per-actor "wall" drag (the [wave-penetration-resistance.md](wave-penetration-resistance.md)
> plan below), the Chaos integrator now rotates the board's world velocity toward the wave **up-slope**,
> gated by nose submersion. The board rides up the face instead of plowing through it. Tuned default
> `PlaningRedirectMaxAngle = 2.0` rad/s — stops the glide-through 3/3 in testing while still carving;
> player-verified a "big improvement." The three work items below were the *pre-redirect* plan; #3
> (wave-penetration-resistance) is effectively superseded by the redirect. Keep this doc as the evidence
> record.
>
> **STATUS (2026-06-12): this is the parent investigation doc. The findings below have been split into three actionable specs, to be done in order:**
> 1. [passive-slope-thrust.md](passive-slope-thrust.md) — **do first.** Restore gravity-down-the-face propulsion (the board currently coasts to a stop with zero forward drive). Upstream of everything else.
> 2. [fin-force-normalization.md](fin-force-normalization.md) — fix fin drag & lift (un-normalized SC basis). Coupled to #1 (fins absorb the perpendicular component of slope gravity).
> 3. [wave-penetration-resistance.md](wave-penetration-resistance.md) — **blocked on #1 & #2.** Per-actor cross-flow drag / wall force. Re-observe the glide-through with propulsion + fins before designing; it may change or resolve.
>
> Keep this doc as the evidence/derivation record; the specs are the work items. The original symptom and early force-map analysis below predate the propulsion finding — read the dated "Update"/"UPSTREAM" sections for the current understanding.

## Symptom

In the `Boards_on_flat_water` level with the `barrel-glide-through` autopilot, the surfboard catches the wave, builds speed, bottom-turns, surfs along the face for a moment in the tunnel — then glides through the unbroken wall of the wave to the outside (open water behind the wave) instead of being deflected back. Real surfing physics would have the wall push the board hard.

## When it happens

- Autopilot: `StateTriggerAutoPilot` named `barrel-glide-through`, Step 5 "Surf along the wave"
- Approximate moment: **~2 seconds into Step 5**, which corresponds to **WaterController.CurrentFrame ≈ 920** (Step 5 activates at frame ≈ 1065; the wave-loop wraps at ~1080 down to ~895, so by t=2s into Step 5 the frame is ~920)
- Useful anchors: activation = 1065; bug window ≈ frames 896–932 (t=1.0s–2.5s into Step 5)

## What the data shows at the bug moment

Re-run with `surf.debug.flags drag,state,abs_velocity,rel_velocity` and `surf.debug.actors left_middle,right_middle,nose,tail`. Around frame 920:

| Quantity | Value | Source |
|---|---|---|
| Absolute water velocity at board | `mag = 1864–2313 cm/s` (~19–23 m/s, dominantly -X "toward shore") | `AbsWaterVel` log lines from `SharedCalculations` |
| Relative water velocity (water − board) | `relWaterVelMag = 1554–2147 cm/s` | per-actor drag debug |
| AmountPlaning | 0.990 | per-actor drag debug |
| Bottom drag (all 6 bottom actors) | **`dragForce = (0,0,0)`** | `=== DRAG DEBUG ... ===` blocks |
| Bottom drag `relVelAlongMag` (along board.forwards) | 6–20 cm/s | same |
| Tail drag | 0 (`cosWaterForwards = 0.019`) | `=== TAIL DRAG DEBUG ===` |
| Rail drag (LEFT) | `dragAmount ≈ 1000–1300`, direction along relWaterVel-normalized | `=== RAIL DRAG (LEFT) ===` |
| Rail drag (RIGHT) | not firing at this moment | grep returned 0 hits in the window |
| Rail drag attenuation | `cosYawLeft = 0.192` × `planingAttenuation = 0.201` | same |

So: the simulation correctly sees a 20+ m/s flow past the board (the wall is "real" in the data), but the drag formulas reduce the resistance to a single rail-drag term of ~1000–1300 force units while bottom drag computes to zero.

## Why bottom drag is zero (mechanism)

Two independent killers on `bottomDrag` (in `FluidDynamics.cpp::calcBottomDrag`):

1. **Planing attenuation `(1 - planing)²`**. With `AmountPlaning = 0.990`, the multiplier is `(0.01)² = 0.0001` — kills any bottom drag almost completely. This is intentional: a planing board "skates on top" with little wetted-surface friction.

2. **Projection onto board.forwards**. The formula uses `relVelAlongMag` (the component of `relWaterVel` along the board's forward axis), which is only **6–20 cm/s** at the bug moment vs. `relWaterVelMag = 2000+ cm/s`. The wall's flow is mostly *perpendicular* to the board's forward axis, so the component the formula cares about is tiny. Squaring that tiny number gives a microscopic force.

## Why rail drag isn't enough

`railDragForce` (the only meaningful pushback term firing) uses the full `relWaterVel` magnitude, which is large — but it's gated by:

- `cosYawAngleOfAttack` (geometric perpendicularity, observed 0.192 — only 11° angle of attack)
- `planingAttenuation = (1 - planing × (1 - perpendicularity)) ≈ 0.201` — another ~5× reduction
- Only the LEFT rail fires at this moment; the RIGHT rail's `cosYawAngleOfAttack` is on the wrong side of zero so its drag is gated off

Net result: rail drag is the *only* significant resistive term, and even it's reduced to ~20% of its un-attenuated value.

## Force map (relevant terms)

For a more complete picture, see [the full force map](#) — paraphrased from the codebase exploration of `FluidDynamics.cpp` and `Buoyancy.cpp`:

- **Drag terms** (oppose motion through water): bottomDrag, railDrag (L/R), finDrag, tailDrag — all use `relWaterVel`, all are heavily planing-gated by design.
- **Lift terms** (perpendicular reactions): bottomLift, railLift, finLift, lateralCarvingForce — most fire only on `VE_Down` side, also planing-related gates.
- **Forward-thrust terms**: `upwardsThrust` (angle-of-attack lift on bottom) and `waveSlopeSupplementForce` (along board.forwards, scaled by `sin(slope) × amountUnderWater`) — these *push the board forward through the water* including when it's heading into/through the wall. They compete against the small ~1000–1300 rail-drag pushback. **Update (2026-05-19):** `waveSlopeSupplementForce` was retired by [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md), replaced first by `waveSlopeGravityForce` and then by the bottom hydrofoil's `forwardsThrustCoefficient` (now the sole forward-propulsion term — perpendicular-to-flow, so it can't push the board through a wall it's not pointed at).
- **Buoyancy**: vertical-mostly. Has a horizontal component from `componentVelocity` magnitudes but it's a wake-effect term, not a wall-pushback term.

The user's intuition is correct: there's no force term that says *"a fast lateral water flow against my hull resists me crossing it"* in a way that survives planing. The rail-drag formula approximates this but is too weak in this regime.

## Diagnosis confidence

High. The specific data (relVelMag huge, relVelAlongMag tiny, planing 0.99, bottomDrag zero, single rail drag ~1200) was captured from a single reproducible autopilot run with debug flags enabled. The mechanisms (planing attenuation in bottom drag, projection onto forward axis) are visible directly in the formulas in `FluidDynamics.cpp`.

## Tools available for verifying a fix

1. **Snapshot test** (`barrel-glide-through`). Records position/velocity/rotation/frame every 50 ms during the autopilot run; `Tests/Compare.ps1` diffs against the committed baseline and reports per-step max-drift with an OK/WARN/REGRESSION verdict. Auto-runs after `RunGameAndCollectLogs.bat`. Calibrated thresholds: WARN > 1000cm or 1500cm/s, REGRESSION > 3000cm or 4000cm/s. Steps 0-4 are tightly reproducible (~10cm); step 5 has higher variance (run-to-run noise observed: 251cm and 739cm).
2. **Debug flags via `surf.debug.flags` / `surf.debug.actors`** (see [debug-logging.md](debug-logging.md) and [run-game-and-collect-logs.md](run-game-and-collect-logs.md)). Available flags include `drag`, `state`, `abs_velocity`, `rel_velocity`, `lift`, `thrust`, `buoyancy`, `weight`, `forces`. Combined with actor tokens like `left_middle,right_middle,tail,nose`, this scopes per-tick diagnostic output to the bottom actors that interact with the wave wall.
3. **`StateTriggerAutoPilot` step trace + WaterController.CurrentFrame**. The autopilot already logs each step transition with the frame number, plus a 0.5s heartbeat with the same. Cross-references to a specific wave-loop phase.

## Candidate fix directions (not yet decided)

These are sketches for future-me to reason from, not commitments. Each has trade-offs against gameplay feel and run-to-run reproducibility.

- **Add a "wall pushback" term**. A force proportional to `relWaterVelPerp²` (the component of relative water velocity *perpendicular* to the board's forward axis), pushing along that perpendicular direction, scaled by `amountUnderWater` and *not* gated by planing. This is the term that's missing — it captures "a fast lateral flow resists my hull" without disturbing the existing along-axis drag/lift behavior.
- **Reduce the planing attenuation on rail drag**. Currently `1 - planing × (1 - perpendicularity)` reduces rail drag to ~20% at this geometry. Easing this would let rail drag carry more of the wall pushback. Risk: changes how the board behaves on cleanly aligned planing runs.
- **Project `relWaterVel` differently for bottom drag**. Current formula uses `relVelAlongMag`. An alternative: use the *full* `relWaterVel` magnitude (or `relWaterVelPerp`) in a separate "wall" term, keeping `relVelAlongMag` for the existing along-axis friction. Effectively splits bottom drag into two terms.
- ~~**Make `waveSlopeSupplementForce` direction-aware**.~~ **Obsolete (2026-05-19):** the supplement, and its successor `waveSlopeGravityForce`, are both retired. Forward propulsion is now the bottom hydrofoil's `forwardsThrustCoefficient`, which is direction-aware by construction (perpendicular to flow in the pitch plane) and can't help the board climb a wall it shouldn't.

The first option is the cleanest because it adds a new term without modifying existing balanced ones. The second is the smallest change but most likely to perturb other things.

## File references

- `Source/GoneSurfing/FluidDynamics.cpp` — drag, lift, thrust, supplement formulas
- `Source/GoneSurfing/SharedCalculations.cpp` — water velocity sampling, relativeWaterVelocity calculation
- `Source/GoneSurfing/StateTriggerAutoPilot.{h,cpp}` — autopilot + recorder
- `Source/GoneSurfing/WaveHeight.cpp` — `calculateWaveVelocity()` reads baked velocity from `FWaveUnifiedFrameData`
- `Tests/baselines/barrel-glide-through.csv` — committed baseline trajectory
- `Tests/Compare.ps1`, `Tests/Approve.ps1` — snapshot test tooling

## Investigation history (so far)

- 2026-05-07: identified bug moment (Step 5 + 2s, frame ~920), added per-step `WaterController.CurrentFrame` logging to `StateTriggerAutoPilot`
- 2026-05-08: captured force-level diagnostics with debug flags, identified bottom-drag-zero + rail-drag-only-LEFT mechanism
- 2026-05-08: built snapshot-test framework (`barrel-glide-through` baseline + auto-compare) so any candidate fix can be regression-checked
- 2026-06-12: re-confirmed against current code with a fresh headless run; root cause re-derived from force vectors (section below). Earlier "no wall-pushback term" framing is now only partly right — a `waveMassFlowDrag` term was added since, but it doesn't address the real failure.

---

## Update 2026-06-12: data-confirmed root cause (supersedes the "no wall-pushback term" framing above)

The sections above predate the `waveMassFlowDrag` term and predate the level rename. This update is the current, data-backed diagnosis. Where it conflicts with the older notes, trust this.

### Current repro

- **Level:** `Surfing_infinite_wave` (NOT the flat-water default — the bug needs a breaking wave). The `surf.ps1`/`_paths.bat` default `TEST_MAP` is still `Boards_on_flat_water`, so set `TEST_MAP=Surfing_infinite_wave` for the run.
- **Autopilot:** `StateTriggerAutoPilotBP_Surfing_down_the_line`, `TestName = surfing-down-the-line`, Step 5 "Surf down the line".
- **Run command** (PowerShell, call the `.ps1` directly to avoid cmd colon-mangling):
  ```powershell
  $env:TEST_MAP="Surfing_infinite_wave"; & "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1" `
    -Argument "state,drag,thrust,abs_velocity,rel_velocity:bottom_left_middle,bottom_right_middle,SharedCalculationsBP,fin:surfing-down-the-line"
  ```
- Produces `Saved/Tests/latest/surfing-down-the-line.csv` (cols: `t,gameSeconds,frame,x,y,z,vx,vy,vz,roll,pitch,yaw,step,slopeSin,planing,underwater`) and the per-tick force log in `Saved/Logs/GoneSurfing.log`.
- **Time anchor:** autopilot heartbeat logs `t=<sec>s ... CurrentFrame=<n>` every 0.5s, each line carrying an engine-frame `[N]` prefix. In the 2026-06-12 run: autopilot `t=9.0s → engine frame [434]`, `t=9.5s → [489]` (~110 engine-frames/sec), so the escape (CSV `t≈9.45–9.56`) is engine frames ~[483]–[495]. The user's reported "9.467631 s" is the *world* clock (`gameSeconds`); same event, run-to-run offset.

### CSV signature of the escape

As `planing` collapses 0.42 → 0.00 (CSV `t` 9.45→9.56), the board *accelerates* (`vx` −68 → −85 → −144 → −203) and climbs only ~40 cm in `z` (320→337). A board being ejected would decelerate; it speeds up. The z-rise is trivial — **the board glides sideways *through* the wave, it is not lofted *over* it.**

### Force vectors at the escape (frame ~482, per bottom actor)

- `board.forwards` = (0.147, 0.132, 0.033) → normalized **(0.73, 0.66, 0.16)** — nose points *along the wave crest line* (dot ≈ 0.98 with crest tangent).
- board velocity ≈ (−74, +43, +44) → **(−0.77, 0.45, 0.46)** — nearly *opposite* the nose in x (forwards·v̂ ≈ −0.19). The board is sliding sideways/backward relative to its own forward axis.
- **Decisive number:** `relativeWaterVelMag = 263.89`, `relVelAlongMag = 9.35`. Of 264 cm/s of board-vs-water motion, only **9 cm/s is along board.forwards** — the other 263 is *perpendicular*. The hull is translating sideways through the water.
- `waveMassFlowDrag` (board-wide) = (−948, −347, −190), mag 1027, direction = `boardWideAbsWaterVel` — the **outward** wave flow. So this term pushes the board *out*, weakly; it is a (too-weak) restoring force, NOT a culprit.
- `forwardDrag` surges on de-planing (~200 while planing 0.8 → ~3600 at planing 0), but it acts along **±board.forwards = down the line / along the crest** — it changes down-the-line speed, it cannot push the board through a wall it is parallel to.
- `waveMassThrust` ≈ 325, small. Buoyancy ≈ vertical only.

### Root cause (corrected)

The board slides **sideways through the water** (263 of 264 cm/s of relative flow is perpendicular to `board.forwards`), and **nothing applies meaningful drag opposing that sideways slide**:
- bottom drag uses only the *along-forwards* projection (`relVelAlongBoard`, the 9 cm/s) and is planing-gated — it discards the 263 cm/s perpendicular component;
- **rail** forward drag IS planing-attenuated (`fwdPlaningAttenuation = 1 - AmountPlaning`, FluidDynamics.cpp:306) and near-zero here;
- **fin** drag is NOT planing-attenuated (the VE_Fin case has no planing factor — confirmed FluidDynamics.cpp:375-406), so it *should* be the term that resists sideways slip. It fails for a different reason: `cosYawAngleOfAttack ≈ 0.04–0.08` at the escape and the **cos⁴ falloff** (`dragFactor = perpFactor⁴`, line 391-392) crushes it to `dragFactor ≈ 0.0005`. `cosYawAngleOfAttackLeft = board.left · relativeWaterVelocityDirectionProjectedUp` (SharedCalculations.cpp:79) keys on the *horizontal* relative-flow direction's alignment with the board's left axis — at the escape that horizontal flow is ~along `board.forwards` (down the line), not across it, so the fins read the flow as "parallel" and disengage. Net fin contribution is **0.5–13 force units across the two fin actors** (C_139, C_146) vs. a board carrying ~250 cm/s of momentum — negligible. Logged across engine frames 480/485/490/495: perpFactor 0.035/0.074/0.069/0.080, fin-drag mag 0.5–0.7 / 5.9–7.7 / 4.3–4.6 / 12.6–11.4.
- `waveMassFlowDrag` points outward (along the *outward* wave flow) but is far too weak;
- buoyancy is vertical; there is still no force along the wave's horizontal normal.

**On the fins specifically — CONFIRMED ROOT CAUSE of fin failure (2026-06-12, second run with board-frame log):**

The board-frame decomposition of `relativeWaterVelocity` at the escape (added to the SC STATE log) settles the geometry: the slip is **horizontal and strongly sideways**, NOT vertical. Back SC (C_2), planing=0:

| engine frame | alongFwd | alongLeft | alongUp | horizMag | vertMag |
|---|---|---|---|---|---|
| 125 | −82 | **+58** | −50 | 109 | 27 |
| 150 | −116 | **+117** | −45 | 169 | 18 |
| 155 | −92 | **+101** | −46 | 142 | 26 |

`horizMag ≫ vertMag`, and `alongLeft` is 60–70% of the horizontal flow. (My earlier "dominant relative flow is vertical" hypothesis was wrong — disproven by this log.) So the fins *should* be biting hard.

They don't, because **`cosYawAngleOfAttack` is not a true cosine — it's scaled by the SC actor's transform scale (~0.2).** [SharedCalculations.cpp:79](../Source/GoneSurfing/SharedCalculations.cpp#L79): `cosYawAngleOfAttackLeft = surfboardLeft | relativeWaterVelocityDirectionProjectedUp`, where `surfboardLeft = this->left` is **un-normalized** (mag ≈ 0.2) while the projected dir is a unit vector. The comment at lines 62-65 says this scale-weighting is intentional ("downstream formulas are calibrated against" it). Consequence:
- `perpFactor = 0.2 × true_cos` (verified: frame 150 `alongLeft/horizMag = 116.5/169 ≈ 0.69` true cosine, but logged `perpFactor = 0.159 ≈ 0.69 × 0.2`).
- `dragFactor = perpFactor⁴` turns the 5× deflation into **0.2⁴ ≈ 625×** suppression. Logged `dragFactor ≈ 0.0006` vs `0.69⁴ ≈ 0.23` with a true cosine — fins are **~350× too weak**. Total fin output at the escape: ~17–280 units across the 2 fin actors vs. a board carrying ~250 cm/s.

**Fin fix (highest-leverage, mostly independent of the bottom term below):** make the fin `perpendicularFactor` use a **normalized** left/cosine (true cosine) so `dragFactor` reflects the real angle of attack. This alone restores ~350× fin drag and likely stops the sideways glide-through. Caveats: (a) line 79's scaling is shared with the rail-engagement gate, but that gate only reads the *sign* (`cosYawLeft > 0`), so it's unaffected; (b) the lines-62-65 comment claims `cosPitch/cosYaw` magnitudes are calibrated into other formulas — so normalize **specifically for the fin term**, don't rip the scaling out globally, or audit every consumer first; (c) consider `cos²` instead of `cos⁴` and/or a small floor so a near-miss in angle doesn't fully disengage; (d) only 2 fin actors (C_139, C_146) carry this whole term.

### Full fin-subsystem audit (2026-06-12) — the cosine bug is one of several, and it also breaks fin LIFT

The fin DRAG analysis above is only half the fin story. Auditing the whole fin subsystem (drag + lift) against a third run (flags `state,drag,lift,thrust`) surfaced a shared root cause and multiple compounding defects. **Root cause: the SharedCalculations basis vectors `this->left/forwards/up` are NOT unit vectors — they carry the actor's transform scale (~0.2). Code that treats them as unit deflates by 0.2 per occurrence.**

**Fin LIFT is the term that's *supposed* to stop sideways slip (airfoil "directional stability", `finLiftMagnitude = 0.001`, enabled). It is crippled ~25× by TWO independent 0.2 scales:**
- `sinSlip = this->cosYawAngleOfAttack` (FluidDynamics.cpp:825) = `0.2 × true sin(slip)` (the line-79 bug) → lift magnitude `L` is 5× too small.
- `liftDir = sign·sharedCalculations->left·cosSlip + sharedCalculations->forwards·|sinSlip|` (FluidDynamics.cpp:835-837) is built from the **un-normalized** `left`/`forwards` (mag ~0.2) → the direction vector is *another* 5× too short.
- Net: `finLiftForce ≈ 0.04× intended`. Measured worst case at the escape: `sinSlip 0.175, cosSlip 0.985, v²=7061 → L=1.24, force = (−0.1, −0.2, 0) ≈ 0.22 units`. Effectively zero directional stability.
- **Direction is also wrong:** because `sinSlip` is capped at ~0.2 by the scale, `cosSlip = sqrt(1−sinSlip²) ≈ 0.985–1.0` *always* — the formula believes every slip is <12° even when the real slip is ~60°. So the lift never rotates toward `+forwards`; the carve-coupling forward component (the entire point of [fin-carve-coupling.md](fin-carve-coupling.md)) is dead. **That spec's premise is broken:** it states `sinSlip = cosYawAngleOfAttack` is a true `sin(α)` ("reused field, not a typo") — it isn't, it's `0.2·sin(α)` — so its magnitude math and skid target (AC2 ≤5°) were computed against a quantity 5× off, and the feature it shipped is ~25× underpowered. A correction note has been added to that spec.

**Other fin findings (documented for completeness):**
- **Fin drag direction is fine** (`relativeWaterVelocityNormalized`, a true unit vector — SharedCalculations.cpp:187) — only its *magnitude* dies to `cos⁴`. But the magnitude uses the **full 3D** `relWaterVel.SizeSquared()` while the engagement gate uses the *horizontal* projection — so a large vertical relative flow could leak vertical fin drag (small at this escape, `vertMag≈18–27`, but a modeling smell).
- **Submersion inconsistency:** fin LIFT is ungated by water depth ("fully submerged by construction"), but fin DRAG still multiplies by `effectiveWaterHeight`. The two fin terms disagree on whether the fin is always wet.
- **Fins read the FRONT SharedCalculations (C_1)** — confirmed: fin `relWaterVelMag` matches `SC_C_1`, not `SC_C_2`. But fins are physically at the *tail*. Possible position/sampling mismatch worth verifying against the umap.
- **`maxDragAmount = 1e8`** → not a binding cap; fixes won't be clamped.
- **The two fin actors (C_139, C_146) log identical forces** — effectively one doubled fin, not a 2-or-3-fin cluster.

### UPSTREAM ROOT: zero forward propulsion the entire ride + no yaw damping (2026-06-12)

Tracing back *why* the board ends up slow and pointed into the wave revealed two upstream facts that reframe the whole bug:

**1. Forward propulsion is OFF for the entire ride.** All 1296 bottom-hydrofoil samples have `boardFwdForceMag = actorFwdForceMag = 0.00`. The hydrofoil's forward thrust (the ONLY forward-propulsion term since `waveSlopeGravity` was retired) is gated by `turnGate`, which opens on carve/pump intent. The `surfing-down-the-line` autopilot commands **no weight shift** (`lateralShift = 0` throughout) and **no pump** (`pumpInput = 0`), so `turnGate = 0` and forward thrust never fires. Upthrust is unaffected (`upForceMag` up to ~378). 

Consequence: the board pops up to ~507 cm/s, then **coasts** — decelerating 507→61 cm/s purely against drag/damping — de-planes, and washes through. The yaw drift (82°→51°) is from wave forces, not a commanded carve. **So the glide-through in this test is largely a no-propulsion coast-out, not a powered carve into the wave** — which walks back the "carves into the wave" framing earlier in this doc.

**The gate has a deadzone, and this is an editor-vs-phone representativeness problem (2026-06-12).** `intentGate = SmoothStep(0.05, 0.20, lateralShift)` where `lateralShift = |amountToTheRight − 0.5|`, and `turnGate = max(intentGate, pumpGate)`, `boardFwdForce ∝ turnGate` (FluidDynamics.cpp:1118-1133). So:
- `amountToTheRight ∈ [0.45, 0.55]` → intentGate 0 → **zero forward thrust**; full thrust needs `≤0.30` or `≥0.70` (a >20% lean), or pumping.
- The editor autopilot holds `amountToTheRight = 0.5` (roll-centered; it commands pitch for pop-up but never a turn), so it sits in the deadzone the whole ride. On the phone the weight is *never* exactly 0.5/0.5 — BUT the deadzone means near-neutral phone trim (lateralShift < 0.05) *also* yields zero thrust. So the editor's centered state is the extreme of a near-centered band that's dead on both platforms.

**Deeper design tension:** all forward propulsion is now *active-only*. Retiring `waveSlopeGravity` in favor of the gated hydrofoil thrust removed the *passive* gravity-down-the-face drive — so a neutrally-trimmed board gets zero forward drive (phone or editor). Either that's intended (deliberate "pump/carve to go", per the comment at FluidDynamics.cpp:1114 — in which case the **autopilot test must carve/pump or it tests an impossible idle state**), or the passive trim drive is missing and should be restored decoupled from the intent gate. **Decision needed before tuning physics against this trajectory.** The representative-input fix is exactly [input-trace-replay.md](input-trace-replay.md): replay a recorded (off-center, noisy) phone tilt trace so the editor lights up the turn-gate like real play. Re-run the glide-through with a propelled input before trusting this trajectory.

**WHY the intent-gate exists (design history from the author, 2026-06-12 — not in code comments):** before the gate, the board **gained/held speed going straight on flat water** when it should have decelerated. Mechanism: the hydrofoil forward thrust is `AoA × v²` and fires whenever the board planes at any angle of attack, *regardless of wave/slope*; meanwhile bottom drag is **planing-attenuated to ~0**. So on flat water at speed: `forwardThrust > 0`, `drag ≈ 0` → net positive → free acceleration. The intent-gate (`turnGate`) was a band-aid that suppresses the thrust unless the player shifts weight/pumps. The author suspects this was the wrong design — and any propulsion fix MUST NOT reintroduce free-speed-on-flat-water-going-straight.

**Implied design direction (satisfies both constraints):** the intent-gate keys propulsion on the wrong discriminator — *player input* — when the physical question is *"is there an energy source?"* i.e. **am I on a wave face gravity can pull me down?** Gate/scale passive forward propulsion on **wave slope** (`boardWideSlopeSin`) instead of intent: flat water (`slopeSin≈0`) → no drive → decelerates (original problem stays fixed); wave face at neutral trim (`slopeSin>0`) → passive drive → no coast-out; keep the pump/carve term as an *additive* booster, not the gate. This is essentially restoring `waveSlopeGravity` as the propulsion source.

**Refined model (author, 2026-06-12) — source from slope, direction along `board.forwards`, magnitude = the projection:** my first caveat (apply down-the-slope, not along-nose) was half-right. The energy *source* is gravity down the slope, but for a board surfing *down the line* the *applied* force should be along `board.forwards` — because the fins/rails REDIRECT the down-slope pull into forward motion. Note `board.forwards` is *almost perpendicular* to the fall line when trimming down the line. The clean formulation is the projection of down-slope gravity onto the forward axis, applied along it:

```
F_drive = board.forwards_unit · k · (waveSlopeDownVec · board.forwards_unit)
```

This self-handles every case with no extra guard: trimming down-the-line (forwards slightly down-face) → small positive drive; nose pointed UP the face → `(waveSlopeDownVec · forwards) < 0` → decelerates, won't push up the wave (the sign falls out); flat water → `waveSlopeDownVec ≈ 0` → no drive. The "almost perpendicular → small force" is fine because bottom drag is planing-attenuated to ~0, so a small continuous drive builds to high terminal speed — the SAME planing-drag property that caused the flat-water free-speed bug is what makes across-the-face trim fast once the drive is slope-sourced. Self-consistent.

**Couples to the fin fix:** decompose `waveSlopeDownVec` in the board frame → a `board.forwards` component (the drive above) + a PERPENDICULAR component. The perpendicular part is exactly what the fins/rails must resist. Working fins → absorb it → gravity converts to down-the-line speed. Broken fins (current) → perpendicular gravity isn't resisted → sideways slip down/across the face → the skid and wash-out. So the fins are the *mechanism that makes the slope-drive work*; the passive-drive design and the fin normalization fix are coupled, not independent.

**2. No force damps yaw/skid via local velocity.** All FluidDynamics forces are applied at the actor's own location (`applyForceAsImpulse` → `AddImpulseAtLocation(impulse, GetActorLocation())`, FluidDynamics.cpp:1339), NOT at COM. But the wave-mass drag (`waveMassFlowDrag`) uses **board-wide** slope/velocity/direction (same for every actor), so by design it produces no yaw torque (symmetric equal forces cancel — deliberate, see comment at lines 194–200) AND is blind to the differential velocity a yaw/skid produces — so it provides **zero yaw damping**. The per-actor `forwardDrag` keeps only the along-nose component and is planing-gated; the fins (which would damp slip) are broken (above). **Net: nothing resists yaw/skid using per-actor local velocity** — so once de-planed the board swings freely. A per-actor cross-flow drag (each actor resisting its OWN local lateral velocity) would damp both translation-slip AND yaw, unlike the board-wide wave-mass term.

### The whole board (NOSE included) glides in — it's a translation, so fins alone are insufficient (2026-06-12)

The crossing is **skid-led, yaw-follows** — the board breaks loose and slides sideways into the wave; it does NOT yaw its nose in to initiate the crossing. From the `surfing-down-the-line` CSV (nose heading = yaw + 90° due to the mesh rotation; `skid = velHdg − noseHdg`):

| t | yaw (Δ from 9.0) | nose hdg | vel hdg (Δ) | skid | plan |
|---|---|---|---|---|---|
| 9.30 | 51.40 (−0.7°) | 141.4° | 144.4° | +3.0° | 0.074 |
| 9.36 | 51.41 (−0.7°) | 141.4° | 146.7° (+4.6) | +5.3° | 0.051 |
| 9.40 | 51.44 (−0.6°) | 141.4° | 151.6° (+9.5) | **+10.1°** | 0.033 |
| 9.46 | 51.66 (−0.4°) | 141.7° | 152.2° (+10.1) | **+10.5°** | 0.011 |
| 9.86 | 54.93 (+3.3°) | 144.9° | 154.3° | +9.3° | 0.000 |
| 10.36 | 58.18 (+6.6°) | 148.2° | 153.2° | +5.0° | 0.800 |

**At the trigger (t≈9.30–9.46, planing collapsing): yaw is flat (Δ<0.3°) while the velocity vector swings ~8° into the wave** — the skid appears entirely from velocity rotation, not heading rotation. The penetration is a sideways break-loose of the rigid body (nose included), not a carve. *Only after* the skid is established does yaw begin to move (+3–6° over the next second, run-variable), and that is the **heading lagging and catching up to the already-skidded velocity** (velHdg plateaus ~152–156° while noseHdg climbs toward it) — a consequence, not the cause. Both SharedCalculations sampled large sideways slip at the escape (front-region `alongLeft ≈ +79`, back-region `≈ +117 cm/s`), confirming the whole hull slides. (Robust across runs: the trigger is skid-led with flat yaw; the aftermath yaw magnitude varies run-to-run, ~2–6°.)

**Consequence for the fix:** the tail fins are necessary but **not sufficient**. The nose has no fins, and because this is a translation, gripping only the tail would make the board **pivot about the gripped tail and swing the nose *further* into the wave** (until the rising slip angle stalls the fins) — converting a broadside slide into a nose-first rotation rather than stopping the crossing. **The bottom cross-flow drag term (distributed across all bottom actors, nose-to-tail) is the lever that actually resists the nose/whole-hull penetration; the fin fix only addresses the tail.** Prioritize the bottom cross-flow term for the glide-through; treat the fin fix as complementary (and as the fix for carving/skid in `fin-carve-coupling.md`).

(Note: the `nose` AFluidDynamics sampler doesn't match a `nose` debug-actor token — it logs under a different label; use the front SharedCalculations as the nose-region proxy, or find the real label in the umap.)

**Consolidated fin fix:** normalize the SC basis vectors at the point of use for *both* fin terms — a true cosine for `perpendicularFactor`/`sinSlip`, and unit `left`/`forwards` for `liftDir`. That single class of fix restores ~350× fin drag and ~25× fin lift simultaneously, and revives the carve coupling. Audit the other `this->left/forwards/up`-as-unit sites before a global change (the bottom hydrofoil's `pitchSinAngleOfAttack`/`cosPitch` are separate un-normalized fields some formulas may be calibrated against; the line-1103 bottom-thrust `slipGate` consumer of `cosYawAngleOfAttack` is "logged only", so it's not load-bearing).

**Tooling note:** in this third run `Saved/Logs/GoneSurfing.log` was truncated (~1712 lines) — a second editor instance contended for the file. The full per-tick debug output was in the wrapper's **stdout capture** (the task output file). If `GoneSurfing.log` looks short after a run, grep the stdout output file instead.

So the board coasts sideways across the wave on its own momentum, essentially frictionless in the cross-flow direction. De-planing is the **trigger** (losing planing lift removes the dynamic grip holding the trajectory *along* the wave, freeing it to slide off across the flow) — NOT a forward "motor."

### Claims retracted this session (were wrong)

- ❌ "`waveMassFlowDrag` pushes the board *into* the face" — it follows the *outward* flow; it pushes *out*.
- ❌ "de-planing `forwardDrag` is a motor that drives the board through" — `forwardDrag` is along-the-crest, can't push across it.
- ❌ "buoyancy lofts the board *over* the lip" — the board goes *through* ~horizontally; the 40 cm z-rise is incidental.

### Fix direction (precise)

Add a drag proportional to the board's velocity component **perpendicular to `board.forwards`** (equivalently `relWaterVelPerp`, the 263 cm/s), directed to oppose it, **ungated by planing** (the failure happens precisely when planing → 0). `calcBottomDrag` already computes `relWaterVel` and the forward projection; it just needs to *use* the discarded perpendicular component as a second drag term instead of throwing it away. Tune the coefficient so it resists punch-through without braking a cleanly-aligned planing run (where `relWaterVelPerp` is small by construction).

Next session: implement the perpendicular cross-flow drag term in `FluidDynamics.cpp::calcDragForce` (VE_Down case), re-run the repro above, confirm the board is deflected back onto the face instead of gliding through, and snapshot-check Steps 0–4 didn't regress.
