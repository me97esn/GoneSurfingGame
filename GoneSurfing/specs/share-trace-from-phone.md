# Spec: Share a ride trace from the phone, mid-run

## Status

**Built 2026-09-15 on branch `share-trace-from-phone`, desktop-compiled, not yet sent from a device.**
`TraceShare.h/.cpp` is the SEND panel and the HTTP post; its toggle sits beside TUNE in
`SurfTuningHUD` (so it exists only while `bShowTuningHUD` is on); `Tools/TraceShareServer.py` is the
listener, `Tools/TraceShareServer.ps1` starts it and prints the address; the pawn writes the ride
verdict into the trace (FR4). The destination is `surf.traceshare.url` in `DefaultEngine.ini`.

Setup still owed by the owner (one-time):

- [x] Install Tailscale on the PC (100.x.y.z) - and on the phone, same account, VPN toggle on.
- [x] Run `.\Tools\TraceShareServer.ps1` once from an elevated shell for the firewall rule; it prints
      `surf.traceshare.url="http://100.x.y.z:8765/trace"` - put that line in `DefaultEngine.ini` (quotes required: the ini parser reads an unquoted `//` as a comment)
      `[SystemSettings]` - done 2026-09-16 (143d719ff); deploy.
- [ ] Sanity check from the phone's browser: `http://100.x.y.z:8765/` answers "TraceShare listening".

## Overview

The owner is tuning when the end-of-ride card appears - not more often, not less - by riding on the
phone, out of the house, and asking Claude over remote control to look at the rides that misbehaved.
The evidence is the ride's trace, a CSV of 0.3-1.5 MB under `Saved/InputTraces/` on the device. Today
it reaches the PC only over USB (`PullInputTraces.ps1`), which means "tonight", which means the loop
runs once a day instead of once a ride.

## Objective

From the phone, mid-session, put the last few rides' traces on the PC with one tap each, so that the
next chat message can be "sent one, the card came up but shouldn't have" and the analysis starts.

## Requirements

**FR1 - SEND panel.** A dev-only toggle beside TUNE opens a list of the newest traces on the device
(newest first, capped at ten), each row showing when it was recorded, its size, and a SEND button.
The list is built on every open so it is always current. The ride being recorded right now, if any,
is flushed to disk first and marked RIDING NOW, so a ride that *failed* to end can be sent while it
is still going.

**FR2 - one POST.** SEND reads the file and POSTs it to `surf.traceshare.url` with the filename in
`X-Trace-Name`. The row shows SENDING..., then SENT or FAILED with the reason (no connection, HTTP
code, no URL). A send in flight survives closing the panel. Nothing else changes on the phone: no
file is moved, renamed or deleted by sending.

**FR3 - the listener.** `Tools/TraceShareServer.py` accepts `POST /trace`, saves the body under
`Saved/InputTraces/fromphone/<X-Trace-Name>` (overwriting: a re-send is a refresh), appends a line to
`fromphone/arrivals.log`, and prints it. Filenames are validated (`name.csv`, no separators); bodies
capped at 32 MB. It never touches the InputTraces root, whose files the game's prune owns.

**FR4 - the trace says what the game decided.** A trace that just stops used to look the same
whether the ride ended, the level was left, or the recording cap hit. Now:

- `# ride_end=WIPEOUT|LOST THE WAVE t=<s> reason=<the trigger's own words>` is written the moment
  `EndRide` fires, before the trace closes.
- `# trace_stop=<why> t=<s>` is the last line every closed trace gets: `ride_end`, `endplay`,
  `max_seconds`, `replay`, `intro_recorded`.
- `# card=<title> after=<s> score=<n>` is appended when the wipeout card actually opens. A
  `ride_end` with no `card` after it is a card that never showed.
- `# shared_mid_ride t=<s>` marks where a live trace was flushed for sending.

Every parser skips `#` lines wherever they sit (`InputReplayAutoPilot`, `CompareTraceReplay.ps1`,
the pawn's own replay loader), so footers cost nothing.

**NFR1 - reachable from mobile data, nothing in the game knows how.** Tailscale on both devices
gives the PC a stable `100.x` address the phone can reach from anywhere without router changes; the
game only sees a URL. Plain HTTP: Android's cleartext policy is Java-level and UE's Android HTTP is
native libcurl, so no manifest change is needed.

**NFR2 - dev builds only.** The toggle is created inside `SurfTuningHUD::Install`, which the pawn
calls only when `bShowTuningHUD` is on. Turning that flag off for release removes SEND with it.

## Acceptance

- Given a finished ride on the phone and the listener running, when SEND is tapped on the top row,
  then within seconds the row reads SENT, the PC console prints the arrival, and
  `Saved/InputTraces/fromphone/<name>.csv` is byte-identical to the device's file.
- Given the listener is down, when SEND is tapped, then the row reads FAILED: no connection and
  nothing on the phone has changed.
- Given a ride that ended with a card, when its trace is read, then it ends with `ride_end`, then
  `trace_stop=ride_end`, then `card=`.
- Given a ride left via Back with no card, then the trace ends with `trace_stop=endplay` and has no
  `ride_end` line.

## Implementation notes

- The HTTP request is ticked by the core ticker, which runs while the world is paused, so the panel
  can stay open (paused) and still see the reply.
- Row state lives in a `TSharedPtr` held by both the button lambda and the reply lambda; a reply
  arriving after the panel was rebuilt writes into an orphaned row and logs. Harmless by design.
- Verified locally 2026-09-15: a real 588 KB trace round-tripped through the listener byte-identical;
  `../evil.csv` was refused with 400.

## Test cases

- `python3 Tools/TraceShareServer.py --port 8799` + `curl -X POST -H "X-Trace-Name: x.csv"
  --data-binary @some.csv http://127.0.0.1:8799/trace` → `saved x.csv (N bytes)`.
- Desktop: `Screenshot.ps1` with the SEND panel open (the TUNE strip is at top centre) to check the
  list renders; sending from desktop to `127.0.0.1` exercises the same path as the phone.
