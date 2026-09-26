# Spec: Scheduled tuning overrides in a trace replay (tick-exact A/B)

## Status
- [x] Built 2026-09-16. `-ReplayOverridesAt=<t> -ReplayOverrides=name=value,...` on the replay
      command line; applied in memory via `USurfTuningSubsystem::SetTransient` on the first tick
      with `PlaybackTime >= t`; restored at `EndPlay`. Log line `REPLAY-OVERRIDE`.
- [x] First use: [broken-wave-no-consequences.md](broken-wave-no-consequences.md) M8 — seven runs,
      one coefficient each, identical to the cm until the scheduled tick.
- [x] PC mouse traces made replayable (see "The mouse-trace fix" — every earlier replay of a PC
      trace was a no-input ride).
- [ ] State-triggered variant (fire on `signedDistanceToCrest` / speed / planing thresholds instead
      of time). Not needed while replays are deterministic; build it the day they are not.
- [ ] Marker column in the trajectory CSV so `surf-telemetry compare` can align windows.

## Problem

Changing a coefficient to fix one event changes the whole ride *before* the event, so under the
new value the situation is never reached and the fix cannot be judged (memory
`ab-video-invalid-when-trajectory-diverges`). Measured 2026-09-16: restoring the pre-damping
force set to test its effect on a t=18 s crest push moved the board 900 cm before t=12, and the
wave then passed it as a swell. The A/B said nothing about t=18.

## Design

Ride the recorded trace under unchanged tuning up to the tick before the event; flip the value(s)
on that tick; everything after differs only because of the flip.

```
$env:TEST_MAP="Surfing_infinite_wave"
$env:EXTRA_ARGS="-ReplayTrace=<trace>.csv -usefixedtimestep -fps=60 -BoardInTests -Board=funboard " +
                "-ReplayOverridesAt=17.2 -ReplayOverrides=yawHydrofoilCoefficient=0,slopeThrustFinRedirect=0"
RunGameAndCollectLogs.ps1 -Argument "torque:SharedCalculations" -TimeoutSeconds 400
```

- **Time is trace time** (t=0 = the handoff, the `Handoff observed — starting playback` log line,
  ~200 ticks after `Replay STARTED`). Same clock as the trace CSV and the trajectory CSV.
- **One argument, no spaces** — `RunGameAndCollectLogs.ps1` splits `EXTRA_ARGS` on spaces; commas
  are fine inside it.
- **Command line, not a CVar**: it has to exist in `BeginPlay`, same reason as `-ReplayTrace`.
- **In memory only.** `SetTransient` writes the property and broadcasts `OnTuningChanged` but never
  sets the dirty flag, so the debounced save cannot push an experiment into
  `Saved/BoardTuning/<id>.json`. (`SetByName` would.) The `FluidDynamics` actors re-read the
  subsystem in `setup()` every tick, so the value is in force from that tick's impulses.
- **Determinism is what makes a time trigger enough.** Three control replays of
  `phone-2026-09-15-20-51-12` (no flags / `torque` / `wavenormal,crossing`) matched to the cm at
  every sampled tick; the seven override runs matched the control exactly up to the scheduled tick.
  `-usefixedtimestep -fps=60` is required for that.

### Reading the result

Compare the runs from the scheduled tick on. The useful readouts, in order:

1. The trajectory CSV (`Saved/Tests/latest/<trace>.csv`): `vx/vy/z` per 0.05 s.
2. `surf.debug.flags 'torque'` on `SharedCalculations`: the per-category force budget, `Fin` =
   into the wave (+X), summed over both SC actors, gravity once (memory
   `wavemass-thrust-unbounded-drive`). Align by counting `TORQUE ... cat=TOTAL` lines of one SC from
   the handoff line, 60 per second.
3. `wavenormal` on `SurfboardUtils` for the fork's damping (gate, boardVn, waterVn), `crossing` for
   `distToCrest`, `wVel`, `slopeSin`.

Never judge on the single event alone once a candidate is chosen — whole-ride aggregates on both
boards, per `FR-C` in the broken-wave spec.

### The mouse-trace fix (same commit)

PC traces record `source=mouse` and put the **per-frame mouse delta in pixels** in `stick_x/y`;
the replayer read those as an absolute ±1 offset, so every PC-trace replay before 2026-09-16 was a
no-input ride down the line (this is why the broken-wave spec's first replay "diverged by t≈16" —
it had diverged by t=1). `AInputReplayAutoPilot::ApplyAtTime` now decodes `mouse` rows from the
recorded `weight_front/right` columns by inverting the pawn's offset→weight map. Those values are
post-cap and post-assist, so **replay PC traces with `AssistDisable=1`** in
`Saved/TuningOverrides.json` or the assist is applied twice.

**`-ReplayUseWeights`** does the same for any source. The raw-tilt path (`bUseCurrentTiltParams`)
decodes the phone's tilt angles with the PC pawn's neutral gravity and mapping, which does not
reproduce the phone's lean (measured 2026-09-16: a hard shoreward turn on the phone was a straight
ride on PC). For physics A/B, drive from the recorded weights — that is what the board received.
Keep the raw-tilt path for re-tuning the tilt mapping itself.

## Files

- `Source/GoneSurfing/InputReplayAutoPilot.h/.cpp` — parse (`BeginPlay`), apply (`Tick`, before
  `ApplyAtTime`), restore (`EndPlay`); mouse decode in `ApplyAtTime`.
- `Source/GoneSurfing/SurfTuningSubsystem.h/.cpp` — `SetTransient`.
- Related: [input-trace-replay.md](input-trace-replay.md),
  [trace-trajectory-comparison.md](trace-trajectory-comparison.md), `Tools/mcp/README.md`
  (`surf-live` is the interactive, non-tick-exact alternative).
