# Spec: Board shadow grounding — cast a shadow that lands where the board looks like it is

## Status

**Built and running 2026-08-29. The machinery is verified; the premise is not.** `ShadowController.h/.cpp`
is the feature, plus a spawn hook in `ASurfboardUtils::BeginPlay`. Builds clean, initializes on
`Surfing_infinite_wave` (`board=surfboard14_should_stop_7, 2 proxies, 20 hull points`), and a headless
run's trajectory is unaffected.

- [x] FR1–FR7 — proxy caster, clearance measurement, clamp, smoothing, no force writes,
      drop-in placement (now self-spawning, so there is nothing to place)
- [x] **The rider proxy is gone (2026-09-02), and that is a deliberate narrowing of the feature.**
      User testing caught the surfer visibly duplicated — both copies drawn — on a frame where the
      offset was large. The board's proxy earns that risk because its shadow lands on the water and
      is the whole point; the rider's did not, because his shadow lands on the *board*, where there
      is no grounding error to correct. Only static meshes are proxied now; the rider casts his own
      shadow from his own mesh. See FR5.
- [x] **Proxies verified present and tracking**, via `ShadowMode 2`: the ghost rider renders
      at the board, in the right pose, following the real one. The leader-pose and transform paths work.
- [x] **`shadowWaterlineZBias = -40` verified**, via `surf.shadow.debugdraw 1`: the cyan contact grid
      lies on the visible water. The one constant nobody could derive is right.
- [x] **Fins excluded from the anchor** — they won the lowest-point search on ~100% of early ticks and
      dragged the measurement 10 cm under. The anchor is now rails and bottom actors, which is what
      the eye judges contact by.
- [ ] **The premise does not hold on this ride, and this is the finding that matters.** Measured over
      a full autopilot run on `Surfing_infinite_wave`, the lowest hull point's clearance above the
      rendered water is **median −9.6 cm** (p25 −17.8, p75 −0.4, p90 +13.6, max +48.0, min −75.6).
      The board is *not* levitating; it rides with its rail at or just under the waterline. So the
      correction computes ~0 most of the time and the feature, though correct, has almost nothing to
      do here. Before tuning anything, settle where the 30 cm came from — a different map
      (`Boards_on_flat_water`), a pre-takeoff moment, or the shadow's own displacement being read as
      height.
- [x] **Shadows confirmed casting and landing on the water** (player screenshot, 2026-08-29). The
      earlier "no shadow anywhere" reading came from headless captures, not from the real setup.
- [~] **Proxy drawing as a visible second board — fixed twice, second fix unconfirmed by a player.**
      `SetHiddenInGame(true)` + `bCastHiddenShadow` (the folklore recipe) left the board duplicated.
      `bRenderInMainPass = false` fixed the *rider* but not the *board*. The board mesh is
      Nanite-enabled, and **Nanite geometry does not honour `bRenderInMainPass`** — so the proxy is
      now hidden by every mechanism at once (`SetProxyVisible`), plus `SetForceDisableNanite(true)`
      on static proxies, which a shadow-only caster should never have been on anyway.
      Verified headlessly as an A/B at a forced 60 cm offset: `ShadowMode 2` shows the
      ghost, default hides it. **Needs a pop-up eye-test to confirm** — see below.
- [ ] **The duplicate is only visible during pop-up, and that is not a coincidence.** The offset
      pushes the proxy *down*, so once the board is riding the copy is under opaque water and
      invisible whether or not it is drawing. It can only be seen while the board is high enough that
      `boardHeight − offset > 0` — pop-up and air. Any headless test that misses that window will
      report a clean pass on a broken build; two of them did. Test at pop-up, not mid-ride.
- [ ] **`shadowMaxGroundingOffset = 60` is now defensible but untested.** Riding clearance never
      exceeded +48 cm in the measured run, so 60 fully corrects everything observed while riding and
      only clamps on genuine air. Revisit once real air exists to look at.
