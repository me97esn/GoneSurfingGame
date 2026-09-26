# Spec: Wave height ↔ water velocity registration (the lip that hits nothing)

## Status
- [x] Investigation complete (2026-07-22): misregistration measured and characterized
- [x] Fix: `calculateWaveVelocity` game path unified onto `CalculateFrameOffsetForPosition` +
  `convertWorldToGridIndices` (per its own TODO). Explicit-tile path (debug viz) kept as-is.
- [x] Re-measured (acceptance PASSED): dh/dt↔vz correlation peak moved **−75 → ~0/+50 cm** and
  strengthened **r 0.35 → 0.54**; crest-peak delta **−200 → −100 cm** median. The residual −100 is
  within physical plausibility for a skewed shoaling wave (velocity max slightly shoreward of the
  crest) — **no compensation knob added**; revisit only if evidence demands.
- [x] Replayed `top-turn`: the board's own column now reads **525–737 cm/s** through the lip-touch
  window (was 60–90 with the band stranded 150–200 cm away). The nose samples the lip's water.
- [ ] Player feel pass + physics regression: this changes `absoluteWaterVelocity` for EVERY consumer
  (waveMassFlow, wavePenetration, relative-velocity lift/thrust/drag) — the whole ride feel may
  shift, likely toward stronger/better-placed wave forces. Re-run the autopilot suite / re-tune if
  needed before trusting old baselines.
- [ ] (Separate, later) lip-impact force — only if correct registration still isn't enough

## The observation (player, 2026-07-22)

`top-turn` autopilot, ~gs 7.6: the nose has redirected up the face and touches the breaking lip —
a moment with visually massive water energy — and the board is completely unaffected.

## Investigation findings (all measured, `wavescan` diagnostic + top-turn headless runs)

### 1. What the board samples at the lip moment
- Water velocity at the front SC: **166–208 cm/s**, almost purely horizontal shoreward, with a
  **small DOWNWARD vz** (−30..−48). The board itself moves 240–265 cm/s — faster than the "lip".
- Player insight that redirected the investigation: face water below a crest moves *upward*
  (orbital flow), so a weak-downward sample cannot be face flow — it's attenuated lip/fall water.

### 2. The lip's energy IS in the data — displaced
Cross-shore scans (`wavescan` flag, ±800 cm through the board, 50 cm steps) show a persistent band
of **400–670 cm/s shoreward+downward water 0.5–3 m SHOREWARD of the board's sample point** at the
lip moment, while the board's own column reads 60–90. Whole-ride front-SC stats: mean 112, max 672
cm/s — the export can clearly represent energetic water.

### 3. Height and velocity fields are MISREGISTERED (~1–4 grid cells, shoreward)
Two independent methods agree:
- **Crest-peak alignment**: on a non-breaking swell the orbital-velocity max must sit at the height
  crest. Measured across ~40 pre-breaking frames (984–1032, amplitude fully built): velocity peak
  **median −200 cm** (shoreward) of the height peak, same side every frame. During breaking it
  grows to −250..−450 (physical jet displacement stacked on top).
- **Kinematic cross-correlation**: at a near-fixed point, dh/dt must track vz. Sweeping the vz
  sampling by −300..+300 cm: correlation peaks at **−50..−100 cm** (r≈0.35) and collapses toward
  positive shifts. (r is modest because the advection term −v·∇h is unmodeled; the peak LOCATION is
  the signal.)
- Registration corrected, the measured fast band (−150..−200 from the board) lands **at the crest —
  exactly where the nose touches the lip**. The "quiet gap" the board samples is likely an artifact.

### 4. Format audit (height vs velocity, same table)
- Same `FWaveUnifiedFrameData` row per frame: `h[]` + `vx/vy/vz[]`, same 17,794-cell grid, size
  hard-validated. Same row-major `getGridIndex`, same clamping, same bilinear, same `bFlipXAxis` in
  both sampling paths → **in-engine consumption is identical**.
- vz correlates positively with dh/dt → velocity axes/signs/orientation are fundamentally right.
  The defect is a **translation**, not a scrambled format.
- **The two world→grid conversions are different code paths**: height uses the full tiled
  `convertWorldToGridIndices` ([WaveHeight.cpp:1175](../Source/GoneSurfing/WaveHeight.cpp#L1175));
  velocity uses a hand-rolled "TEMPORARY: Simplified coordinate conversion for debugging" with an
  explicit `TODO: Re-enable convertWorldToGridIndices`
  ([WaveHeight.cpp:1498-1523](../Source/GoneSurfing/WaveHeight.cpp#L1498)), including a suspicious
  tile offset that uses TileX for the Y component (line ~1517). The offset may live here, in the
  export origins, or both.
- `waterVelocityCoefficient = 2000` (WaveHeight.h) is **dead code** — never read. Velocity is
  consumed raw; magnitudes (40–670) suggest the export already writes ~cm/s.
- Heights are in data units (~16–20 over the swell; world Z comes from the height path's own
  scaling), velocities in ~cm/s — units differ per array but each is consumed consistently.

### 5. Structural limit noted for later (NOT this spec's fix)
A column-velocity field cannot represent airborne water: while the lip is in flight over the board,
its momentum exists in no column the board occupies; it registers where the sheet lands. Correct
registration moves that landing band to the crest (where the board is at the touch moment), which
may be enough. If not, a synthesized lip-impact force (sampling the shoreward band's velocity,
gated on crest proximity) is the follow-up — see the punch-through spec's non-goals.

## Fix plan

1. In `calculateWaveVelocity`, for the game path (tile indices NOT provided): replace the simplified
   conversion with `convertWorldToGridIndices(location, MetadataRow)` per the existing TODO. Keep
   the explicit-tile path untouched (used by the debug visualization, which pre-offsets positions).
   Keep the frame-offset calculation as-is (separate concern).
2. Re-run the `wavescan` measurement (top-turn headless). Acceptance: pre-breaking crest-peak delta
   ≈ 0 (±1 cell) and dh/dt↔vz correlation peaking at shift 0.
3. Any residual = baked into the exports → add a constant sampling-offset knob instead of touching
   exporters; default = measured residual; same acceptance test.
4. Replay top-turn and check the lip moment: front-SC `AbsWaterVel` should now read the fast band
   (several hundred cm/s, shoreward+down) as the nose meets the lip — then evaluate board reaction
   (`wavePenetration` / `waveMassFlow` consume `absoluteWaterVelocity`).

## Tools built for this (kept, flag-gated)
- `surf.debug.flags 'wavescan'`: per-tick cross-shore dump of h + v through each SC (±800 cm,
  50 cm steps), in SharedCalculations.cpp next to the crest scan. Heavy — diagnostics only.
- Analysis recipes (PowerShell one-liners over the log): crest-peak delta per frame; dh/dt↔vz
  shift sweep. See this spec's session (2026-07-22) for the exact commands if re-deriving.

## Related
- [pitch-righting-and-redirect-escape.md](pitch-righting-and-redirect-escape.md) — the punch-through
  fix that got the board riding up to the lip in the first place; its Correction section documents
  the negative servo result and the crest fades.
- specs/wave-crossing-deceleration.md — prior wave-data-geometry work (resolved back axis).
