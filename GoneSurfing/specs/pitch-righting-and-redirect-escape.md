# Spec: Surface-relative pitch righting + one-sided planing redirect (stop the nose-first crest punch-through)

## Status
- [x] Spec drafted (2026-07-21)
- [x] A/B: `AngularDampingY` 0.4 vs 0.135 on `bottom-turn` (2× each, repeatable): 0.135 cuts the
  crest punch-through (submersion 0.97→0.88, climb +65→+25 cm, turns 24° deeper) but doesn't cure
  the buried nose — see "A/B result" below
- [x] Phase 1 — one-sided (inward-only) planing redirect, knob default = current (symmetric):
  `PlaningRedirectOutwardScale` (engine + tuning), default 1.0
- [x] Phase 2 — surface-relative pitch righting, rate default 0 (inert): `PitchRightingRate`
  servo (engine + tuning), contact-gated in the SurfboardUtils feed
- [x] Phase 3a — probe tuning on `bottom-turn` (2026-07-21, JSON A/B): **candidate config found —
  `PlaningRedirectOutwardScale 0` + `PitchRightingRate 8`, `AngularDampingY` KEPT at 0.4** (no need
  to sacrifice the weight-authority composure). See "Probe results" below. Defaults left inert.
- [ ] Phase 3b — regression pass on the working autopilots (`surfing-down-the-line`, `pop-up`,
  `surf-straight`, `hard_turn_towards_the_wave`, hang-ten, flat-water snapshots), 3× each, then bake
  the candidate into the header defaults
- [x] Phase 4 — crest-proximity gate (promoted to the main fix after the Phase-2 negative result):
  `PlaningRedirectCrestFade` + `CarveGripCrestFade` (cm, default 0 = off) multiply the redirect/grip
  gate feeds by `smoothstep(0, fade, −frontSC.signedDistanceToCrest)` — full strength on the face,
  0 at/behind the crest. Probe at 600/600 (2×, headless): **wholeMaxUW 0.80/0.83** (baseline pins
  0.95–1.0), step-4 climb +17/+18 cm (vs +62–76), maxVZ ~98 (vs 195–226), turn deeper (−58°/−57° vs
  −49°), final submersion holding ~0.7 instead of collapsing — no punch-through signature. Same
  down-line travel. **Awaiting player PIE validation** (the Phase-2 lesson: headless improvement
  must be confirmed in-editor before claiming victory).
- [x] **Player-validated in PIE ("much better", 2026-07-22)** → crest fades **600/600 baked into
  `SurfTuningSubsystem.h` defaults** (scratch UPROPERTYs synced).
- [x] Regression pass (2026-07-22, baked defaults, headless): **no regressions attributable to the
  change.**
  - `bottom-turn` maxUW 0.97→0.80, turns deeper ✓; `surf-straight` unchanged ✓; `hang-ten` travel
    428→722 ✓; `pop-up` rides 6.6s→36.6s, travel 827→1235 ✓; `surfing-down-the-line` pre-change run
    ended EJECTED (submersion 0.000 at gs 9.1 after a 1034 cm/s spike) — post stays on the wave 40 s
    (top speed lower, 619; the 1034 was part of the ejection dynamics).
  - `hard_turn_towards_the_wave` / `sharp_turn_right` appeared to lose their deep turns (−116°/−179°
    → ~+66-72°), 3× repeatable — but a **4-way knob isolation + both-fades-off CONTROL in the same
    session also gives ~72°**: the shallow turn is the pre-existing common bistable branch (matches
    the 2026-07-02 note "hard_turn already not executing, minYaw≈70"); the morning deep-turn runs
    were outlier branches. Not caused by this change.
  - `top-turn`: absent from the current saved map (autopilot filter matches nothing; CSV stale from
    07-20) — untestable.
  - `Tests/baselines/` remain STALE (predate current physics, per 2026-07-02 note) — the runner's
    auto-compare output vs them is noise; re-Approve when the current feel is settled.
- [ ] Optional follow-ups: crest-fade validity guard for flat water (dist reads 0 there → grip gated
  off; currently harmless since grip no-ops below 1 cm/s, but a paddling player on flat water loses
  grip assist); revisit the pre-existing hard-turn-doesn't-execute issue (separate bug).

## Probe results (2026-07-21, JSON A/B on `bottom-turn`, defaults verified inert first)

- **AC3 verified:** with the new code at defaults (`OutwardScale 1`, `RightingRate 0`) the run
  reproduces the punch-through baseline within run-to-run noise (maxUW 0.964, minYaw −50.9,
  final-row submersion collapsing = still exits through the crest).
- **`OutwardScale 0` + `RightingRate 3` + `AngularDampingY 0.135`:** ≈ the damping-only A/B
  (maxUW 0.888) — in this autopilot the ride used to end ~0.17 s after burial starts, too little
  time for a weak servo to matter.
