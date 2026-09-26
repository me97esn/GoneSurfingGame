# Release checklist — things flipped for development that must be flipped back

Debug switches, forced defaults and dev conveniences get turned on during investigations and are
easy to forget. This is the single place they are collected. **Add a line here at the moment you
flip something**, not later — the value of the list is that it is complete.

Each entry says what to change, where, and why it was flipped, so whoever does the pass can tell a
deliberate ship value from a leftover.

## Must revert

- [x] **Default board back to `foamie`.** (Done 2026-09-24.)
      `Boards_DefaultId` in [SurfBoards.cpp](../Source/GoneSurfing/SurfBoards.cpp) was
      `"shortboard"`, set on branch `tune-the-boards` (2026-09-08) because the shortboard is what the
      wave-interaction rework is tuned and ridden against. The foamie is the beginner board and the
      right first impression for a player. *(Note: a saved `ActiveBoardId` in the player's config
      still wins over this, so on a machine that has already picked a board the change is
      invisible - only a fresh install sees it.)*

- [x] **TUNE and SEND cannot reach a player.** (Done 2026-09-24 - and it is no longer a flip.)
      `bShowTuningHUD` stays `true`; what keeps the two dev buttons out of the store build is
      `ASurfboardPawn::IsTuningUIAvailable()` ([SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h)),
      which is `bShowTuningHUD` AND `!UE_BUILD_SHIPPING` - a compile-time `false` in Shipping, so the
      widgets are never installed however the flag is set, by a level, a Blueprint default or a hand
      that forgot. `SurfTuningHUD::Install` returns early in Shipping as a second lock. Every
      decision that asks "is the dev UI here?" goes through the accessor, the input mode included.
      *Consequence to remember: a Shipping APK has no TUNE panel, so on-device coefficient work and
      trace sharing happen on a Development APK.* Was: flipped on 2026-08-19 for a one-off tilt-yaw
      sign check. Also tracked in [per-board-tuning.md](per-board-tuning.md) and
      [tilt-yaw-fusion.md](tilt-yaw-fusion.md).

- [x] **`bDebugTilt` back to `false`.** (Done - `false` as of 2026-09-14.)
      [SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h). Flipped to get per-tick tilt numbers
      on device, because an APK cannot easily take `-ExecCmds` so the `surf.debug.flags` route is not
      available there. See [tilt-yaw-fusion.md](tilt-yaw-fusion.md).

- [ ] **Intro autopilots disabled in playable levels.**
      An enabled `AStateTriggerAutoPilot` fights player weight input. The `surf.autopilots` filter
      cannot select an autopilot with an empty `TestName`, so an intro autopilot has to be enabled by
      hand — and disabled by hand again. See
      [deterministic-ride-handoff.md](deterministic-ride-handoff.md).

## Check before shipping

- [x] **The wave-interaction rework is validated, or reverted as a set.** (Validated on device, 2026-09-14.)
      Branch `tune-the-boards` ships damping-and-redirect defaults that have **not been ridden**. If
      they are not validated, restore the whole set — the block at the top of the wave-mass section
      in [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) lists it. Half a swap
      is worse than either side. See
      [wave-interaction-damping-and-redirect.md](wave-interaction-damping-and-redirect.md).

- [x] **Per-board tuning re-derived.** (Done, 2026-09-14.) Several dials stopped differentiating the boards once the
      wave-mass forces were retired — measured: the shortboard's `SurfboardForwardsDamping` and
      `yawFwdOffFaceFloor` now make no difference at all. See
      [per-board-tuning.md](per-board-tuning.md).

- [ ] **No stale `Saved/BoardTuning/<id>.json` overlays.** They outrank the shipped board profiles
      and are written by the in-game tuning HUD, so a dev machine can be riding something no player
      will ever get. Archive rather than delete, so the values are recoverable.

- [x] **Mobile perf pass.** (Done, 2026-09-14.) Profile on the target device — see
      [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md).

## Unused assets and plugins

Added 2026-09-24 (owner) - the list had no entry for this, and a third-party plugin is a licence
question, not only a size one. The cooker takes what a cooked map references plus
`DirectoriesToAlwaysCook`, so an unused *asset* usually costs nothing; an enabled *plugin* ships
whether or not anything references it.

