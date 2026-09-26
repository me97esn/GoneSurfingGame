# Spec: Board spray particles (force-driven, water-surface-aware)

## Status
- [x] Spec drafted (2026-07-27)
- [x] Phase 1 C++ implemented (2026-07-27): `AFluidDynamics::appliedForceThisTick` accumulator
  (+ `appliedSlopeGravityThisTick` exclusion stash) and `ASprayController`
  (`Source/GoneSurfing/SprayController.h/.cpp`). Compiles clean.
- [ ] Editor-side authoring: `NS_BoardSpray` Niagara asset + level placement (see
  "Editor-side setup" below — cannot be done from C++)
- [x] Tier 2 C++ implemented (2026-07-28): Tier 1's board-sampled plane produced all three
  predicted artifacts in playtesting (spawn height error on curved faces / fast turns, foam
  bobbing with the board's wave phase, stranded floating foam after the wave moved on) → per-tick
  16×16 height grid shipped to Niagara via Array DI + per-endpoint spawn lift +
  `sprayWaterlineZBias` (player-calibrated -40; INTENTIONAL offset — the wave display meshes are
  deliberately lowered ~40cm below the height data so they align with the submerged-riding board;
  anything placed at the visible waterline must subtract the same offset).
- [ ] Niagara-side Tier 2 migration: grid user params + bilinear settle module (HLSL in chat/spec)
- [ ] On-device Android perf sanity check (frame time before/after, spray at max emission)

## Motivation

The board currently cuts through water with no visual response. Real surfing has three dominant
spray sources, and they carry most of the "speed" read in surf footage:

1. **Rail spray** — the fan of water thrown off the wave-side rail during a carve (the big one).
2. **Tail/wake spray** — modest, continuous while planing.
3. Nose paddle splash — explicitly OUT of scope for phase 1.

The physics already computes everything needed to drive this honestly: each `AFluidDynamics`
actor knows its wetted fraction, relative water velocity, and the force it applied this tick.
Spray is the **reaction** to that force (Newton's third law), so particle ejection direction ≈
−(applied force), magnitude ∝ force — which scales with v² for free.

## Constraints (non-functional)

- **Zero physics impact.** Purely cosmetic: no new forces, no change to any force pipeline.
  Snapshot tests must show zero drift with the feature on or off.
- **Android particle budget: a few hundred particles max** (200–400), sprite material
  (reuse a white-water texture). GPU sim, following the white-water precedent (already GPU sim
  on Android) — the Android constraint is particle COUNT, not sim target. This is separate from
  the white-water system's
  `MaxTotalParticles = 3000` cap in `AParticleSystemsController` — that cap only governs its own
  data channels — but the GPU/CPU load adds up, so the spray system enforces its own hard cap
  via fixed max spawn rate × max lifetime.
- Don't touch the white-water data-channel pipeline. Board spray is a plain `UNiagaraComponent`
  with user parameters — the data-channel machinery is overkill for a single board-local system.

## Design

### 1. Per-tick force accumulator on `AFluidDynamics`

Each FluidDynamics actor accumulates the total force it applied this tick (sum over all
`applyForceAsImpulse` / wave-mass / rail-lift call sites, in force units — divide impulses back
by DeltaTime or accumulate pre-impulse force, whichever is cleaner at the call sites):

- `FVector appliedForceThisTick` (`BlueprintReadOnly`, `Transient`) — zeroed at the top of Tick.

**Exclude** from the accumulator the contributions that don't correspond to visible spray:
- `waveSlopeGravityCoefficient` supplement (synthetic gravity, not water deflection)
- the wave-mass family (flow drag, penetration, lip impact — everything through
  `applyWaveMassForceAsImpulse`): bulk water CARRYING the board, momentum exchange with
  slow-moving mass, not a sheet thrown off a rail. Added 2026-07-28 after playtesting: the wave
  catch's ~35k flow push + 25k slope thrust out-sprayed a hard carve's ~20-30k rail forces —
  backwards. Excluding them leaves the v² contact forces (drag, lift, thrust, rail lift), so a
  fast carve splashes hardest and a slow takeoff barely at all. (Trade-off: the lip slamming the
  nose no longer sprays; wire it as its own splash source if missed.)
- `bottomSlopeThrust` and `dragBottom_waveMassThrust` (added 2026-07-28, from a measured torque
  budget on the `surfing_down_the_line_then_sharp_turn_right` run): slope thrust is slope-geometry
  propulsion (waveSlopeGravity's sibling) and measured ~30k planar riding down the line — the
  dominant spray driver, out-spraying the actual carve; waveMassThrust is wave-carry momentum that
  rides the plain apply path unlike its pending-path siblings, ~18k planar mid-turn. After these,
  the counted drivers are fore-aft drag (wake-like) down the line vs drag + hydrofoilYaw +
  railLift + lateralTurn + fin (the sideways fan) in a carve.
- buoyancy is a separate actor class anyway — not involved

**Naming gotcha reminder**: "lift" here is Bernoulli suction (downward/inward). It is real water
interaction and MAY be included, but the dominant honest spray drivers are drag + thrust on
wetted rail/tail actors. Phase 1: accumulate drag + thrust + rail-lift; evaluate visually.

### 2. Spray controller (new small actor `ASprayController`, or a member of `ASurfboardUtils`)

Per tick, for each of 3 emitter sites (left rail, right rail, tail):

1. Sum `appliedForceThisTick` over the 2–4 FluidDynamics actors assigned to that site
   (assignment by actor reference array, editor-configured — do NOT hardcode board axes;
   `board.forwards` is local +Y).
2. Compute per-site Niagara user parameters:
   - Emission line `SprayPosA/B<site>` = the site's two furthest-apart actor positions, LIFTED up
     onto the waterline + `spraySurfaceOffset` clearance (the board planes 30-50cm submerged, so
     raw actor positions are underwater — spray leaves at the waterline; the clearance stops the
     settle module claiming particles on their spawn frame). Emitters scatter along lerp(A, B, rand).
   - `SprayVel<site>`: the Newton-3 reaction (−summed force) deflected along the water surface,
     with BOTH speed and direction taken from the SURFACE-PARALLEL component only:
     speed = min(|planar reaction| × `sprayForceToVelocity`, `sprayMaxEjectSpeed`),
     direction = normalize(planar direction + `sprayUpwardTilt` × plane normal). Never renormalize
     the deflected direction to the full-force speed — at takeoff the net force is ≈ pure upthrust
     (reaction ≈ straight down, planar ≈ 0) and the full speed escaped through the tilt as a
     vertical geyser (observed). A vertical push's momentum goes into the deep, not the sheet.
   - `SpawnRate<site>` = `sprayBaseRate` × saturate(|planar reaction| / `sprayForceForFullRate`)
     × site's mean `actorWetted` — no spray from a dry rail. Planar-keyed like the velocity, NOT
     total force: the wave-catch force spike is ≈ pure upthrust, and total-force keying pegged the
     rate the instant the speed gate cracked open (particle burst at low speed, observed).
3. **Global gate**: smoothstep on HORIZONTAL board speed over `sprayMinSpeed`..`sprayFullGateSpeed`.
   Deliberately NOT `AmountPlaning` (first implementation used it): planing is a thresholded
   switch with hysteresis (0 → 0.8 within a few ticks of takeoff), which popped spray in at
   full blast. And deliberately not 3D speed (second implementation): vertical wave-carry on a
   rolling swell is 100-300 cm/s, which opened the gate for a board that wasn't travelling.
4. Water-surface parameters (Tier 1): `WaterPlanePos`, `WaterPlaneNormal` — the surface position
   and normal at the board, read from data the bottom actors already compute per tick (or one
   `waveHeightAndNormal` query at the board center — cost is one bilinear lookup, negligible).
   Optionally `WaterVelocity` (already computed per-actor) so settled foam drifts with the wave.

### 3. Niagara system (one system, 3 emitters — one per site)

- **Attached to the surfboard mesh** so emitter origins track the rails/tail, but
  **Local Space = OFF** (world-space simulation) — spawned droplets must be left behind at
  speed, not ride along with the board.
- GPU sim (white-water precedent). Sprite renderer, white-water texture, ~0.6–1.2 s lifetime,
  gravity on.
- Spawn velocity = `SprayDir × SprayStrength` + cone jitter.

### 4. Surface interaction — settle-to-foam (the point of this spec)

Particles must not fall through the water surface. Tiered by cost:

**Tier 1 (phase 1) — local plane test.** Scratch module per particle:
`d = dot(ParticlePos − WaterPlanePos, WaterPlaneNormal)`. When `d < 0` the particle **settles**:

- snap onto the plane (`ParticlePos −= d × WaterPlaneNormal`)
- damp the particle's FULL 3D velocity hard toward `WaterVelocity` — including vertical: a
  breaking face has real upward water velocity, and settled foam must ride the rise, not sit
  at a fixed height. The plane-snap is the positional constraint that prevents re-sinking;
  the velocity damp prevents re-launching. (Tier 1 caveat: `WaterVelocity` is sampled at the
  board, so foam a couple of meters behind gets board-local water velocity — acceptable;
  Tier 2's grid can supply per-particle velocity via the existing `getVelocityBilinear`.)
- switch to foam look (larger sprite, lower opacity) and fade out over ~1 s

The settle behavior doubles as a **foam trail** behind the board — likely a bigger realism win
than the ballistic spray itself. Justification for the plane approximation: spray lives ~1 s and
lands within a couple of meters of the board, where the wave face is locally near-planar; the
plane tilts with the local normal so it tracks the face during carves.

**Tier 2 (phase 2, only if needed) — height grid.** Same pattern `AWaveParticleSystemActor`
already uses: CPU samples an ~8×8 grid around the board per tick via the existing
`GetWaveDataAroundLocation` (bilinear lookups into precomputed frame data — the codebase already
does 100/tick for the wave mesh, proven cheap), passed as a Niagara position array; the scratch
module bilinearly interpolates height under each particle instead of the plane test. The
authoring cost is the world↔grid coordinate mapping (see the `(200, −200, 200)` scale the
white-water controller needs — coordinate-space alignment is the known pitfall). **Structure the
Tier 1 module so "surface height under particle" is the single swappable evaluation.**

Tier 3 (custom Niagara Data Interface calling `waveHeightAndNormal` per particle) is explicitly
rejected: more C++ surface area for accuracy Tier 2 already provides.

## Knobs (editor-tunable on the spray controller)

| knob | default (starting point) | meaning |
|---|---|---|
| `sprayForceToVelocity` | tune | applied force (N-ish) → ejection speed (cm/s) |
| `sprayMaxEjectSpeed` | ~800 cm/s | absolute cap on ejection speed (raise to ~1200-1500 to let the sideways cap show — it binds first otherwise) |
| `sprayMaxEjectVsBoardSpeed` | 1.0 | FORE-AFT component cap: wake ≤ this × horizontal board speed (sheared off along the track, can't exceed the flow) — kills the takeoff backward-streak class at the source |
| `sprayMaxEjectVsBoardSpeedSideways` | 1.75 | CROSS-BOARD (+ tilt) component cap: the carve fan is a REDIRECTED JET, which in world frame reaches up to ~2× the flow speed (elastic-deflection limit). Separate from fore-aft because hard carves BLEED board speed — a whole-vector cap clamped the fan exactly at the biggest-fan moment (observed at the 8s turn) |
| `sprayBaseRate` | ~150 /s/site | spawn rate at full force, before gates |
| `sprayForceForFullRate` | tune | force at which spawn rate saturates |
| `sprayLateralWeight` | 2.0 | multiplier on the cross-board component of the planar reaction (speed + rate); 1 = neutral — carves out-spray the fore-aft wake by this factor |
| `sprayMinSpeed` | 100 cm/s | HORIZONTAL board speed where the spray gate starts opening (below: silence) |
| `sprayFullGateSpeed` | 400 cm/s | horizontal board speed where the gate is fully open |
| `spraySurfaceOffset` | 3 cm | emission-endpoint clearance above the waterline (keep > 0 or spawns settle instantly) |
| `sprayUpwardTilt` | 0.35 | ejection tilt out of the water-tangent plane (0 = skim flat) |
| `sprayWaterlineZBias` | −40 cm | data→render waterline offset (display meshes are deliberately lowered to align with the submerged-riding board); applied to every waterline (lifts, plane, grid) — apply in ONE place only, never also in Niagara |
| `spraySpawnDepth` | 15 cm | emission line sits this far BELOW the biased waterline (spray born at the hull bottom; foam still floats at the bias level). Needs the settle module's grace age (`NormAge >= SettleGraceAge`, ~0.08) or spawns are instantly claimed |
| `sprayOutboardOffset` | 20 cm | pushes the emission line away from the centerline (rails: ±board.left, sign auto-resolved; tail: behind) — bottom actors sit inboard of the rail edge |
| `heightGridSize` | 16 | Tier 2 grid is N×N samples (0/1 = grid off) |
| `heightGridSpacing` | 200 cm | Tier 2 grid cell size (~30m span at defaults — covers the foam trail at speed) |
| `sprayMaxParticles` | 400 | hard budget across all sites (spawn rate × lifetime bound) |
| `sprayAssumedLifetime` | 2.0 s | lifetime assumed by the budget clamp — keep ≥ the Niagara asset's max (ballistic + foam fade) |
| `bSprayEnabled` | true | master switch (foam fade duration itself lives in the Niagara asset) |

Debug: `surf.debug.flags 'spray'` — per-site force sum, direction, spawn rate, settle counts.

## Editor-side setup (required before any spray is visible)

The C++ side pushes user parameters onto one Niagara component; the system asset and level wiring
are editor work:

1. **Create the Niagara System** (e.g. `Content/Particles/NS_BoardSpray`), 3 emitters
   (SprayLeft, SprayRight, SprayTail), all **GPU sim**, **Local Space OFF** (world simulation).
2. **Add User Parameters** (exact names — `ASprayController` sets them by FName each tick):
   - Vector: `SprayPosALeft`, `SprayPosBLeft`, `SprayVelLeft`, `SprayPosARight`, `SprayPosBRight`,
     `SprayVelRight`, `SprayPosATail`, `SprayPosBTail`, `SprayVelTail`, `WaterPlanePos`,
     `WaterPlaneNormal`, `WaterVelocity`
   - Float: `SpawnRateLeft`, `SpawnRateRight`, `SpawnRateTail`
3. **Per emitter** (using its own site's three params):
   - Spawn Rate module: rate = `User.SpawnRate<Site>` (already fully gated/budgeted in C++ — use it raw)
   - Initialize Particle: position = `lerp(User.SprayPosA<Site>, User.SprayPosB<Site>, random 0..1)`
     (world) so spawns scatter along the rail; lifetime ~0.6–1.2 s random, small sprite (reuse a
     `white-water-texture` material)
   - Add Velocity: `User.SprayVel<Site>` plus a random cone spread (~15–25°) and ~±20% speed jitter
   - Gravity Force: on (standard −Z)
   - **Settle scratch module** (Particle Update, the Tier 1 surface logic):
     ```
     d = dot(Particles.Position − User.WaterPlanePos, User.WaterPlaneNormal)
     if Particles.Settled < 0.5 and d < 0:
         Particles.Position −= d × User.WaterPlaneNormal   // ONE-TIME snap at first contact
         Particles.Settled = 1
     if Particles.Settled > 0.5:
         Particles.Velocity = lerp(Particles.Velocity, User.WaterVelocity,
                                   saturate(SettleDamping × DeltaTime))   // full 3D, incl. the rise
         // Settled drives sprite scale-up + opacity-down + ~1 s fade kill
     ```
     `SettleDamping` ~10–20 (module input float). Snap-once, NOT per-frame: the plane is sampled at
     the board and moves with the board's wave phase, so re-snapping every frame makes settled foam
     metres away bob with the board's sample (observed). After the one-time snap, gravity vs. the
     velocity damp lets foam sag naturally; residual error hides under the translucent surface.
     Keep "surface height under particle" isolated in this one module — Tier 2 swaps only this
     evaluation for a grid lookup.
   - Emitter Properties: set a fixed max particle count per emitter (e.g. 150) as belt-and-braces
     under the C++ budget.
4. **Level wiring**: place one `ASprayController` per board, set `Surfboard` (the board actor the
   FluidDynamics actors point at) and `SpraySystem` = the new asset. Site membership auto-resolves
   from ESide + actor labels on first tick; the `LeftRailActors`/`RightRailActors`/`TailActors`
   arrays override it if filled. `sharedCalculations` auto-resolves too.

## Replay

Spray outputs (per-site ejection velocity + rate) are recorded into the input trace (columns
16-27) and played back verbatim during on-device kinematic replay via
`ASprayController::SetReplaySpray` — see specs/on-device-ride-replay.md ("Spray in replays") for
the full design and the frozen-accumulator bugs it fixes.

## Acceptance criteria

- **Given** the board carving hard on a wave face, **when** rail force spikes, **then** a spray
  fan ejects from the loaded rail, opposite the applied force, and visibly separates from the
  board (world-space).
- **Given** spray particles descending, **when** they reach the water surface, **then** they
  settle into drifting foam and fade — no particle visibly sinks below the surface near the board.
- **Given** the board at rest / paddling slowly (below `sprayMinSpeed`), **then** no spray; spray
  fades in smoothly over the takeoff acceleration instead of popping at the planing threshold.
- **Given** a snapshot-test run (`RunGameAndCollectLogs.bat`), **then** all trajectory compares
  are byte-identical to a run with `bSprayEnabled=false` (zero physics impact).
- **Given** the Android build at max emission, **then** live particle count ≤ `sprayMaxParticles`
  and no measurable frame-time regression vs. white-water-only baseline.

## Test cases

1. Headless: run an existing autopilot snapshot test with spray on → Compare.ps1 exit 0, zero drift.
2. Visual (PIE + device): hard carve on `Boards_on_flat_water` / wave level — rail fan on the
   loaded side only; wake from tail while planing straight; nothing while stationary.
3. Settle: slow-mo / high spray gravity — particles visibly stop at the face, drift shoreward
   with `WaterVelocity`, fade.
4. Budget: log live particle count at max emission on device.
