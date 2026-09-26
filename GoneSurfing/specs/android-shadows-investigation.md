# Investigation: no dynamic shadows on Android

**Status: RESOLVED (2026-09-01) — confirmed on `Surfing_infinite_wave` itself, not just the repro.** Mobile resolves
`r.Shadow.CSM.MaxCascades=1` at `sg.ShadowQuality=1`, and the levels' directional lights ask for a
20000–40000 unit shadow distance. One 1024² cascade stretched over that range is ~27 cm per texel,
so a metre-scale caster's shadow is 3–4 texels and dies under depth bias. Verified by fixing it two
independent ways on the phone, in GoneSurfing's own build, and then on the wave level by the
player watching a live ride. **What remains is choosing which fix to bake in** — see "The fix".

Read "Corrections" before trusting anything older than this.

This is a *separate problem* from `specs/board-shadow-grounding.md`. That spec describes the
shadow-proxy feature; this file is the Android blank. The feature was eliminated as a cause early on
and is not involved.

---

## The problem

On a Pixel 10, GoneSurfing casts **no dynamic shadows at all** — not from the board, not from the
rider, not from a bare cube. Desktop/PIE shadows fine. A stock `TestUnrealProject` on the *same
phone* casts shadows correctly, including with everything Movable.

---

## Established, with evidence

| Fact | How it was established |
|---|---|
| Reference project casts dynamic mobile shadows on this phone | `TestUnrealProject`, all actors Movable, measured: floor dips 138–149 vs ~195 surroundings in a diagonal band; player confirmed in GIMP |
| It still shadows after becoming a **C++ project** | Added a stock blank module so it compiles its own Android binary from this fork; shadow survived |
| It still shadows with **GoneSurfing's 189 renderer settings injected** | Whole `[/Script/Engine.RendererSettings]` block copied in; shadow band intact (179 vs 219 baseline; frame ~25 brighter because GoneSurfing disables auto-exposure/bloom) |
| `TestShadowLevel`'s two Static actors are now **Movable** | 2026-09-01; device log no longer reports `LIGHTING NEEDS TO BE REBUILT`. Did **not** restore shadows |
| The "pure black" frame was a **buried camera**, not a lighting failure | The level has no PlayerStart, so the pawn spawns at (0,0,0) — inside the floor slab (z 10→110). Desktop renders it equally black |
| **Minimal repro is now valid and reproduces the bug** | With the camera placed via `BugItGo`: desktop casts a clean shadow, the phone casts none. Floor across the whole expected shadow zone is flat 238–240 |
| The desktop shadow is a **real dynamic shadow**, not an unbuilt preview | Survives `r.Shadow.UnbuiltPreviewInGame 0` unchanged (floor drops to 9–26 in the band) |
| **The level is the variable, not the project or the binary** | GoneSurfing's `TestShadowLevel` cooked into the known-good `TestShadowCpp` and run on the phone: **no shadow** (zone spread 2). Minutes earlier, that same binary rendered its *own* level with a clean shadow (dip to 168 vs 216 surround) |
| The level's light and caster are configured correctly | `obj dump` of the light: `Mobility=Movable`, `CastShadows=True`, `CastDynamicShadows=True`, `bAffectsWorld=True`, `Intensity=5.0`, `DynamicShadowCascades=4`, **`DynamicShadowDistanceMovableLight=40000`**. Caster `Mobility=Movable` |
| Device clamps CSM hard | Resolved on device: **`r.Shadow.CSM.MaxCascades=1`**, `r.Shadow.MaxCSMResolution=1024`, `r.Shadow.DistanceScale=0.7` — the `sg.ShadowQuality=1` scalability row wins over the earlier 3 / 2048 / 1.0 |
| **Two independent runtime fixes restore the shadow on device** | `r.Shadow.DistanceScale 0.05` alone → SHADOW. `r.Shadow.CSM.MaxCascades 4` alone → SHADOW. Both re-verified in **GoneSurfing's own build** on its own `TestShadowLevel` |
| `Surfing_infinite_wave` has the same shape of problem | `obj dump` of its light: `Mobility=Movable`, `CastShadows=True`, `CastDynamicShadows=True`, **`DynamicShadowDistanceMovableLight=20000`**, `DynamicShadowCascades=3`, `CascadeDistributionExponent=3.0`. 20000 × 0.7 into one 1024² cascade = **13.7 cm per texel** |