- [x] **No third-party plugin ships uncredited - or unwanted.** (2026-09-24.)
      **HoudiniNiagara** (`Plugins/HoudiniNiagara`, 495 MB on disk, 369 MB of it prebuilt binaries)
      carries **Side Effects Software's own BSD-style licence, not the Unreal EULA**: redistribution
      in source and binary form is permitted, source redistributions must keep the copyright notice,
      and the SideFX name may not be used to endorse. It is **not in the build**, on four independent
      signals: `"EnabledByDefault": false` in its `.uplugin`, absent from the `Plugins` array in
      [GoneSurfing.uproject](../GoneSurfing.uproject), zero mentions in a full ride log, and - the
      decisive one - the 2026-09-24 Shipping cook has no `Plugins/` directory at all and no file
      matching `*houdini*`. **If it is ever enabled it must be added to**
      [ThirdPartyNotices.txt](../Content/Legal/ThirdPartyNotices.txt). The folder is untracked by git,
      so it exists only on this machine and deleting it is a local disk decision, not a repo one.
      **`GeometryCacheAbcFile` was removed from the uproject** on 2026-09-24. *(Correction to an
      earlier reading of this file: it never reached the device. Its single module is `Type: Editor`
      with a `PlatformAllowList` of Win64/Mac/Linux, so the missing `TargetAllowList` in the uproject
      was harmless.)* Its `GeometryCache` dependency is `EnabledByDefault: true` engine-side and so
      survives the removal - which matters because `Waves_display_BP` derives from
      `GeometryCacheActor`. That Blueprint is referenced by no level and is not cooked; it is a
      leftover of the Alembic wave pipeline. `ModelingToolsEditorMode` and `RemoteControl` remain,
      both `TargetAllowList: [Editor]`, so neither can reach a device build.

- [x] **Orphan Houdini content out of `Content/`.** (Deleted 2026-09-24.) `Content/Modules` (12
      files, 8 of them tracked) and `Content/WaterHoudiniNiagara` (2 files) were the plugin's Niagara
      modules copied into project content - referenced by no level, and unloadable with the plugin
      disabled. The four untracked ones still exist in `Plugins/HoudiniNiagara/Content/Modules` if
      they are ever wanted back. **Verified the water FX survive the deletion** (owner asked):
      no spray or whitewater asset references `/Game/Modules` or anything Houdini (0 hits across all
      ten `NS_BoardSpray`, `WhiteWater_*`, `NDC_WhiteWater*`, `Ocean_Surface_*` and whitewater
      material assets); a post-deletion run logs no missing package and no Niagara error; whitewater
      renders in four screenshots on both maps; and with `surf.debug.flags 'spray'` the
      `SprayController` initialises 21 emitter sites and drives a **non-zero spawn rate in 95 of 108
      logged frames** on left rail, right rail and tail. *Gap: that is the emission being driven, not
      a pixel count - a single-frame capture on flat water shows only a few particles.*

- [ ] **Verify against the artifact, not the config.** The authoritative check is the cooked output
      and the package: no `HoudiniNiagara` under `Saved/Cooked/Android_ASTC/GoneSurfing/Plugins/`,
      and nothing Houdini in the pak. A plugin can be pulled in by a dependency chain that no ini
      shows.

## Packaging and store

- [x] **Package size.** 916 MB -> 197 MB on 2026-09-13/14, then re-measured twice on 2026-09-24:
      once when the data moved inside the package, and again on the first release-signed build.

      | artifact | size | |
      |---|---|---|
      | `GoneSurfing-Android-Shipping.aab` | 216.2 MB | the store bundle, as a file |
      | `GoneSurfing-Android-Shipping_universal.apk` | 164.5 MB | sideloadable, same contents |

      **The 216 MB is not a limit problem, and the reason matters.** Play's 200 MB cap is on the
      *compressed download size of the base module Play generates*, not on the upload file. Inside
      the bundle: `base/assets` 107.6 MB + `base/lib` 44.2 MB + dex/res ~1.5 MB = **~153 MB of
      deliverable content**, and a further **62.2 MB of `BUNDLE-METADATA/…debugsymbols`**, which is
      native debug symbols kept Play-side for crash symbolication and never sent to a device. So the
      estimate is ~153 MB against a 200 MB cap, ~47 MB of headroom - *an estimate: the Console prints
      the real download size on upload, and that is the number that counts.*

      Note the release-signed build came out **smaller** than the debug-signed one (universal APK
      181.6 -> 164.5 MB): `-distribution` moves the native symbols out of the delivered `.so` and into
      that bundle metadata. Keep the symbols - they are what makes a tester's crash readable.

