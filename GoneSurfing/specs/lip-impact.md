# Spec: Synthesized lip impact (the airborne lip finally hits the nose)

## Status
- [x] Spec drafted (2026-07-22)
- [x] Phase 1 implemented (default OFF: `LipImpactCoefficient = 0`); probe iterations changed the
  design in two measured ways (below)
- [x] Probed at coefficient 0.2:
  - **`top-turn`: WORKS** — 129 impact ticks across the ride, jetSpeed mean 503 / max 741, force
    mean ~36k / max ~108k, shoreward+down at the nose arm. The mechanism is validated end-to-end.
  - **`hard_turn` 6.7 s (the motivating moment): CANNOT fire, and no sampling ever will** — a 4×3
    probe fan (out to 375 cm shoreward, ±150 along-crest) found nothing above ~120 cm/s. At that
    early-curl instant the sheet is ENTIRELY airborne; the data holds no fast water anywhere near
    the board until it lands (later, further down-line). This is the representational limit in its
    pure form. A sampling-free **kinematic jet fallback** (magnitude from phase speed × steepness,
    direction shoreward+down) is the only way to cover it — deferred, own decision.
- [x] Design changes from probing: probe pattern is a **fan** ({0, 0.5, 1, 1.5}×offset shoreward ×
  {−1, 0, +1}×offset along-crest — the wave PEELS, so the landed water is often displaced along the
  crest; and offset 0 included because the nose can sit IN the band). `LipImpactCrestRange`
  default 300.
- [x] Player-tuned and baked as defaults (2026-07-22): `LipImpactCoefficient = 1.0` (F = jetSpeed²),
  `LipImpactMaxForce = 1,000,000` (~10× board weight cap) — the lip hits HARD by design.
- [ ] Re-baseline snapshots (deliberately deferred by the player — baselines currently predate both
  the blend-1 default and the lip impact; every suite run will show drift until re-Approved)
- [ ] (Deferred, own decision) kinematic jet fallback for the fully-airborne early-curl case

## Motivation — the representational limit this closes

A column-velocity field stores one velocity per (x, y): water that is AIRBORNE above the board (the
curling/pitching lip) exists in no column the board occupies — its momentum registers only where the
sheet LANDS, shoreward of the crest. Measured consequences (2026-07-22, blend-1 per-actor sampling):

- `top-turn` lip touch: nose happened to sit in the LANDING band → real push (~65k shoreward, ~16k
  down). Works because the water had already come down where the nose was.
- `hard_turn_towards_the_wave` 6.7 s: nose pokes into the curl EARLIER in the breaking cycle — the
  sheet is still overhead. Water at the nose reads only 225–319 cm/s; total lip forces ~20k (~1/5
  board weight). The visual says "axed by the lip"; the physics says "light spray".

No sampling or coefficient tuning fixes this — the input data at the nose genuinely lacks the jet.
The fix is to synthesize the impact from data the sim DOES have: the landing band a couple of cells
shoreward carries the jet's true velocity (500–700 cm/s, shoreward+downward — wavescan-measured).

## Design (phase 1 — nose actor only)

Computed in the nose FluidDynamics actor's force pass (`ESide::VE_Nose`, previously empty):

1. **Detect nose-under-the-curl** (all gates multiply):
   - **Crest proximity**: `crestGate = 1 − |frontSC.signedDistanceToCrest| / LipImpactCrestRange`
     (clamped 0..1). The curl hangs at/over the crest line.
   - **Breaking section**: `slopeGate = smoothstep(LipImpactMinSlopeSin, +0.08, boardWideSlopeSin)`
     — excludes unbroken swell AND (mostly) flat whitewater zones.
   - **A jet is actually landing**: probe 3 points shoreward of the nose at
     `{0.5, 1.0, 1.5} × LipImpactBandOffset` along `−resolvedWaveBackDirection`, same
     frame/rotation machinery as all sampling; take the fastest. Require
     `jetSpeed > LipImpactMinJetSpeed` with a smoothstep onset (×1.3). The band's speed IS the
     evidence of a lip overhead — no separate "is it breaking" heuristic needed.
2. **Force**: momentum-flux model `F = LipImpactCoefficient × jetSpeed² × gates`, capped at
   `LipImpactMaxForce`, **direction = the sampled jet's own direction** (shoreward + downward for a
   falling sheet — honest to the data, no synthetic down vector).
3. **Application**: through the wave-mass option-C path — linear at the CoM-axis nose arm +
   pitch-kept torque, roll/yaw stripped (`applyWaveMassForceAsImpulse(force, dt, 1.0)`). The lip
   slams the nose DOWN (forward arm × downward force = pitch) without injecting roll/yaw chaos.
   Registered in the torque budget as `lipImpact`.

Self-limiting where it should be: when the nose itself reaches the landing band (top-turn case), the
probes ahead read past-the-band water (slower) while the regular flow drag takes over — the two
mechanisms hand off rather than double-count. Not zero overlap; watch it in the probe.

## Knobs (Tuning|WaveMass, all live-tunable)

| knob | default | meaning |
|---|---|---|
| `LipImpactCoefficient` | **0 = OFF** | F = coef × jetSpeed²; 0.2 ≈ 72k at a 600 cm/s jet (probe start) |
| `LipImpactBandOffset` | 150 cm | probe spacing anchor ({75, 150, 225} shoreward of the nose) |
| `LipImpactMinJetSpeed` | 350 cm/s | band speed below this = no curl overhead |
| `LipImpactMinSlopeSin` | 0.30 | breaking-section gate |
| `LipImpactCrestRange` | 200 cm | crest-proximity gate width |
| `LipImpactMaxForce` | 150000 | hard cap (~1.5× board weight) |

Debug: `surf.debug.flags 'lip'` logs jetSpeed, gates, and the applied force per tick on the nose actor.

## Risks
- **Whitewater false positives**: broken sections carry 500+ cm/s water; the slope + crest gates are
  the guard. If the board gets phantom-slammed riding whitewater, tighten `LipImpactMinSlopeSin` or
  add an "on the face side" sign requirement on `signedDistanceToCrest`.
- **Double-count with flow drag** at the landing band (see self-limiting note) — check the probe's
  combined `lipImpact + waveMassFlow` against plausibility (≲ board weight for a shoulder-high wave).
- Trajectory shifts once defaulted → snapshot re-baseline required.

## Related
- [wave-data-velocity-registration.md](wave-data-velocity-registration.md) — documented this limit.
- [per-actor-water-velocity.md](per-actor-water-velocity.md) — the sampling groundwork; measured the
  band and the two contact scenarios.
- specs/wave-mass-drag-torque-decoupling.md — the option-C application path reused here.