---

## Eliminated (all verified on device, not argued)

- **The shadow-proxy feature.** `ShadowMode 0` = board casts its own shadow, proxies silent —
  indistinguishable from the controller never existing. No shadow.
- **`r.ShadowQuality`** — resolves to **1** on device, not 0.
- **The directional light** in `Surfing_infinite_wave` — Movable, Cast Dynamic Shadows on,
  `Dynamic Shadow Distance MovableLight` 20000, `Num Dynamic Shadow Cascades` 3, only one in the level.
- **Mobile CSM shader culling** — `r.Mobile.Shadow.CSMShaderCullingMethod 0`, **re-run 2026-09-01 on a
  valid build with a working camera**. Verified applied in the log; the frame came back md5-identical
  to the run without it. This is the run that counts (see correction 3). Note its sibling
  `r.Mobile.EnableMovableLightCSMShaderCulling` is **read-only** and cooked to 1 — it logs
  `is read only!` and cannot be tested this way; changing it needs an ini edit + recook.
- **`r.AllowStaticLighting=False`** — tried, no effect, reverted (it would cost the level its baked
  lighting for nothing).
- **All 25 mobile/shadow renderer settings** — neutralised to engine defaults, confirmed absent from
  the device log, still no shadow. Restored.
- **The whole `RendererSettings` block** — see table above.
- **The engine fork, toolchain, C++ module path, deploy pipeline** — see table above.
- **RHI / shader platform / feature level** — identical in both projects: OpenGL ES 3.2,
  `GLSL_ES3_1_ANDROID`, `ES3_1`.
- **Plugins** (differ only by `GeometryCacheAbcFile`), **engine GUID** (same fork).
- **Materials** — `WaveMaterial` is `MSM_DefaultLit` + `BLEND_Opaque`, not Single Layer Water.
- **This project's C++** — nothing touches shadow CVars except `GridLODActor.cpp:299`
  (`SetCastShadow(false)` on water cells, wave level only, casting not receiving).

## One real bug found and KEPT

`r.Mobile.EnableStaticAndCSMShadowReceivers` was `False` while `r.AllowStaticLighting=True`. In
`MobileBasePass.cpp:420` that sends every **movable** primitive down the no-CSM branch — and every
receiver here is movable. Now `True` (the engine default). **Necessary but not sufficient**; it did
not fix the blank on its own.

---

## Corrections — claims I made that turned out wrong

1. **"SOLVED — it's the shader permutation gate."** It was a real bug (above) but not the cause.
2. **"No object in the level casts a shadow, suspect a Static sun."** Wrong; derived from headless
   `Screenshot.ps1` frames, which **do not show shadows in this project at all**. Never diagnose
   lighting from a headless capture here.
3. **"CSM shader culling is eliminated."** The first culling test ran on a build where the CSM
   permutation didn't exist, so it proved nothing. Re-run later on a valid build — that one counts.
4. **"The bare level proves it's project-level."** **The most important correction.** The two
   `TestShadowLevel`s (one per project) are *different assets*, 15633 vs 13289 bytes. I treated them
   as equivalent without checking. GoneSurfing's renders black even in the working project, so the
   level is a variable, not a control.
5. **"The test project proves mobile CSM works here."** Initially its actors were **not Movable** —
   the player caught this, not me. Later re-verified properly with everything Movable.