- [ ] **The Shipping build has been run on the device.** `PackageAndroidRelease.bat` compiles and
      packages Shipping, but every device session so far has been a Development APK deployed by
      `DeployAndroid.bat`. Install `Saved/Releases/Android_ASTC/` once and ride it: no editor-only
      code paths, no `GetActorLabel()` surprises, tables load through the 16-bit path.

- [x] **AAB with data inside.** (Configured 2026-09-24; not yet verified on the store.)
      `bEnableBundle=True`, `bPackageDataInsideApk=True` and `bEnableUniversalAPK=True` are now in
      `DefaultEngine.ini` (AndroidRuntimeSettings). Play does not accept OBB expansion files with app
      bundles, so the data moves inside the package; the universal APK is the sideloadable artifact
      built from the same bundle, and is what goes on a test device (`.aab` cannot be
      `adb install`ed). Two consequences: **the 197 MB split measurement no longer describes the
      artifact** - re-read the sizes `PackageAndroidRelease.bat` prints, because Play's limit applies
      to the bundle's download size with the data inside it; and `DeployAndroid.bat`'s loose-pak
      verification is now meaningless, since there are no loose paks.

- [x] **Signing keystore.** (Created and **proven** 2026-09-24.) The upload key exists, the owner has
      backed up the file and the password off this machine, and a `-distribution` package signed with
      it verifies as `Signer #1 certificate DN: CN=Emil Stenberg, O=Gone Surfing, C=SE` under
      `apksigner verify --print-certs` - not the Android debug certificate. **That settles the open
      question: UE does read `KeyStore`/`KeyAlias`/`KeyStorePassword`/`KeyPassword` from
      `Config/Android/AndroidEngine.ini`**, so the passwords stay out of git permanently. Gradle's
      `validateSigningRelease` task ran and passed. Signed under APK Signature Scheme v2 only, which
      is correct for `MinSDKVersion=26` and irrelevant to Play anyway, since Play App Signing re-signs
      what users install. Recreate on a new machine with `MakeUploadKeystore.bat`; the key is the
      *upload* key, so losing it is recoverable through Play support at the cost of days.

- [ ] **`StoreVersion` bumped, `VersionDisplayName` set.** `DefaultEngine.ini`, AndroidRuntimeSettings.
      Play rejects an upload whose version code it has already seen. `StoreVersion=1` /
      `VersionDisplayName=1.0.0` are correct for a *first* upload and need a bump for every one after
      it. `ProjectVersion` in `DefaultGame.ini` - what the About screen shows - was brought in step at
      `1.0.0` on 2026-09-24.

- [ ] **Privacy policy URL in Play Console.** Play requires a public web page (App content →
      Privacy policy) before publishing; it shows on the store page. Host the text of
      [Content/Legal/PrivacyAndTerms.txt](../Content/Legal/PrivacyAndTerms.txt) at a stable URL
      (GitHub Pages, a Gist, own domain) and paste the URL in. **The page is written:**
      [docs/privacy.html](../../docs/privacy.html), a standalone file with no dependencies, ready to
      serve. GitHub Pages from `/docs` on the default branch is the least work *if the repo is
      public* - Pages on a private repo needs a paid plan, so a small public repo or a own-domain
      upload is the fallback. Then fill in **Data safety** as
      "collects nothing" — true only if the SEND panel (trace share to the Tailscale server) and
      TUNE are out of the Shipping build, so confirm that on the Shipping-on-device run above.
      Whenever the game's data handling changes, update the hosted page, the in-app text and the
      Data safety form together; a privacy statement that is no longer true is what Play reviews.
      See [about-screen.md](about-screen.md) T7.

- Not needed: Epic's release form (`unrealengine.com/release`). The game is free with no ads and
  no in-app purchases, so it is not a Royalty Product (EULA §4a ii). The form becomes mandatory
  *before* the first sale, ad or purchase if that ever changes.

