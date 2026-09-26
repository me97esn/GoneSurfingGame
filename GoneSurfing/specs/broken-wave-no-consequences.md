# Spec: Nothing stops the board in the broken part of the wave

> **Re-based 2026-09-21 on the wave-geometry service.** The spec was first written when the physics had
> no way of knowing where the wave was breaking; Part 1 is the record of finding that out, and every
> board-side signal it measured — foam density, water column, water speed, slope — is **superseded** by
> [wave-geometry.md](wave-geometry.md) (built 2026-09-18, verified in PIE by the owner 2026-09-20).
> **The one signal this spec uses is `UWaveGeometrySubsystem::Sample().Broken`.** Part 1 is kept for the
> numbers that still carry weight (the whitewater's own speed, the fixture rides, what did not work);
> each superseded section is marked at its top. Part 2 is the design against the physics as it is today.
> Earlier text: `881b58a4b` (original), `083a06285` (the 09-18 re-base).

## Status

**READY — player-validated on the device by the owner, 2026-09-22** (*"everything looks good on the
device"*), on the M20 build (`674e3a5cc`, fork `0b65606c1514`). What ships: the whitewater shoves the
board with the water (A1, anisotropic in the board's frame), takes its face drive (A2), takes its
steering (A3), the ride ends when it wallows there (the off-wave ending, surfer-fall-ragdoll.md) — all
keyed on the wave-geometry service's `Broken`, nothing on foam. The owner's three sentences in P1.1 are
answered: the wave shoves, control goes, no free speed in the foam. Tunables to feel with:
`BrokenDampingRateScale` 10, `WhitewaterAlongNoseScale` 0.1, `BrokenDriveCut` 2, `BrokenYawCapScale` 0.5,
`FallOffWaveSeconds` 3 / `FallOffWaveSpeed` 300; the map's `boreFrontSpeed` 300 / `boreFrontFade` 400.

- [x] **The signal — `Sample().Broken` from the wave-geometry service** (T0, closed 2026-09-20). Where the
      lip is landing, the crest and the whitewater band are known per tick from the loop clock and a
      two-point peel model, to ±2 m along the line / ±0.7 m cross-shore, without reading the foam; the
      owner verified the zone map over several tiles in PIE. `Broken` is 0 on the face, in the pocket
      and over the back; 1 in the bore band behind the peel (P2.2 A4).
- [x] **Superseded and to be treated as such:** the foam-density read (`brokenAmount` /
      `brokenSurroundAmount`, P1.4 M11–M13 — *near ≠ in*), the water column, the sampled water speed and
      slope as *detectors* (P1.4 M9–M10). None of them is read by anything that changes the ride, and no
      task below reads them. `brokenAmount` stays only as a diagnostic on the `foam` log line until T2
      removes or demotes it.
- [x] **Consequences built so far:** A2, the drive cut (`BrokenDriveCut`, `calcBrokenDriveScale()`), built
      2026-09-17 on the foam read and **off** since 2026-09-18 because that read fired on a clean face
      (M13). T2 re-keys it onto `Broken`. The score's whitewater factor was retired the same day for the
      same reason (`ScoreWhitewaterCreditRate` 1.0, trick-scoring FR12); re-adding it is the scoring
      spec's call, on the service's `Zone`.
- [x] **Part 1 closed 2026-09-16** — the wrong-way *push* through a crest was the yaw-hydrofoil forward
      drive alone (M8), fixed in [yaw-hydrofoil-flow-direction.md](yaw-hydrofoil-flow-direction.md) (v3).
      With it gone a breaking wave passes over a stopped board *gently* (−59 cm/s peak): the absence of a
      bug, not a breaking wave. That is what this spec still owes.
- [x] **Owner's priority order (2026-09-18):** speed, tricks and surfing on the face first; the board
      slowing when it leaves the wave second; the board *not accelerating in the whitewater* last. A fix
      for the third that costs the first is off by default (memory `ride-priority-order`). Because
      `Broken` is 0 on the face by construction, A1–A3 cost the face nothing *if* NFR2 holds (AC3).
- [x] **Re-based 2026-09-18** after the lip-snap work: the recorded 20-51-12 ride no longer reproduces
      the event like-for-like (T1 replaces it); AC2's bore-push clause is orphaned by hydrofoil v3; every
      speed number in P1 pre-dates the yaw cap and un-fade. P2.0 has the table.
- [x] **T1 built + measured 2026-09-21 (M15)** — the parked-board fixture: `-ParkBehind/-ParkCross/-ParkNoseDeg`
      on the replay autopilot teleports the board to a wave-relative spot at handoff, stopped, and a
      synthetic neutral trace holds it. A5 baseline (nose up the line, the player's case): the board's
      shoreward speed reaches **0.31×** the water's in the whitewater (AC1 wants ≥ 0.5); the lip spins it
      35°/s. Owner's italics accepted the same day: A1's target is the *sampled* water velocity (M15
      measured it strong where the bore is), A1/A2/A3 ship ON if AC3 holds, the over-the-back board is
      left alone, A3 is a rate cap (M15: the foam run-up yaws 91–110°/s, the cap would hold it to ~24).
- [x] **T2–T4 built + measured 2026-09-21 (M16).** `ASharedCalculations::brokenGeo` (+ `waveZone`,
      `distBehindImpact`, `crossFromImpact`) from the service each tick; the foam read runs only under
      the `foam` flag. **A1** `BrokenDampingGate` 1 / `BrokenDampingRateScale` 4, **A2** `BrokenDriveCut` 1,
      **A3** `BrokenYawCapScale` 0.5 — all ON by default (owner's italics). On the fixture the board's
      shoreward speed over the collapse went from 0.39× to 0.61× the water's; on 14-38-37 the wrong-way
      run-up is gone (speed relative to the water along the nose in the whitewater 285 → 36 cm/s), the
      whitewater yaw p50 57 → 25°/s; the pocket and shoulder of every ride identical to the cm. One
      service fix came out of T2: the previous wave's bore band is now tested before a point reads
      `flat` (it vanished on the frame the next impact passed — a one-frame whitewater/flat flicker).
      **The zone map needs the owner's eye again for that** (more white 10–20 m shoreward of an aged break).
- [x] **M17, 2026-09-21 — the shove made real, on the owner's eye.** The first A1 (cross-shore component
      only, ×4) moved the parked board 0.5 m; the owner: *"it should move a lot more."* Cause: the
      damping never touched the board's along-nose velocity, so it threaded the bore at −150 along the
      line while the water went shoreward. Now a **fork term damps the whole horizontal velocity toward
      the water while broken** (`p.Chaos.Solver.WhitewaterDampingRate`, fed as rate × `BrokenDampingRateScale`
      10 × broken × soft contact) and the target blends toward the stronger of the two SCs' water. The
      parked board is carried 7.8 m by t=9 s (control 0.9), 9.4 m by t=14 (1.6); 14-38-37's whitewater
      motion is the water's (relative-along-nose −26 mean). The 2 m pocket margin stays: a board riding
      tight under the lip reads the same water as a lip landing (−262 at 659 cm/s on 10-48-07), so the
      front cannot be claimed within the model's ±2 m — the board catches the bore's wake, not the front.
      Also fixed: the away-sign yaw cap fired at full value on a 1e-5 `broken` graze (now cap / broken).
      Before/after clips: https://claude.ai/artifact/R3YALakt78L7422Mppa96g
- [x] **M18, 2026-09-21 — the shove has the wrong shape (owner, device trace `phone-2026-09-21-17-54-27`):**
      *"when the sideways white water reaches the board, the board's forwards velocity stops and the
      board goes from moving only forwards to only sideways."* Measured in the trace: along-line speed
      404 → 139 in 0.75 s while the cross-shore went 0 → −394 in 0.25 s, then 3 s sideways at 3–4 m/s
      with the nose still down the line. The M17 term takes the whole velocity at one rate. **A1 becomes
      anisotropic in the board's frame** (FR-A A1, third paragraph): full rate across the hull, a
      fraction along the nose (`WhitewaterAlongNoseScale` 0.2). **Built + measured (M18): on the
      overtake fixture the forward speed holds ~400 for 0.7 s and is 63 % one second on (isotropic:
      404 → 208 in 0.2 s) while the shove reaches −440; AC1b ✅.** Owner's feel on the device owed (T7).
- [x] **M19, 2026-09-21 — the band's shoreward edge was a hard switch (owner, device trace
      `phone-2026-09-21-19-47-52`):** *"the board faced backwards in the whitewater (~150–180° from down
      the line), then suddenly accelerated forwards."* In the trace: t=13.8, the board drifting in the wake
      facing 130° off the line, the zone flips `whitewater → flat` in one tick (it had ridden 23 cm past the
      modelled bore front — the water carries it at 230–290, the front drifts at 150), every whitewater
      term switches off at once and the face drives surge it 240 → 688 along its nose. Fix in the service:
      `Broken` fades to 0 over the last `boreFrontFade` (400 cm) before the front edge. The shore-facing
      fixture now reads 1.0 → 0.7 → 0.48 → 0.35 as it outruns the front instead of flipping; the other
      fixtures unchanged. Zone map: white now fades out 4 m further shoreward — owner's look (T5).
- [x] **M20, 2026-09-21 — the young bore was 8 m wide the moment the lip landed (owner, device trace
      `phone-2026-09-21-21-37-57`):** a cutback, then turning back down the line 2–3 m behind the impact
      and 4 m shoreward of the lip's landing — reaccelerating 344 → 514 — read `whitewater` 0.2 → 1.0 and
      the board was taken. A lip that landed 0.3–0.5 s ago has spread its foam ~1–1.5 m; the board was ahead
      of it on the low face. The band's front now spreads from the impact at `boreFrontSpeed` 300 cm/s
      (M15's shore-facing board rode the young front at −287 / −637 / −954 cm at 1 / 2 / 3 s) until it meets
      the old 800 + 150·age line (~5 s). Recovery fixture (3 m behind, 4 m shoreward, 450 down the line):
      old front captured it in 0.8 s; new front, it reaches the peel's speed and races the foam. **And the
      owner's "even in the whitewater it shouldn't brake that hard": it did — at `broken` 1 the along-nose
      decay was 0.14 s (the M18 hold came from the ramp).** `WhitewaterAlongNoseScale` 0.2 → 0.1 (0.28 s)
      with `BrokenDriveCut` 1 → 2 (no drive past `broken` 0.5): 0.05 let a half-broken board surge to 730
      on the drives. Zone map: the white band is now narrow behind the peel and widens at 3 m/s — owner's look.
- [x] **T5 + T7 — device, 2026-09-22.** The model JSON is staged (the device rides read the zones — the
      five traces of 2026-09-21 were analysed on the phone's own positions); rides on the hybrid and the
      shortboard through M17–M20, each one's complaint measured in its trace and fixed the same day;
      the owner's verdict on the M20 build: *"everything looks good on the device."* T6 headless in M16.
      The zone map's PIE re-look after the three service changes is folded into that sign-off.
- [x] **Closed (owner, by measurement, M17/M20):** the lip landing on a *moving* board in the pocket. A
      board riding tight under the lip reads the same water as a lip landing, so the shove starts 2 m
      behind the modelled impact and the young bore's front spreads from there; the pocket ride wins.

---

# Part 1 — Investigation (closed; the record)

> Everything in Part 1 was measured **before the wave-geometry service existed**. It records how the
> problem was found and which board-side signals were tried and failed; the signal the design uses
> is in Part 2 (A4). Read the numbers, not the conclusions about signals.

## P1.1 The report

Player report, PC, funboard, 2026-09-15 (trace `phone-2026-09-15-20-51-12`), in the player's words:

> First the board surfs down the line. Then at 13 seconds it goes through to the other side and
> stops. It turns around a bit, and when the next wave reaches it, the nose points slightly back
> compared to down the line. The wave breaks on top of the board, but the board is hardly affected
> at all, and continues to the far side of this second wave as well. But now it's gained speed, and
> goes at slow but steady velocity on the flat water in the opposite direction of down the line
> (~23 s). The nose still points almost 180° opposite of down the line. It gradually stops, then
> when the third wave reaches it and breaks, at 27 s, the board catches the wave and gains speed.
> This is somewhat expected because it caught the wave in perfect timing. But it now surfs the
> breaking wave in the wrong direction, straight into the broken part of the wave. It surfs
> steadily with high speed, and continues when it has reached the white water.
>
> A surfer avoids going into the broken part of the wave because it's really difficult to have
> any control there, plus the collapsing wave would push the surfer and board with strong forces.
> That doesn't happen in this game.

Three defects, in order of weight:

1. **The broken section is free to ride.** No shove, no loss of control, no drag. *(This spec.)*
2. **A wave breaking on top of a stopped board does nothing to it** — except, on the 09-15 build,
   hand it 400 cm/s along its nose, even *through* the crest against the wave's travel. *(The push
   was the hydrofoil, fixed; the "does nothing" is this spec — A1/A5.)*
3. **Speed appeared along the nose regardless of where the water came from.** *(Hydrofoil spec.)*

## P1.2 The recorded rides (M1, M2)

Conventions: +Y = down the line, the broken section is at lower Y (behind the peel). The wave travels
along `−resolvedWaveBackDirection`; **on `Surfing_infinite_wave` that is (−0.89, −0.45), not −X**
(lip-snap M6), so the `vx` columns below are a ~27° approximation of "into the wave". The mesh's
forwards is local +Y, so nose = `board_yaw + 90°`; down-the-line heading ≈ +115°.

`wave_frame` loops 886 → 1078 (192 frames, 8.0 s). Every push a stationary board received in 20-51-12
landed at the same loop phase, frames 899–923: t = 9.1, 17.7, 25.2, 33.0. One face per loop, arriving
from the back, whatever has happened to "the wave" the player was riding.

| player | t (s) | trace |
|---|---|---|
| goes through to the other side and stops | 9.6 → 11.1 | nose swings 95° → 24°; x stops at ≈3890 while the crest keeps moving; speed 684 → 6. On the back. |
| turns around a bit | 11 → 17 | stationary; nose −40° → −50° under weight input only |
| next wave breaks on top, hardly affected, goes to the far side | 17.7 → 19.2 | 6 → **423 cm/s in 0.5 s**, +143 cm/s *into* the wave, over the crest (z 288 → 338). *Hydrofoil; +10 today.* |
| slow steady speed on flat water, nose ~180° off | 19.2 → 22 | 293 → 58 cm/s, stops at x≈4020 |
| third wave, catches it, surfs the wrong way at speed | 25.2 → 26.8 | 1 → **668 cm/s in 1.0 s**, nose −121° — into the broken section |
| continues into the white water | 27 → 32 | 160 → 90 cm/s crawl shoreward for 4 s, then stops |

`20-50-05` is the same ride within ~1 s; `20-02-02` has the same drive from a hard turn-around, with
one surge (185 → 570 in 0.4 s) while lying *broadside* to the wave's travel — the cleanest single-term
evidence for the hydrofoil defect.

## P1.3 Why — the physics *had* no broken-wave state (M3)

> **Superseded 2026-09-18:** the service now gives the physics that state (the peel model over the
> loop clock). What stays true: the *data* has no broken state — a bore reads as a steep face — which is
> why the state had to come from the clock and the tiling, not from anything sampled under the board.

The height/velocity data is a 192-frame loop of a steady peel, seamless across 1078 → 886. The tiles at
lower Y — the "already broken" section — are the **same loop at a phase offset** (`FrameOffsetPerTileX`,
80 per tile; since `d23fd7c9a` derived at BeginPlay from the placed `GridLODActors`) with X wrapped
inside `tiling_x`. A crest scan (`surf.debug.flags 'crossing'`, `crestCross=`) shows a periodic train
moving shoreward at ~275 cm/s; every 8 s any Y is back to a clean face.

So "broken" is not in the forces anywhere. The whitewater *does* exist in the export — a per-tile foam
point cloud (`AParticleSystemsController`, `WhiteWaterData->Positions`) — and before this spec two
systems read it: the spray particles, and the surf-audio break clustering (`ClusterFoam`,
`specs/surf-audio.md`). No force, damping, gate or control term sampled it.

The wrong-way *drive* (yaw hydrofoil, `v²·sin²(slip)` along the nose with no flow-direction term) is
documented and fixed in [yaw-hydrofoil-flow-direction.md](yaw-hydrofoil-flow-direction.md). Tick-exact
A/B on the 20-51-12 replay (M8, 2026-09-16, build `9911e268f`): zeroing `yawHydrofoilCoefficient` on
the tick before the push removed it completely (vx +207 → −8); nothing else did — fin-redirect,
off-face floor, 10× wave-normal damping, the pre-damping force set. The lip impact
([lip-impact.md](lip-impact.md)) did not fire; the propulsion governor scaled nothing.

**What M8 left for this spec:** with the drive gone, the wave passing over the stationary board carried
it shoreward at **−59 cm/s peak** and it stayed where it was. That is the absence of a bug, not a
breaking wave. AC1's shove is a *minimum*.

## P1.4 Which board-side signal can say "broken" — none of them (M9–M13)

> **Superseded by the service.** Slope, submersion, the water column, water speed and foam density were
> each tried as *the* signal and each fails on the face or in the foam in some case below. **No task in
> Part 2 reads any of them.** Kept for two numbers: the whitewater's own speed (M10 — the target A1 damps
> toward, to be re-measured on the current wave) and the fixture rides (14-38-37, 20-57-25).

All measured on headless replays of the checked-in traces (`Tests/InputTraces/`), `crossing` /
`drag` / `foam` flags, p25/p50/p75 unless stated.

### Slope cannot (M9, `070e12681`)
`phone-2026-09-16-20-57-25` (hybrid, tilt): broadside in the whitewater ~2000 cm behind the peel, the
arriving bore reads `slopeSin` 0.23–0.34 in the height data — as steep as the ridden face. A bore front
is a steep surface; the data cannot tell it from a face.

### Submersion and the water column cannot; water speed sees the bore but not the tick (M10, `e25284c6e`, `490d64e70`)

| state | submersion cm | amountUnderWater | water speed | slopeSin |
|---|---|---|---|---|
| 20-57-25 riding the face t=0–1.2 | 36/38/40 | 0.47/0.56/0.65 | 29/36/55 | 0.36/0.43/0.47 |
| **20-57-25 in the whitewater t=2.5–5.3** | 43/47/51 | 0.45/0.54/0.64 | **42/276/380** | **0.03/0.06/0.10** |
| 20-57-25 the bore push t=5.6–6.1 | 48/51/53 | 0.66/0.69/0.71 | 50/53/86 | 0.17/0.24/0.30 |
| 20-51-12 riding the face t=0–9 | 36/41/46 | 0.36/0.46/0.60 | 91/110/233 | 0.04/0.21/0.32 |
| 20-51-12 still water t=12–17 | 40/44/47 | 0.28/0.47/0.62 | 18/43/70 | 0.02/0.04/0.05 |
| 20-51-12 crest-onto-board t=17.5–18.1 | 51/54/57 | 0.67/0.70/0.76 | 30/51/99 | 0.27/0.33/0.37 |

- **Water column above: no** — board-wide or per actor (max 66 cm anywhere in either ride). The board
  floats up with the surface; a bore is an elevated steep surface the board rides *on*. Note also
  `effectiveWaterHeight = min(10 + col + slopeSin·1000, 200)` sits at the cap in every state above —
  the column contributes nothing to the forces scaled by it.
- **Water speed was the only board-side signal that saw the bore** (276–380 cm/s shoreward under a
  0.06 slope while sitting in the whitewater; nothing else in either ride above ~230) — but at the push
  tick itself the water had gone quiet (53). It is not the detector (the service is); it matters here as
  the **A1 target**: the damping carries the board toward the water's sampled velocity, so if that
  velocity is quiet where `Broken` reads 1 the shove is quiet too (P2.2 A1, open point).
- **Two things this table settles for Part 2.** (1) The whitewater's own speed — the A1 target — is
  **~280–380 cm/s shoreward**, measured, not tuned. (2) The whitewater's `slopeSin` 0.03–0.10 is below or
  inside the wave-normal damping's slope gate (`SmoothStep(0.05, 0.10)`), which is why the one damping
  term that *would* carry a board with the water is mostly off exactly there (P2.2 A1).

### Foam density: near, not in (M11–M13)

`foam` flag: cached world-space foam points (`AParticleSystemsController::GetWhiteWaterWorldPoints`)
within XY radii of the SC. *Build note:* M11 and M12 were measured **before** the registration fix
`d23fd7c9a` (the physics wave sat ~27 cm/tile + 3 frames/tile off the rendered wave — and the foam is
rendered-side); M13 after it. Part of the M12 → M13 jump in face counts may be the fix.

M11 (`ea38e84b5`), all round, p25/50/75:

| state | n300 | n600 |
|---|---|---|
| 20-57-25 riding the face t=0–1.2 | 36 / 73 / 107 | 359 / 411 / 463 |
| **20-57-25 in the whitewater t=2.5–5.3** | **561 / 751 / 868** | 1126 / 1235 / 1692 |
| 20-57-25 the bore push t=5.6–6.1 | 469 / 525 / 536 | 871 / 933 / 989 |
| 20-51-12 riding the face t=0–9 | 11 / 64 / 137 | 335 / 436 / 594 |
| 20-51-12 still, out the back t=12–17 | 0 | 0 |

7–10× between a ridden face and the whitewater, holding through the push tick (unlike water speed).
The face is never zero — the pocket sits beside the lip. Cost: O(N) over 1 450–2 740 cached points per
SC per tick.

M12 (`91d98a378`): `phone-2026-09-17-14-38-37` (hybrid) turned 180° in the whitewater and went
119 → 663 cm/s (hydrofoil FR1 reads 1 there by design — the board moves *with* the wave, along the
crest the wrong way). The all-round count under it was only 95–215, while the *face* has a fat tail
(p90 284, p99 393, max 445): no all-round count separates them. Counting the half-disc **ahead** of the
motion does: on a face the foam is all behind the board and the water ahead is clean out to 6 m
(ahead-within-600 p95 34, max 104); heading into the broken section it is 148–327. Built as

```
aheadTest  = SmoothStep(FoamAheadCountLow 60, FoamAheadCountHigh 160, foam ahead within FoamAheadRadius 600)
denseTest  = SmoothStep(FoamBrokenCountLow 420, FoamBrokenCountHigh 520, foam all round within FoamBrokenRadius 300)
brokenAmount = smooth(max(aheadTest, denseTest), FoamBrokenSmoothSeconds 0.25)
```

M13 (`77aea9af3`): the owner's shortboard slalom (`phone-2026-09-17-20-44-07`, PC) bled 350 → 79 cm/s
mid-turn and skidded; bisected to `91d98a378`. `brokenAmount` read 0.95–1.0 for 1.5 s on a *clean face*
because a hard turn swung the nose at the lip's foam (ahead-within-600 = 238–630; all round 512–1170).
Rebuilt as `brokenSurroundAmount` (dense disc split into four 90° sectors round the motion, 4 × the
thinnest through the same knee), and measured (SC_1, sector p25/50/75 within 300 cm):

| situation | ahead | behind | left | right | surround > 0.5 |
|---|---|---|---|---|---|
| slalom, turns up at the lip on a clean face (**no cut wanted**) | 1/29/131 | 265/358/461 | 0/0/84 | 6/62/113 | 0 % |
| whitewater sit 20-57-25 t=2.5–5.3 (**cut wanted**) | 0/20/55 | 56/148/273 | 19/96/176 | 22/55/172 | 0 % |
| wrong-way run-up 14-38-37 t=3.5–5.0 (**cut wanted**) | 0/0/0 | 47/69/246 | 24/47/60 | 0/4/191 | 0 % |

The foam is a **thin band along the broken lip**. A board carving up into it on a clean face has *more*
foam round it than a board sitting in the whitewater, with the same one-sided shape. No sector rule
separates row 1 from rows 2–3. **Conclusion: the point cloud carries "is there broken water near the
board", not "is the board in it".** That is why the only two consumers built on it — the drive cut and
the score factor — were both switched off, and why the signal was replaced by the service. The service
separates all three rows (wave-geometry AC2).

## P1.5 Conclusions

1. The recorded rides match the player's account exactly; not a perception issue.
2. To the forces there was no broken section (M3). The fix is a new *signal*, not a coefficient.
3. Nothing sampled under the board can be that signal (M9–M13). It has to come from where the wave
   *is*: the loop's own clock and the tile phase — the data cannot tell a bore from a face; the calendar
   can. **That became the wave-geometry service** (T0 / P2.1), and foam is not part of it.
4. With the wrong-way drive gone, a breaking wave passes over a stopped board *gently* (M8). The
   whitewater has to shove, and the numbers for how hard are in M10.

---

# Part 2 — Design (re-based 2026-09-18)

## P2.0 What changed under this spec, and what it means here

Everything in this table is on `wave-crash-3` (`3636d6286` and later), player-validated on PC unless
noted. Read with CLAUDE.md "Forces vs damping vs redirects" — the whitewater must be built with the
same three tools.

| change | what it does now | consequence for this spec |
|---|---|---|
| **Yaw-rate ceiling** (fork `MaxAngularVelocityZPos/Neg`, fed at `YawRateCapMax × min(1, Knee / v)`, 2.5 rad/s / 200 cm/s) on the sign that yaws the nose **toward the wave** only | stops the snap at speed spinning out | A slow board in the foam gets the full 2.5 rad/s — the cap does nothing there today. But the fork now *has* a per-sign yaw ceiling the project feeds per tick: **A3's tool** (P2.2). |
| **Carve-grip un-fade** (`CarveGripTurnUnfadeRate` 1.0): the crest fade on the grip redirect is lifted while the board yaws toward the wave | momentum follows the nose through a snap at the lip | Any turn in the whitewater that passes through a toward-wave yaw now has full grip: velocity follows the nose. The 14-38-37 run-up is a 180° turn in the foam — **it may be stronger now** (T6 re-measures). |
| **Wave-normal damping left as is** at the crest (`waveNormalDampingRate` 0.06, slope-gated 0.05–0.10): lip-snap M6/M11 measured it as the sink that stops a lip snap becoming an air; owner's call to leave it | the penetration block behind the crest | A1 must not touch the *slope*-gated path. It opens the same damping on `broken` instead (P2.2 A1). Also proof of scale: the term took ~200 cm/s in 0.25 s at the crest — violent enough for A1. |
| **Hydrofoil gate v3** (`yawFwdWithWaveGate*`, fades out above 150 cm/s) | no forward drive for a *slow* board driven into a crest | The broadside bore push (20-57-25, starts at 198 cm/s) is **outside** the gate now; with A2 off nothing addresses it. AC2's clause is orphaned (P2.3). |
| **Registration fix** (`d23fd7c9a`, `-WaveDeriveTiling=0` for the old wave) | physics wave follows the rendered wave down the line | M11/M12 foam thresholds were calibrated on the old wave (T5). The 09-15/09-16 traces were *recorded* on the old wave; their replays are deterministic against themselves but not against the phone. |
| **Headless takeoff is physics** (lip-snap M8) | a baked tunable acts during the 3.3 s takeoff and moves the handoff | Every A/B of a default under this spec uses the device-like form (P2.5). |
| `-ReplayOverrides=a,b` applied only `a` before `3636d6286` | — | M8's "both" column matches the single `yawHydrofoilCoefficient=0` run, so its conclusion stands; the "pre-damping set restored/halved" rows are unverified unless they went through the JSON. Not re-run: nothing hangs on them. |

## P2.1 The gate — where is the wave (T0, closed)

> **Closed 2026-09-20.** The owner made locating the wave the gate for everything else; it was measured
> (M14), signed off on the pictures (2026-09-18) and on the zone map in PIE (2026-09-20), and the result
> is [wave-geometry.md](wave-geometry.md). The section is kept as the record of what was asked, what was
> measured and what did not work. The table below describes the code **before** the service.

Every consequence in this spec keys on three places, and each of them was at the time either unlocated
or located by a proxy that M13 showed to be wrong for the purpose:

| what | what it is | what the code has today | why that is not enough |
|---|---|---|---|
| **the crest line** | the ridge the board rides under, moving shoreward at ~275 cm/s | `signedDistanceToCrest` / `resolvedWaveBackDirection` from a height scan of the data (`crossing` flag) | scans the *data*; never checked against the *rendered* wave since the registration fix; the scan once picked the wrong wave from 0.02 cm of noise (memory `crest-scan-noise-picked-wrong-wave`) |
| **the impact point** | where the lip is landing *now* — the peel, moving down the line; the pocket is just up-line of it, the whitewater just down-line | nothing in physics. The audio's `ClusterFoam` finds a per-tile break centroid + crash front from foam (`FFoamBreakCluster::Front`); the lip-impact term gates on slope and crest distance | foam lags the collapse and is a thin band along the whole broken lip (M13) — it marks *that* the wave has broken along a stretch, not *where it is breaking now* |
| **the whitewater region** | behind the impact point along the line, from the crest line shoreward as far as the bore runs | `brokenAmount` (foam near the board) | near ≠ in (M13) |

### T0 — the task

1. **Derive candidates for all three from the data**, per tick, in one place (`ASharedCalculations` or
   `AWaveHeight`): the crest line through the board's Y (the existing scan, now checked); the impact
   point on that crest from the loop phase — the tiles are the same loop offset by
   `FrameOffsetPerTileX` (80) per tile step, so "where the break is at frame *f*" is a function of *f*
   and the tile geometry alone; the whitewater as the half-plane down-line of the impact point,
   clipped to a band shoreward of the crest.
2. **Draw them in the world** — `DrawDebugLine` / `DrawDebugSphere`, gated by a
   `surf.debug.flags 'wavegeo'` flag (`AWaveHeight::DrawDebugVisualization` is the existing hook): the
   crest as a line along the wave, the impact point as a sphere, the whitewater band as a filled quad.
   Draw the *same* three on the radar — it already draws the foam footprint, so the overlay is
   self-checking.
3. **Confirm by eye against the rendered wave and the Niagara foam.** `Screenshot.ps1` windowed, a
   fixed camera above and behind the peel, at eight frames spanning one loop (886, 910, 934 … 1054)
   and at a position spanning a tile seam; the same eight frames from the chase camera while riding.
   Published as one page of frames (memory `publish-artifacts-for-remote-viewing`) for the owner to
   judge, plus PIE by eye.
4. **Measure the error** at each frame: crest line vs the visible ridge (cross-shore, cm); impact
   point vs the visible lip landing (along the line, cm; and the frame at which the drawn point
   reaches a fixed Y vs the frame the visible lip does); whitewater band edge vs the visible foam
   front.

**Pass tolerances** (proposed; the owner's eye is the final judge): crest within **50 cm** cross-shore,
impact point within **200 cm** (a board length) along the line and **0.25 s** (6 frames) in time,
whitewater edge within 200 cm — at every one of the eight frames and on both sides of a seam. The
registration fix ([wave-mesh-data-registration.md](wave-mesh-data-registration.md), T2 player pass
still owed) is a prerequisite: if the rendered and physics waves disagree, fix that first, not this.

### If the data cannot place them: the simple model — *this is what was built*

> **2026-09-20:** the two-point model below is `UWaveGeometrySubsystem` ([wave-geometry.md](wave-geometry.md)),
> fitted from the foam by `Tools/WaveGeoFit.py` rather than by eye (M14: it reproduces the foam front to
> ±1.9 m), with the whitewater as an *aging* bore band — front 8 m shoreward of the impact drifting at
> 1.5 m/s, back edge from the crest drifting with it — instead of the fixed `BoreExtent` rectangle
> written here. The loop is 193 frames, not 192. Kept as written for the record.


The wave moves linearly. Within one tile the impact point advances down the line at a constant
rate and shoreward with the crest; across tiles the same thing repeats one tile step later and
`FrameOffsetPerTileX` frames later; across the loop wrap (1078 → 886) the wave is back a wavelength.
So **two measured coordinates `(x, y, frame)` of the visible lip landing**, taken by eye in PIE at two
frames of the same wave, define the impact point at every frame:

```
impact(f)      = P0 + (P1 - P0) * (f - f0) / (f1 - f0)     tiled by (tileStep, FrameOffsetPerTileX),
                                                            wrapped by the loop (192 frames)
crest(f)       = the line through impact(f) perpendicular to resolvedWaveBackDirection
whitewater(f)  = { P : (P - impact(f)) . lineDir < 0  and  0 <= (impact(f) - P) . backDir <= BoreExtent }
```

Two points give both speeds (down-line and shoreward). **The owner expects the peel to move at a
constant rate** (2026-09-18), and the tiling says the same — one tile step per `FrameOffsetPerTileX`
frames — so the two-point line is the model, not a first guess. A third measurement one tile over is
the check, not a design branch. Only if it fails (the peel visibly accelerates, or the seam is not a
clean offset) is the fallback a **hand-authored table for one tile** — the impact point at every 12th frame,
16 rows — tiled the same way. Either lives in a small JSON next to the board profiles
(`Content/WaveGeometry/<map>.json`) and is read once at BeginPlay. It is not much data, it is exactly
as accurate as the eye that measured it, and it depends on neither the foam cloud, the particle
budget nor the camera cull.

The data-derived candidate (step 1) and the hand-measured model should agree; where they don't, the
hand model is the reference and the data-derived one is what is wrong.

### M14 — measured 2026-09-18: the wave is where the tiling says it is

Tool: `Source/GoneSurfing/WaveGeoDebugSubsystem.cpp` (`surf.debug.wavegeo 1`; `.model x/y/f/x/y/f`,
`.cam dShore/up/dLine/fov`, `.shots start:step:count`, `.shotloop`, `.fix`). Draws the crest line
(yellow; height scan every 2 m along the line, one wave followed), the impact point (orange; the newest
foam cluster's down-line front, from the audio's `ClusterFoam`), the two-point model (magenta), the
whitewater band (cyan), and turns on the controller's foam/centroid/front draw. Logs `WAVEGEO`
(per tick, wave-frame s/c), `WAVEGEO-CREST` (crest profile along the line), `WAVEGEO-FRONTS` (every
cluster), `WAVEGEO-PROFILE` (cross-shore height profile). Run: `Screenshot.ps1 -Map Surfing_infinite_wave
-ExecCmds "surf.debug.wavegeo 1,surf.debug.wavegeo.shots 886:24:8,surf.debug.wavegeo.shotloop -1,
surf.debug.wavegeo.cam 2600/900/0/80,surf.debug.wavegeo.model 4817/4518/1030/2557/8954/1222"
-LoadSeconds 40`; frames land in `Saved/Screenshots/WindowsEditor/wavegeo_loop<n>_f<frame>.png`.

| quantity | value | how |
|---|---|---|
| tiling (derived) | mesh step 2 542 cm along the line, 95 frames per tile; loop = 193 frames (886–1078) at 24 fps | `WaveHeight` log line |
| impact speed along the line | **25.93 cm/frame = 622 cm/s** (tiling predicts 26.8; 3 % off, inside the cell quantisation) | least squares on the newest foam front, 2 105 ticks, 6 loops |
| linearity | residual sd 126 cm (400 cm cluster cells); per-loop mean within ±50 cm; no step at 12 seams | same |
| impact cross-shore | constant, sd 67 cm; 1–3 m shoreward of the breaking crest | front c over the run |
| period | one new break per loop, 5 005 cm down the line | — |
| **two-point model vs foam front** | **mean +49 cm, p5 −186, p95 +184 cm** (1 761 ticks) | model `4817/4518/1030 → 2557/8954/1222` (world x/y/frame) |
| standing lip vs foam front | crest height peak 2–8 m *down-line* of the front (mode +4 m) | crest profile vs model |
| crest cross-shore speed | swell ~340 cm/s from the back → ~120–180 cm/s as it breaks and as a bore; height peaks +30 cm at the break | fixed-point profile peak tracks |
| foam trail | fresh foam hugs the crest up-line of the impact; older fronts 5–15 m shoreward | front c vs age |

What did *not* work, recorded so nobody rebuilds it: (1) the nearest-peak crest scan from the board is
unstable (c sd 506 cm — the board sits between the breaking crest and the swell; it also latches onto
the bore once the board is in foam) — the model's constant c is the reference; (2) the foam-derived
impact fails when the board is in the whitewater (loop 4 of one run: the pick followed the aging bore
near the board while the model kept the peel 11 m ahead) — **the runtime signal is the model; foam
only calibrates it**; (3) a fixed world point cannot be used for the visual — tiles and foam follow the
board; (4) a top-down camera shows nothing on a 1 m wave.

**Consequence for A4:** `impact(f) = P0 + V·(f − f0)` with the fitted V and the loop period along the
line, `c_impact` constant; `behindImpact` from s, `inBand` from c against `c_impact` (not the scan).
The residual question for the owner (T0 sign-off) is only the constant offset between the foam front and
the visible lip landing.

### Abandon criterion

If, after the data path and the two-point model, the drawn geometry still cannot be made to sit on
the visible wave within the tolerances above at every phase of the loop, **this spec is closed
WONTFIX**: the Status gets the measured errors, the frames, and the sentence "the physics cannot know
where the wave is breaking to within a board length, so any whitewater consequence would fire on the
face as often as in the foam (M13), and the owner's priority order forbids that". `brokenAmount`, the
drive cut and the score factor are then removed rather than left as dead code.

## P2.2 Requirements

### FR-A. The broken section has to cost something

The forces need a per-board scalar `broken ∈ [0,1]` for "the board is in collapsed water". **It comes
from the wave-geometry service and nowhere else (A4).** Then:

- **A1. Shove — damping toward the whitewater's velocity, not a force.** The mechanism already exists
  in the fork: the wave-normal damping damps the board's velocity component along
  `resolvedWaveBackDirection` toward the *water's* velocity along it (`p.Chaos.Solver.WaveNormalDampingRate/Gate`,
  `WaterVelX/Y/Z`, fed from [SurfboardUtils.cpp](../Source/GoneSurfing/SurfboardUtils.cpp) ~L280–335).
  Today its gate is `contact × SmoothStep(waveNormalDampingMinSlopeSin 0.05, +0.05, slopeSin)`, so in
  the whitewater (slopeSin 0.03–0.10, M10) it is mostly off while the water there moves 280–380 cm/s
  shoreward. A1 is therefore:

  ```
  waveNormalGate = contact × max(slopeGate, broken)
  rate           = waveNormalDampingRate × lerp(1, BrokenDampingRateScale, broken)
  ```

  with `BrokenDampingRateScale` a new tunable (disable value 1). The target and axis are what the feed
  already uses — the sampled water velocity along `−resolvedWaveBackDirection` — so the shove is the
  water's own measured speed, and it asymptotes (a board broadside in whitewater ends up moving *with*
  it, never through it). The slope-gated path is untouched, so the crest behaviour validated in lip-snap
  T5 is unchanged when `broken` = 0 — which on a clean face it must be (A4).
  *Rejected:* a shove force (overshoots and reverses — memory `forces-are-not-the-default-tool`); a
  second trigger on the lip impact (it fired in none of the M8 pushes nor in any lip-snap snap; its gates
  describe a board under the curl, not a board the wave breaks onto).
- **A2. No face drive.** Built (`BrokenDriveCut`, `AFluidDynamics::calcBrokenDriveScale()` on the
  yaw-hydrofoil forward and the passive slope thrust), **off**, and stays off until `broken` means *in*
  (A4). Re-keyed onto the A4 signal it is a one-line change. Owner's order: a cut that ever fires on the
  face is worse than no cut.
- **A3. Control degrades.** Scale the **yaw-rate ceiling** by `broken` on *both* signs:
  `cap = YawRateCapMax × min(1, Knee / v) × lerp(1, BrokenYawCapScale, broken)`, the away-from-wave sign
  fed the same value only while `broken > 0` (it is unlimited today and must stay so on the face —
  lip-snap M7, the cutback). This is the redirect-family way to take authority away; it does not touch
  the lateral-turn torque or the player-validated commanded-lean carve
  (`hard-carve-progressive-boost`). Stamina draining faster in foam is the other half, decided with the
  player ([stamina.md](stamina.md)).
- **A4. Where the signal comes from — the wave-geometry service.**
  `broken = UWaveGeometrySubsystem::Sample(boardPos, frame).Broken` — 0 on the face and over the back,
  0 through the pocket (6 m ahead to 2 m behind the impact point along the line), then ramping 0 → 1 from 2 m to 6 m behind it, inside the aging bore band, `Zone == Whitewater`
  ([wave-geometry.md](wave-geometry.md) FR4; verified in PIE by the owner 2026-09-20). This separates
  all three M13 rows without any foam: the slalom and the pocket are at or ahead of the impact point
  (`Broken` 0), the sit and the run-up are behind it (`Broken` 1 — measured on the 14-38-37 and
  20-57-25 replays, wave-geometry AC2). No foam term at runtime: the point cloud is what M13 showed to
  lie, and it depends on the device's particle budget and the camera cull. The old foam read
  (`brokenAmount`, `brokenSurroundAmount`) stays only as a diagnostic and, if ever wanted, an optional
  *presence* floor (a tunable, off). The speed gate (M13 option 2) is not needed as a discriminator;
  kept as a *feel* tunable, off.
- **A5. The "wave breaks on top of a stopped board" case** is A1 in its purest form and its acceptance
  test. Floor today: −59 cm/s (M8, pre-cap build; re-read on T1's fixture). Target: the whitewater's own
  speed (M10), arriving at the collapse, not once a foam trail has formed behind it.

  **Shape (M18).** A hull is a wall to water across it and streamlined along it, so the whitewater
  damping is anisotropic in the board's frame: the relative velocity is split along the nose and
  across it; across is damped at the full rate, along at `rate × WhitewaterAlongNoseScale` (default
  0.2: a 0.3–0.4 s decay at ×10 against 0.07 s across). A board riding down the line that the peel
  overtakes is shoved shoreward hard *and keeps most of its forward speed for a second or two*,
  instead of stopping and sliding. A board broadside to the bore (the parked fixture, nose along the
  line) is unchanged: the water comes across its hull. The fork gets the nose as `WhitewaterNoseX/Y`
  and the scale as `WhitewaterAlongScale`; a zero nose falls back to isotropic.

**Who this applies to — any board, stationary or moving.** A1–A3 read `broken` every tick and ask nothing
about the board's speed or heading, so they act on a stationary board the wave breaks onto (A5, the
cleanest case), a board that turns back into the foam (the 14-38-37 run-up, AC2), a board lying
broadside when the bore arrives (20-57-25), and a board that has ridden the wave to its end and is now
in the whitewater. All of these are *behind the peel*. **What the signal does not cover is the lip landing
on a moving board in the pocket:** `Broken` is 0 within `PocketBehind` (2 m) of the impact point and ramps
to 1 over the next 4 m, so a board the lip closes out on feels nothing until it is a board length behind
the peel (~0.3–1 s at 622 cm/s). Whether that case is wanted, and whether it is this spec's or
[lip-impact.md](lip-impact.md)'s (built for a board *under the curl*; never fired in M8), is the owner's
call — open in Status.

### FR-B. (moved) The yaw-hydrofoil forward drive requires flow from ahead
[yaw-hydrofoil-flow-direction.md](yaw-hydrofoil-flow-direction.md), built, v3. Independent of FR-A.

### FR-C. Judge on whole-ride aggregates, both directions
Mean/median down-the-line speed, time-in-pocket, count of no-input ≥300 cm/s gains in <0.5 s **of the
board's speed relative to the water along its nose** (must go to zero *behind the peel*, stay unchanged
on the face), the turn-surge sawtooth metrics (`shortboard-turn-surge-sawtooth`) — and now the lip-snap
set (P2.5). Never a single-event maximum (`ab-video-invalid-when-trajectory-diverges`).
*Relative to the water, since M16:* the bore carries a board at 300–600 cm/s, which an absolute-speed
count reports as a "gain" in the whitewater — A1 doing its job. The along-nose relative speed is the
drive; the `CROSSING` line carries `bVel`, `wVel` and `fwdH` to compute it.

