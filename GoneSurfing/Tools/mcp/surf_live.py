#!/usr/bin/env python3
"""
surf-live -- an MCP server that talks to a RUNNING GoneSurfing.

This is the one of the three that actually needs to be an MCP server. It holds
an HTTP connection to the engine and remembers, for the whole session, where the
tuning subsystem lives in the object graph -- state a fresh subprocess per call
could not keep. Set a coefficient here and it is in effect on the next physics
tick, mid-ride, with no relaunch and no file round-trip.

    surf_tuning.py  edits files the game reads when a board is next applied.
    surf_live.py    changes the value the board is riding on, right now.

HOW IT WORKS
    Unreal's Remote Control plugin serves HTTP on :30010. Two routes matter:

        PUT /remote/object/describe   {objectPath}
        PUT /remote/object/call       {objectPath, functionName, parameters}

    Object resolution is StaticFindObject, so anything in the object graph is
    addressable, including a UGameInstanceSubsystem living in /Engine/Transient.
    USurfTuningSubsystem already exposes exactly what is needed as
    UFUNCTION(BlueprintCallable): GetByName, SetByName, ResetToDefault and
    GetAllPropertyNames. Nothing new had to be written on the C++ side.

TO MAKE THE GAME LISTEN
    1. RemoteControl must be enabled in GoneSurfing.uproject (it is, on this
       branch) and the editor target rebuilt once.
    2. The web server is off by default and must be started:
         - in the editor: console `WebControl.StartServer`, or set the CVar
           `WebControl.EnableServerOnStartup 1`
         - in `-game`: the launch flag `-RCWebControlEnable` as well, because
           the plugin refuses to serve outside the editor without it.
    3. `status` reports whether any of that worked.

    Nothing here starts the game. If the game is not running, every tool says
    so plainly rather than hanging.

SAFETY
    A value set here is live but NOT saved by itself -- the subsystem marks
    itself dirty and its debounced save writes the active board's overlay a
    moment later, exactly as if the in-game HUD had moved the slider. So live
    experiments do persist into Saved/BoardTuning/<id>.json. Use surf_tuning's
    `revert` to undo one.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import socket
import time

import mcp_lite
from mcp_lite import dumps, tool

SERVER_INFO = {"name": "surf-live", "version": "0.1.0"}

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30010  # URemoteControlSettings::RemoteControlHttpServerPort
SUBSYSTEM_CLASS = "SurfTuningSubsystem"

# Object paths are built as Package.Object:SubObject.SubSubObject -- ':' marks
# the first outer that is not a package, '.' every level after it. The verified
# shape for this project is:
#
#   /Engine/Transient.GameEngine_0:GameInstance_0.SurfTuningSubsystem_0
#
# i.e. the UGameInstance hangs off the UEngine, not off the package directly.
# PIE puts a different engine class at the front, hence the candidate lists; the
# probe walks the chain a level at a time rather than guessing whole strings.
ENGINE_NAMES = ("GameEngine", "UnrealEdEngine", "EditorEngine", "GoneSurfingGameEngine")
GAME_INSTANCE_NAMES = ("GameInstance", "GoneSurfingGameInstance")
TRANSIENT = "/Engine/Transient"


class Engine:
    """One kept-alive HTTP connection to the running engine, plus what we have
    learned about it. This object is the reason the server is worth its process:
    the object path is discovered once and reused for every later call."""

    def __init__(self, host: str, port: int):
        self.host, self.port = host, port
        self.conn: http.client.HTTPConnection | None = None
        self.object_path: str | None = None
        self.discovered_at: float | None = None
        self.calls = 0

    def _connect(self):
        if self.conn is None:
            self.conn = http.client.HTTPConnection(self.host, self.port, timeout=5)
        return self.conn

    def request(self, route: str, payload: dict | None = None,
                method: str = "PUT") -> tuple[int, object]:
        # A GET carries no body. Sending one made the engine close the
        # connection, which looked exactly like "the game is not running".
        body = json.dumps(payload).encode() if payload is not None else None
        headers = {"Content-Type": "application/json"} if body is not None else {}
        for attempt in (1, 2):  # a dropped keep-alive costs one silent retry
            try:
                conn = self._connect()
                conn.request(method, route, body=body, headers=headers)
                resp = conn.getresponse()
                raw = resp.read().decode("utf-8", "replace")
                self.calls += 1
                try:
                    return resp.status, json.loads(raw) if raw else None
                except json.JSONDecodeError:
                    return resp.status, raw
            except (http.client.HTTPException, socket.error, ConnectionError):
                self.close()
                if attempt == 2:
                    raise ConnectionError(
                        f"no Remote Control server on {self.host}:{self.port}. Either the game is "
                        "not running, or its web server was never started -- see `status` for the "
                        "exact steps.")
        raise AssertionError("unreachable")

    def close(self):
        if self.conn is not None:
            try:
                self.conn.close()
            except Exception:
                pass
            self.conn = None


ENGINE: Engine


def _describe(path: str) -> tuple[int, object]:
    return ENGINE.request("/remote/object/describe", {"objectPath": path})


def _require_path() -> str:
    if ENGINE.object_path is None:
        found = find_subsystem()
        if not found.get("object_path"):
            raise RuntimeError(found.get("hint") or "tuning subsystem not found")
    return ENGINE.object_path


def _call(function: str, params: dict | None = None, transaction: bool = False) -> object:
    status, body = ENGINE.request("/remote/object/call", {
        "objectPath": _require_path(),
        "functionName": function,
        "parameters": params or {},
        "generateTransaction": transaction,
    })
    if status != 200:
        raise RuntimeError(f"{function} returned HTTP {status}: {body}")
    return body


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@tool(
    "status",
    "Is a running game reachable, and has the tuning subsystem been located? "
    "Call this first, and whenever anything else reports a connection problem. "
    "Explains how to switch the engine's web server on if it is off.",
    {"type": "object", "properties": {}},
)
def status() -> dict:
    out = {"endpoint": f"http://{ENGINE.host}:{ENGINE.port}",
           "object_path": ENGINE.object_path,
           "calls_this_session": ENGINE.calls}
    try:
        code, info = ENGINE.request("/remote/info", None, method="GET")
        out["reachable"] = code == 200
        if isinstance(info, dict):
            # /remote/info answers with the engine's whole route table, which is
            # a few thousand tokens of no interest. Summarise it: all that
            # matters is whether the two routes these tools use are there.
            routes = {r.get("Path") for r in info.get("HttpRoutes", []) if isinstance(r, dict)}
            needed = {"/remote/object/call", "/remote/object/describe"}
            out["engine"] = {
                "routes_advertised": len(routes),
                "required_routes_present": sorted(needed & routes) == sorted(needed),
                "active_preset": (info.get("ActivePreset") or {}).get("Name"),
            }
    except ConnectionError as exc:
        out["reachable"] = False
        out["error"] = str(exc)
        out["how_to_enable"] = [
            "The game must be running with the RemoteControl plugin enabled.",
            "In the editor: run `WebControl.StartServer` in the console, or set the CVar "
            "WebControl.EnableServerOnStartup 1.",
            "In -game: add the -RCWebControlEnable launch flag as well; the plugin refuses "
            "to serve outside the editor without it.",
        ]
    return out


@tool(
    "find_subsystem",
    "Locate USurfTuningSubsystem in the running game's object graph and cache "
    "its path for the rest of the session. Called automatically by the other "
    "tools; call it directly only to re-discover after a restart, or to see "
    "which candidate paths were tried.",
    {
        "type": "object",
        "properties": {
            "force": {"type": "boolean", "default": False,
                      "description": "Re-probe even if a path is already cached."},
            "object_path": {"type": "string",
                            "description": "Skip probing and use this path (from `obj list class=SurfTuningSubsystem`)."},
        },
    },
)
def find_subsystem(force: bool = False, object_path: str = None) -> dict:
    if object_path:
        code, body = _describe(object_path)
        if code != 200:
            return {"object_path": None, "tried": [object_path],
                    "hint": f"that path did not resolve (HTTP {code}): {body}"}
        ENGINE.object_path, ENGINE.discovered_at = object_path, time.time()
        return {"object_path": object_path, "source": "supplied"}

    if ENGINE.object_path and not force:
        return {"object_path": ENGINE.object_path, "source": "cached",
                "age_s": round(time.time() - (ENGINE.discovered_at or 0), 1)}

    tried = []

    def first_hit(candidates):
        for path in candidates:
            tried.append(path)
            code, _ = _describe(path)
            if code == 200:
                return path
        return None

    # Level 1: the engine object.
    engine = first_hit([f"{TRANSIENT}.{e}_{i}" for e in ENGINE_NAMES for i in range(3)])
    # Level 2: the game instance, under the engine if we found one, else
    # directly under the package in case some other arrangement applies.
    gi_roots = [engine] if engine else [TRANSIENT]
    sep = ":" if engine else "."
    game_instance = first_hit([f"{root}{sep}{g}_{j}"
                               for root in gi_roots for g in GAME_INSTANCE_NAMES
                               for j in range(3)])
    # Level 3: the subsystem hangs off the game instance.
    subsystem = None
    if game_instance:
        inner_sep = "." if ":" in game_instance else ":"
        subsystem = first_hit([f"{game_instance}{inner_sep}{SUBSYSTEM_CLASS}_{k}"
                               for k in range(3)])

    if subsystem:
        ENGINE.object_path, ENGINE.discovered_at = subsystem, time.time()
        return {"object_path": subsystem, "source": "probed", "probes": len(tried),
                "chain": {"engine": engine, "game_instance": game_instance}}

    return {
        "object_path": None, "probes": len(tried),
        "found_so_far": {"engine": engine, "game_instance": game_instance},
        "tried": tried[:8] + (["..."] if len(tried) > 8 else []),
        "hint": ("Could not walk the chain to the subsystem. If no engine object resolved at all, "
                 "the game is probably not running a game instance yet -- note the subsystem only "
                 "exists in -game or PIE, never in an editor sitting idle. Otherwise run "
                 "`obj list class=SurfTuningSubsystem` in the game console and pass the printed "
                 "path as object_path."),
    }


@tool(
    "get_live",
    "Read a coefficient's value from the running game -- the number the board "
    "is riding on this tick, after every override layer has been applied. This "
    "is ground truth; surf_tuning's `resolve` predicts it from files.",
    {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]},
)
def get_live(name: str) -> dict:
    body = _call("GetByName", {"PropertyName": name})
    value = body.get("ReturnValue") if isinstance(body, dict) else body
    return {"name": name, "value": value, "source": "running game"}


@tool(
    "set_live",
    "Set a coefficient in the running game. In effect on the next physics tick "
    "-- no relaunch, no board switch. Note it does not stay in memory only: the "
    "subsystem marks itself dirty and its debounced save writes the active "
    "board's overlay a moment later, exactly as the in-game HUD does. Use "
    "surf_tuning's `revert` to undo one.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "value": {"type": "number"},
        },
        "required": ["name", "value"],
    },
)
def set_live(name: str, value: float) -> dict:
    before = get_live(name)["value"]
    _call("SetByName", {"PropertyName": name, "NewValue": value})
    after = get_live(name)["value"]
    out = {"name": name, "before": before, "after": after,
           "in_effect": "next physics tick",
           "persists_to": "the active board's overlay, via the subsystem's debounced save"}
    if after != value:
        out["warning"] = (f"Read back {after}, not {value}. SetByName ignores names it does not "
                          "recognise as tunables -- check the spelling against list_live.")
    return out


@tool(
    "reset_live",
    "Reset a coefficient in the running game to its baseline -- what the "
    "in-game reset button would do. Baseline is the global override, else the "
    "board profile, else the compiled default; it deliberately skips the "
    "board's own overlay.",
    {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]},
)
def reset_live(name: str) -> dict:
    before = get_live(name)["value"]
    _call("ResetToDefault", {"PropertyName": name})
    return {"name": name, "before": before, "after": get_live(name)["value"],
            "note": "baseline = global ?? board profile ?? compiled default"}


@tool(
    "list_live",
    "Every tunable name the running game actually has, straight from engine "
    "reflection. Worth comparing against surf_tuning's header parse: a "
    "difference means the running build is not the tree those tools are "
    "reading.",
    {"type": "object", "properties": {
        "contains": {"type": "string", "description": "Optional case-insensitive filter."}}},
)
def list_live(contains: str = None) -> dict:
    body = _call("GetAllPropertyNames")
    names = body.get("ReturnValue") if isinstance(body, dict) else body
    names = names or []
    if contains:
        names = [n for n in names if contains.lower() in str(n).lower()]
    return {"count": len(names), "names": sorted(map(str, names))}


@tool(
    "call_function",
    "Escape hatch: call any BlueprintCallable UFUNCTION on any object in the "
    "running game, for exploring beyond the tuning subsystem. Object paths look "
    "like /Engine/Transient.GameInstance_0:SurfTuningSubsystem_0 or "
    "/Game/Maps/Map.Map:PersistentLevel.Actor_1; `obj list` in the game console "
    "prints them.",
    {
        "type": "object",
        "properties": {
            "function_name": {"type": "string"},
            "object_path": {"type": "string",
                            "description": "Defaults to the located tuning subsystem."},
            "parameters": {"type": "object", "description": "Named UFUNCTION arguments."},
            "generate_transaction": {"type": "boolean", "default": False,
                                     "description": "Wrap in an undoable editor transaction."},
        },
        "required": ["function_name"],
    },
)
def call_function(function_name: str, object_path: str = None, parameters: dict = None,
                  generate_transaction: bool = False) -> dict:
    path = object_path or _require_path()
    code, body = ENGINE.request("/remote/object/call", {
        "objectPath": path, "functionName": function_name,
        "parameters": parameters or {}, "generateTransaction": generate_transaction,
    })
    return {"object_path": path, "function": function_name,
            "http_status": code, "result": body}


def main() -> None:
    global ENGINE
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=os.environ.get("SURF_RC_HOST", DEFAULT_HOST))
    ap.add_argument("--port", type=int, default=int(os.environ.get("SURF_RC_PORT", DEFAULT_PORT)))
    # Accepted and ignored, so one .mcp.json entry shape works for every server.
    ap.add_argument("--project", help=argparse.SUPPRESS)
    args = ap.parse_args()
    ENGINE = Engine(args.host, args.port)
    mcp_lite.serve(SERVER_INFO, f"engine endpoint = http://{args.host}:{args.port}")


ENGINE = Engine(os.environ.get("SURF_RC_HOST", DEFAULT_HOST),
                int(os.environ.get("SURF_RC_PORT", DEFAULT_PORT)))

if __name__ == "__main__":
    main()