- [ ] **`NS_BoardSpray` references a deleted material - confirm which renderer holds it.**
      *(Owner 2026-09-24: deferred to after release. Upgraded from "confirm it is disabled" the same
      day: a run log now shows the dependency is real - `LoadErrors: Warning: While trying to load
      package /Game/Particles/NS_BoardSpray, a dependent package
      /Game/StarterContent/Particles/Materials/M_Radial_Gradient was not available`. So some renderer
      in the asset still points at the material StarterContent's purge took; whether it is the
      disabled one decides whether anything is actually missing on screen.)* The spray uses
      `WhiteWaterMaterial` (`SprayRight`, `SprayTail` sprite renderers), so the broad "needs a
      material" worry was outdated. But `SprayLeft`'s sprite renderer lost its material when
      StarterContent's `M_Radial_Gradient` was deleted in `f4fcce55a`, and the asset carries one
      disabled renderer somewhere. If that is the one, nothing to do; if it is enabled, give it
      `WhiteWaterMaterial`. `Ocean_Surface_Niagara_System` (`M_Spark`), `WaterController-Datatable-BP`
      (`M_Brick_Clay_Beveled`) and `WaterForces-Datatable-BP` (`SM_Arrows`) lost assets in the same
      commit; none of them is player-facing.

## Getting it to testers (decided 2026-09-24)

Owner is registering a **new personal** Play developer account, so the closed-testing requirement
applies: **12 distinct Google accounts, opted in to the closed track, continuously for 14 days**,
and actually playing - Google rejects for insufficient engagement, and one tester dropping out
resets the clock. Internal testing does not count toward it. Organization accounts (D-U-N-S) are
exempt. The 14 days is calendar, not work, so the build only has to be good enough that 12 people
open it more than once.

Critical path, longest lead time first:

1. [ ] **Register the developer account** ($25 one-off). Identity verification is the step of
       unknown length and nothing else can start until it clears.
2. [ ] **Create the upload key** - `MakeUploadKeystore.ps1`, see above.
3. [ ] **Host [docs/privacy.html](../../docs/privacy.html)** and note the URL.
4. [ ] **Create the app in the Console**, fill store listing, Data safety ("collects nothing" - true
       as of the Shipping TUNE/SEND gate), content rating, target audience.
5. [ ] **Upload the release-signed `.aab` to the closed track** and invite the testers.
6. [ ] **Recruit 12 + spares.** Real people with Google accounts. Not a paid tester farm: Google
       rejects and bans for it, which costs more than the wait.
7. [ ] **14 days**, then Apply for production.

**Consequence of the Shipping gate to plan around:** testers on a Shipping build have no SEND
button, so no traces come back from them. Their feedback is words and video only. If traces from
testers matter, they need a Development APK instead, outside Play.

## Deferred past release, deliberately

Owner calls, 2026-09-24. Recorded so they are not re-raised as blockers:

- **Tune the "tired" feel** (`StaminaTiredSteerGain`, `StaminaTiredAssistAlpha`) - needs the TUNE
  panel mid-ride on the device, and TUNE is one of the last two things to be switched off. See
  `stamina.md`.
- **Android resume slowdown** - not reproduced since the performance work, may already be fixed. If
  it shows up again, capture it in the degraded state (`CaptureTrace.ps1 -NoLaunch`).
- ~~**`bShowTuningHUD` and the SEND panel**~~ - no longer deferred and no longer a flip: they are
  compiled out of Shipping and left on everywhere else (see the Must-revert entry). The Play **Data
  safety** answer of "collects nothing" now rests on the build, since SEND is the game's only
  outbound request - confirm it on the Shipping-on-device run by looking for no TUNE/SEND strip.

## Not a problem, recorded so nobody "fixes" them

- The four-finger-tap dev console on Android is gone twice over: the handler is compiled out of
  Shipping (`LaunchAndroid.cpp`, `!UE_BUILD_SHIPPING`) and `bShowConsoleOnFourFingerTap=False` in
  `DefaultInput.ini` removes it from Development APKs as well. Device console commands go through
  the adb console receiver (`CaptureTrace.ps1`).
- Debug draws and dumps behind `surf.debug.flags` / `surf.debug.*` CVars (`wavedump`,
  `wavenormals`, `slopeprobe`, `crossing`, `torque`, `propulsion`, …) are off unless asked for, and
  cost nothing when off. They are tools, not leftovers.
- `GetActorLabel()` is `WITH_EDITOR`-only. Every device build, Development or Shipping, runs
  without it, so anything keyed on actor labels (the `surf.debug.actors` filter) is editor/`-game`
  only. Shipping compiles and packages as of 2026-09-13 (`PackageAndroidRelease.bat`); whether it
  *runs* is the checklist item above.