6. **"`TestShadowLevel` is pure black, so the sun contributes nothing."** Wrong, and it wasted a
   whole round. The level has **no PlayerStart**, so the pawn spawns at world origin, which is
   *inside* the floor slab. The camera was entombed in an opaque box. The desktop build renders the
   same level equally black, which should have been the tell. Nothing about lighting was ever
   measured here.
7. **"Headless `Screenshot.ps1` captures never show shadows in this project"** (correction 2's
   conclusion). Overstated. Once the camera is out of the floor, the desktop capture shows the cast
   shadow clearly. What was actually true is narrower: *those particular* captures were black for
   the same camera reason.

**Pattern worth carrying forward: measure, don't eyeball, and verify the control is a control.**
A frame that is uniformly black is far more likely to be a camera in the wrong place than a
renderer failing — check where the camera is before theorising about lighting.

---

## Root cause

`ADirectionalLight` in these levels asks for a very long dynamic shadow range — 40000 units in
`TestShadowLevel`, 20000 in `Surfing_infinite_wave` — with 4 (resp. 3) cascades. On this phone the
`sg.ShadowQuality=1` scalability row clamps the renderer to:

```
r.Shadow.CSM.MaxCascades  = 1        (level asked for 4)
r.Shadow.MaxCSMResolution = 1024
r.Shadow.DistanceScale    = 0.7
```

So the *entire* 40000 × 0.7 = 28000 unit range is covered by **one** 1024² shadow map:

```
28000 cm / 1024 texels  =  ~27 cm per texel
```

A 100 cm cube casts a shadow about 3–4 texels across, which vanishes under CSM depth bias. Nothing
is disabled and nothing is misconfigured in the level — the shadow is being rendered at a resolution
too coarse to survive. That is why every "is feature X off?" hypothesis failed: the feature was on
the whole time.

It also explains the desktop/device split. Desktop runs at full shadow quality with several
cascades, so the same light resolves the same shadow fine.

## The fix

Two knobs each fix it on their own, verified on device:

| Fix | How | Trade-off |
|---|---|---|
| **Shorten the light's shadow range** (recommended) | Set `DynamicShadowDistanceMovableLight` on the level's DirectionalLight to ~3000–5000 instead of 40000 / 20000 | Free. The surfing camera sits close to the board; hundreds of metres of shadow distance buys nothing. At 5000 units, one 1024² cascade is ~5 cm per texel — crisp |
| **Give mobile more cascades** | Override `r.Shadow.CSM.MaxCascades` (and/or raise `sg.ShadowQuality`) *after* the scalability row that sets it to 1 | Costs a shadow pass per extra cascade. Note `[Android_Vulkan]` is never selected — the device picks `Android_Default`, see `[[android-vulkan-device-profile-unused]]` |

Prefer the first: it fixes the cause rather than paying more GPU to paper over it.

### What was actually shipped (2026-09-01)

`Config/DefaultDeviceProfiles.ini` — new file — pins `sg.ShadowQuality=3` on `[Android_Default
DeviceProfile]`, the profile the phone actually selects.

Two bugs were in the way, both fixed by that file existing:

1. The earlier `sg.ShadowQuality=2` pin (commit `2b47aa929`) went into `[Android_Vulkan
   DeviceProfile]`, which the phone never selects — see `[[android-vulkan-device-profile-unused]]`.
2. It was also in `DefaultEngine.ini`. `UDeviceProfileManager` reads the **DeviceProfiles** ini
   hierarchy, so device-profile sections in `DefaultEngine.ini` are ignored regardless of the name.
   The `[Android_Vulkan DeviceProfile]` block still sitting there is dead twice over.

**Verified end to end on device, no runtime CVar override:**

```
Selected Device Profile: [Android_Default]
Set CVar [[r.Shadow.CSM.MaxCascades:3]]      <- exactly one occurrence; the @1 downgrade is gone
Set CVar [[r.Shadow.MaxCSMResolution:2048]]
Set CVar [[r.Shadow.DistanceScale:1.0]]
```

and the cube repro renders a clean shadow (floor 91% lit, zone spread 240 vs 1 before).