- [x] **SUPERSEDED 2026-09-02 — the board's proxy does cast**, and lands its shadow in the right
      place; confirmed in play. The finding below stands only as the reason `ShadowMode` defaults to
      0 in the shipped tuning, which is now out of step with what actually works. Anything read below
      about the rider's proxy is history: it no longer exists.
- [ ] **The board's proxy does not cast; the rider's does (editor, 2026-08-31).** `ShadowMode`
      therefore now defaults to **0** — the board casts its own shadow and the proxies stay silent.
      The suspect is `SetForceDisableNanite(true)` on static proxies: the board mesh is Nanite, the
      rider is skeletal and never was, and the rider worked throughout. Either the Nanite fallback
      mesh is too thin to cast or hidden-shadow casting does not survive that path.
      **This costs nothing today** — `ShadowContactZBias` is 90, so the grounding correction is off
      and the proxy was contributing nothing while breaking the board's shadow. It does mean the
      whole proxy path is dormant until someone wants grounding back.
      **Not diagnosable headlessly**: `Screenshot.ps1` frames do not show shadows at all here, so this
      needs eyes in the editor. Test by flipping `ShadowMode` to 1 in the tuning HUD.
- [ ] **New tunables need a full rebuild and editor restart to appear in the HUD.** The `Tuning|Shadow`
      rows went missing once because the editor was running a DLL built before the properties existed
      — Live Coding patches code but cannot add UPROPERTY reflection data. If a tuning row is absent,
      suspect a stale build before suspecting the HUD.