### NFR1. The wave data and tiling stay as they are
M3 is a data property; the broken zone is a signal layered on top, not a change to the loop.

### NFR2. Nothing here changes a clean-face ride
`broken` = 0 on every tick of the face rides (20-51-12 t=0–9, 10-48-07, 10-15-54, 09-51-17, the slalom
turning up into the lip) — then A1/A2/A3 are identities and the lip-snap results stand by construction.

## P2.3 Acceptance criteria

- **AC0 (T0) — the wave is located.** The drawn crest line, impact point and whitewater band sit on the
  rendered wave and the Niagara foam at eight frames across the loop and across a tile seam, within
  P2.1's tolerances, signed off by the owner on the screenshot page and in PIE. **Gate for every AC below.**
- **AC1 (A1/A5) — the stopped board.** On the T1 fixture `park_ahead2000_nose180` (M15: parked 20 m
  ahead of the peel on the break line, nose up the line, the player's case), the board goes with the
  whitewater: its shoreward speed at the water's peak **not less than 0.5× the water's** (control 0.31;
  M17 0.77; **M18: 0.92 ✅**) and it is **carried at least 3 m shoreward** over one wave (control 0.9 m;
  M17 7.8 m with the isotropic term, which also glued its drift along its own length; **M18: 4.2 m ✅**,
  9 m over two waves), it does not go into the wave, and — with A3 — its yaw rate while `broken` > 0
  stays under the scaled cap. Owner's eye on the clips owed (T7). *Not the single peak tick:* the water peaks at the impact,
  inside the 2 m pocket where `broken` is 0 by design, and the nose and the tail sit in different water
  (M16: −118 vs −343 two metres apart), so the metric names the feed's SC. *The recorded 20-51-12 ride is no longer
  the fixture: its replay diverges at t=6.55 under the un-fade (lip-snap AC3) and parked 1 800 cm from
  the phone's stop even before that (M11).*
- **AC1b (A1, M18) — the overtaken board keeps its line.** On the `phone-2026-09-21-17-54-27` replay's
  overtake (riding down the line at ~400, `broken` ramping in), the along-line speed one second after
  `broken` passes 0.5 is **not less than 60 % of what it was at `broken` = 0.1**, while the cross-shore
  speed still reaches ≥ 0.7× the water's; the parked fixture (AC1) is unchanged within 5 %.
- **AC2 (A2/A4) — the wrong-way run-up.** `phone-2026-09-17-14-38-37` t=8.5–9.6 (180° turn in the foam,
  119 → 663 on the M12 build): with A4 position + A2 on, the board's speed *relative to the water along
  its nose* in the whitewater stays low (mean < 100 cm/s) and the board is carried shoreward instead.
  **M16 ✅:** control 285 mean / 546 max → 36 mean / 183 max; whitewater |v| 358 → 146; the run-up is gone
  (t=7.4 on the current build's replay: 179 → 359 up the line in the control, absent with A2).
  *The 20-57-25 broadside bore push (t=5.5–6.0) is dropped as a criterion:* hydrofoil v3 fades out above
  150 cm/s and the push starts at 198; by the owner's order it is priority three. Keep it as a
  measurement only.
- **AC3 (NFR2) — the face is untouched.** The lip-snap regression set rides identical to the cm with
  `broken` reading 0 throughout: 10-15-54 (spin-out trace), 10-48-07 (hybrid cutback), 09-51-17
  (four snaps), the slalom, `hard_turn_towards_the_wave` (630 / maxUW 0.76), and 20-51-12 to t=6.55.
- **AC4 (FR-C)** — whole-ride aggregates on shortboard, hybrid and funboard within run-to-run noise on
  the face windows; the no-input-gain count behind the peel → 0.
- **AC5** — player-validated on device on the funboard and one lower-floor board; the owner's three
  sentences in P1.1 answered in order: the wave shoves, control goes, no free speed in the foam.

## P2.4 Tasks

- **T0 Locate the wave (the gate, P2.1).** Data-derived crest / impact / whitewater drawn in the world
  and on the radar; eight-frame screenshot sets from a fixed camera and the chase camera, across a
  seam; measured errors; the owner's visual sign-off. If the data fails: the two-point model, then the
  16-row table; if those fail: WONTFIX and the dead code removed. **Nothing below starts before this
  passes.**
- **T1 Fixture.** Synthesise a trace (as lip-snap M11 did) or a `AStateTriggerAutoPilot` that rides
  down the line, goes over the back at a known Y, and waits — so "board stopped, face arrives, foam
  present" reproduces without a recorded ride's inputs. Record the control numbers on the current build
  (vx into the wave, shoreward speed, nose rate) before any A1 work.
- **T2 `broken` from the service.** `ASharedCalculations` reads `Sample().Broken` per tick into a new
  `brokenGeo` (the old foam `brokenAmount` demoted to a diagnostic, or deleted), `calcBrokenDriveScale()`
  and the new A1/A3 terms key on it; logged next to the raw counts on the `foam` line (the `CROSSING` line already
  carries `geo=`). Check on the three M13 rows: 0 on the slalom and the pocket, 1 on the sit and the
  run-up; 0 on every tick of the AC3 face set.
- **T3 Shove (A1).** The gate/rate change in the wave-normal feed + `BrokenDampingRateScale`. Tick-exact
  A/B on the T1 fixture; then AC3.
- **T4 Control (A3).** `BrokenYawCapScale` on both signs while `broken > 0`. Same fixture.
- **T5 Calibration — demoted.** The runtime signal no longer reads foam, so the four foam counts need
  re-deriving only if the optional presence floor is ever turned on. What does need doing on the device:
  confirm `Content/WaveGeometry/Surfing_infinite_wave.json` is staged (the `WaveGeometry: model ok` log
  line) and that the zone map (`surf.debug.wavegeo.zones 250`) reads the same as on PC.
- **T6 Regression.** AC3 set + AC4 aggregates, device-like harness; re-measure 14-38-37 first.
- **T7 Device.** Owner rides into the whitewater on purpose on the funboard and the shortboard.

## P2.5 Harness

```
# a checked-in trace, replayed against the current wave
$env:TEST_MAP="Surfing_infinite_wave"
$env:EXTRA_ARGS="-ReplayTrace=phone-2026-09-17-14-38-37.csv -ReplayUseWeights -usefixedtimestep -fps=60 -BoardInTests -Board=hybrid"
RunGameAndCollectLogs.ps1 -Argument "torque,crossing,foam,wavenormal,yawcap:SharedCalculations,SurfboardUtils" -TimeoutSeconds 300

# DEVICE-LIKE A/B of a default (lip-snap M8): OFF in Saved/TuningOverrides.json for the headless
# physics takeoff, ON from the handoff (trace t=0). Multi-entry overrides work since 3636d6286.
#   Saved/TuningOverrides.json: {"AssistDisable":1,"BrokenDampingRateScale":1,"BrokenYawCapScale":1}
$env:EXTRA_ARGS="... -ReplayOverridesAt=0.0 -ReplayOverrides=BrokenDampingRateScale=4,BrokenYawCapScale=0.3"

# the pre-registration wave, for replaying a 09-15/09-16 trace as it was recorded
$env:EXTRA_ARGS="... -WaveDeriveTiling=0"

# T1 — the parked fixture (M15). behind < 0 = ahead of the peel (the lip arrives at 622 cm/s);
# -ParkSpeed=<cm/s> = initial velocity along the nose (M18: the overtake = -ParkBehind=350 -ParkCross=-150 -ParkNoseDeg=0 -ParkSpeed=400)
# cross > 0 = toward the back of the wave; noseDeg 0 = nose down the line, 180 = up the line, -90 = at the shore.
$env:EXTRA_ARGS="-ReplayTrace=synth-parked-neutral.csv -ReplayUseWeights -usefixedtimestep -fps=60 -BoardInTests -Board=hybrid -ParkBehind=-2000 -ParkCross=0 -ParkNoseDeg=180 -ParkName=park_ahead2000_nose180"
RunGameAndCollectLogs.ps1 -Argument "crossing,wavenormal:SharedCalculations,SurfboardUtils" -TimeoutSeconds 300
# then: grep PARK-TICK Saved/Logs/GoneSurfing.log — boardVn vs waterVn through the collapse (t=2.9-4.0)
# A/B on the fixture: the park overwrites the takeoff state, so an in-memory switch at t=0 is exact:
#   ... -ReplayOverridesAt=0.0 -ReplayOverrides=BrokenDampingGate=0,BrokenYawCapScale=1,BrokenDriveCut=0   (the control)
# Ride A/Bs (M16): the same switch on 14-38-37 / 10-48-07 was identical to the cm on the face. On the
# shortboard slalom it drifted sub-cm in the TAKEOFF before any override applied; there use the JSON form
# for the control ({"BrokenDampingGate":0,"BrokenYawCapScale":1,"BrokenDriveCut":0} in TuningOverrides.json).
# Metrics: crossing_metrics-style — relative-along-nose from the CROSSING line's bVel/wVel/fwdH.
```

`AssistDisable=1` is needed for PC (mouse) traces. Every `CROSSING` line carries `geo=<zone> behind= cross=
broken=` from the service; `surf.debug.wavegeo 1` + `.zones 250` draws it. `foam` logs `n300/n600/ahead/sectors=a/b/l/r` +
`brokenAmount`/`brokenSurroundAmount`; `wavenormal` logs the gate, rate and `boardVn/waterVn/relVn`;
`yawcap` the per-sign cap. Sum `TORQUE` categories over both SCs (gravity once). Judge the ride from the
`Recorder flushed` line, not the process end (memory `stop-hook-kills-background-test-runs`).

## P2.6 Measurements on the current build

### M15 — the parked fixture (T1, 2026-09-21, `8c57d20e0` + park)

`-ParkBehind=<cm> -ParkCross=<cm> [-ParkNoseDeg=<deg>] [-ParkName=<test>]` on `AInputReplayAutoPilot`:
on the handoff tick the board is teleported to `(ImpactS − behind, ImpactC + cross)` of the wave nearest
it, on the surface, nose along the line rotated `noseDeg` toward the back, velocities zeroed; the trace
`Tests/InputTraces/synth-parked-neutral.csv` (20 s of 0.5/0.5) holds it. `PARK` logs the spot,
`PARK-TICK` (every 50 ms) the service's zone/behind/cross/`Broken` at the board, the board's and the
water's cross-shore (`Vn`, + = toward the back) and along-line (`Vs`) velocities, speed, yaw rate,
z, submersion and slope. Handoff is at frame 1064 every run; the takeoff's physics does not touch the
fixture (the park overwrites the state), so a baked default can be A/B'd in place without the
device-like JSON dance. Hybrid, `AssistDisable` 1, fixed 60 Hz.

Four parkings, all with the lip due 3.2 s after handoff unless noted:

| run | nose | what happened |
|---|---|---|
| `park_ahead2000_c0` | down the line | **took off.** 113 → 523 → 764 cm/s along the line as the peel arrived (t=3.0–3.6) and surfed the pocket at 600–750 for the remaining 16 s, 2.5–4.5 m ahead of the impact, neutral weight. A board pointed the right way in the pocket catches the wave — realistic, and not this spec's case. |
| **`park_ahead2000_nose180`** | up the line (the player's) | before the lip the board drove itself *toward* the peel at 300 cm/s (t=2.6, pocket, `Broken` 0). Collapse t=2.9–3.1: board −104 shoreward while the water was still near 0. Whitewater t=3.15–3.6 (`Broken` 0 → 1): water −134…−196, board *decelerating* −83 → −44; **ratio at the water's peak 0.31**. Spun to 35°/s at t=3.75, stopped by t=4.5, then `behind` as the band drifted on. Same again on the next wave (t=10.5: water −307, board −73). **The A5 baseline.** |
| `park_ahead2000_noseShore` | toward the shore | **caught the whitewater straight**: −600 at the collapse (water −417), then rode the bore shoreward at 200–380 for 6 s tracking the water's own speed (−380/−373, −302/−193, −371/−285), `Broken` 1 throughout. Realistic (a foamie to the beach); the board outruns the water on the bore front by up to 1.5× — slope gravity on a steep bore. |
| `park_behind1500_c-300` | down the line | the aged wake: `Broken` 1 for 1.5 s (age 2.4–3.9 s) with the water at only −15…−43, then `behind`. When the next lip arrived (t=6.5) it took off down the line as row 1. |

What this settles:

- **A1's target is real.** The sampled water velocity under a `Broken` = 1 board is −134…−540 cm/s
  shoreward at the collapse and on the bore front (rows 2–3), quiet only in the aged wake near the
  band's back edge (row 4). No modelled bore speed is needed (owner's question 1, italics accepted).
- **The shove today** is the board's own slide down the face at the collapse (−104 before the water
  moves), not the water carrying it: once in the bore the board *loses* shoreward speed while the
  water gains it. That is the wave-normal damping being slope-gated off (P2.2 A1) — exactly what T3 opens.
- **A3 is not dead** (owner's question 5). The 14-38-37 phone trace's own `board_avz`: the foam run-up
  (t=8.5–9.6, 541–715 cm/s) yaws at 91–110°/s (p50–p90), the slow foam turn (t=3.5–5.0) at 45–72°/s,
  ride max 120°/s on the face. Today's cap at 600 cm/s is 0.83 rad/s = 48°/s on the toward-wave sign
  only; with `BrokenYawCapScale` 0.5 on both signs it is 24°/s at 600 and 72°/s under the knee — it
  bites the run-up (AC2's case) and barely touches paddling-speed turns. The lip itself spins the
  parked board to 35°/s (row 2), under either.
- **Zone reads as designed** through every row: pocket ahead of the impact, whitewater with `Broken`
  ramping over 0.4 s once 2 m behind it, `behind` when the band has drifted past, `flat` shoreward of
  it. The 2 m pocket gap (Status, open) is visible in row 2: the board is 0.4 s into the collapse
  before `Broken` starts.

### M16 — T2–T4 on the fixture and the rides (2026-09-21)

Build: `957553616` + T2–T4 + the service fix. Every A/B here is tick-exact: `-ReplayOverridesAt=0.0`
switching the *control* OFF (`BrokenDampingGate=0,BrokenYawCapScale=1[,BrokenDriveCut=0]`) against the
baked defaults, on the same binary. (On the shortboard slalom that form drifted sub-cm in the *takeoff*,
0.7 s before any override applied — a harness quirk, not the feature; the JSON-form control below was
identical to the cm.)

**The fixture (`park_ahead2000_nose180`, A5).** Against the feed's own water (the front SC's), mean
board/water shoreward ratio over the collapse (ticks with water < −80): control 0.39, gate-only (scale 1)
0.39, scale 4 **0.61**, scale 10 0.73. Two things the gate-only result taught:

1. **At the collapse the slope gate is already open** (the bore front reads slopeSin 0.25–0.5), so
   opening it on `broken` changes nothing there; M10's "slope-gated off" is the aged whitewater sit. What
   limits the shove is the *rate*: 0.06/tick is a 0.3 s time constant against a water transient that
   peaks and decays within 0.5 s. Hence `BrokenDampingRateScale` 4 (0.07 s while broken).
2. **The nose and the tail sit in different water.** Nose up the line, the tail is at the fresh collapse
   (−343) and the nose 2 m into older whitewater (−118); the feed targets the front SC's water and
   converges to it (relVn 73 → 41 → 22 → 17 over four ticks at scale 10), which the tail-side number
   reported as a 0.2 ratio. The metric names its SC (AC1).

The second wave on the same fixture is the player's report verbatim: with the nose up the line the
board is **driven along its nose into the collapse at 450 cm/s** while still in the pocket (`broken`
0 → 0.34), then decelerates over 1.5 s. That is the 2 m pocket gap (Status, open) plus A2: the drive
fires before `broken` ramps.

**The rides**, `CROSSING`-line metrics (60 Hz), control → all three on:

| ride | face (pocket+shoulder) | whitewater |v| mean | rel-along-nose in broken, mean / max | rel gains ≥300/0.5 s in broken | yaw p50 / p90 in the whitewater |
|---|---|---|---|---|---|
| 14-38-37 hybrid (the run-up) | identical to the cm to t=4.10 (445 / 649 mean) | 358 → 146 | 285 / 546 → **36 / 183** | 24 → 3 ticks | 57 / 76 → 25 / 58 °/s |
| 10-48-07 hybrid (the cutback) | identical to t=7.45 (553 / 697) | 341 → 243 | 289 / 577 → 136 / 345 | 12 → 0 | 15 / 40 → 18 / 39 |
| 20-44-07 shortboard slalom (JSON control) | identical to t=8.70 (the stall meets the next bore) | 259 → 237 | 217 / 380 → 123 / 346 | 0 → 0 | — |

A1 alone (14-38-37 `ac2_defaults`) did **not** remove the run-up: the board was carried with the bore
at up to −597 shoreward (the water there) and at t=7.4 still ran 179 → 359 up the line along the nose.
A2 removes it (A2 ships ON). With A1 the board's whitewater motion is the water's — mean relative
along-nose speed 36 — which is the owner's third sentence in P1.1 answered headless.

### M17 — the whole-velocity whitewater damping (2026-09-21, fork + project)

Owner's verdict on the M16 clips: the board barely moves. The parked board's velocity in the bore was
mostly *along its nose* (−150 up the line) with only −60 across; the wave-normal term (FR3: cross-shore
only, by design for the face) cannot touch the rest, and the contact gate halved it as the lip lifted
the board. A board in a bore is a cork: the aerated water takes its whole velocity. Built:

- Fork `PBDRigidsEvolutionGBF.cpp`: `p.Chaos.Solver.WhitewaterDampingRate` — after the wave-normal
  term, `V.xy += (V.xy − WaterVel.xy) × (keep − 1)`, the same `FrDampMul` decay, horizontal only.
  Rate 0 = off, so the face never sees it.
- Project feed: `rate = waveNormalDampingRate × BrokenDampingRateScale × broken × SmoothStep(0.02, 0.15,
  underwater)` — any water contact counts in the foam, only a board thrown clear is exempt. The
  water target blends from the front SC's toward whichever SC's water runs faster shoreward
  (nose −118 / tail −343 at the collapse, M16).
- `BrokenDampingRateScale` 4 → **10**: the board tracks the water fully (ratio 0.77 at the water's
  peak against 0.67 at ×4; carried 7.8 m vs 5.4 m by t=9).

| fixture `park_ahead2000_nose180` | control | M16 (normal only ×4) | M17 ×4 | **M17 ×10** |
|---|---|---|---|---|
| board ÷ water at the water's peak | 0.31 | 0.52 | 0.67 | **0.77** |
| board most shoreward (cm/s) | −104 | −120 | −160 | **−194** |
| cross-shore position at t=5 / 9 / 14 s (cm from the break line) | −96 / −88 / −164 | — | −284 / −544 / −873 | **−334 / −776 / −939** |

**The front is out of reach, and why that is right.** Starting the ramp 1 m behind the impact
(`pocketBehind` 100, `brokenRamp` 200) put the board on the bore front (−333 against water −372, 10.5 m
by t=9). But the 10-48-07 cutback ride spends 1.6 s at 659 cm/s with the modelled impact 1–2 m ahead of
the board and the water under it at **−262 shoreward** — riding tight under the lip and a lip landing
read the same to the data and to the model. So the 2 m margin stays, `Broken` ramps 2–6 m behind the
impact, and a board the lip lands on catches the bore's wake (1.2–1.8 m/s), not its front (3 m/s). That
is the owner's open question ("the moving board in the pocket") answered by measurement: it cannot be
told apart within ±2 m, and the priority order says the pocket ride wins.

**A3 bug found by the identity test.** At the band's edge `broken` reads ~1e-5 (prints 0.00) and the
away-sign cap was fed on `broken > 0` at the full toward-sign value — the lip-snap M7 cutback killer
for a tick. Now the away sign gets `cap / broken` (unlimited as broken → 0). With that, 10-48-07 is
identical to the cm until t=5.9 (its first band-edge graze), pocket/shoulder means unchanged
(553 → 556 / 697 → 697); 14-38-37 pocket/shoulder identical.

| ride, whitewater | control mean |v| / rel-along-nose mean / max | **M17** |
|---|---|---|
| 14-38-37 (run-up) | 355 / 284 / 546 | **254 / −26 / 208** — the water there runs ~196 |
| 10-48-07 (cutback) | 340 / 330 / 584 | **189 / 45 / 480** |

### M18 — the shove's shape: anisotropic in the board's frame (2026-09-21, fork + project)

Owner's device trace `phone-2026-09-21-17-54-27` (hybrid, in `Tests/InputTraces/`): riding down the line
at ~400 with the peel 2.5 m behind, the whitewater arrives from the side and *"the board's forwards
velocity stops and it goes from moving only forwards to only sideways"*. In the trace (the model's read
ported to Python over the phone's own positions): along-line 404 → 139 in 0.75 s, cross-shore 0 → −394
in 0.25 s, then 3 s sideways at 3–4 m/s with the nose still down the line. Its replay stalls at t=3 in
this harness (the headless takeoff), so the event is reproduced on the fixture with the new `-ParkSpeed`:
parked 3.5 m behind the impact (`broken` 0.4), 1.5 m shoreward, nose down the line, 400 cm/s.

Built: the fork's whitewater term splits the relative velocity along the nose (`WhitewaterNoseX/Y`, fed
per tick) and across; across at the full rate, along at rate × `WhitewaterAlongScale`
(`WhitewaterAlongNoseScale`, default 0.2). A zero nose falls back to isotropic.

| overtake fixture `ov2_*`, along-line speed at +0.25 / +0.65 / +0.85 / +1.25 / +2.25 s | shoreward peak |
|---|---|
| control (no whitewater terms): 570 / 823 / 803 / 752 / 710 — outruns the foam | −84 |
| isotropic (M17): **208** / 272 / 213 / 215 / 101 | −440 |
| along 0.4: 335 / 378 / 136 / 214 / 112 | −477 |
| **along 0.2 (shipped): 403 / 407 / 227 / 255 / 99** — 63 % one second after `broken` 0.5 | −440 |

Head-on (the parked nose-up-the-line fixture): ratio at the water's peak 0.92 (M17 0.77), carried 4.2 m
by t=9 s (M17 7.8 — the isotropic term also glued the board's drift along its own length, which is why
it stayed in the band longer) and 9 m by t=14; 0.4 along did not carry it further (−377 vs −424 at t=9).
0.2 ships; the owner's feel decides (T7).

### M19 — the bore band's front edge (2026-09-21)

Owner's device trace `phone-2026-09-21-19-47-52` (hybrid), t=13.4–14.4: drifting in the whitewater wake
27–30 m behind the peel, 15 m shoreward, nose 120–135° from down the line, carried shoreward at 2.5 m/s
— then at t=13.8 the read flips `whitewater(1.00) → flat(0.00)` in one tick and the board goes 240 →
414 → 645 → 688 along its nose in 0.6 s, with the drive cut, the whitewater damping and the yaw cap all
released together onto what is still a steep, fast bore front. The board had crept 23 cm past the
modelled front edge (`−BoreFrontStart − BoreDriftRate·age` = −1535 at age 4.9 s; the board at −1558):
the water carries a board at 230–290 cm/s shoreward, the modelled front drifts at 150, so a board
riding the front sits *on* the edge and noise flips it.

Fix (`UWaveGeometrySubsystem::Sample`, JSON `boreFrontFade` 400): the band extends `BoreFrontFade`
past the front and `Broken` is multiplied by `SmoothStep(front − fade, front, cross)` — 1 inside, 0 at
the outer edge. Rules 1–2's previous-wave test use the same. The back edge (the trough behind the
bore) stays hard. Fixtures: nose-up parked bit-identical (never near the edge); overtake within noise;
the shore-facing board that rides the bore to the beach reads `broken` 1.0 → 0.70 → 0.48 → 0.35 over
ages 7–10 s instead of flipping to `flat` at t≈9 with the drives returning at full strength.

