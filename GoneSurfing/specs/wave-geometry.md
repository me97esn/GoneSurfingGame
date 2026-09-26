# Spec: Wave geometry — one place that knows where the wave is

## Status
- [x] Owner's direction (2026-09-18, after T0 of [broken-wave-no-consequences.md](broken-wave-no-consequences.md)
      passed on the pictures): *"a structured way to get the positions of these different positions/areas
      of the wave, because it will likely be used by both the scoring, the hydrofoil, the wave crash and
      possibly even the sounds."*
- [x] **Built 2026-09-18** — `UWaveGeometrySubsystem` (world subsystem, pure query): wave-frame axes, the
      peel model from `Content/WaveGeometry/<map>.json`, `Sample(worldPos, frame)` → distance behind the
      impact point, cross-shore offset, seconds since broken, `Broken` 0..1, `Zone`. The `wavegeo` debug
      draw reads the model from it, so the picture and the physics cannot disagree.
- [x] `Tools/WaveGeoFit.py` — fits the two points from a `surf.debug.wavegeo` log and prints the JSON.
- [x] **AC1 met** — `Tools/WaveGeoFit.py` on two runs: 25.95 / 25.96 cm/frame, residual sd 103 / 104 cm,
      p5 −164 / p95 +183, cross-shore sd 56 cm; the two logs' points agree to 5 cm.
- [x] **AC2 read on replays 2026-09-18** (`geo=` on the `CROSSING` line): 14-38-37 pocket → whitewater
      through the sit and the run-up (Broken 1.0) → `flat` only when the next impact arrives; 20-57-25
      pocket → whitewater at the sit → flat as the bore arrives → whitewater; 10-48-07 pocket/shoulder
      until it stalls. 10-15-54 stalls at ~0.7 s in this harness — the baked yaw-cap/un-fade headless
      takeoff artefact (lip-snap M8), not the service; a stopped board ahead of the peel reads `shoulder`.
- [x] **Zone map built 2026-09-20** (`surf.debug.wavegeo.zones 250`) — checked from the shore-side camera over
      six tiles; the young-bore rule (step 3 in FR4) came out of it.
- [x] **Owner's PIE check of the zone map over multiple tiles — VERIFIED 2026-09-20:** "I have verified the
      different colors, they are all correct." Two debug fixes came out of the session (draws live 0.1 s so
      they survive a pause; the cyan band follows the model impact, not the foam front, which hopped on the
      clustering's 400 cm cells). **AC4 met. The service is cleared for consumers.**
- [x] **First consumer 2026-09-21:** the broken-wave spec's A1–A4 read `Sample().Broken` through
      `ASharedCalculations::brokenGeo` (its M16). One rule change came out of it: **`Sample()` tests the
      previous wave's bore band before returning `Flat` in rules 1–2.** On a replay the frame the next
      impact passed the board read `flat → whitewater(behind=5006) → flat`: `kb` changes identity at the
      impact and the previous bore was no longer tested — a one-frame flicker, and after it the aged bore
      was forgotten while the water there still ran −100…−300 (broken-wave M15 row 3, ages 6–9 s). The
      map gains white 10–20 m shoreward of an aged break. **Owner's PIE look at that region owed (AC4
      re-check).**
- [x] **`Broken` fades at the band's shoreward edge (2026-09-21, broken-wave M19):** `boreFrontFade` 400 in
      the JSON; the band extends that far past the front and `Broken` × `SmoothStep(front − fade, front, c)`.
      A board riding the bore front outruns the modelled front (230–290 vs 150 cm/s) and a hard edge
      switched every consumer off in one tick. Zone map: white fades out 4 m further shoreward — owner's
      PIE look owed with the previous-wave rule.
- [x] **The young bore's front spreads from the impact (2026-09-21, broken-wave M20):** `boreFrontSpeed` 300
      in the JSON; the front is `min(boreFrontSpeed·age, boreFrontStart + boreDriftRate·age)`. The 800 + 150·age
      line was fitted to bores 1.2–7.9 s old and made the band 8 m wide the moment the lip landed; a board
      4 m shoreward of a fresh landing, ahead of the foam, read whitewater. Zone map: the white band is
      narrow right behind the peel and widens at 3 m/s. **Device-validated 2026-09-22** with the broken-wave
      spec (the owner's five rides of 2026-09-21 were read on the phone's own positions and the fixes
      confirmed by feel); the PIE re-look of the map after the three changes is folded into that.
- [ ] Consumers, each in its own spec: scoring (time in pocket, whitewater), hydrofoil gates, the crash
      audio voice, the radar.
- [ ] Phase 2: the crest line as a function of phase (`CrestC(s, frame)`, `CrestZ`) precomputed from the
      height data at BeginPlay, to retire the per-board nearest-peak scan where consumers want it.