- [ ] Air case unvalidated. Needs a ride with a real launch to confirm the shadow separates.
- [ ] **No shadows at all on Android — one real cause fixed, still not casting.** Device test 2026-08-30 found no
      shadow at `ShadowMode` 0, 1 *or* 2. Mode 0 is the board casting its own shadow with the proxies
      silent, which is indistinguishable from never having built this controller, so the blank is
      project-level mobile lighting.

      **Device log, pulled 2026-08-30** (Pixel 10, `adb pull` of the on-device
      `Saved/Logs/GoneSurfing.log`). Two findings, and both kill a standing theory:

      1. **Shadows are NOT switched off on device.** `sg.ShadowQuality` resolves to 1, giving
         `r.ShadowQuality=1`, `r.Shadow.CSM.MaxCascades=1`, `r.Shadow.DistanceScale=0.7`. Low, but
         on  so the `ShadowQuality@0` theory is dead and pinning `sg.ShadowQuality` was pointless
         (that edit has been reverted).
      2. **The project's `[Android_Vulkan]` device profile is never used.** The phone selects
         `Android_Default` -> `Android_Mid` -> `Android`. Proof in the log: `r.MobileContentScaleFactor`
         comes out **1.0** (from `[Android]`), and `r.ScreenPercentage=60` is never applied. **Every
         optimisation in that block is dead**  the 60% resolution, AA off, AO off, dithered LOD,
         software occlusion. That is a standalone perf bug, unrelated to shadows, and worth more than
         this feature is.

      Also confirmed from the device log: the controller itself works on Android  2 proxies,
      20 hull points, and all three `ShadowMode` values were exercised.

      **Ruled out: the stationary-light CVar pair.** `r.Mobile.EnableStaticAndCSMShadowReceivers` and
      `r.Mobile.AllowDistanceFieldShadows` are both `False` here, but they only gate CSM from
      *Stationary* directional lights. The level's sun is **Movable** (confirmed 2026-08-30).

      **Eliminated on device, 2026-08-30.** In order, each ruled out by evidence rather than argument:

      1. *This feature.* `ShadowMode 0` is the board casting its own shadow with proxies silent —
         indistinguishable from no controller. No shadow.
      2. *Shadows switched off.* `r.ShadowQuality` is **1** on device, not 0.
      3. *The stationary-light CVar pair.* The sun is **Movable**, so they do not apply. And for
         movable receivers CSM runs through `FNoLightMapPolicy`, which
         `MobileEnableStaticAndCSMShadowReceivers` does not gate — `LightMapRendering.cpp:168`.
      4. *The light itself.* Cast Dynamic Shadows on, `Dynamic Shadow Distance MovableLight` 20000,
         `Num Dynamic Shadow Cascades` 3.
      5. *Mobile CSM shader culling.* Ran the device live with
         `r.Mobile.Shadow.CSMShaderCullingMethod 0` — all primitives receive CSM — rode a wave and
         captured the frame. Still no shadow.

      **One real cause found and fixed 2026-08-31 — `r.Mobile.EnableStaticAndCSMShadowReceivers=False` while
      `r.AllowStaticLighting=True`.** Found by A/B against a stock UE test project on the same phone,
      which shadows fine.

      `FMobileDirectionalLightAndCSMPolicy::ShouldCompilePermutation` (`LightMapRendering.cpp:160`)
      gates the mobile directional-light CSM shader on:

      ```
      (!IsStaticLightingAllowed() || MobileEnableStaticAndCSMShadowReceivers())
      ```

      This project satisfied **neither** half — static lighting allowed (the level ships `_BuiltData`)
      *and* the receivers flag off. So the permutation was never compiled, and **nothing on the device
      could receive a shadow from anything**, which is precisely the symptom. The test project passes
      the first half with `r.AllowStaticLighting=0`.

      **Second device round, 2026-08-31 — bisect ran, cause still not found.** Deployed and tested
      directly (see `DeployAndroid.bat`). Everything below was verified on device, not argued:

      - **All 25 mobile/shadow renderer settings neutralised to engine defaults** — confirmed absent
        from the device log — and still no shadow. None of them is the cause; all restored.
      - **`r.AllowStaticLighting=False`** (matching the working project) — no shadow. Reverted, since
        it costs this level its baked lighting and buys nothing.
      - **RHI, shader platform, feature level identical** to the working project:
        OpenGL ES 3.2, `GLSL_ES3_1_ANDROID`, `ES3_1`.
      - **One directional light only** (`DirectionalLight_0`), so mobile's one-CSM-light limit is not
        picking the wrong one.
      - **Plugins**: only `GeometryCacheAbcFile` differs. **Engine**: same fork GUID.

      What survives from all this is one genuine bug, kept:
      `r.Mobile.EnableStaticAndCSMShadowReceivers` was False while static lighting was allowed, which
      sent every movable primitive down the no-CSM branch at `MobileBasePass.cpp:420`. Necessary, not
      sufficient.

      **Next, and it has to be a truly clean repro:** an EMPTY level in this project with a cube, a
      floor and a movable directional light. The cube test so far lived in `Surfing_infinite_wave`, so
      the level has never actually been eliminated — only its materials. Note `-map=` plus
      `-iterativecooking` silently fails to add a map (`Boards_on_flat_water` was passed to the cooker
      and still came back "Failed to load package"), so cook the test level with iterative OFF.

      **Baseline CONFIRMED 2026-09-01.** `TestUnrealProject` was retested on the same phone with the
      casting cube, the receiving cube AND the directional light all **Movable** — shadows cast. So
      mobile CSM from movable geometry genuinely works on this device: not a platform limitation, not
      an engine-fork limitation, and the project-vs-project comparison is sound after all. The
      difference is inside GoneSurfing.

      **The one test still not run** is the mirror of it: a bare level in GoneSurfing with a movable
      cube, floor and directional light. `TestShadowLevel` exists and deploys, but renders black,
      because the project's `GoneSurfingGameMode` spawns a surfboard pawn with no wave to sit on.
      **No level edit is needed to fix that** — override the game mode in the launch URL:

      ```
      /Game/levels/TestShadowLevel?game=/Script/Engine.GameModeBase
      ```

      which spawns a stock `DefaultPawn` with an ordinary camera at the PlayerStart. Put that in the
      device `UECommandLine.txt` in place of the map argument.

      **DECISIVE, 2026-09-01: it is project-level, not the wave level.** `TestShadowLevel` — a bare
      level, movable cube on a movable floor with a movable directional light, which casts a clean
      shadow in the editor — was deployed and run on the phone. It renders, and there is **no shadow**.
      Same device, same movable setup that the stock `TestUnrealProject` shadows correctly.

      So `Surfing_infinite_wave` is exonerated, and so is every material and actor in it. Config is
      exonerated too: a full section-by-section diff of both `DefaultEngine.ini` files leaves only
      `r.AllowStaticLighting` (tested, no effect), `r.RayTracing` and `r.SkinCache.CompileShaders`
      (both desktop-only), and neither project has any platform-specific config override.

      **The remaining structural difference is the binary.** `TestUnrealProject` is Blueprint-only —
      no `Source/` — so it runs the engine's prebuilt `UnrealGame`. GoneSurfing has a C++ module and
      compiles its own Android binary from the fork. Same engine GUID and the fork has no local
      modifications under `Engine/Source/Runtime/Renderer` or `Engine/Shaders`, so the source should
      match — but the two are not running the same executable.

      **Next test, and it is cheap:** add any empty C++ class to `TestUnrealProject`, so it too
      compiles its own binary, and redeploy. If its shadows disappear, the cause is in the
      locally-built binary rather than in this project's content or config, and the search moves to
      the fork's build configuration.

      **The binary is NOT the cause (2026-09-01).** `TestUnrealProject` was converted to a C++ project
      — a stock blank-module `Source/` tree, so it compiles its own Android binary from this fork
      exactly as GoneSurfing does — rebuilt, deployed and run on the same phone. **It still casts a
      shadow.** Confirmed numerically rather than by eye: floor brightness dips to 138–149 in a
      diagonal band against ~195 either side, while control rows above and below show only a smooth
      166→194 ambient gradient. (Screenshot kept at
      `Saved/Screenshots/testproject-cpp-shadow.png`; player independently verified it in GIMP.)

      So the engine fork, the local toolchain, the C++ module path and the deploy pipeline are all
      exonerated. Two projects, same engine, same compiler, same phone, same deploy — one shadows and
      one does not.

      **That leaves configuration, and the space is bounded**: the working project sets **10** entries
      under `[/Script/Engine.RendererSettings]`; GoneSurfing sets **189**. The earlier bisect only
      neutralised 25 of them, chosen by mobile/shadow-sounding names — 154 were never tested, and the
      culprit need not look shadow-related.

      **Do not bisect by stripping GoneSurfing's settings**: removing all 179 extras fails the cook
      outright (several are cook-critical — Nanite, virtual textures, lightmap support). Bisect in the
      other direction instead — inject GoneSurfing's renderer settings into the working test project,
      which cooks in ~3 minutes against GoneSurfing's ~15, and halve from there.

