# Spec: Rails up to the event — a reproducible A/B fixture from a recorded ride

## Status
- [x] Spec drafted (2026-09-22)
- [x] `-RailsUntil=<trace t>` — rails release at a chosen row instead of the last one
- [x] Replay input playback resumes at the release time, not at trace t=0
- [x] Controls enable on the release tick (no `ControlDelayAfterAutoPilot` gap)
- [x] **All five ACs met on `phone-2026-09-22-20-26-02` at `-RailsUntil=3.00`** (2026-09-22) —
      see "Validation".

## Validation (2026-09-22, fish, `-RailsUntil=3.00`)

Released on row 179 at trace t=2.992. `HANDOFF DELTA pos=0.00 cm rot=0.00 deg`.

- **AC1** — two identical runs: max position difference **0.10 cm** over the whole ride, which is
  the trajectory CSV's one-decimal print precision. Deterministic.
- **AC2** — `HANDOFF STATE` pos=(5576.5, 2920.1, 299.0) vel=(-75.9, 433.1, -42.5) waveFrame=946,
  exactly the trace's row 179. The stamp is a recorded state, not an interpolated one.
- **AC3** — the recorded failure reproduces. Down-the-line speed 412 -> 49 -> 4 cm/s (recording:
  414 -> 38 -> -70); climb reverses +320 -> -408 (recording +305 -> -459); slip peaks **133 deg**
  (recording 142). Velocity leading a near-fixed nose — the recorded signature, not the yaw spin
  the fixture-less replay produced. Nose submersion reads **0.000 at t=4.09-4.24**.
