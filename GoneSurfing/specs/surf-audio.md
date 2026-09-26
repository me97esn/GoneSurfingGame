# Spec: Procedural surf audio

## Status

- [x] Design drafted
- [x] **Breaking-wave emitter placement validated** (foam-cluster debug tool, player-confirmed 2026-08-06)
- [ ] Layer 2 (breaking-wave crash) audio controller
- [ ] Layer 1 (reactive board-water) audio controller
- [ ] MetaSound assets authored (in-editor, by hand)
- [ ] Mobile voice-budget / attenuation tuning
- [ ] On-device (Android) verification

## Overview

The game currently ships with **zero audio** (no audio module deps, no project-authored sound assets — only unused Epic StarterContent). This spec covers generating surf sound procedurally / data-drivenly on **mobile**, from data the simulation already computes, rather than hand-placing sound cues.

Two independent layers, mixed together:

1. **Reactive board–water layer** — the hiss/rush/spray of the board interacting with the water. Must be synthesized/modulated **live** from per-tick scalars.
2. **Deterministic breaking-wave layer** — the roar/crash of the wave itself. The wave is a baked, frame-indexed loop, so this layer is **scheduled/placed**, not reacted to.

## Objective

Give the game a believable, mobile-cheap surf soundscape that tracks what the player sees and does, driven from existing simulation data, with a bounded voice count.

## Background: why this is a good fit

Surf sound is physically mostly **filtered broadband noise** (hiss, rush, foam) — the one class of sound that synthesizes cheaply and convincingly on mobile (noise generator + biquad filters). And the simulation already computes clean, smoothed, per-tick drive scalars, which is normally the hard part.

MetaSounds is the vehicle for both layers (node-graph DSP, float input params, mobile-friendly). C++ reads scalars on the game thread and pushes them as MetaSound params each tick — the **exact pattern `ASprayController` already uses to drive `NS_BoardSpray`** via `SetVariableFloat`. We mirror that against a `UAudioComponent` instead of a `UNiagaraComponent`.

---

## Layer 1 — Reactive board–water audio

Synthesize the hiss/rush layer (noise → bandpass, cutoff+gain from speed/spray) and optionally sample-loop a low body tone. Drive signals (all per-tick, mostly on `ASharedCalculations`; units = UE cm/s):

| Sound layer | Drive signal | Source | Notes |
|---|---|---|---|
| Rush / whoosh (main speed) | `relativeWaterVelocityMagnitude` | `ASharedCalculations` | Already smoothed; "how hard water rushes past the hull" |
| Spray hiss / foam | sum of `ASprayController::GetSprayOutputs()` rates | `ASprayController` | Already gated by planing+wetting+speed+budget; locked to visible spray |
| Rail bite / carve | `AFluidDynamics::actorWetted` (per rail) | `AFluidDynamics` | Sharp, per-contact, L/R asymmetric → stereo carve |
| Dunk / submersion (low-pass) | `amountUnderWater` (0..1, smoothed) | `ASharedCalculations`/`ABuoyancy` | Bury board → muffle everything |
| "On the face" ambience | `boardWideSlopeSin` | `ASharedCalculations` | Steep face → more wave power |
| Discrete "now planing" state | `AmountPlaning` | `ASharedCalculations` | Snappy by design — a trigger/state, not a crossfade |

**FR1.** A single `UAudioComponent` (MetaSound) attached to the surfboard, its params set each tick from the normalized scalars above.
**FR2.** Normalize each cm/s scalar to 0..1 against tuned thresholds before feeding filters; **reuse the spray gates** (`sprayMinSpeed`/`sprayFullGateSpeed`) so audio and VFX agree.

**First proof (do this first):** one MetaSound = noise→bandpass, one float input `RushIntensity` from normalized `relativeWaterVelocityMagnitude`, attached to the board. Answer "does speeding up sound like speeding up?" before layering more.

---

## Layer 2 — Deterministic breaking-wave crash

The wave surface is a **baked 193-frame loop** (`WaterController.CurrentFrame` wraps 886..1078). Each `AGridLODActor` plays that same loop at a fixed per-tile `FrameOffset`. So the break is deterministic: phase off `CurrentFrame`, **never wall-clock** (loop advances 1 frame/tick → its period in seconds is frame-rate dependent).

There are **3 GridLODActors** (repositioned, never spawned) → 3 crash tiles. Adjacent tiles are phase-offset (default `FrameOffsetPerActor = 80` of 193), so their crashes **stagger down the line for free** — recompute the effective offsets for 3 tiles during tuning.

### Emitter placement — VALIDATED

Per tile, per frame, cluster the whitewater foam point cloud (`AParticleSystemsController` per-channel `WhiteWaterData->Positions`, transformed to world) and emit **one pair per distinct break**:

- **Roar emitter** = cluster **centroid** (green in the debug tool) — sustained whitewater-body loop.
- **Crash accent** = cluster **down-line (+Y) front edge** (red) — the actively-breaking lip; louder = more foam.

