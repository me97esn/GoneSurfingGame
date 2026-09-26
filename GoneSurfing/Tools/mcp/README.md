# MCP servers for GoneSurfing

Three servers, dependency-free Python, sharing one protocol module.

| File | |
|---|---|
| `mcp_lite.py` | The MCP protocol and nothing else. ~120 lines. Read this first. |
| `surf_telemetry.py` | Five tools over ride trajectory CSVs — what the board *did*. |
| `surf_tuning.py` | Six tools over the surfing coefficients — why it did it. |
| `surf_live.py` | Seven tools against a **running** game — change what it is doing, now. |
| `mcp_test_client.py` | The client half of the transport, for the tests. |
| `test_*.py` | Smoke tests that drive real stdio; the live one mocks the engine. |

## Why these are MCP servers and not another .ps1

A script is one-shot, stateless, and returns text you have to re-parse every
time. An MCP server gives the model **named tools with typed schemas** it can
discover on its own, **structured JSON answers** instead of a file dump, and a
**process that stays alive** across calls.

The test for whether something belongs here: *is there a domain to query
repeatedly, or is it one command you run and read?* Ride trajectories are a
domain. So is a four-layer coefficient stack. `DeployAndroid.bat` is a command —
that one stays in Bash.

Being honest about the trade: **a Claude Code skill could do the first two of
these.** `surf_telemetry` and `surf_tuning` are stateless file I/O, so neither
needs the one thing MCP uniquely provides. A skill would also cost nothing when
unused, because skill instructions load on demand while MCP tool schemas sit in
context from session start — these three are about 2,900 tokens of schema
between them, every session, used or not.

`surf_live` is the one that has to be a server. It holds an HTTP connection to
the engine and remembers where the tuning subsystem sits in the object graph;
a fresh process per call would re-probe every time and could not keep a session
open at all. If you only ever build one of these, build that one.

## Quickstart

```sh
cd GoneSurfing/Tools/mcp

# Prove they work, with no MCP client involved at all. The tests spawn each
# server over stdio and drive the whole protocol by hand.
python3 test_surf_telemetry.py --project E:/windowsgrejor/git/GoneSurfingUE5/GoneSurfing
python3 test_surf_tuning.py    --project E:/windowsgrejor/git/GoneSurfingUE5/GoneSurfing

# Watch the actual JSON-RPC go past:
python3 test_surf_telemetry.py --project ... --verbose
```

Registration is checked in at `GoneSurfing/.mcp.json`, so a Claude Code session
started in `GoneSurfing/` picks both up (it asks once to approve them). `/mcp`
lists them. Editing a server needs a session restart — the running process is
holding the old code.

## surf-telemetry — what the board did

| Tool | Answers |
|---|---|
| `list_runs` | What runs exist, when they were recorded, which are stale |
| `run_summary` | Whole-ride aggregates: speed, distance, planing, slope-gate fractions, per-step breakdown |
| `window` | Raw samples for a slice — chosen seconds, chosen columns, downsampled not truncated |
| `compare` | Verdict vs the committed baseline, with per-step drift |
| `find_events` | Turns (and what they cost in speed), stalls, airborne stretches, planing flips, step boundaries |

Two habits are baked into the output, because they are mistakes this project
keeps re-making:

- **Staleness is reported, never assumed away.** `Saved/Tests/latest/` is never
  pruned, so months-old runs sit next to today's and get graded as current.
- **Aggregates lead.** Single-sample maxima have sent this project down blind
  alleys more than once.

`run_summary` reports the fraction of the ride above slopeSin 0.12 / 0.30 / 0.35
because those are the propulsion gates — when a ride feels slow, that row is
usually the answer.

**Not here:** wave-relative events (the CSV carries no crest information — that
needs a replay, which `Tests/AnalyzeCrossing.ps1` does), and `Saved/InputTraces/`
(different schema, different job).

## surf-tuning — why it did it

| Tool | Answers |
|---|---|
| `list_boards` | The five boards, their identity, how much tuning each carries |
| `search_tunables` | Find coefficients by name, category, or by what their comment says |
| `resolve` | The full four-layer story for one coefficient, plus the comment explaining it |
| `set_tunable` | Write into a board overlay or the global override file |
| `revert` | Remove a key so the layer beneath takes over; `*` clears the layer |
| `diff` | Where two boards actually differ, attributed to the layer each value came from |