### Tuning

The four coefficients live in `USurfTuningSubsystem` under **`Tuning|Shadow`** — live in the tuning
HUD (filter "shadow"), A/B-able through `Saved/TuningOverrides.json`, and pulled onto the actor at the
top of every tick, so an edit lands on the same frame. They are on the subsystem rather than the actor
because the controller is auto-spawned and its UPROPERTYs cannot be reached in the editor at all.

**Which way does the shadow move?** The caster is only ever pushed *down*, and a lower caster throws
its shadow closer to straight beneath the board. So more drop = shadow pulled in toward the board;
less drop = it drifts further out. `ShadowContactZBias` is the drop knob and **its sign is inverted
relative to the effect: negative pulls the shadow in.**

| Tunable | Default | Does |
|---|---|---|
| `ShadowMode` | **0** | **0** = the board casts its own shadow, proxies silent (no grounding, but an ordinary visible mesh casting an ordinary shadow). **1** = the proxy casts, board silenced. **2** = as 1 but the proxies are drawn. Applied live, so it can be flipped on a phone. |
| `ShadowContactZBias` | 90 | **The "shadow sits too far from the board" knob.** Negative pulls it in toward directly beneath; positive lets it drift out. **At the shipped 90 the correction is off** — see below. |
| `ShadowMaxGroundingOffset` | 60 | The declared line between rendering offset and real air. Raise it alongside a negative contact bias, or riding saturates the clamp and air stops reading as air. |
| `ShadowWaterlineZBias` | −40 | Wave data → rendered mesh surface. A **calibration**, verified by eye against the drawn contact plane — tune the contact bias instead unless the waterline itself moved. |
| `ShadowOffsetSmoothingSeconds` | 0.12 | Smoothing time constant on the drop. |

