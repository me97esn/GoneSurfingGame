# Spec: About screen — credits, licences, privacy

## Status

- [x] Spec drafted (2026-09-17) from the release-readiness pass over the Unreal Engine EULA
- [x] C++: `AboutPanel` (three cards + two text pages), `ABOUT` pill on the hub, pawn wiring
- [x] Words in `Content/Legal/*.txt`, staged as UFS; `ProjectVersion` in DefaultGame.ini
- [x] Desktop-verified 2026-09-17 with `Drive.ps1` (StartScreenSkip 0): hub → ABOUT → licences → BACK →
      cards → privacy → BACK → BACK → hub, START intact. Pill at (1168,522), LICENCES (147,500),
      PRIVACY (321,500) at 1200x540. Shots `Saved/Screenshots/ab_*.png`.
- [x] Device-verified 2026-09-17: the pak carries `Content/Legal`, the licence page scrolls by touch,
      and the build date reads the package's compile date
- [ ] Play listing: privacy policy URL points at the same text as `PrivacyAndTerms.txt`

## Overview

The game had no credits, no licence notices and no privacy text anywhere a player could reach.
Three of its parts require one:

- **Unreal Engine EULA §7a** — a product *with credits* must carry two fixed sentences (the
  Unreal trademark line and the Epic copyright line). §7b — third-party notices bundled with the
  engine are retained. §4c — end users get the product under terms that disclaim warranties for
  the Licensed Technology.
- **"Evening" by Kevin MacLeod, CC BY 4.0** (`Music License.txt`, repo root) — attribution in
  the artist's requested form. CC BY (not BY-SA) is compatible with the EULA's §6c.
- **Google Play** — a privacy policy regardless of whether anything is collected.

Not required but wanted: a thank-you for the surfer model (DesertLab3d, Fab standard licence —
no attribution clause) and the wave simulation toolchain (Blender + FLIP Fluids).

## Objective

One screen, reached from the hub, that answers "who made this, what with, and what does it do
with my phone" — and that never has to be recompiled to change a name, an address or a credit.

```
  HUB ── ABOUT (corner pill) ──► ABOUT
                                 ┌──────────┬────────────┬─────────────┐
                                 │ the game │ credits    │ built with  │
                                 │ version  │ music      │ Epic lines  │
                                 │ author   │ character  │ your data   │
                                 │ contact  │ art        │             │
                                 │ [LICENCES][PRIVACY]   │             │
                                 └──────────┴────────────┴─────────────┘
                                   │ LICENCES / PRIVACY ──► text page (scrolls) ── BACK ──► ABOUT
                                 ‹ BACK ──► HUB
```

## Functional Requirements

- **FR1 Entry.** A small ghost-tier `ABOUT` pill in the hub's bottom-right corner. Not in the
  hub row: testers tap START without reading (two-screen-navigation.md), and the row under it
  stays two things you do between waves. Hidden on the tutorial cards.
- **FR2 The three cards**, on the rack's scrim, at the rack's card style, designed at the phone's
  1200×540 logical size and `ScaleToFit` like the rack (desktop and phone differ ~2x in layout
  units):
  1. *The game*: wordmark, `Version <ProjectVersion> · build <compile date>`, blurb, "Made in
     Sweden by <author>", contact, and two quiet buttons: THIRD-PARTY LICENCES, PRIVACY.
  2. *Credits*: sections and rows straight from `Credits.txt`.
  3. *Built with*: the two Epic sentences verbatim (year = build year), one line about the
     modified engine, and the data statement.
- **FR3 Text pages.** Each button swaps the cards for one scrolling text card, title changed to
  match; BACK returns to the cards. The text is `ThirdPartyNotices.txt` / `PrivacyAndTerms.txt`.
- **FR4 One BACK, one corner** (`BackPill`, two-screen-navigation.md FR2a). BACK steps out one
  level: text page → cards → hub.