**Both fixes are in.** The player cut the wave light's `DynamicShadowDistanceMovableLight` from
20000 to 5000 in `ba2885567`, and measured the result on device 2026-09-02: **shadow visible, no
notable performance hit.** That measurement covers the two together, which is the shipped
configuration.

**The pin is now belt-and-braces.** At a 5000 unit range, `sg.ShadowQuality=1` alone would give
5000 × 0.7 / 1024 = 3.4 cm per texel — plenty for a surfboard — so three cascades at 2048 is heavily
over-provisioned (the near cascade lands around 0.1 cm per texel). The pin's remaining job is keeping
auto-detection out of `ShadowQuality@0`, which disables shadows outright on any device that lands
there.

Dropping the pin to 1 is therefore **safe now** — the ordering constraint ("fix the light first") is
satisfied — and would recover two shadow passes. Not urgent, since perf measured fine as it stands.

### Confirmed on the wave level

**2026-09-01, player-verified on device.** A clean A/B, watched live rather than sampled:

- Player's own run, device command line at its default (no CVar): **no shadow**.
- The very next run with `-ExecCmds="r.Shadow.CSM.MaxCascades 4"`: **shadow under the board**.

**The board's shadow is only visible while the board is OFF the water.** Planing in contact with the
surface, its shadow sits directly underneath and the board hides it from the chase camera. This is
why my own wave-level runs looked negative: `screencap` at 2 s intervals landed on a planing board
every single time, so the shadow could not have appeared in any frame I took. The test was
structurally incapable of showing what it was looking for, and I nearly wrote it up as "inconclusive,
possibly doesn't transfer". The player watching the screen caught it immediately.

**Method lesson: for an intermittent, motion-dependent effect, watch a live run.** Periodic frame
capture is the wrong instrument and its negatives are not evidence of absence.

Even fixed, 20000 units over one 1024² cascade is 13.7 cm per texel, so the board is only a few
texels wide and the shadow stays coarse. Shortening the light's range is what would make it solid
rather than marginal — see the fix table above.

## The working repro recipe

`TestShadowLevel` has no PlayerStart, so the camera must be placed explicitly. Everything goes
through the device `UECommandLine.txt` (the install step deletes it, so push it **after** deploy):

```
-project="../../../GoneSurfing/GoneSurfing.uproject" /Game/levels/TestShadowLevel?game=/Script/Engine.GameModeBase -ExecCmds="BugItGo 350 -700 500 -25 90 0"
```

- `?game=/Script/Engine.GameModeBase` stops `GoneSurfingGameMode` spawning a surfboard pawn with no
  wave, and gives a stock `DefaultPawn` with an ordinary camera.
- `BugItGo 350 -700 500 -25 90 0` puts that camera above and behind the cube, looking +Y and down 25°.
  Cheats are available in a Development build, and the log confirms with `LogCheatManager: BugItGo to:`.

The same URL works on desktop, which is the control:

```
./Screenshot.ps1 -Map "TestShadowLevel?game=/Script/Engine.GameModeBase"                  -ExecCmds "BugItGo 350 -700 500 -25 90 0" -LoadSeconds 30
```

### What is actually in TestShadowLevel

Decoded from the `.umap` (no PlayerStart, no SkyLight, no sky, no fog — the black band at the top of
every capture is empty sky, and that is expected):

| Actor | Location | Scale | Extent |
|---|---|---|---|
| `Shape_Cube` (floor, `WorldGridMaterial`) | (370, 10, 60) | 10000×10000×1 | z **10 → 110**, x/y ±500k |
| `Shape_Cube2` (caster, `M_Chair`) | (350, −40, 230) | 1×1×1 | z 230 → 330 |
| `DirectionalLight_0` | (210, −656, 360) | — | — |

## Tooling built

**`DeployAndroid.bat`** (repo root) — cook + stage + package + install.

