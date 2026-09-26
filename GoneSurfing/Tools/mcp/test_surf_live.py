#!/usr/bin/env python3
"""
Smoke test for surf_live.py, against a mock Remote Control server.

The real endpoint is Unreal's RemoteControl plugin inside a running game. This
stands up a tiny HTTP server that speaks the same three routes over a dict, so
everything except the engine itself is exercised: the probe that locates the
subsystem, the kept-alive connection, the call payloads, read-back after write,
and what happens when the game is not there at all.

Passing this does NOT prove it works against a real engine -- only that the
client half is right. The engine half is verified by running it.

    python3 test_surf_live.py [--verbose]
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVER = HERE / "surf_live.py"

# The real shape, confirmed against a running engine on 2026-09-11:
#   /Engine/Transient.GameEngine_0:GameInstance_0.SurfTuningSubsystem_0
# Non-zero indices here so the test proves the probe actually searches, and the
# intermediate levels resolve too so it exercises the chain walk rather than a
# single lucky string.
MOCK_ENGINE = "/Engine/Transient.GameEngine_0"
MOCK_GI = MOCK_ENGINE + ":GameInstance_1"
MOCK_PATH = MOCK_GI + ".SurfTuningSubsystem_2"
MOCK_RESOLVABLE = {MOCK_ENGINE, MOCK_GI, MOCK_PATH}

MOCK_TUNABLES = {
    "AngularDampingZ": 0.08,
    "lateralTurnCoefficient": 4000.0,
    "slopeThrustCoefficient": 30000.0,
}
MOCK_BASELINE = dict(MOCK_TUNABLES)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # keep the test output clean
        pass

    def _send(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/remote/info":
            # Shaped like the real answer: a route table, no Name/Version.
            return self._send(200, {"HttpRoutes": [
                {"Path": "/remote/object/call", "Verb": "Put"},
                {"Path": "/remote/object/describe", "Verb": "Put"},
                {"Path": "/remote/info", "Verb": "Get"},
            ], "ActivePreset": {"Name": "None"}})
        self._send(404, {"errorMessage": "no such route"})

    def do_PUT(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n) or "{}")

        if self.path == "/remote/object/describe":
            path = req.get("objectPath")
            if path in MOCK_RESOLVABLE:
                return self._send(200, {"Name": path.rsplit(".", 1)[-1]})
            return self._send(404, {"errorMessage": "object not found"})

        if self.path == "/remote/object/call":
            if req.get("objectPath") != MOCK_PATH:
                return self._send(404, {"errorMessage": "object not found"})
            fn = req.get("functionName")
            p = req.get("parameters") or {}
            if fn == "GetByName":
                return self._send(200, {"ReturnValue": MOCK_TUNABLES.get(p.get("PropertyName"), 0.0)})
            if fn == "SetByName":
                # Mirrors the C++: unknown names are logged and ignored, not an error.
                if p.get("PropertyName") in MOCK_TUNABLES:
                    MOCK_TUNABLES[p["PropertyName"]] = p.get("NewValue")
                return self._send(200, {})
            if fn == "ResetToDefault":
                name = p.get("PropertyName")
                if name in MOCK_TUNABLES:
                    MOCK_TUNABLES[name] = MOCK_BASELINE[name]
                return self._send(200, {})
            if fn == "GetAllPropertyNames":
                return self._send(200, {"ReturnValue": sorted(MOCK_TUNABLES)})
            return self._send(400, {"errorMessage": f"no function {fn}"})

        self._send(404, {"errorMessage": "no such route"})


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    # Accepted and ignored, so one command shape runs all three suites.
    ap.add_argument("--project", help=argparse.SUPPRESS)
    args = ap.parse_args()

    # Imported after argv is parsed so the shared client picks up nothing odd.
    from mcp_test_client import Client, check, report

    print("=== game not running ===")
    dead_port = free_port()
    os.environ["SURF_RC_PORT"] = str(dead_port)
    c = Client(SERVER, None, args.verbose)
    c.handshake("surf-live")
    st = c.call("status")
    check("reports unreachable rather than hanging", st["reachable"] is False, st)
    check("explains how to switch the engine's server on",
          any("RCWebControlEnable" in h for h in st.get("how_to_enable", [])), st)
    err = c.call_raw("get_live", name="AngularDampingZ")
    check("a tool call with no game is an isError, not a crash",
          err.get("isError") is True and "not running" in err["content"][0]["text"],
          err["content"][0]["text"][:200])
    c.close()

    print("\n=== mock engine ===")
    port = free_port()
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    os.environ["SURF_RC_PORT"] = str(port)
    c = Client(SERVER, None, args.verbose)
    c.handshake("surf-live")

    tools = c.request("tools/list")["result"]["tools"]
    names = sorted(t["name"] for t in tools)
    check("seven tools advertised", len(tools) == 7, names)
    print("        " + str(names))

    st = c.call("status")
    check("reachable", st["reachable"] is True, st)
    check("reports that the routes it needs are present",
          st.get("engine", {}).get("required_routes_present") is True, st)

    print("\nlocating the subsystem")
    f = c.call("find_subsystem")
    check("probe finds a path that is not the first candidate",
          f["object_path"] == MOCK_PATH, f)
    check("probing actually searched", f["probes"] > 1, f)
    check("it walked the chain rather than guessing a whole string",
          f["chain"]["engine"] == MOCK_ENGINE and f["chain"]["game_instance"] == MOCK_GI, f)
    print("        found %s after %d probes" % (f["object_path"], f["probes"]))
    again = c.call("find_subsystem")
    check("second call is served from session state, not re-probed",
          again["source"] == "cached", again)
    forced = c.call("find_subsystem", force=True)
    check("force re-probes", forced["source"] == "probed", forced)
    supplied = c.call("find_subsystem", object_path=MOCK_PATH)
    check("an explicitly supplied path is validated and accepted",
          supplied["source"] == "supplied", supplied)
    bad = c.call("find_subsystem", object_path="/Engine/Transient.Nope_0:Nope_0")
    check("a bad supplied path is rejected with a hint",
          bad["object_path"] is None and "did not resolve" in bad["hint"], bad)

    print("\nread / write / reset")
    g = c.call("get_live", name="lateralTurnCoefficient")
    check("reads the live value", g["value"] == 4000.0, g)
    s = c.call("set_live", name="lateralTurnCoefficient", value=5400.0)
    check("write reports before and after", s["before"] == 4000.0 and s["after"] == 5400.0, s)
    check("says when it takes effect", s["in_effect"] == "next physics tick", s)
    check("warns that it persists to the overlay", "persists_to" in s, s)
    check("read-back confirms the write",
          c.call("get_live", name="lateralTurnCoefficient")["value"] == 5400.0)
    r = c.call("reset_live", name="lateralTurnCoefficient")
    check("reset returns to baseline", r["after"] == 4000.0, r)

    print("\nthe C++ quirk: SetByName ignores unknown names")
    q = c.call("set_live", name="notARealCoefficient", value=1.0)
    check("a silently-ignored write is surfaced as a warning",
          "warning" in q and "does not recognise" in q["warning"], q)

    print("\nlist_live and the escape hatch")
    li = c.call("list_live")
    check("lists every tunable the engine reports", li["count"] == len(MOCK_TUNABLES), li)
    filt = c.call("list_live", contains="damping")
    check("filter works", filt["names"] == ["AngularDampingZ"], filt)
    cf = c.call("call_function", function_name="GetAllPropertyNames")
    check("escape hatch reaches an arbitrary function", cf["http_status"] == 200, cf)
    miss = c.call("call_function", function_name="NoSuchFunction")
    check("a failing call reports the status instead of raising",
          miss["http_status"] == 400, miss)

    print("\nthe engine going away mid-session")
    httpd.shutdown()
    gone = c.call_raw("get_live", name="AngularDampingZ")
    check("a dropped connection is an actionable error",
          gone.get("isError") is True and "status" in gone["content"][0]["text"],
          gone["content"][0]["text"][:200])
    c.close()
    return report()


if __name__ == "__main__":
    sys.exit(main())