Verified end to end 2026-08-29: `{"ShadowContactZBias": -60, "ShadowMaxGroundingOffset": 120}` in
`TuningOverrides.json` loaded clean (0 stale keys) and produced a ~69 cm drop against a ~68 cm
clearance.

**The shipped default turns the correction off, deliberately** — because the sun was steepened
instead (commit `1f0b8a8e4`, see Alternatives), which removed the visible error at its source.
`ShadowContactZBias` was promoted to **90** from the PC overrides on 2026-08-30. Measured clearance
runs about
−10..+48 cm, so subtracting 90 drives it below zero on effectively every tick, the clamp floors the
drop at 0, and the caster sits exactly on the board — the shadow is cast from the board's true
position and none of the grounding machinery does anything. That is consistent with the finding that
the board is not levitating, and it means what ships today is "a correct shadow" rather than "a
grounded shadow". The clamp, the cap, the smoothing and the air-separation behaviour are all dormant,
waiting on a case where the board really is riding high. Anyone re-tuning this should start by going
**negative**, not by nudging 90.

### Debug console

Toggles, not coefficients — these stay CVars because the tuning subsystem carries floats only.

| CVar | Does |
|---|---|
| `surf.shadow.enable 0` | Build no proxies at all. Read once at init, so it must be set at launch. Use `ShadowMode 0` instead unless you need the proxies gone entirely. |
| `surf.shadow.debugdraw 1` | Draw the contact plane, the lowest hull point, and where it grounds to. |
| `surf.debug.flags 'shadow'` | Per-tick log of clearance, target, offset, and which hull actor is lowest. Needs a matching `surf.debug.actors` token (`Shadow`). |

## Overview

The board renders roughly 30 cm above the water mesh while riding. That offset is deliberate — the
board planes submerged relative to the wave *data*, the render meshes were dropped ~40 cm to match,
and the remaining lift is what keeps the hull from disappearing inside the mesh on the frames where
it dips. It is a visual compensation, not a physical fact.

Turning on `Cast Shadow` exposes it. The shadow lands at horizontal displacement
`clearance × tan(sun angle from vertical)` from directly-underneath, which reads as the board
levitating. So the feature that would make air time legible is the same feature that makes ordinary
riding look wrong.

Both halves matter and they pull opposite ways:

- While riding, the ~30 cm is an artifact and the shadow must be **planted**.
- When the board actually launches, the gap is real and the shadow must **separate**.
- When the board is pitched back, the tail is in contact and the nose is genuinely high, so the
  nose's shadow must fall further away than the tail's — within a single ride, in one frame.

## Objective

Cast a real, shape-accurate shadow from the board and rider that reads as planted while riding,
separates when the board leaves the water, and preserves nose-vs-tail displacement under pitch —
without moving the board, the water meshes, or anything in the force pipeline.

## Requirements

### FR1 — The caster is a hidden proxy, not the board

The visible board does not cast. A duplicate of each of its static meshes is created at
runtime, `Hidden in Game` + `Cast Hidden Shadow`, no collision. (The rider is out of scope — FR5.) It carries the source component's
exact rotation, scale, mesh and materials, so the shadow silhouette is the real silhouette. Only
its world Z differs from the source.

This is what keeps the fix invisible: the board itself never moves, so nothing already tuned about
how it looks or rides changes.

### FR2 — Clearance is measured against the *rendered* surface

Per tick, measure how far the board's lowest hull point sits above the water **mesh**:

