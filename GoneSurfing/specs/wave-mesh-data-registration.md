# Spec: Rendered wave mesh ↔ physics height data registration (the board that rides behind the lip)

## Status
- [x] Investigation complete (2026-09-17): the drift is measured, both causes found, the fix verified
  live against the rendered meshes (Δ crest ≤ 50 cm along a 53 s / 13-tile ride).
- [x] T1b (chosen over T1, 2026-09-17): `AWaveHeight::DeriveTilingFromWaveManager` takes the tile step
  and frame-per-tile from the placed GridLODActors at BeginPlay (`AInfiniteWaveManager::InferTileLayout`,
  shared with the manager). Log line at BeginPlay reports derived vs authored values. Verified: fresh
  probe of the recorded ride vs mesh vertices, Δ ≤ 50 cm over 13 tiles; `wavesurface` screenshot at 50 s
  shows the red crest line on the rendered lip. Opt out per level with `bDeriveTilingFromWaveManager`.
- [ ] T2 Player pass: the physics wave now follows the visible wave down the line, so the ride
  beyond tile ~5 changes. Hands-off ride, 40 cm mesh lowering, crest-fade gates: re-judge.

## The observation (player, 2026-09-17, PC)

Hands-off ride on `Surfing_infinite_wave`, ~53 s, straight down the line. After a while the board
is visibly **behind the crest, on the far side**, and still gaining speed. Question: mis-aligned
meshes (physics has it on the face) or a physics bug (propelled from behind the wave)?

## Answer: mis-aligned meshes. The physics never left the face.

`surf.waveprobe` re-sampled the wave DATA at every one of the 3,704 recorded board positions (same
function, same frame the physics used — `tileFrame == wcFrame` throughout). The physics' own crest
scan (`signedDistanceToCrest`) put the crest **100–250 cm seaward of the board on every single row**:
the board rode the foot of a ~75 cm rise, ~15 cm submerged, the whole ride. There is no far-side
propulsion in the physics; the physics simply wasn't looking at the wave the player was looking at.

## Measurement: where the rendered crest is vs where the physics crest is

Method (`mesh_vs_data.py`, run from the scratchpad; recipe below): for a row every ~3.5 s, find the
`GridLODActor` tile + frame drawn under the board (manager: tile *p* at `p·(−1155, 2265)`,
`FrameOffset = 95(p+1)`, LOD0 = even frames), pull that static mesh's vertices over RemoteControl
(`KismetProceduralMeshLibrary.GetSectionFromStaticMesh`), interpolate the rendered surface along the
same cross-shore line the physics scans (±1200 cm, 50 cm steps), locate its crest with the same rule.

| t (s) | tile | physics crest | rendered crest | Δ |
|---|---|---|---|---|
| 0.0 | 0 | −100 | −100 | 0 |
| 10.5 | 3 | −200 | −150 | 50 |
| 21.0 | 5 | −150 | 0 | 150 |
| 31.4 | 8 | −100 | **+100** | 200 |
| 41.6 | 10 | −200 | +100 | 300 |
| 51.9 | 13 | −150 | **+200** | 350 |

(cm along the cross-shore axis from the board; negative = crest seaward of the board = board on the
face; positive = board behind the crest.) Aligned at the start, then ~25–30 cm of drift per tile;
from tile ~8 (~200 m down the line) the board is visually behind the rendered crest while the
physics has it on the face. Exactly the observation.

## Root cause: two data-side tiling parameters disagree with the mesh tiling

Both systems tile the same 193-frame loop (886..1078) down the line, but they are configured
independently and two of the numbers differ:

| | mesh (`InfiniteWaveManager`) | data (`WaveHeight_1`, umap values) | effect |
|---|---|---|---|
| tile step | `(ActorSpacing, YOffsetPerActor) = (2265, −1155)` | `WorldOffsetPerTile(X,Y) = (2270, 1100)` | directions differ by ~1.15° → the data wave slides ~50 cm cross-shore per tile relative to the mesh |
| frame per tile | `FrameOffsetPerActor = +95` | `FrameOffsetPerTileX = +95`, but the data tile index runs the *other way* (probe: tile −1 → −14 down the line) → effectively −95 ≡ +98 mod 193 | 3 frames of phase per tile |

Verified by re-probing the recorded ride with the parameters changed live (RemoteControl on
`WaveHeight_1`, then `ProbeTraceWaterSurface`) and re-running the comparison:

