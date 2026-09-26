# Spec: Fix Tilt Axis Swap in Landscape Pose

> ## ⚠️ Superseded in part (2026-08-20)
>
> The fix below picked `Seed = +Z` believing device +Z is the screen normal. **It is not.**
> Sensor capture ([sensor-probe.md](sensor-probe.md)) measured device **-X** as the screen
> normal; **+Z is the phone's in-plane vertical axis** (screen top-to-bottom).
>
> Consequence: `|Fwd| = |cos(tilt-from-flat)|`, so the basis degenerated *exactly at
> vertical* — the play pose — and flipped sign across it:
>
> | pose | \|Fwd\| with +Z | \|Fwd\| with -X |
> |---|---|---|
> | flat | 1.000 | 0.018 |
> | upright | **0.048** | 0.999 |
> | reclined | **0.390** | 0.921 |
>
> Near-degenerate, the stray `g.y` (~0.03) dominates the remainder of `Fwd`, swinging it
> toward ±Y and `Right` toward ∓X — the pitch and roll axes swap, so pitching the phone
> drives sideways weight. Reported from playtest 2026-08-20.
>
> Seed is now **`+X`**. With `N = (-cos t, 0, sin t)` for tilt-from-flat `t`:
>
> | seed | Fwd | behaviour |
> |---|---|---|
> | `+Z` (old) | `sign(cos t)·(sin t, 0, cos t)` | **flips at vertical** — the bug |
> | `+X` | `(sin t, 0, cos t)` | consistent, matches old below-vertical |
> | `-X` | `−(sin t, 0, cos t)` | consistent but **inverted** |
>
> `-X` was tried first and inverted pitch on playtest. The mistake was checking
> sign-preservation against the captured *reclined* pose only — which is past vertical,
> precisely where the old seed had already flipped, so it "verified" the broken sign.
> **Below vertical is the reference**, because that is where the old behaviour was correct.
> `+X` therefore keeps `bInvertTiltPitch` / `bInvertTiltRoll` at their playtested defaults
> and leaves the gyro's `MeasuredYawPolarity` as measured.
> **The "degenerate pose is phone-flat, unreachable while playing" reasoning in the Root
> Cause section was correct in intent but only became true with this change.**

## Bug Description
On Android in landscape orientation, the phone's tilt axes are swapped relative to the board's: **pitching the phone rolls the board, rolling the phone pitches the board.** The intended mapping (phone pitch → `amountInFront`, phone roll → `amountToTheRight`) only holds when the calibration heuristic happens to pick the "right" device axis as "forward" — which it doesn't in normal landscape gameplay poses.

## Current Behavior
At handoff (`bPlayerControlsEnabled` false → true), [SurfboardPawn.cpp:1087-1118](../Source/GoneSurfing/SurfboardPawn.cpp) (pre-fix) derives `TiltForwardAxis` by:

1. Iterating `{+X, +Y, +Z}` device axes.
2. Picking the one with the *smallest* `|dot(axis, NeutralGravity_norm)|` (i.e. most perpendicular to gravity).
3. Projecting onto the gravity-perpendicular plane; that becomes `TiltForwardAxis`.
4. `TiltRightAxis = TiltForwardAxis × N`.

For a typical left-landscape neutral pose — `gravity ≈ (-0.72, 0.06, 0.69)` from input trace [phone-2026-06-07-10-43-37.csv](../Tests/InputTraces/phone-2026-06-07-10-43-37.csv):

- `|dot(N, +X)| = 0.72`
- `|dot(N, +Y)| = 0.06` ← picked
- `|dot(N, +Z)| = 0.69`

So `TiltForwardAxis ≈ +Y`. But device-frame +Y in landscape is the **screen's left-right axis** (long edge of phone), not "forward." Specifically:
- Player rotates phone around its long axis (banking left/right = player roll) → rotation about device +Y → gravity components in X and Z change → `RollSin = dot(G, TiltRightAxis)` reads it → registered as roll. ✓
- Player rotates phone around its short axis (tipping nose-up/nose-down = player pitch) → rotation about device +X → gravity components in Y and Z change → `PitchSin = dot(G, TiltForwardAxis ≈ +Y)` reads it → **registered as pitch in the WRONG axis** — actually this is the player's roll motion.

Wait — re-check: a rotation about device-X axis changes G.Y and G.Z. So `PitchSin = G.Y` *does* change. But the **physical motion** that rotates about device-X in landscape is the player's roll (banking), not pitch. So `PitchSin` tracks player-roll. And `RollSin = dot(G, +Y × N)` ≈ tracks G in the X-Z plane → tracks player-pitch.

Net effect: phone pitch → `amountToTheRight`, phone roll → `amountInFront`. Confirmed by playtest.

## Expected Behavior
Phone pitch (player perspective: nose up/down) drives `amountInFront`. Phone roll (player perspective: bank left/right) drives `amountToTheRight`. Independent of how the device is gripped, as long as the screen is visible (i.e. not laid flat).

## Root Cause
The seed-picking heuristic optimizes for a numerical property (`|Fwd_raw| ≈ 1`) instead of a semantic one (the seed should consistently track "out of the screen toward the player"). In landscape, the device's +Z axis (screen normal) does have a moderate component along gravity (≈0.69 in the recorded pose) — but **not enough to be numerically fragile**. The picker rejected +Z anyway because +Y was even more perpendicular to gravity, and that's the wrong "more."