- **FR5 Words outside the binary.** Everything a person might edit lives in `Content/Legal/`:
  - `Credits.txt` — `key: value` (author, contact, blurb), `[SECTION]`, `Name | Detail` with
    `\n` for line breaks. Attribution a licence *requires* is kept word for word.
  - `ThirdPartyNotices.txt` — Epic's lines, the music, then the engine's own licence files for
    the components a mobile build links (zlib, libpng, libjpeg-turbo, FreeType, HarfBuzz, ICU,
    Ogg/Vorbis/Opus, LZ4, OpenSSL, curl, Vulkan headers + VMA, astcenc), byte-for-byte as they
    ship in `Engine/Source/ThirdParty/Licenses`, converted to UTF-8.
  - `PrivacyAndTerms.txt` — the privacy statement (also the Play listing's policy) and the terms
    with the §4c warranty disclaimer naming Epic.
  Plain files are invisible to the cooker: `+DirectoriesToAlwaysStageAsUFS=(Path="Legal")`, the
  same trap the board profiles and intro traces fell into.

## Non-Functional Requirements

- **NFR1** The panel pauses the world only if nothing else has (it opens over the hub, which has).
- **NFR2** Closing restores `bRideUIBlocked` from `bStartScreenActive`, exactly as the rack does;
  set at the transition, never polled (the world is paused, the pawn does not tick).
- **NFR3** Nothing in the About screen is test-gated on its own: it is only reachable through the
  hub, which `-unattended` already suppresses.
- **NFR4** Ride-feel untouched: no physics file changes.

## Acceptance Criteria

- Given the hub, when ABOUT is tapped, then the three cards show over the paused wave, the hub's
  START is not reachable, and BACK returns to the hub with START working.
- Given the cards, when THIRD-PARTY LICENCES is tapped, then the title reads THIRD-PARTY
  LICENCES, the text starts at the top, it scrolls, and BACK returns to the cards (not the hub).
- Given a packaged Android build, the credits card shows the rows from `Credits.txt` (not the
  "missing from this build" fallback).
- Given `Credits.txt` edited and the game relaunched without a rebuild, the change shows.

## Constraints checked

- Forces are not the default tool — UI only, no force, damping or redirect touched. OK.
- UMG/Slate overlap — pure Slate on ZOrder 260 (the modal tier) over the Slate hub; `bRideUIBlocked`
  set at the transition. OK.
- Test runs enable player controls — not reachable in tests (hub is suppressed). OK.
- String-referenced assets are not cooked — plain text files, staged as UFS. OK.
- Android unity build anon-namespace collisions — every helper is `About_`-prefixed. OK.
- Plain language, not physics jargon — the copy names what the game does, not how. OK.

## Open items

- `__DATE__` is the compile date of `AboutPanel.cpp`, not of the package. A full package build
  compiles it fresh; an incremental one may not. If that bites, touch the file in
  `PackageAndroidRelease.bat`.
- The notices list is the components a mobile build *typically* links. If the engine fork's
  Android build pulls in something else with a notice clause, add its file to the list.
- HoudiniNiagara is unreferenced outside `Content/Modules` and can go (with `Content/Modules`);
  its BSD notice is deliberately not in the list on that basis.

## Tasks

1. [x] T1 `AboutPanel.h/.cpp` — cards, text pages, install/close
2. [x] T2 Hub: `FHooks::OnAbout`, corner pill; pawn: `OpenAbout()`, teardown, `IsOpen` in the
   ride-start block
3. [x] T3 `Content/Legal/*.txt`, `DefaultGame.ini` (`ProjectVersion`, UFS staging)
4. [x] T4 Desktop screenshots of all three states (`ab_about`, `ab_lic`, `ab_priv`)
5. [x] T5 Device build; the pak carries `Legal/`, the licence page scrolls, build date correct
6. [ ] T6 [P] Remove HoudiniNiagara + `Content/Modules`
7. [ ] T7 [P] Play listing: host `PrivacyAndTerms.txt` at a stable public URL (Play Console
   requires one under App content; it shows on the store page) and fill in Data safety as
   "collects nothing" - true only if SEND/TUNE are out of the Shipping build, so check that first.
   Keep the hosted text and the in-app text saying the same thing whenever either changes.
8. ~~T8 Epic release form~~ — not needed: the game is free with no ads and no in-app purchases,
   so it is not a Royalty Product (EULA §4a ii). Becomes mandatory BEFORE the first sale/ad/IAP
   if that ever changes (royalties start at $1M lifetime and $10k/quarter; the form and the
   bookkeeping, §12, start at the first krona). Decision 2026-09-17.