### The four layers

```
1. compiled default     Source/GoneSurfing/SurfTuningSubsystem.h   (201 floats)
2. board profile        Content/Boards/<n>-<id>.json  "tuning": {}
3. global override      Saved/TuningOverrides.json
4. board overlay        Saved/BoardTuning/<id>.json

effective = overlay ?? global ?? profile ?? default
baseline  = global ?? profile ?? default          <- note: skips the overlay
```

That asymmetry is deliberate in the C++ (`USurfTuningSubsystem::GetBaseline`):
resetting a value the global file pins returns to the *global* value, not the
board's, which is the honest answer to "undo my change". The overlay is written
sparsely against the baseline, so deleting it returns a board to exactly its
shipped feel. `resolve` reports both numbers for this reason.

### Where the defaults come from

Reflection is the runtime source of truth, but nothing here runs an engine, so
layer 1 is parsed out of the C++ header. That is a compromise with one
compensating advantage: the header carries the **comment above each
declaration**, which in this project is where the reasoning lives — the A/B that
justified the number, the date, the spec. `search_tunables` searches those
comments, which is often faster than searching the code.

A float only counts as a tunable if it is a `UPROPERTY`: the subsystem collects
them with `TFieldIterator<FFloatProperty>`, which walks reflected properties
only, so a plain private member is not tunable no matter how float it looks.
The parser was checked against the header — 202 of 202 declarations found, zero
value mismatches — and then one was excluded for not being reflected.

### Writing

Only layers 3 and 4 are writable. `Content/Boards` is board identity and belongs
in a commit, so `set_tunable` refuses it rather than silently writing elsewhere.
A `.bak` is kept of every file changed. Three things to know:

- **This is not live tuning.** Values are read when a board is *applied*, so a
  change needs a board switch or a relaunch. The in-game tuning HUD is the live
  path — it pauses, you move a slider, and the value is in effect on the next
  physics tick. These tools edit the files that HUD reads and writes; they do
  not talk to a running game.
- A **running game will overwrite these files** from memory on its debounced
  save. Write between sessions.
- A global write does not reach a board that pins the same coefficient in its
  own overlay. `set_tunable` names those boards when it happens.

## surf-live — change what it is doing, now

| Tool | |
|---|---|
| `status` | Is a game reachable, and how to switch its web server on if not |
| `find_subsystem` | Locate `USurfTuningSubsystem` in the object graph, once per session |
| `get_live` / `set_live` / `reset_live` | Read, write and reset a coefficient in the running game |
| `list_live` | Every tunable the running build actually has, from engine reflection |
| `call_function` | Escape hatch: any BlueprintCallable UFUNCTION on any object |

`set_live` is in effect on the **next physics tick** — mid-ride, no relaunch, no
board switch. That is the difference from `surf_tuning`, which edits files the
game reads when a board is next applied.

Nothing new was needed on the C++ side: `GetByName`, `SetByName`,
`ResetToDefault` and `GetAllPropertyNames` are already
`UFUNCTION(BlueprintCallable)` on the subsystem, and Remote Control resolves
objects with `StaticFindObject`, so a `UGameInstanceSubsystem` in
`/Engine/Transient` is addressable like anything else.

**A live set does not stay in memory.** `SetByName` marks the subsystem dirty
and its debounced save writes the active board's overlay a moment later, exactly
as if you had moved a slider in the tuning HUD. Live experiments persist; use
`surf_tuning`'s `revert` to undo one.

### Making the game listen

1. `RemoteControl` is enabled in `GoneSurfing.uproject` on this branch, with
   `TargetAllowList: ["Editor"]` — `-game` launched from the editor binary is
   still the Editor target, so the bridge works there, while packaged Android
   builds never see it. **The editor target must be rebuilt once** for this.
2. The web server is off by default and has to be started:
   - in the editor: console `WebControl.StartServer`, or CVar
     `WebControl.EnableServerOnStartup 1`
   - in `-game`: add `-RCWebControlEnable` as well; the plugin refuses to serve
     outside the editor without it.
3. Port 30010 (`URemoteControlSettings::RemoteControlHttpServerPort`).
   `bRestrictServerAccess` defaults to false, so no passphrase on localhost.