Clustering method (validated, see debug tool below):
1. Rasterise a tile's foam into fixed 2-D cells (`BreakClusterCellSize` ≈ 400 cm) in the horizontal plane.
2. Keep only cells whose count ≥ `BreakClusterDensityFrac` (0.25) × peak cell — this **drops the sparse foam trail** that otherwise merges two breaks.
3. Flood-fill (8-connected) the surviving dense **core** cells into components; each component ≥ `MinClusterPoints` is one break.

A tile **often has 2 break areas** (the wave peels along the tile width over time) and **gaps between waves with none**. Below a foam-count threshold (`BreakPointMinFoamCount`) → no emitter (silent between waves).

### Dead ends (do not retry)
- Total foam count as a temporal envelope — flat; something's always breaking somewhere.
- Raw min/max-Y extreme points — peg to tile edges (measure spread, not density).
- 1-D histogram-along-Y clustering — merges two blobs joined by a sparse trail; centroid floats in open water when foam is offset in X. **This is why clustering must be 2-D + density-core.**

**FR3.** One roar `UAudioComponent` per active break cluster, positioned at the cluster centroid, volume from cluster foam count; crash accent driven from the front position.
**FR4.** Emitters follow their `AGridLODActor` as it repositions (correct phase + world position for free).
**NFR (voice budget):** cap crash clusters to the **nearest ~2–3 tiles**; let UE attenuation + concurrency silence the rest. Aggregate — never one voice per foam particle.

---

## The validated debug tool (already implemented)

Lives in `AParticleSystemsController::UpdateSingleDataChannel`; draw-only, off by default (cannot affect play or snapshot tests). Toggles on the ParticleSystemsController actor:

- `bShowBreakPointDebug` — master toggle; draws foam dots (grey), cluster centroids (green), fronts (red), and an on-screen `breaks=N` per-tile readout.
- `BreakClusterDensityFrac` (0.25) — split sensitivity; raise to separate merged breaks, lower to merge fragments.
- `BreakClusterCellSize` (400 cm) — resolution / smallest resolvable gap.
- `BreakPointMinFoamCount` (30) — "any break at all" gate.
- `BreakPointDebugDrawDuration` (0.5 s) — draw lifetime; positive so draws persist across a pause for inspection.

The audio controller reuses this exact clustering, swapping `DrawDebugSphere` for `UAudioComponent` placement/params.

## Acceptance criteria

- **AC1 (rush):** Given the board accelerates from rest to planing, When speed rises, Then the rush layer's pitch/brightness/level rises monotonically and audibly.
- **AC2 (spray):** Given spray particles spawn, When their rate rises, Then spray hiss rises in lockstep; no spray → no hiss.
- **AC3 (crash placement):** Given a breaking tile, When it has two whitewater areas, Then two roar emitters sound, each localized on its own area (not the gap); given a gap between waves, no crash from that tile.
- **AC4 (stagger):** Given the board rides down the line, Then adjacent tiles' crashes fire staggered, not in unison.
- **AC5 (mobile budget):** Concurrent voices stay within budget (≈ ≤10) regardless of how many breaks exist; distant breaks are attenuated/culled.
- **AC6 (tests intact):** Snapshot/trace-replay CSVs are unchanged with audio enabled (see gotcha).

## Implementation plan

1. Add audio module deps (`AudioMixer`, `MetaSoundEngine`) to `GoneSurfing.Build.cs`; enable the MetaSound plugin.
2. Layer 1 proof: `RushIntensity` MetaSound on the board (AC1).
3. Layer 1 full: spray hiss + submersion low-pass + rail carve.
4. Layer 2: `ASurfWaveAudioController` reusing the validated clustering to place roar+crash emitters (AC3–AC5).
5. Author MetaSound graphs in-editor (noise/bandpass, crash sample loop).
6. Android on-device pass.

## Gotchas

- **Gate audio behind the test filter.** Test runs enable player controls after autopilot handoff; anything controls- or state-gated must key on the `surf.autopilots` CVar (+ `bExternalWeightOverride`) or it can corrupt snapshot/trace-replay CSVs. Audio is output-only so lower risk, but any *gameplay* side effect (e.g. state changes) must respect this.
- **`GetActorLabel()` / `GetWhiteWaterWorldPoints()` availability** — fine in editor `-game`, but confirm nothing audio-side is `WITH_EDITOR`-only if a Shipping Android build is ever targeted.
- **Phase off `CurrentFrame`, not seconds** — frame-rate-dependent loop period.
- **3 tiles, not 5** — recompute effective per-tile phase offsets when tuning the stagger.

## Open questions

- Crash: pure sample loop, or synthesized noise crescendo? (Sample loop likely; deterministic placement means we can author it.)
- Roar vs crash as separate voices per cluster, or one voice whose brightness rides the front? (Start: one roar voice per cluster; add crash accent if needed.)
- Line-source approximation when the board is *beside* a long break (single point sounds too pinpoint) — multiple emitters along the break, or widen attenuation spread.