The original code comment (pre-fix) misnamed the failing pose: it claimed +Z degenerates "when the phone is held upright," but upright-portrait has `G ≈ (0, -1, 0)` and `+Z` is perpendicular to gravity (`|Fwd_raw| = 1`). The actual degenerate pose is **phone laid flat** — screen-normal parallel to gravity. That pose is unreachable in surfing because the player can't see the screen.

## Fix
Replace the seed picker with a fixed `Seed = +Z` (screen normal toward player's face):

```cpp
if (bTiltCalibrated)
{
    const FVector N = NeutralGravity.GetSafeNormal();
    const FVector Seed(0.0f, 0.0f, 1.0f);
    const FVector Fwd = Seed - FVector::DotProduct(Seed, N) * N;
    TiltForwardAxis = Fwd.GetSafeNormal();
    TiltRightAxis   = FVector::CrossProduct(TiltForwardAxis, N).GetSafeNormal();
}
```

Why +Z (and not -Z):
- Preserves the existing sign convention from [SurfboardPawn.cpp:905-906](../Source/GoneSurfing/SurfboardPawn.cpp): `PitchSin > 0 → phone tipped forward (top of screen away from player)`. -Z would flip every PitchSin/RollSin sign and force flipping the `bInvertTiltPitch` / `bInvertTiltRoll` defaults too.
- Equivalent semantically — both choices land on the same gravity-perpendicular line, just opposite signs.

## Sign Convention (Restated for Reference)
With `TiltForwardAxis = horizontalize(+Z)` and `TiltRightAxis = TiltForwardAxis × N`:
- **PitchSin > 0** → phone tipped forward (top of screen tipped away from player). With `bInvertTiltPitch = true` (default), this writes negative `Offset.Y` → `amountInFront < 0.5` (weight back).
- **RollSin > 0** → phone tipped right (right edge of screen toward floor). With `bInvertTiltRoll = false` (default), this writes positive `Offset.X` → `amountToTheRight > 0.5` (weight right).

## Acceptance Criteria

### AC1: Pitch on phone → pitch on board
Calibrate in normal landscape pose. Tilt the phone forward (top of screen away from face) by ~15°. Within ~50 ms, `amountInFront < 0.5` (weight goes back, given `bInvertTiltPitch = true`). Tilt back by 15° → `amountInFront > 0.5`.

### AC2: Roll on phone → roll on board
Same calibration. Bank the phone right (right edge of screen toward floor) by ~15°. Within ~50 ms, `amountToTheRight > 0.5`. Bank left → `amountToTheRight < 0.5`.

### AC3: Cross-axis isolation
Pure pitch motion on phone moves `amountInFront` and leaves `amountToTheRight` within deadzone of 0.5. Pure roll motion moves `amountToTheRight` and leaves `amountInFront` within deadzone of 0.5.

### AC4: Landscape orientation invariance
AC1–AC3 hold for both left-landscape (right edge up) and right-landscape (left edge up). Confirmed by re-calibrating from each pose.

### AC5: Portrait still works
AC1–AC3 hold when device is held in portrait too (legacy case — not the gameplay target, but should not regress). In portrait, `NeutralGravity ≈ (0, -1, 0)`, `+Z` is fully perpendicular to gravity → `TiltForwardAxis = +Z`. PitchSin tracks G.Z (rotation around device +X = portrait-frame pitch). RollSin tracks dot(G, +Z × (0,-1,0)) = dot(G, +X) → portrait-frame roll. Correct.

## Test Cases

### TC1: Replay [phone-2026-06-07-10-43-37.csv](../Tests/InputTraces/phone-2026-06-07-10-43-37.csv) post-fix
The trace records raw `tilt_pitch_deg` / `tilt_roll_deg` deltas pre-deadzone (see [InputReplayAutoPilot.cpp](../Source/GoneSurfing/InputReplayAutoPilot.cpp)). The replay re-runs the deadzone/scale pipeline on the PC side. With the fix, the *recorded gravity-derived* PitchDeg/RollDeg values themselves don't change (the trace was recorded with the OLD buggy axes — those values are frozen), so this trace replays **identically** to pre-fix behavior. The fix only affects gameplay/recording from this point forward.

### TC2: Fresh on-device playtest
Calibrate, tilt forward — board nose should dip (or rise, depending on `bInvertTiltPitch`). Tilt right — right rail should engage. Compare against pre-fix behavior on the same device pose to confirm the swap is gone.

### TC3: New trace for regression baseline
Record a fresh trace after the fix. Pull via `adb pull` (see [memory: pull input traces from phone](../../.claude/memory/reference_pull_input_traces_from_phone.md)) and store under [Tests/InputTraces/](../Tests/InputTraces/) for future replay comparison.

## Status
- [x] Root cause identified
- [x] Fix landed in [SurfboardPawn.cpp:1072-1091](../Source/GoneSurfing/SurfboardPawn.cpp)
- [ ] Verified on device (post-build playtest)
- [ ] New regression trace recorded

## Related Files
- [SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp) — `UpdateTiltWeight()` and the calibration block in `UpdatePlayerControlState`.
- [SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h) — `TiltForwardAxis`, `TiltRightAxis` declarations.
- [InputReplayAutoPilot.cpp](../Source/GoneSurfing/InputReplayAutoPilot.cpp) — replays from CSV; not affected by this fix (replays raw recorded PitchDeg/RollDeg).
- [surfing-controls.md](surfing-controls.md) — parent spec; Phase 6 (Android tilt) Open Question #9 explicitly flagged axis decomposition for verification, which this spec resolves.
- [input-trace-replay.md](input-trace-replay.md) — recording / replay infrastructure.