### M20 — the young bore's front, and the along-nose fraction (2026-09-21)

Owner's device trace `phone-2026-09-21-21-37-57` (shortboard), t=15.4–18.2: riding at 757 four metres
ahead of the peel, a hard cutback (shoreward at 442, the nose to −0.35), then turning back down the line
— by which time the peel has passed: 2–3 m behind the impact, 4 m shoreward of the lip's landing,
reaccelerating 344 → 514. The model read `whitewater` 0.21 → 0.98 over the next 0.8 s and the board
went 514 → 178. Its replay is not usable (the takeoff stall), so: the recovery fixture
`-ParkBehind=300 -ParkCross=-400 -ParkNoseDeg=0 -ParkSpeed=450`.

**The band was 8 m wide at age 0.** `BoreFrontStart` 800 + `BoreDriftRate` 150·age was fitted to old
bores (ages 1.2–7.9 s). A lip that landed 0.3–0.5 s ago has run its foam 1–1.5 m shoreward; a board
4 m shoreward of the landing is ahead of the foam, on the low face. The front is now
`min(BoreFrontSpeed·age, BoreFrontStart + BoreDriftRate·age)` — 300 cm/s from the impact (M15 row 3:
−287 / −637 / −954 cm at ages 1 / 2 / 3 s on the water) until it meets the old line at ~5 s, where that
line's numbers came from. Offline on the trace the recovery reads `broken` 0.00–0.13 instead of
0.08–0.47.