## Problem

Four systems each derive "where is the wave" on their own, and none of them knows where the wave is
*breaking*:

| system | what it derives | from | limit |
|---|---|---|---|
| `ASharedCalculations` | `signedDistanceToCrest`, `resolvedWaveBackDirection` | a nearest-peak height scan across the wave from the board | cross-shore only; unstable between the breaking crest and the swell (c sd 506 cm, M14); latches onto the bore in the whitewater |
| `ASharedCalculations::brokenAmount` | "broken water near the board" | foam point density | near ≠ in (M13); fires on a clean face in a turn |
| `AParticleSystemsController::ClusterFoam` | break centroid + crash front per tile | foam | per tile, cut at tile edges; 400 cm cells; needs the particles |
| the assist, the score, the hydrofoil gates, the fork feed | pocket / face / off-face | the two above | no notion of *along the line relative to the peel* |

T0 measured what the data actually does: the field is one 193-frame loop copied along the line with a
95-frame phase step per 2 542 cm tile, so **the impact point (where the lip lands) moves down the line
in a straight line at 25.9 cm/frame = 622 cm/s with a constant cross-shore position (sd 67 cm), and a
new break starts every loop 50 m on**. Two measured `(x, y, frame)` points reproduce the foam's own
front to ±1.9 m (p5–p95) over six loops. Everything wave-relative can be phrased against that point.

## Design

### FR1. One service, pure query
`UWaveGeometrySubsystem : UWorldSubsystem` (`Source/GoneSurfing/WaveGeometrySubsystem.*`). No tick,
no state per board; lazily initialises on first use from the level (axes from `AInfiniteWaveManager`,
loop length from `AWaveHeight`, the model from the JSON). Any actor asks
`GetWorld()->GetSubsystem<UWaveGeometrySubsystem>()`.

### FR2. The wave frame
`Line` = the tiling axis, oriented down the line (+Y-ish on `Surfing_infinite_wave`); `Back` = its
horizontal perpendicular toward the back of the wave (+X-ish; the same axis `resolvedWaveBackDirection`
resolves). `S(P)`, `C(P)`, `World(s, c, z)`. All distances in cm.

### FR3. The peel model
`impact(f, k) = P0 + V·(f − f0) + k·V·LoopFrames`, `V = (P1 − P0)/(f1 − f0)` in world XY per frame,
`k` the wave index (the same phase one loop later is the next wave, `PeriodS = |V|·LoopFrames` down the
line). `NearestWaveIndex(f, s)` picks the wave whose impact is nearest a point along the line.
The two points, and the zone constants, come from `Content/WaveGeometry/<MapName>.json`
(staged as UFS like the board profiles); `surf.wavegeo.model x/y/f/x/y/f` overrides them for an A/B.
No JSON = no model: `Sample()` returns `bValid = false` and every consumer must degrade to its
pre-service behaviour.

### FR4. `Sample(worldPos, frame)` → `FWaveGeoSample`
| field | meaning |
|---|---|
| `S`, `C` | the point in the wave frame |
| `ImpactWorld`, `ImpactS`, `ImpactC` | the impact point of the wave nearest along the line |
| `DistBehindImpact` | `ImpactS − S`: > 0 = up the line of the impact, on the broken side |
| `CrossFromImpact` | `C − ImpactC`: > 0 = toward the back of the wave |
| `SecondsSinceBroken` | `DistBehindImpact / PeelSpeed`; negative = seconds until the peel gets here |
| `Broken` | 0..1: `SmoothStep(PocketBehind, PocketBehind + BrokenRamp, DistBehindImpact)` while inside the bore band |
| `Zone` | see below |

**Two waves are real at once.** A point is *behind* the wave whose peel has passed it (kb — its
whitewater is what is there) and *ahead of* the next wave up the line (ka — still to break here).
The bore of kb is a lump that drifts shoreward as it ages while the swell of ka comes up behind it,
so the sample tests kb's bore band first, then ka's face:

1. `DistBehindImpact(kb) ≤ PocketBehind` → `Pocket` (or `Behind` / `Flat` by cross-shore — but a point
   on the `Flat` side that is inside the *previous* wave's bore band, `kb + 1`, is `Whitewater`: since
   2026-09-21, see Status).
2. Else, if `CrossFromImpact(kb)` is inside the bore band for its age
   `[−BoreFrontStart − BoreDriftRate·age, ImpactToCrestC − BoreDriftRate·age]` → `Whitewater`, `Broken` ramps.
