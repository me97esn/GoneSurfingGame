# Spec: First-play pop-up hitch — PSO cache + spray warm-up

## Status
- [x] Diagnosed (2026-08-14)
- [x] Spray PSO warm-up implemented (2026-08-14): `ASprayController` forces a brief low emission on
  the first ride per app launch so the spray material's PSO compiles during the paddle phase instead
  of at the visible pop-up. Compiles clean; interim mitigation.
- [x] Config prerequisites set (2026-08-14): `bShareMaterialShaderCode=True` (DefaultGame.ini) +
  `r.PSOPrecaching`/`Components`/`Resources` (DefaultEngine.ini). The pipeline cache was already
  enabled but inert.
- [x] On-device: warm-up + precaching prerequisites player-validated (2026-08-14) — "works well".
- [ ] POSTPONED (not required for launch): record + bundle a `.upipelinecache` from an on-device play
  session to flatten the hitch entirely (manual; see workflow below). Revisit during Shipping-build pass.
- [ ] Validate PSO precaching behaves on the Vulkan Android path (it may be a no-op or need tuning)

## Symptom

On the **first play after launching the app**, there is a lag/hitch at the exact moment of the
surfer **pop-up** (stand-up). Hitting in-level **Restart** makes subsequent plays smooth through the
pop-up, but fully **relaunching the app** brings the hitch back on the first play.

## Root cause

The hitch is **graphics pipeline state object (PSO) compilation** on the **first draw of the spray
(board-foam) Niagara material**, which first becomes visible at the pop-up:

- Spray emission is speed-gated: zero below ~100 cm/s, ramping in over 100→400 cm/s of horizontal
  board speed (`SprayController.cpp`, `sprayMinSpeed`..`sprayFullGateSpeed`). The board only crosses
  that window as it accelerates onto the wave — i.e. **at takeoff / pop-up**.
- The first frame the spray actually draws, the graphics driver must compile the material's PSO (and,
  for a GPU emitter, allocate/first-dispatch the compute sim). That's a synchronous stall.
- A compiled PSO is **cached for the process lifetime** → in-level Restart reuses it (smooth); an app
  relaunch starts cold (hitch returns). This exactly matches the observed behaviour.

Note the **surfer is not the culprit**: the rider mesh is visible (paddle/cobra) from frame 0, so its
materials compile at level load, not at pop-up. Nothing in the pop-up path loads assets — the pop-up
is an anim-state enum flip. The one material genuinely new on screen at pop-up is the spray.

Secondary, smaller first-play costs at the same moment (documented, not yet addressed): first
evaluation/decompression of the PopUp anim clip, and first rasterization of the 60 pt "…and surf!"
cue glyphs into the Slate font atlas (`RideCueOverlay.cpp`).

## Fix — two pronged

### (a) Spray warm-up (interim, shipped 2026-08-14)

`ASprayController` forces a brief, low emission on the **first ride per app launch** so the spray
PSO compiles early (during the paddle phase, off the critical pop-up frame) instead of at the visible
stand-up. See `bSprayWarmup` / `sprayWarmupDuration` (0.5 s) / `sprayWarmupRate` (40/s) in
`SprayController.h`, and the warm-up block in `ASprayController::Tick`.

- **Once per process**: guarded by a `static bool` — the PSO stays cached after the first ride, so
  restarts skip the warm-up (no repeated visual artifact).
- **Both renderers**: a non-zero warm-up velocity sends particles ballistic then settling, so both the
  ballistic-spray and settled-foam renderers draw and compile their PSOs.
- **Skipped during replay** (replay pushes its own recorded rates).
- **Limitation**: a PSO only compiles when something actually *draws*. The warm-up helps only if the
  board/spray is on-screen during the window (it is, during the intro/paddle). It **relocates** the
  compile to a less-noticeable moment; it does not eliminate it. The robust fix is (b).
- **Caveat**: warm-up rates are visible to `GetSprayOutputs`, so a trace recording that starts within
  the first 0.5 s of the first ride per process would capture the warm-up spray. Rare (once-per-process
  guard) and small.

### (b) Bundled PSO cache (the real fix — manual, on-device)

The project **already enables** the shader pipeline cache (`r.ShaderPipelineCache.Enabled=1` etc. in
DefaultEngine.ini) but was missing two things, now added:

1. `bShareMaterialShaderCode=True` (DefaultGame.ini) — **prerequisite**; without it the pipeline
   cache carries no shader code and stays inert.
2. `r.PSOPrecaching=1` + `r.PSOPrecache.Components=1` + `r.PSOPrecache.Resources=1` (DefaultEngine.ini)
   — automatic runtime precaching that needs no recorded file. Must be validated on Vulkan Android.

**Still required** (cannot be done from C++/CI — needs a device): record a bound-PSO log from a real
play session that includes a pop-up, convert it to a stable cache, and bundle it so it ships and
pre-warms at load.

#### Record → bundle workflow

1. Package a **Development** (or Test) build for Android with the pipeline cache enabled.
2. Launch with PSO logging on so the run writes a `*.rec.upipelinecache`:
   - set `r.ShaderPipelineCache.LogPSO=1` and `r.ShaderPipelineCache.SaveBoundPSOLog=1` for the
     recording run (currently `LogPSO=0` for normal runs).
3. **Play through everything that draws** — critically the wave catch and **pop-up** (so the spray
   material's PSO is recorded), plus carves, wipeouts/ragdoll, replay overlay, tutorial, etc.
4. Pull the recorded log off the device (under the app's `Saved/` dir).
5. Convert it into a stable cache with `ShaderPipelineCacheTool` and place the resulting
   `.upipelinecache` under `Content/PipelineCaches/<Platform>/` (or the project's configured location)
   so the cook includes it.
6. Repackage; on the next cold launch the bundled PSOs pre-compile at load and the first-play pop-up
   hitch is gone.

This is best done as part of the **"Produce & test a Shipping build"** blocker in RELEASE_CHECKLIST.md,
since the cache should be recorded against the shipping content/config.

## Verification

- **Warm-up firing**: watch the first-ride spray debug log (`surf.debug.flags 'spray'`) — a non-zero
  `SpawnRate` should appear during the early paddle phase on the first ride per launch, then the
  normal speed-gated behaviour thereafter.
- **Hitch gone**: on device, `stat unit` / `stat gpu` across the pop-up frame on a **cold** app launch;
  compare against a build without the warm-up / bundled cache. The bundled cache is the one that
  should flatten it entirely.

## Related
- [[flat-face-speed-gain-diagnosis]] and the spray design in `specs/board-spray-particles.md`.
- Ties into the "Produce & test a Shipping build on-device" and profiling items in RELEASE_CHECKLIST.md.