| recovery fixture, along-line speed at 0.3 / 0.8 / 1.8 / 3.0 s | |
|---|---|
| old front | 300 / 259 / 164 / 111 — `broken` 0.21 → 1.0 in 0.8 s, carried 11 m shoreward |
| young front, along 0.2 | 347 / 380 / **640** / 465 — reaches the peel's speed, holds ~6.7 m behind the impact at `broken` 0.05–0.2, racing the foam |
| young front, along 0.1 + cut 2 | 365 / 515 / 631 / 421 |

**"Even in the whitewater it shouldn't brake that hard anymore, right?"** It did. At `broken` 1 the
along-nose rate is 0.6 × `WhitewaterAlongNoseScale` per tick: 0.2 = a 0.14 s decay — the forward hold
in M18's table was the `broken` ramp, not the fraction. On the "deep" fixture (`-ParkBehind=800
-ParkCross=-600 -ParkSpeed=500`), along-line speed every 0.25 s:

| | 0 – 3 s |
|---|---|
| 0.2, cut 1 | 495 373 353 314 239 386 355 342 296 196 228 164 |
| 0.05, cut 1 | 495 427 522 468 438 **732 713 728** 622 559 512 415 — surges on the drives at `broken` 0.3–0.5 |
| **0.1, cut 2 (shipped)** | 495 393 360 299 258 386 368 412 373 361 297 224 |

0.05 keeps the momentum but hands the half-broken board back to the drives; so `BrokenDriveCut` goes
1 → 2 (no face drive past `broken` 0.5) and the fraction 0.1 (0.28 s at `broken` 1). Overtake and
head-on fixtures within noise of M18/M19.

**The service fix (T2).** On 14-38-37 the frame a new impact passed the board's `s` read
`flat → whitewater(behind=5006) → flat` — `kb` changes identity at the impact and the previous wave's
bore was no longer tested. `Sample()` now tests the previous wave's band before returning `flat` in
rules 1–2, so an aged bore keeps carrying through the hand-over. It adds whitewater 10–20 m shoreward
of an aged break where the map read flat; M15 row 3 measured the water there still at −100…−300 at ages
6–9 s. **Owner's zone-map look owed (T5).**

## Files

- `Source/GoneSurfing/SharedCalculations.cpp/.h` — `UpdateBrokenGeo()` → `brokenGeo`, `waveZone`,
  `distBehindImpact`, `crossFromImpact` (A4, T2); the superseded foam read `UpdateBrokenAmount()`,
  `brokenAmount`, `brokenSurroundAmount`, the sector counts, the `foam` log (diagnostic only, runs only
  under the flag).
- `Source/GoneSurfing/SurfboardUtils.cpp/.h` — the fork feed: wave-normal damping gate/rate/axis/target
  + the whitewater damping rate and the stronger-water target blend (A1), yaw-rate ceiling per sign
  + `towardSign` (A3, away sign = cap / broken), carve-grip un-fade, `wavenormal` / `yawcap` logs.
- Fork `Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp` —
  `WaveNormalDampingRate/Gate`, **`WhitewaterDampingRate`** (M17), `WaterVelX/Y/Z`, `MaxAngularVelocityZPos/Neg`.
- `Source/GoneSurfing/FluidDynamics.cpp/.h` — `calcBrokenDriveScale()` (A2), `=== YAW HYDROFOIL ===`.
- `Source/GoneSurfing/SurfTuningSubsystem.h` — `Foam*` (diagnostic), `BrokenDriveCut` (A2, on),
  `waveNormalDamping*`, `YawRateCap*`, `CarveGripTurnUnfadeRate`, `ScoreWhitewaterCreditRate`;
  `BrokenDampingGate` + `BrokenDampingRateScale` (A1), `BrokenYawCapScale` (A3).
- `Source/GoneSurfing/WaveGeometrySubsystem.cpp/.h` + `Content/WaveGeometry/<map>.json` — the signal (A4):
  the peel model, `Sample()`, `Broken`, zones ([wave-geometry.md](wave-geometry.md)).
- `Source/GoneSurfing/WaveGeoDebugSubsystem.cpp/.h` — the T0 tool (M14): draw, zone map, camera, self-screenshots, logs.
- `Source/GoneSurfing/WaveHeight.cpp/.h` — tiling, `FrameOffsetPerTileX`, `DeriveTilingFromWaveManager`,
  `-WaveDeriveTiling`.
- `Content/WaveGeometry/<map>.json` (new, T0 fallback) — two `(x, y, frame)` lip-landing measurements, or the
  16-row per-tile table; read once at BeginPlay.
- The wave radar (`specs/wave-radar.md`) — overlay of the three T0 shapes next to the foam footprint it draws.
- `Source/GoneSurfing/ParticleSystemsController.h/.cpp` — `GetWhiteWaterWorldPoints`, `ClusterFoam`,
  `FFoamBreakCluster::Front` (the audio's crash front — a T0 cross-check, not the source).
- `Source/GoneSurfing/InputReplayAutoPilot.cpp/.h` — the parked fixture (`-Park*`, `ParkBoard()`,
  `PARK`/`PARK-TICK` logs) (T1).
- `Tests/InputTraces/synth-parked-neutral.csv` — 20 s of centred weight for the fixture (T1).
- `Tests/InputTraces/phone-2026-09-21-17-54-27.csv` — the owner's device ride behind M18 (the overtake).
- `Tests/InputTraces/phone-2026-09-21-19-47-52.csv` — the owner's device ride behind M19 (the band edge).
- `Tests/InputTraces/phone-2026-09-21-21-37-57.csv` — the owner's device ride behind M20 (the cutback recovery).
- `Tests/InputTraces/` — `phone-2026-09-15-20-51-12` / `-20-50-05` / `-20-02-02` (the report),
  `phone-2026-09-16-20-57-25` (whitewater sit + bore), `phone-2026-09-17-14-38-37` (wrong-way run-up),
  `phone-2026-09-17-10-48-07`, `phone-2026-09-18-10-15-54`, `-09-51-17` (the face set).
- Related specs: `yaw-hydrofoil-flow-direction.md`, `lip-snap-spinout-and-air.md`,
  `wave-interaction-damping-and-redirect.md`, `wave-mesh-data-registration.md`,
  `replay-scheduled-overrides.md`, `surf-audio.md`, `lip-impact.md`, `trick-scoring.md` (FR12 retired),
  `stamina.md`, `carve-grip-via-redirect.md`, `pitch-righting-and-redirect-escape.md`.