```
DeployAndroid.bat                            iterative cook
DeployAndroid.bat /Game/levels/MyTestLevel   cook that map, iterative forced OFF
```

It handles three traps:
- **`NDKROOT` points at NDK `25.1.8937393`** (the engine's `DEFAULT_NDK_VERSION`) but only
  `25.2.9519653` is installed → UAT dies with "Couldn't find ... Android toolchain". Overridden locally.
- **Kills the editor first** — it fights the cooker for file locks, and an editor started before an
  ini edit has cached the read-only CVars, so cooking from it silently uses stale values.
- **Verifies pak data actually landed** and pushes it manually if not (see below).

---

## Gotchas that cost real time

- **UAT's deploy silently delivers incomplete data.** Three times. Once nothing at all; once a
  truncated `.ucas` plus a missing `.utoc` (crashes at `ShaderCodeLibrary` init). It reports
  `BUILD SUCCESSFUL` throughout. **Always compare device pak sizes against
  `Saved/StagedBuilds/Android_ASTC/.../Content/Paks/`.**
- **`-map=` plus `-iterativecooking` silently ignores a newly added map** — passed to the cooker,
  build succeeds, device says `Failed to load package`.
- **Read-only CVars bake at cook time.** `r.AllowStaticLighting`,
  `r.Mobile.EnableStaticAndCSMShadowReceivers` etc. cannot be moved by `-ExecCmds`. Every runtime
  experiment against them is meaningless.
- **Stripping GoneSurfing's renderer settings wholesale fails the cook** — several are cook-critical
  (Nanite, virtual textures, lightmap support). Bisect by *injecting into the test project* instead;
  it cooks in ~3 min against GoneSurfing's ~15.
- **New tuning UPROPERTYs need a full rebuild AND editor restart** before they appear in the HUD.
  Live Coding cannot add reflection data.
- **Unsaved levels.** A level edited in the editor but not saved cooks as its old version — two
  rounds were lost to this. Verify the `.umap` mtime on disk before cooking.
- **`[Android_Vulkan]` device profile is never used** — the phone selects
  `Android_Default` → `Android_Mid` → `Android`, so every optimisation in that block is dead
  (60% resolution, AA off, AO off...). Unrelated to shadows but a standalone perf bug. See
  `[[android-vulkan-device-profile-unused]]`.

## Reading device state

```
MSYS_NO_PATHCONV=1 adb pull \
  /sdcard/Android/data/com.dsh.gonesurfing/files/UnrealGame/GoneSurfing/GoneSurfing/Saved/Logs/GoneSurfing.log
grep "Selected Device Profile" GoneSurfing.log
grep -oE "Set CVar \[\[[^]]*\]\]" GoneSurfing.log      # what CVars actually resolved to
```

`MSYS_NO_PATHCONV=1` is required or git-bash mangles `/sdcard` into a Windows path.

**Measure shadows, do not eyeball them** — sample floor rows with `System.Drawing` and look for a
localized dip against a smooth gradient. A visual read produced a wrong call once already.

## Device etiquette

The phone is a personal device. **Stop immediately on any system/credential dialog** and do not
inject input around it — a wake+launch sequence once landed on an authentication prompt, and a
screencap once caught a personal SMS. Delete captures after use; check `topResumedActivity` is the
game before capturing.

---

## Scratch state to clean up

- `D:\Temp\TestUnrealProject` — converted to C++ by me (`Source/` added, `.uproject` modified).
  Original descriptor at `TestUnrealProject.uproject.bak`.
- `C:\Temp\TestShadowCpp` — my SSD copy of it (D: is an HDD; builds there are unusably slow).
  Contains GoneSurfing's renderer settings injected into `Config/DefaultEngine.ini`
  (original at `DefaultEngine.ini.bak`) and `Content/levels/GSTestShadowLevel.umap`.
- `Content/levels/TestShadowLevel.umap` in GoneSurfing is committed (came in accidentally with
  `721bf7639`); keep or drop as you like.