4. `status` tells you which of those is missing.

### Verified against a real engine, 2026-09-11

Built the editor target, launched `-game` with the flags above, and ran the
tools against it. Read `lateralTurnCoefficient` 5400, set 7777, read back 7777,
reset to 5400. `find_subsystem` located the subsystem in three probes:

```
/Engine/Transient.GameEngine_0:GameInstance_0.SurfTuningSubsystem_0
```

Note the shape. Object paths are `Package.Object:SubObject.SubSubObject` — `:`
marks the first outer that is not a package and `.` every level after it — and
the UGameInstance hangs off the **UEngine**, not off the package. A first guess
that omitted the engine level failed all 32 candidates, which is why the probe
now walks the chain a level at a time and reports what it found at each.

An independent cross-check fell out of it: `list_live` reports **186** tunables
from engine reflection, and `surf_tuning`'s header parser finds **186** in the
same tree. The two arrive at that number by completely different routes.

`test_surf_live.py` keeps a mock Remote Control server for the parts that are
awkward to provoke on a real engine — no game running, the engine vanishing
mid-session, a function that does not exist — and its mock now uses the real
path shape so the chain walk is what gets exercised.

Two bugs only reality found:

- `status` sent a JSON body on a `GET /remote/info`. The engine closed the
  connection, and the tool reported "the game is not running" about a server
  that was answering every other request fine.
- `/remote/info` replies with the engine's entire route table. Returning it raw
  cost a few thousand tokens per call to say "yes, it is up"; `status` now
  reports the route count and whether the two routes it needs are present.

## Reading the protocol

All of MCP that these servers use is in `mcp_lite.py`:

| Concept | Where |
|---|---|
| stdio transport — one JSON object per line | `serve()` |
| the handshake | `handle()`, `initialize` branch |
| notifications get no reply | `handle()`, the `req_id is None` guard |
| tool discovery | `tools/list` branch + the `@tool` decorator |
| tool invocation, and errors as *results* | `tools/call` branch |

Two rules worth internalising:

1. **stdout belongs to the protocol.** One stray `print()` corrupts the stream
   and the client drops the server. Diagnostics go to stderr — `log()`.
2. **A tool failing is not a protocol failure.** A bad argument comes back as a
   normal result with `isError: true` and a message the model can act on; a
   JSON-RPC `error` is reserved for "that method or tool does not exist".

And one the tests caught: **nothing validates a tool's arguments for you.** The
client may check them against `inputSchema`, but the server must assume it will
be called with anything. An out-of-enum `layer` once fell through an
`if overlay else global` and wrote the wrong file.

The tool `description` and `inputSchema` strings *are* the prompt for each tool
— they are all the model gets. That is why they carry units, defaults, and the
caveats.

## Project root, and the gitignored-data trap

Resolved in this order: `--project` → `$GONESURFING_PROJECT` → walked up from the
script (`Tools/mcp/` → `GoneSurfing/`).

The override exists because **`Saved/` is gitignored**. A fresh worktree has all
the code, the tuning header and the board profiles, but no run data and no live
tuning. Point it at a tree that has them:

```sh
GONESURFING_PROJECT=E:/windowsgrejor/git/GoneSurfingUE5/GoneSurfing python3 surf_tuning.py
```

Whichever tree you point at is the tree whose header gets parsed — a branch that
added coefficients will show them and one that did not will not.

One more: ages come from file mtime, so in a freshly checked-out worktree the
committed baselines all read as recorded today. Run data under `Saved/` keeps
its real mtime because it is never checked out; for a baseline's true age in a
fresh tree, ask git.

## Relationship to the PowerShell scripts

`Tests/Compare.ps1` stays the authority for CI exit codes. `compare` here
reimplements the same arithmetic to return it with structure attached; its
thresholds are copied from that script (`CMP_DEFAULTS`) and must be updated with
it. Verified identical on `hard_turn_towards_the_wave` and `surf-straight` —
every figure matches to the reported decimal.

## Next

The loop these three make possible, once `surf_live` is verified against a real
engine: `set_live` a coefficient mid-ride, let the board ride, then read the
trajectory back through `surf_telemetry` and adjust — inside one session,
without a relaunch between steps. That is the thing none of the PowerShell
scripts, the tuning HUD, or a skill can do on its own.