- **`OutwardScale 0` + `RightingRate 8`, `AngularDampingY` kept 0.4 (candidate, 2× repeatable):**
  the failure mode is gone and replaced by recovery: the nose still buries transiently (UW ~1.0 at
  gs ~7.2 as the face steepens) but **pitches back out within ~0.4 s** (UW drops to ~0.16), the
  board rides over the steep section, comes back DOWN the face (vz −165 vs +208 escort-up in the
  baseline), and completes the deepest, longest rides of any config: minYaw −61°/−65° (vs −49°),
  down-line travel 1107/1148 (vs ~926), ride continues ~0.4 s past where every punch-through run
  ended. AC1 (no crest crossing) and AC2 (attitude follows — the recovery IS the attitude coming
  up) both met on this autopilot.
- ~~Interpretation: the righting servo is the load-bearing fix~~ **RETRACTED — see Correction below.**
- Run CSVs in the session scratchpad: `bottom-turn-newknobs-defaults.csv`,
  `bottom-turn-probe-oneside-righting3-dampY0.135.csv`, `bottom-turn-probe-righting8-dampY0.4{,-run2}.csv`.

## Correction (2026-07-21, after player test): the servo targets the WRONG reference — negative result

The player ran the candidate config in-editor (overrides confirmed loaded, new engine DLL confirmed
running) and saw **no difference** — the PIE-session CSV shows the full punch-through signature
(maxUW 0.99, z escorted +74, submersion collapsing at ride end). A follow-up headless run with
`p.Chaos.Solver.DebugDamping 1` captured the servo's actual inputs during the dive and explains it:

- **`PitchMisalignment` (waveRelativePitchSin) is TINY during the dive** — it wobbles between −0.19
  and +0.25 with sign flips while the nose is 100% buried. The board is NOT attitude-misaligned with
  the local face during the escort-up: **it rides up roughly PARALLEL to the face — that IS the
  punch-through geometry.** Near the crest, "aligned with the surface" is exactly the doomed state,
  so a surface-alignment servo has nothing to correct. wTarget peaks ~−2 rad/s for only a few frames.
- **The contact gate collapses right when it matters**: as the board exits through the lip,
  front/back `amountUnderWater` drop → alpha fell 0.067 → 0.010 in the last ~25 frames.
- **The earlier probe "improvement" was cross-session wave-phase luck**, not the servo: the debug
  re-run of the identical righting-8 config ALSO fully buried (maxUW 1.000, lastUW falling 0.37).
  Same-config runs within one session repeat tightly, but the ride shifts between sessions/PIE —
  the 2× "repeatability" was within-session only. The player's observation was correct.

**Where this leaves the design:** the servo machinery (Phase 2) works mechanically and stays (default
0, harmless, may yet help surface-tracking elsewhere), but *surface alignment* is the wrong target
for THIS bug. The failure is the **trajectory** being escorted up a face the board is parallel to.
The fixes that address the actual geometry:

1. **Phase 4 is not optional after all — it's the main event.** Stop the escort: fade the planing
   redirect (and possibly carve grip) by crest proximity (`signedDistanceToCrest` fed as a gate), so
   "up the face" stops being the target as the board nears the lip.
2. **Righting toward a different reference**: near the crest the nose must pitch AWAY from the face
   (deliberate misalignment) — e.g. drive pitch toward the *horizontal* (world) gated by crest
   proximity, or toward the *velocity* direction. A "don't exceed the face" cap, not "match the face".
3. The one-sided redirect (Phase 1) remains correct in principle (it can't hurt; it only stops
   suppressing escape) but is insufficient alone — measured effect small.

## Bug / motivation

`bottom-turn` autopilot: during the hard turn back against the wave (step 4, ~gameSeconds 7.3–7.8),
the nose buries (front SC `amountUnderWater` 0.60 → 0.97) and the board punches **through the crest to
the far side** nose-first at ~gs 7.6.

Measured in `Saved/Tests/latest/bottom-turn.csv` (2026-07-21, `AngularDampingY = 0.4` working tree):

- **Trajectory climbs, attitude doesn't.** At gs 7.60: `v = (+235, +177, +172)` → climb angle ~30°
  into/up the face, while CSV pitch sits frozen at ~−7.8° through the whole climb. Angle of attack
  ≈ −38°: the board is escorted up the face nose-first, belly to the sky. z gains 274 → 341 in 0.5 s.
- The planing redirect is doing exactly its designed job (bend velocity face-parallel, speed-preserving).
  What's missing is any coupling that makes the **attitude follow** the trajectory/surface.

### Root cause (three channels, all confirmed in code)

