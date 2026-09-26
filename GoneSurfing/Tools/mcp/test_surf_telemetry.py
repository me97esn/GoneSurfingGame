#!/usr/bin/env python3
"""
Smoke test for surf_telemetry.py -- and a readable transcript of the protocol.

Spawns the server exactly the way an MCP client does (see mcp_test_client.py),
runs the handshake, then calls every tool and checks the shape of what comes
back. No MCP client needed.

    python3 test_surf_telemetry.py [--project <GoneSurfing dir>] [--verbose]

Exit 0 = all good. Any failure prints the check that broke.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from mcp_test_client import Client, check, report

HERE = Path(__file__).resolve().parent
SERVER = HERE / "surf_telemetry.py"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    c = Client(SERVER, args.project, args.verbose)

    print("handshake")
    init = c.request("initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "smoke-test", "version": "0"},
    })
    check("initialize returns serverInfo",
          init.get("result", {}).get("serverInfo", {}).get("name") == "surf-telemetry", init)
    check("initialize echoes protocolVersion",
          init["result"]["protocolVersion"] == "2024-11-05")
    check("declares the tools capability", "tools" in init["result"]["capabilities"])
    c.notify("notifications/initialized")

    ping = c.request("ping")
    check("ping answers (proves the notification was not answered)", ping.get("result") == {})

    bad = c.request("nonsense/method")
    check("unknown method -> JSON-RPC error -32601", bad.get("error", {}).get("code") == -32601)

    print("\ntools/list")
    tools = c.request("tools/list")["result"]["tools"]
    names = sorted(t["name"] for t in tools)
    check("five tools advertised", len(tools) == 5, names)
    check("each has a description and a schema",
          all(t.get("description") and t.get("inputSchema", {}).get("type") == "object"
              for t in tools))
    print("        " + str(names))

    print("\nlist_runs")
    runs = c.call("list_runs", source="all")
    check("found runs", runs["count"] > 0, "project_root=" + runs["project_root"])
    if runs["count"] == 0:
        print("\n  No CSVs under the resolved project root -- pass --project pointing at a "
              "tree that has Saved/Tests/latest data.")
        c.close()
        return 1
    check("runs carry age and staleness",
          all("age_days" in x and "stale" in x for x in runs["runs"]))
    baselines = [x["name"] for x in runs["runs"] if x["source"] == "baselines"]
    latest = [x["name"] for x in runs["runs"] if x["source"] == "latest"]
    print("        %d latest, %d baselines, %d stale"
          % (len(latest), len(baselines), sum(1 for x in runs["runs"] if x["stale"])))

    subject = next((n for n in latest if n in baselines), latest[0])
    print("\nrun_summary(%s)" % subject)
    s = c.call("run_summary", name=subject)
    check("speed stats present", s["speed_cms"]["median"] is not None)
    check("slope gate fractions present",
          set(s["slope_sin"]["fraction_above_gate"]) == {"0.12", "0.3", "0.35"},
          s["slope_sin"]["fraction_above_gate"])
    check("per-step breakdown present", len(s["steps"]) >= 1)
    check("distance >= net displacement", s["distance_cm"] >= s["net_displacement_cm"])
    print("        %ss, median %s cm/s, planing %s, above slope 0.12: %s"
          % (s["duration_s"], s["speed_cms"]["median"], s["planing"]["fraction_above_0.5"],
             s["slope_sin"]["fraction_above_gate"]["0.12"]))

    print("\nwindow(%s) -- a 3 s slice" % subject)
    t_mid = s["duration_s"] / 2
    w = c.call("window", name=subject, t0=t_mid, t1=t_mid + 3, columns=["t", "speed", "pitch"])
    check("returns only the requested columns", w["columns"] == ["t", "speed", "pitch"])
    check("rows are inside the window",
          all(t_mid - 0.1 <= row[0] <= t_mid + 3.1 for row in w["rows"]), w["t_range"])
    check("respects max_rows", len(c.call("window", name=subject, max_rows=10)["rows"]) <= 10)

    print("\nfind_events(%s)" % subject)
    ev = c.call("find_events", name=subject, kinds=["turn", "stall", "step"])
    check("events are time-ordered",
          all(a["t0"] <= b["t0"] for a, b in zip(ev["events"], ev["events"][1:])))
    check("events stay inside the run",
          all(e["t0"] <= ev["duration_s"] + 0.1 for e in ev["events"]))
    kinds_found = sorted({e["kind"] for e in ev["events"]})
    print("        %d events %s%s" % (ev["count"], kinds_found,
          (", turn rollup " + str(ev["turn_rollup"])) if "turn_rollup" in ev else ""))

    if subject in baselines:
        print("\ncompare(%s)" % subject)
        cmp_result = c.call("compare", name=subject)
        check("verdict is one of the three",
              cmp_result["verdict"] in ("OK", "WARN", "REGRESSION"))
        check("per-step drift present", len(cmp_result["per_step"]) >= 1)
        print("        %s: max pos %scm @ t=%ss step %s"
              % (cmp_result["verdict"], cmp_result["position_cm"]["max"],
                 cmp_result["position_cm"]["max_at_t"],
                 cmp_result["position_cm"]["max_at_step"]))
    else:
        print("\ncompare -- skipped, no baseline named " + subject)

    print("\nerror handling")
    missing = c.request("tools/call",
                        {"name": "run_summary", "arguments": {"name": "no-such-run"}})
    check("missing run -> isError with a helpful message",
          missing["result"].get("isError") is True
          and "Available" in missing["result"]["content"][0]["text"],
          missing["result"]["content"][0]["text"])
    unknown = c.request("tools/call", {"name": "no_such_tool", "arguments": {}})
    check("unknown tool -> JSON-RPC error", "error" in unknown)

    c.close()
    return report()


if __name__ == "__main__":
    sys.exit(main())