- **AC4** — the A/B discriminates, and **reverses** the result the fixture-less replay gave. The
  same three overrides that showed "no change" on the diverged ride are all load-bearing on the
  real one (window t=3.0-5.5, override scheduled at the release tick):

  | run | min down-line | max slip | down-line @4.5 | up-face @4.5 |
  |---|---:|---:|---:|---:|
  | control | **4** | **133 deg** | 68 | -395 |
  | `waveNormalDampingRate=0` | 265 | 30 deg | 345 | **+235 (still climbing)** |
  | `PlaningRedirectCrestFade=0,CarveGripCrestFade=0` | 207 | 28 deg | 291 | +115 |
  | `WaveCarryRedirectRate=0` | 178 | 73 deg | 216 | +77 |
  | `CarveGripRate` 4 -> 16 | 99 | 65 deg | 187 | +56 |

  The wave-normal damping (toward the water's velocity) dominates; the crest fades are close
  behind. **Attribution, not a prescription** — each is a single run of a single event, and
  `waveNormalDampingRate=0` is known to make the board glide off the wave and stop dead on flat
  water, which is why its slope gate exists.
- **AC5** — `-RailsUntil` is inert without `-RailsTrace` (warns and disables); with `-RailsTrace`
  alone the release row defaults to the last row, i.e. today's behaviour.

### Gotcha

Ending a turn while one of these runs is in its early game phase gets the `-game` process killed by
the `Stop` hook and the run silently produces no trajectory CSV. Stay in-turn until the run flushes.

## Problem

`-ReplayTrace=<phone trace>` cannot A/B anything today, because the replay does not ride the
recording — it rides a *different* ride that happens to receive the same inputs.

Measured on `phone-2026-09-22-20-26-02` (the fish, a turn up the face that ended with the board
sliding backwards):

| | the recording | the replay of it |
|---|---|---|
| speed entering the turn | 340–430 cm/s | 700–830 cm/s |
| how it went wrong | velocity slid 142° off a fixed nose | the board yawed round, velocity followed |

The cause is [headless-takeoff-is-physics](deterministic-ride-handoff.md): `StartRails` refuses to
run in a test/replay run, so the board catches the wave on live physics and arrives at the turn in a
state the recording never had. The owner's report is the operative one — *"I can't reproduce this
behaviour with less back weight. Reproducing something is really difficult."* A fixture that starts
from the recorded state is the only way to study an event that happened once.

Three A/B runs on that replay (`waveNormalDampingRate=0`, `WaveCarryRedirectRate=0`,
`PlaningRedirectCrestFade=0,CarveGripCrestFade=0`, each scheduled at t=3.40) all returned "no
change" — a result that says nothing, because the ride under test was not the ride in question.

## Insight — the rails already are the fixture

[deterministic-ride-handoff.md](deterministic-ride-handoff.md) built exactly this machinery for the
intro, and its central claim generalises:

> Pose + linear velocity + angular velocity + wave frame is the complete state vector for this
> system: the board's rigid-body state, plus the field it moves through. Pin all of it and the
> physics has nothing left to disagree about.

`StartRails` / `ApplyRailsAtTime` / `FinishRails` already: drive the board kinematically along a
recorded pose track (`ETeleportType::None`, so Chaos derives V and W), keep the whole force pipeline
*computing* while suppressing only force **application** (`SurfRails::AreForcesSuppressed`), pin the
wave clock per tick, and on release stamp pose + recorded linear/angular velocity and let the wave
free-run. `-RailsTrace=<path>` already exists specifically to "drive the rails against a known trace"
headlessly.

What is missing is only a **release point**: the rails always run to the trace's last row, which
makes the whole ride kinematic and leaves no physics to test.

## Design

One new flag: **`-RailsUntil=<trace t>`**.

The board is carried on the recording's own pose track from BeginPlay to trace time `t`, then
released into physics with that row's state stamped. Everything before `t` is the recorded ride *by
construction*, bit-identical across runs; everything after is physics under the tuning being tested.

```
$env:TEST_MAP="Surfing_infinite_wave"
$env:EXTRA_ARGS="-ReplayTrace=<trace>.csv -ReplayUseWeights " +
                "-RailsTrace=<the same trace>.csv -RailsUntil=3.20 " +
                "-usefixedtimestep -fps=60 -BoardInTests -Board=fish " +
                "-ReplayOverridesAt=3.20 -ReplayOverrides=CarveGripRate=0"
RunGameAndCollectLogs.ps1 -Argument "crossing:SharedCalculations" -TimeoutSeconds 420
```

Set `RailsUntil` a few tenths *before* the event so the physics has time to settle onto the state
before the thing under test happens, and point `ReplayOverridesAt` at the same instant.

### Decisions

- **Release on a recorded row, not an interpolated one.** `-RailsUntil=t` releases at the last row
  with `row.t <= t`. Seating and stamping the same row means the release state is exactly a state
  the phone recorded — no interpolation error in the one place the whole fixture rests on. The log
  reports the actual release time.
- **`FinishRails` stamps the release row**, not `ReplayRows.Last()`. Today the two are the same;
  with `RailsUntil` they are not, and stamping the last row would fling the board to the end of the
  ride.
- **Controls enable on the release tick** when the rails came from `-RailsTrace` *and* a replay is
  driving weight (`bExternalWeightOverride`). Otherwise `ControlDelayAfterAutoPilot` (1.0 s) leaves
  the board running physics with no input through the second that matters.
- **Input playback resumes at the release time.** `AInputReplayAutoPilot` sets `PlaybackTime` to the
  pawn's release time at handoff instead of 0, so the weights the board gets are the weights the
  phone was applying at that instant.
- **One tick of skew is accepted.** The replay autopilot is a tick prerequisite of the pawn, so it
  ticks *before* the pawn releases the rails and only sees the release on the following tick. The
  board therefore runs one frame (16.7 ms at fixed 60) on the pre-release weight. This is identical
  in every run, so it does not affect an A/B; it is documented rather than engineered away.
- **Scope: dev fixture only.** `-RailsUntil` is command-line, has no UPROPERTY, and does nothing
  without `-RailsTrace`. The shipped intro path is untouched.

## Acceptance criteria

- **AC1 — Deterministic.** Two runs with identical arguments produce trajectory CSVs matching to the
  cm at every sampled tick from the release tick on.
- **AC2 — The fixture starts where the recording was.** `HANDOFF DELTA` is ~0, and the trajectory
  CSV's first post-release row matches the trace's row at the release time in position (< 5 cm),
  velocity (< 10 cm/s) and yaw (< 1°).
- **AC3 — The event reproduces.** With `-RailsUntil` set a few tenths before the stall on
  `phone-2026-09-22-20-26-02`, the replay reproduces the *recorded* failure signature — velocity
  rotating off a roughly fixed nose (slip > 90°), not a yaw spin.
- **AC4 — A/B discriminates.** With the fixture in place, at least one single-coefficient override
  at the release tick changes the outcome measurably. (If none does, that is now a real result about
  the physics rather than an artefact of the fixture.)
- **AC5 — No regression to the shipped intro.** A run with `-RailsTrace` and no `-RailsUntil`
  behaves exactly as today (rails to the last row); the playable level is unaffected.

## Related

- [deterministic-ride-handoff.md](deterministic-ride-handoff.md) — the rails driver this reuses
- [replay-scheduled-overrides.md](replay-scheduled-overrides.md) — the other half of the A/B: flip
  one coefficient on one tick. The two compose; this spec is what makes that one trustworthy on a
  phone recording.
- [on-device-ride-replay.md](on-device-ride-replay.md) — the kinematic replay this must not disturb