```
surface   = calculateWaveLocationAndNormalAuto(boardCentre)      // wave DATA surface, world space
planeZ    = surface.Z + shadowWaterlineZBias + shadowContactZBias // + rendered-mesh offset
clearance = min over hull points p of ( p.Z - tangentPlaneZAt(p.XY) )
```

Two things are load-bearing here:

- **The data surface is not the mesh surface.** `sprayWaterlineZBias = -40` is the existing constant
  for that conversion and this reuses its value rather than inventing a second one.
- **The tangent plane, not per-point sampling.** One wave lookup at the board centre plus the
  surface normal, then each hull point evaluated against that plane. The wave is locally flat over a
  2–3 m board, this is the same board-wide-plane approximation spray already uses for foam settling,
  and it costs one lookup instead of twenty.

Hull points are the board's `AFluidDynamics` sampler actors, **minus the fins**. They are hand-placed
*on* the hull, so they are honest contact candidates, and using them means "shadow is planted" and
"physics says contact" are anchored to the same geometry. Any constant offset between the samplers
and the visible hull is absorbed by `shadowContactZBias`.

Fins are excluded because they are "fully submerged by construction"
(`AFluidDynamics::calcThrustForce` says so and skips their wetting gates): they hang below the hull,
so they win the lowest-point search on essentially every tick and anchor the shadow to something that
never touches the surface. Measured: including them pulled the median clearance from −9.6 cm to
−13.0 cm and made the left-back *fin* the anchor instead of the left-back *rail*.

### FR3 — A clamp separates artifact lift from real air

```
offset = clamp(clearance, 0, shadowMaxGroundingOffset)
```

This single expression is the whole behaviour:

| situation | clearance | offset | result |
|---|---|---|---|
| riding | ~30 cm | 30 cm (full) | contact point's shadow lands directly beneath it — **planted** |
| airborne | 200 cm | 60 cm (capped) | proxy still floats 140 cm — **shadow separates** |
| hull dips into the mesh | negative | 0 | shadow sits directly under the board, never pushed up through the water |

`shadowMaxGroundingOffset` is the only real tuning number, and it is a *declaration*: above this,
a gap is real air and gets rendered honestly. Set it just above the highest clearance seen while
riding. The ambiguity band is narrow because real air is metres and the artifact is decimetres.

**The correction is light-independent.** Lowering the caster by its true clearance grounds the
shadow for any sun direction — there is no light angle anywhere in this. Sun angle only scales how
visible the *residual* error is (`residual × tan(angle from vertical)`), so a steeper sun forgives a
worse cap and a low dramatic sun demands a better one.

### FR4 — Smoothed, framerate-independently

`offset` is exponentially smoothed toward its target with time constant
`shadowOffsetSmoothingSeconds` (`alpha = 1 - exp(-dt / tau)`). Per-tick clearance is noisy — it is a
min over ~20 points against sampled wave data — and without smoothing the clamp engaging at takeoff
pops. A shadow may lag the board by a frame or two invisibly; it may not jitter.

If the surface sample fails, hold the previous offset rather than collapsing to 0. The proxy keeps
following the board; a stale correction is better than a shadow that snaps.

### FR5 — Nose-vs-tail comes free; the rider is left alone

The proxy is translated in Z, not re-posed. **A rigid translation preserves every relative height
difference in the mesh**, so a tail-down board still throws its nose shadow further away than its
tail shadow, at exactly the right ratio. There is no per-vertex work and no separate code path for
the pitch case — anchoring on the *lowest* hull point (the `min` in FR2) is what makes the wetted
end the one that plants.

**The rider is not proxied at all** (changed 2026-09-02; he was, via a `SetLeaderPoseComponent`
follower). Two reasons, and the second is the one that decided it:

- **There is nothing to correct.** His shadow falls almost entirely on the *board*, not on the water,
  so the grounding offset — which exists to fix where a shadow lands on the water surface — buys him
  nothing.