3. Else, while the bore is younger than `ShoulderAfterSeconds` (4 s): `Behind` (the bore's back and the
   trough behind it, `CrossFromImpact > boreBack`) or `Flat` (in front of the bore — again unless the
   previous wave's band covers the point). The next wave's face has not re-formed here yet — the first
   zone map showed "shoulder" one row behind every impact.
4. Else the point belongs to ka: `Behind` (over its crest, `> ImpactToCrestC + BehindMargin`),
   `Flat` (shoreward of its face, `< −FaceExtent`), `Pocket` (within `PocketAhead` of its impact) or
   `Shoulder`. The sample's impact fields then describe ka.

Constants (JSON, defaults in the header; measured in M14 / the 14-38-37 replay where a number is given):
`ImpactToCrestC` 200 (the breaking crest is 1–3 m behind the fresh foam), `PocketAhead` 600,
`PocketBehind` 200, `BrokenRamp` 400, `BehindMargin` 300, `FaceExtent` 900, `BoreFrontStart` 800,
`BoreDriftRate` 150 cm/s (a board pushed by the bore front sat at −722 → −1862 cm over ages 1.2 → 7.9 s)
`BoreFrontFade` 400 (the front edge is soft: `Broken` → 0 over it) and `BoreFrontSpeed` 300 cm/s (a young
bore's front runs from the impact at this speed until it meets the slow-drift line, ~5 s).

### FR5. The picture cannot lie — and the zone map before any consumer
`surf.debug.wavegeo.zones <cell cm>` draws, around the board over ±75 m along the line (±3 tiles) and
±25 m cross-shore, a point per cell coloured by `Sample()`'s zone (green shoulder, yellow pocket, white
whitewater with brightness = `Broken`, blue flat, grey behind), every wave's impact point in range, and
an on-screen legend with the board's own reading. Owner's requirement (2026-09-20): confirm the areas
in PIE over multiple tiles before any code consumes them. Pictures: the T0 page's third block.
The `wavegeo` debug (`surf.debug.wavegeo 1`) draws the *service's* impact point and band, and logs
the service's sample at the board on the `WAVEGEO-BOARD` line — and every `CROSSING` line carries `geo=<zone> behind= cross= broken=`, so any ride analysis has it. Owner's
requirement: keep the draw, the positions get a close in-game look later.

### FR6. Calibration is a script, not a session
`Tools/WaveGeoFit.py <GoneSurfing.log>` reads the `WAVEGEO`/`WAVEGEO-FRONTS` lines of a run with the
debug on, fits the newest foam front's motion (least squares along the line, mean cross-shore), and
prints the JSON with the residuals. Re-run after any change to the wave data, the tiling or the map.

### NFR
- A query is a few multiplies; no allocation; safe from any actor's Tick.
- Works on the device without the particles (nothing reads foam at runtime).
- Deterministic in replays: depends only on `frame` and position.

## Consumers (follow-ups, each in its own spec)
- **Wave crash** — `broken = Sample().Broken` (broken-wave spec A4, then A1–A3); no foam at runtime.
- **Scoring** — time in `Pocket` vs `Shoulder`; whitewater riding by `Zone` (FR12 was retired because
  `brokenAmount` lied; this does not).
- **Hydrofoil / drives** — `Zone == Whitewater` as the gate M13 could not find.
- **Audio** — the crash voice at `ImpactWorld` (the clustering stays as the calibration and fallback).
- **Radar** — draw the impact point and the band; **assist** — the guard band relative to the peel.

## Acceptance
- **AC1** — on a `surf.debug.wavegeo` run of ≥ 3 loops, `|model − foam front|` p95 ≤ 250 cm along the
  line, cross-shore sd ≤ 100 cm (the M14 numbers: p95 184 / sd 67).
- **AC2** — zone timeline on the checked-in traces reads as the rides did: `10-15-54` (spin-out trace)
  and `10-48-07` in `Pocket`/`Shoulder` throughout; `14-38-37` t=3.5–10 in `Whitewater`; `20-57-25`
  t=2.5–5.3 in `Whitewater`.
- **AC3** — no consumer changes behaviour until its own spec says so (the service ships unused).
- **AC4** ✅ — owner's in-game look at the drawn positions and the zone map over multiple tiles (2026-09-20).

## Files
- `Source/GoneSurfing/WaveGeometrySubsystem.h/.cpp` — the service.
- `Content/WaveGeometry/Surfing_infinite_wave.json` — the fitted model (M14) + zone constants.
- `Config/DefaultGame.ini` — `DirectoriesToAlwaysStageAsUFS=(Path="WaveGeometry")`.
- `Tools/WaveGeoFit.py` — the fit.
- `Source/GoneSurfing/WaveGeoDebugSubsystem.cpp` — draws from the service.
- Related: `broken-wave-no-consequences.md` (T0, M14), `wave-mesh-data-registration.md` (the tiling the
  model rides on), `surf-audio.md` (`ClusterFoam`), `wave-radar.md`, `trick-scoring.md`,
  `yaw-hydrofoil-flow-direction.md`.