| variant | Δ at tile 13 | Δ trend |
|---|---|---|
| baseline `(2270,1100), +95` | +350 | +27 cm/tile |
| step only `(2265,1155), +95` | −400 | −30 cm/tile (worse the other way) |
| frame only `(2270,1100), −95` | +850 | much worse |
| **both `(2265,1155), −95`** | **+50** | **0–50 everywhere (one scan step)** |

Neither fix alone works; both together register the physics wave onto the rendered one along the
whole ride. With `FrameOffsetPerTileX = −95` and data tile *t* = −*p*−1 the data frame is exactly
`F + 95(p+1)` — the mesh's frame — at every tile.

## Fix (T1)

In `Surfing_infinite_wave`, actor `WaveHeight_1`:
- `WorldOffsetPerTileX` 2270 → **2265**
- `WorldOffsetPerTileY` 1100 → **1155**
- `FrameOffsetPerTileX` 95 → **−95**

Better (T1b, optional): derive these three from the `AInfiniteWaveManager` in `AWaveHeight::BeginPlay`
the way `ASharedCalculations` derives `resolvedWaveBackDirection`, so the two systems can't drift
apart again. The mapping under the −90° actor yaw is `WorldOffsetPerTile = (ActorSpacing,
−YOffsetPerActor)`, `FrameOffsetPerTileX = −FrameOffsetPerActor`. Keep the umap values as fallback
for levels with no manager (`Boards_on_flat_water`).

## Consequences to expect (T2)

- Near the start (tiles 0–2) nothing changes — the intro rails and the first seconds ride the same
  field. From tile ~5 on, the physics wave moves to where the visible wave is.
- A mid-ride live switch made the hands-off ride lose planing 13 s in. Not a verdict on the fix (the
  board was in the wrong place for the new field), but the hands-off ride and assist band need a look.
- The −40 cm mesh lowering and the "board rides 30–50 cm submerged" statement were made against the
  mis-registered field; the aligned board sits higher on the rendered face down the line. Re-judge.
- Old memories/specs about the ride "far down the line" (glide-through, crest punch-through timing)
  were measured on a wave that was drifting away from the visible one.

## Tools that fell out of this (kept)

- `surf.debug.flags 'wavesurface'` — draws the PHYSICS water surface: spheres along the cross-shore
  line through the board (green shoreward, cyan seaward, yellow at the board), the physics crest as a
  red line running down the line, and its `d` label. Foreground depth group, so the lowered mesh can't
  hide it. Screenshot it against the rendered wave.
- `surf.waveprobe` now also writes `dist_crest`, `h0`, `crest_h` and a 14-column cross-shore height
  profile per trace row — the physics' view of "where is the crest" at recorded positions, no replay
  needed. `AWaveHeight::ProbeTraceWaterSurface` is `BlueprintCallable`, so it can be re-run over
  RemoteControl after changing tiling properties live (forward slashes in the path).
- Rendered-mesh sampling without collision: `PUT /remote/object/call` on
  `/Script/ProceduralMeshComponent.Default__KismetProceduralMeshLibrary` → `GetSectionFromStaticMesh`
  with the `/Game/Waves/chunks_ratio_0_03_seamless/{cell}_mesh_{frame}` asset; world =
  tile origin + 0.1 × vertex (unloaded frames load on demand).
- Camera re-aim on a running game: the ride's world-fixed Beside camera is `CameraOffsetSide` /
  `CameraRotationSide` on `SurfboardPawn_2`, but the `Camera` component carries its own rotated
  offset (−1057, 736, 285) — zero its `RelativeLocation` first or the aim lands underwater.

## Constraints checked
- Forces are not the default tool — no force, damping or coefficient touched; this is registration.
- Takeoff is kinematic — the rails intro rides tiles 0–1 where nothing changes.
- Aggregates not maxima — the verdict is the Δ trend over 16 samples / 13 tiles, not one frame.
- Wave velocity registration spec — separate issue (velocity vs height grid), unaffected by this.

## Tasks
1. ~~T1 Set the three `WaveHeight_1` properties in the level~~ — superseded by T1b; the umap values
   are now the fallback for levels without a manager and can stay as they are.
2. [x] T1b Derive the three values from the placed GridLODActors at `BeginPlay` (fallback: umap).
   Confirmed with `surf.debug.flags 'wavesurface'`: the red crest line rides the visible lip 50 s in.
3. [ ] T2 Player pass down the line; re-check hands-off ride, assist band, mesh lowering.