- **A hidden skeletal duplicate that fails to hide is a second visible surfer.** Seen once in user
  testing, at a large offset: two riders, both drawn. The board has the same failure mode but pays
  for the risk with a shadow that visibly needs the correction; the rider was paying it for free.

So he keeps `Cast Shadow` on his own mesh, casting from his true position, and this controller never
touches him. Only `UStaticMeshComponent`s are proxied.

The known consequence: whatever sliver of the rider's shadow *does* reach the water is uncorrected,
displaced by the same clearance the board's used to be. It is a sliver at the board's edge, next to a
correctly-planted board shadow — a much smaller error than the one it replaces.

### FR6 — Purely cosmetic

The controller reads transforms and wave height. It applies no force, writes nothing any physics
actor reads, and holds no state that survives a tick beyond the smoothed offset. It therefore needs
no `surf.autopilots` gate — unlike controls-gated features, it cannot corrupt a snapshot CSV
([[test-runs-enable-player-controls]]).

### FR7 — Nothing to place, nothing to wire

`AShadowController` needs no per-level configuration — with `Surfboard` unset it resolves the board
from the first `AFluidDynamics` in the world — so requiring a drag into every `.umap` would be
friction with nothing behind it. `ASurfboardUtils::BeginPlay` spawns one (transient, so it never
dirties the level) if the world has none. Placing one by hand suppresses the spawn, because a placed
actor carries hand-tuned properties the auto-spawn must not shadow. One per world: a multi-board
level needs them placed explicitly.

With `bTakeOverSourceShadows` (default on) it also switches `Cast Shadow` off on the components it
proxies, so the feature cannot double-shadow whether or not the board's own casting was enabled.

### NFR1 — One wave lookup per tick

Same order of cost as the spray controller, which does seven plus a grid.

### NFR2 — No lighting side effects

Proxies set `bAffectDynamicIndirectLighting = false` and `bAffectDistanceFieldLighting = false`. A
hidden mesh parked inside the water surface would otherwise leak into Lumen GI and distance fields.

## Acceptance criteria

- **Given** the board riding a wave at its normal ~30 cm clearance, **when** the sun is low,
  **then** the shadow sits directly beneath the board with no visible gap.
- **Given** the board launching off the lip, **when** it is 2 m clear of the water, **then** the
  shadow is visibly displaced from the board and tracks back toward it on landing.
- **Given** the board pitched tail-down with the tail in contact, **then** the tail's shadow is
  planted and the nose's shadow is displaced further away.
- **Given** the board dipping momentarily inside the water mesh, **then** the shadow does not jump
  or invert.
- **Given** any of the above, **then** the recorded trajectory CSV is bit-identical to a run with
  the controller absent.

## Test cases

1. **Screenshot pair at `-Phone`** ([[screenshot-the-game-on-desktop]]) — one riding frame, one air
   frame. This is the whole eye-test; the riding frame sets `shadowContactZBias` and the air frame
   sets `shadowMaxGroundingOffset`.
2. **`surf.debug.flags 'shadow'`** with a non-empty `surf.debug.actors`
   ([[rungame-arg-comma-splitting]]) — logs raw clearance, clamped target, smoothed offset, surface
   Z and which hull point is lowest. Read the riding clearance off this before touching the cap.
3. **Regression**: run any existing snapshot test with the controller in the level and confirm the
   CSV is unchanged (FR6). Baselines are stale ([[stale-baselines-block-regression-sweep]]) but the
   *delta against a control run in the same session* is still meaningful.

## Gotchas

- **No single flag hides a shadow proxy.** `bHiddenInGame` is a *visibility* flag the editor viewport
  ignores; `bRenderInMainPass` is a *rendering* flag **Nanite ignores** — and the surfboard mesh is
  Nanite. Each one hid one of the two proxies and left the other drawing. `SetProxyVisible` applies
  both plus `bRenderInDepthPass`, and static proxies get `SetForceDisableNanite(true)`.
  `bCastHiddenShadow` keeps the shadow alive through the hidden path.