1. **No velocity→attitude coupling exists.** `RotateVelocityTowardDir`
   ([PBDRigidsEvolutionGBF.cpp:1016](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L1016))
   writes only `V`; nothing anywhere converts velocity direction into a pitch torque. The only
   attitude mechanisms are force/buoyancy/weight torques plus the surface-relative pitch **damping**
   (engine, `PitchMisalignment` feed) — and a damper can only resist rotation, never initiate it.
2. **The redirects eat the escape velocity (symmetric tangency-seeking).** Decompose
   `V = v_t·(along face) + v_n·(out of face)`. The planing redirect drives `v_n → 0` regardless of
   sign: penetration velocity (good — the anti-plow behavior it was built for) **and** the outward
   velocity that buoyancy adds each tick (bad — that was the board's only translational escape route).
   The carve grip does the same in the horizontal plane, steering momentum back toward the into-wave
   nose heading at 4 rad/s. Gates = nose `amountUnderWater` → **positive feedback**: burying the nose
   strengthens the very steering that buries it.
3. **Attitude channel damped/weakened exactly when needed.** The buoyancy nose-up **torque** is the
   channel the redirects can't touch (they never write `W`) — but it fights `AngularDampingY`
   (raised 0.135 → 0.4 in the same working-tree change as `WeightTorqueMagnitude` 5000, tuned for
   weight-authority composure, not this scenario), and at ~0.97 submersion the nose-vs-tail
   displacement differential is small.

### A/B result (`AngularDampingY` 0.135 vs 0.4, JSON override, no recompile, 2× each — 2026-07-21)

Highly repeatable (both configs reproduced within noise; this scenario is NOT bistable). Step-4
metrics from `bottom-turn.csv`:

| metric (step 4) | 0.4 run A | 0.4 run B | 0.135 run A | 0.135 run B |
|---|---|---|---|---|
| max front submersion | **0.97** | 0.95 | **0.88** | 0.88 |
| z gain during turn (cm) | **+67** | +62 | **+23** | +26 |
| max vx into wave (+X) | **253** | 251 | **162** | 153 |
| max vz (climb) | **208** | 195 | **100** | 106 |
| min yaw (turn depth) | **−49.4°** | −49.3° | **−73.7°** | −73.6° |
| final-row submersion | **0.59** ↓ | 0.65 ↓ | 0.88 → | 0.88 → |

- At 0.4 the final rows show submersion **collapsing** (0.97 → 0.59) with z still climbing — the
  signature of punching out through the back of the crest. At 0.135 submersion holds ~0.88: still
  deep, but the board stays on the face.
- At 0.135 the bottom turn carves **24° further** (−73.6 vs −49.4) and starts ~0.3 s earlier —
  freeing the pitch response changes the whole turn, not just the climb.
- **Verdict: the damping bump is a major contributor (channel 3 confirmed) but not the whole bug** —
  even at 0.135 the nose ends ~0.88 submerged, so the escape-eating redirect (channel 2) and the
  missing attitude coupling (channel 1) still warrant Phases 1–2.
- **Caveat:** 0.4 was player-tuned (PC, 2026-07-21) for composure with `WeightTorqueMagnitude` 5000 —
  a bare revert may regress that feel. Preferred end-state: base `AngularDampingY` low, with the
  righting spring (Phase 2) + the existing away-extra 0.8 carrying composure.
- Run CSVs preserved in the session scratchpad as `bottom-turn-dampY-{0.4,0.135}-run{1,2}.csv`.

## Proposed design

### 1. One-sided (inward-only) planing redirect

In the planing-redirect block
([PBDRigidsEvolutionGBF.cpp:1176](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L1176)),
skip (or scale down) the rotation when the board's velocity already points **out of** the face:

- **(as implemented)** The outward face normal is derived **in-engine** from the fed up-slope tangent:
  `n = normalize(worldUp − (worldUp·t̂)·t̂)` — the component of world-up perpendicular to the tangent.
  Exact for the planar face model and guaranteed ⊥ t̂ (a separately-sampled fed normal could be
  non-perpendicular, which would corrupt the decomposition); no new feed CVars needed.
- If `V·n > 0` (moving out of the face): redirect strength × `PlaningRedirectOutwardScale`
  (default **1.0 = current symmetric behavior**, bit-identical until tuned; target 0 = fully
  one-sided). Blended linearly over the first 30 cm/s of outward speed so the rate is continuous
  at `V·n = 0`.
- Carve grip is left symmetric for now — its symmetric horizontal grip is load-bearing for the
  anti-slip feel (AC1 of carve-grip-via-redirect.md). Revisit only if Phase 1+2 don't cure the
  punch-through.

### 2. Surface-relative pitch righting (attitude follows the surface)

The angular analog of the redirect: a **spring toward surface alignment** where today there is only a
damper. The plumbing already exists — the project feeds `PitchMisalignment`
(= avg `waveRelativePitchSin`, [SurfboardUtils.cpp:229-241](../Source/GoneSurfing/SurfboardUtils.cpp#L229))
and a contact gate to the engine every tick.

- **(as implemented)** Engine, in the custom angular damping block after the damping multiplies
  (so the damper doesn't immediately eat the nudge): a **first-order servo**, not an accel spring —
  single knob, no separate settle-time or overshoot clamp needed:

  `wTarget = −PitchRightingRate × PitchMisalignment` ;
  `Wy = lerp(Wy, wTarget, min(1, PitchRightingRate × Dt))`

  The blend weight can't overshoot the target within a tick, and `wTarget → 0` as the misalignment
  closes, so alignment is asymptotic (misalignment decays with time-constant ~1/rate). The damper
  then acts as the servo's rate limiter (stable second-order response).
- New CVar `p.Chaos.Solver.PitchRightingRate` (1/s), **default 0 = inert**, wired through
  `SurfTuningSubsystem` (`PitchRightingRate`) + `RefreshFromTuningSubsystem` like every other knob.
  The project feeds it **pre-multiplied by the contact gate** (same smoothstep as the damping
  extras), since `PitchMisalignment` itself is fed un-gated.
- Sign convention: verify against the existing `pitchP = Wy × PitchMisalignment` away/toward test —
  righting must push `Wy` in the direction that shrinks `|PitchMisalignment|`. The mesh-90° gotcha
  does not bite here: the engine already works in the same local frame as the pitch damping, which is
  proven correct.
- Interaction with the pitch damping: the damper then acts as the righting spring's rate limiter
  (spring + damper = stable second-order alignment). The current away-extra 0.8 stays; if the A/B
  shows base 0.4 is what froze the nose, drop base `AngularDampingY` back toward 0.135 and let the
  spring + away-extra carry composure instead.

### 3. Explicit non-goals (this spec)

- No change to carve grip rate/gate or wave-carry (working, player-verified).
- No crest-proximity gate yet (Phase 4 only if 1+2 measure insufficient) — same for a burial-cap
  reshape of the redirect gates.
- No re-tune of `WeightTorqueMagnitude` (5000 stays; it's the damping side that gets revisited).

## Acceptance criteria

- **AC1 (the bug):** `bottom-turn` no longer crosses the crest nose-first at step 4 — max front-SC
  submersion during the turn < 0.9, and the board stays on the shore side of the crest
  (x stays below the crest line; compare `AnalyzeCrossing.ps1`).
- **AC2 (attitude follows):** during the step-4 climb, CSV pitch rises toward the face slope instead
  of freezing (pitch tracks within ~10° of the climb angle by the end of the climb, vs the current
  38° gap).
- **AC3 (no regressions at defaults):** `PlaningRedirectOutwardScale = 1`, `PitchRightingRate = 0`
  reproduce current behavior bit-for-bit. Snapshot tests unchanged at defaults.
- **AC4 (working rides survive the tuned config):** `surfing-down-the-line`, `pop-up`, `surf-straight`,
  `hard_turn_towards_the_wave` show no loss of ride quality (down-line travel, planing time, minYaw)
  vs current. 3× repeats each (bistable autopilots).
- **AC5 (pumping/takeoff intact):** the steep-face takeoff (commit 756a3f53a behavior) still works —
  the one-sided redirect must not weaken the wave's carry during takeoff (takeoff has `V` pointing
  *into* the face, so the inward redirect path is unchanged; verify anyway).

## Test plan

1. **A/B baseline (this session):** `AngularDampingY` 0.135 vs 0.4 on `bottom-turn` via
   `TuningOverrides.json` — quantifies channel 3 before any new code. Record pitch trace + max
   submersion + crest crossing into this spec.
2. Phase 1 alone (`OutwardScale` 0, righting 0): does letting buoyancy escape already stop the
   punch-through? Measure AC1.
3. Phase 2 alone (righting tuned, redirect symmetric): does attitude-follow alone fix it? Measure AC2.
4. Tuned combo + AC4 regression sweep, 3× each.
5. Restore/empty `TuningOverrides.json` after every A/B; bake winners into `SurfTuningSubsystem.h`.

## Related

- [planing-redirect.md](planing-redirect.md) — the mechanism being made one-sided; shares machinery.
- [carve-grip-via-redirect.md](carve-grip-via-redirect.md) — the horizontal sibling (left symmetric).
- [wave-carry-redirect.md](wave-carry-redirect.md) — the opposing shoreward redirect (rate 1 vs grip 4).
- [surface-relative-pitch-damping.md](surface-relative-pitch-damping.md) — the damper the righting
  spring composes with; same feed/plumbing.
- [[submersion-gates-comparison]] — why the gates use `amountUnderWater`.
- [[downline-yaw-into-wave-is-overcarve]] — prior art on keel/decouple fixes and the A/B workflow.