- **A dropped proxy hides itself under the water, which will fake a passing test.** The offset only
  ever pushes the caster down, so mid-ride the copy is below an opaque surface and invisible whether
  or not it is being drawn. Reproduce and verify at **pop-up**, where the board is high enough for
  the copy to clear the water. Forcing the offset with `ShadowContactZBias` does not substitute — a
  bigger offset buries the proxy deeper, it does not expose it.
- **A missing shadow and a misplaced shadow are different bugs.** `WaveMaterial` is `BLEND_Opaque`
  and shadows do land on the water (confirmed on the player's machine). Headless captures showed no
  shadow at all, which was misleading — do not diagnose lighting from a `Screenshot.ps1` frame.
  `ShadowMode 2` sidesteps the question entirely by showing where the caster *is*.
- **`waveHeightAndNormal`'s absolute return is not world Z** ([[waveheight-return-not-world-z]]) —
  but `calculateWaveLocationAndNormalAuto` *is* world space, and is what this uses, same as spray.
- **Two offsets are in play and they are easy to confuse**: the render meshes sit ~40 cm below the
  wave data ([[wave-render-meshes-lowered-40cm]]), and the board rides submerged relative to that
  data ([[planing-depth-equilibrium-fix]]). `shadowWaterlineZBias` is the first one only. The
  second one is what gets *measured* per tick, and must not be baked into a constant.

## Alternatives considered

- **Steepen the sun. → ADOPTED, and it is what actually fixed the visible problem.** The error is
  `clearance × tan(angle from vertical)`, so rotating the light toward vertical shrinks it for free.
  Done in commit `1f0b8a8e4` ("rotated the light to make the shadow appear more close under the board
  instead of far down"), and it is *why* `ShadowContactZBias` then went to 90: with the sun steepened
  the shadow already lands close under the board, so the grounding correction had nothing left to do
  and would have over-corrected. Cost: shorter, less dramatic shadows, and it does not survive
  wanting a low sun later — which is exactly when the dormant grounding machinery becomes useful
  again.
- **A projected blob or decal.** Exact placement control, but it throws away the shape and pitch
  fidelity that makes air legible — and decals do not land on translucent water anyway.
- **Offset the visible board instead of a proxy.** The same clamped correction applied to a
  render-offset node above the mesh, rider and camera anchor. The board itself would then look
  planted, not just its shadow, with no duplicated geometry. Genuinely interesting: a *clamped
  dynamic* correction is smarter than the fixed ~30 cm lift, since it only pushes down as far as the
  current clearance allows. Rejected for the first pass because it moves things that are already
  tuned; revisit if the proxy version proves the measurement is trustworthy.
- **Raise the water render meshes.** Trades this artifact for the one the 40 cm drop was introduced
  to fix.

## Implementation details

Suggestions, not prescriptions.

- `ASprayController` is the closest template and was followed deliberately: `Surfboard` +
  `sharedCalculations` properties, retry-until-wired `InitializeIfNeeded`, `TG_PostPhysics` tick so
  the board transform is final for the frame, and an identical `SampleSurface` helper.
- Proxy setup, all of which matters: `SetHiddenInGame(true)` + `SetCastHiddenShadow(true)` +
  `SetCastShadow(true)`, `NoCollision`, `bReceivesDecals = false`, `Movable`, and the two lighting
  flags from NFR2. Materials are copied from the source so masked geometry casts a masked shadow.
- Component discovery walks the surfboard actor and proxies every `UStaticMeshComponent` with a mesh,
  skipping anything already `bHiddenInGame` — those are not drawn, so they should not cast either —
  and skipping `USkeletalMeshComponent`s outright (FR5), which it logs so the rider's absence from
  the proxy list reads as a decision rather than a discovery failure.

## Related

- `specs/board-spray-particles.md` — where `sprayWaterlineZBias` comes from and what it means
- `specs/planing-depth-equilibrium.md` — why the board rides submerged relative to the wave data
- `specs/wave-data-velocity-registration.md` — the wave-data frames this samples against
