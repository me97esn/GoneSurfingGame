#!/usr/bin/env python3
"""
Smoke test for surf_tuning.py.

Two halves:

  READ  -- against a real project tree (--project), because the interesting
           parts are the real header, the real board profiles and whatever
           tuning is live. Nothing here writes.

  WRITE -- against a throwaway sandbox built in a temp directory: a synthetic
           header, one board profile, an empty Saved/. The write path is never
           pointed at a real project, because set_tunable and revert change the
           files the game actually reads.

    python3 test_surf_tuning.py [--project <GoneSurfing dir>] [--verbose]
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

from mcp_test_client import Client, check, report

HERE = Path(__file__).resolve().parent
SERVER = HERE / "surf_tuning.py"

SANDBOX_HEADER = """#pragma once
UCLASS()
class GONESURFING_API USurfTuningSubsystem : public UGameInstanceSubsystem
{
GENERATED_BODY()
public:
// Bumped 0.04 -> 0.08 after the board kept yawing into the wave. See
// specs/framerate-independent-angular-damping.md.
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Damping")
float AngularDampingZ = 0.08f;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
float lateralTurnCoefficient = 4000.f;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Thrust")
// A comment below the macro, which is a real pattern in the header.
float slopeThrustCoefficient = 30000.0f;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Pump")
float PumpReleaseSeconds = 0.35f;

private:
// Not a UPROPERTY, so the engine's reflection never sees it and it is not
// a tunable. The parser must agree.
float SaveDebounceSeconds = 0.0f;
};
"""

SANDBOX_BOARD = {
    "id": "testboard",
    "displayName": "Test Board",
    "tagline": "Exists only in a temp directory.",
    "ratings": {"speed": 3, "turning": 3, "difficulty": 3},
    "tuning": {"lateralTurnCoefficient": 5400.0, "AngularDampingZ": 0.1},
}


def build_sandbox(root: Path) -> None:
    (root / "Source" / "GoneSurfing").mkdir(parents=True)
    (root / "Source" / "GoneSurfing" / "SurfTuningSubsystem.h").write_text(
        SANDBOX_HEADER, encoding="utf-8")
    (root / "Content" / "Boards").mkdir(parents=True)
    (root / "Content" / "Boards" / "1-testboard.json").write_text(
        json.dumps(SANDBOX_BOARD, indent=2), encoding="utf-8")
    (root / "Saved").mkdir()


def read_half(project, verbose) -> None:
    c = Client(SERVER, project, verbose)
    c.handshake("surf-tuning")

    print("tools/list")
    tools = c.request("tools/list")["result"]["tools"]
    names = sorted(t["name"] for t in tools)
    check("six tools advertised", len(tools) == 6, names)
    print("        " + str(names))

    print("\nlist_boards")
    boards = c.call("list_boards")
    check("found tunables in the header", boards["tunable_count"] > 50,
          "header=" + boards["header"])
    check("found boards", len(boards["boards"]) > 0, boards["project_root"])
    if not boards["boards"]:
        print("\n  No Content/Boards in the resolved root -- pass --project.")
        c.close()
        return
    ids = [b["id"] for b in boards["boards"]]
    print("        %d tunables, boards %s" % (boards["tunable_count"], ids))
    if boards["inactive_overlay_files"]:
        print("        inert overlay files: %s" % boards["inactive_overlay_files"])

    print("\nsearch_tunables('damping')")
    found = c.call("search_tunables", query="damping", limit=5)
    check("name matches rank above comment matches",
          all("damping" in r["name"].lower() for r in found["results"][:2]),
          [r["name"] for r in found["results"][:3]])
    check("every hit carries its category and default",
          all(r["category"] and "default" in r for r in found["results"]))
    print("        %d matched, e.g. %s" % (found["matched"],
          [r["name"] for r in found["results"][:3]]))

    print("\nresolve on every layer that is actually in use")
    board = ids[-1]
    covered = set()
    for b in ids:
        for name in c.call("search_tunables", board=b, changed_only=True,
                           limit=200)["results"]:
            st = c.call("resolve", name=name["name"], board=b)
            covered.add(st["set_by"])
            check_once = (st["effective"] == st["layers"][st["set_by"]]
                          if st["set_by"] != "default" else True)
            if not check_once:
                check("effective matches its winning layer (%s)" % name["name"], False, st)
                break
    check("effective always equals the winning layer's value", True)
    check("layer resolution exercised on real data", "profile" in covered, covered)
    print("        layers seen in use: %s" % sorted(covered))

    print("\nbaseline skips the overlay (the C++ GetBaseline rule)")
    overlaid = [b for b in boards["boards"] if b["overlay"]["keys"] > 0]
    if overlaid:
        b = overlaid[0]["id"]
        for r in c.call("search_tunables", board=b, changed_only=True, limit=200)["results"]:
            st = c.call("resolve", name=r["name"], board=b)
            if st["set_by"] == "overlay":
                check("overlay wins effective but not baseline (%s on %s)" % (r["name"], b),
                      st["effective"] == st["layers"]["overlay"]
                      and st["baseline"] != st["layers"]["overlay"]
                      and st["baseline_from"] in ("profile", "global", "default"), st)
                print("        %s: effective %s (overlay) vs baseline %s (%s)"
                      % (r["name"], st["effective"], st["baseline"], st["baseline_from"]))
                break
    else:
        print("        skipped -- no board has a live overlay in this tree")

    print("\ndiff")
    if len(ids) >= 2:
        d = c.call("diff", a=ids[0], b=ids[-1])
        check("diff reports only differences", all(r["a"] != r["b"] for r in d["differences"]))
        check("diff attributes each side to a layer",
              all(r["a_from"] and r["b_from"] for r in d["differences"]))
        print("        %s vs %s: %d of %d differ" % (ids[0], ids[-1], d["differing"], d["compared"]))
    same = c.call("diff", a=ids[0], b=ids[0])
    check("a board does not differ from itself", same["differing"] == 0)

    print("\nerror handling")
    bad = c.call_raw("resolve", name="noSuchCoefficient")
    check("unknown coefficient -> isError", bad.get("isError") is True)
    typo = c.call_raw("resolve", name="angulardampingz")
    check("near-miss name suggests the real one",
          typo.get("isError") is True and "Did you mean" in typo["content"][0]["text"],
          typo["content"][0]["text"][:160])
    board_err = c.call_raw("search_tunables", board="not-a-board")
    check("unknown board -> isError listing the real ones",
          board_err.get("isError") is True and "Known:" in board_err["content"][0]["text"])
    c.close()


def write_half(verbose) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "GoneSurfing"
        build_sandbox(root)
        c = Client(SERVER, root, verbose)
        c.handshake("surf-tuning")

        print("\nsandbox: parsing")
        t = c.call("list_boards")
        check("four tunables parsed, the private member excluded",
              t["tunable_count"] == 4, t["tunable_count"])
        check("board profile read", t["boards"][0]["id"] == "testboard")

        r = c.call("resolve", name="slopeThrustCoefficient", board="testboard")
        check("comment below the UPROPERTY macro is still captured",
              r["why"] and "real pattern" in r["why"], r["why"])
        check("float literal '30000.0f' parsed", r["layers"]["default"] == 30000.0)
        check("'4000.f' style literal parsed",
              c.call("resolve", name="lateralTurnCoefficient")["layers"]["default"] == 4000.0)

        print("\nsandbox: set_tunable into the board overlay")
        before = c.call("resolve", name="AngularDampingZ", board="testboard")
        check("profile wins before any overlay",
              before["set_by"] == "profile" and before["effective"] == 0.1, before)
        w = c.call("set_tunable", name="AngularDampingZ", value=0.25, board="testboard")
        check("write reports the effective change", w["effective_before"] == 0.1
              and w["effective_after"] == 0.25, w)
        check("baseline stays the profile value", w["baseline"] == 0.1
              and w["baseline_from"] == "profile", w)
        check("overlay file created at Saved/BoardTuning/<id>.json",
              (root / "Saved" / "BoardTuning" / "testboard.json").exists())
        after = c.call("resolve", name="AngularDampingZ", board="testboard")
        check("overlay now wins", after["set_by"] == "overlay" and after["effective"] == 0.25)

        print("\nsandbox: the global layer shadows the overlay? (it must not)")
        g = c.call("set_tunable", name="AngularDampingZ", value=0.9, layer="global")
        check("global write names the boards whose overlay ignores it",
              "shadowed_on_boards" in g
              and "testboard" in g["shadowed_on_boards"]["boards"], g)
        st = c.call("resolve", name="AngularDampingZ", board="testboard")
        check("overlay still wins over global", st["effective"] == 0.25)
        check("but baseline moves to the global value",
              st["baseline"] == 0.9 and st["baseline_from"] == "global", st)

        print("\nsandbox: writing the baseline value is flagged as a no-op")
        n = c.call("set_tunable", name="PumpReleaseSeconds", value=0.35, board="testboard")
        check("equals-baseline write is called out", "note" in n, n)

        print("\nsandbox: revert")
        c.call("set_tunable", name="lateralTurnCoefficient", value=9999, board="testboard")
        rv = c.call("revert", name="AngularDampingZ", board="testboard")
        check("revert removes just that key", rv["removed"] == ["AngularDampingZ"], rv)
        check("the layer beneath takes over -- global, not profile",
              rv["effective_now"] == 0.9 and rv["set_by"] == "global", rv)
        check("a .bak was kept", rv["backup"] and Path(rv["backup"]).exists())
        noop = c.call("revert", name="AngularDampingZ", board="testboard")
        check("reverting an unset key is a no-op, not an error", noop["removed"] == [])

        print("\nsandbox: revert '*' clears the layer")
        star = c.call("revert", name="*", board="testboard")
        check("all remaining overlay keys removed", len(star["removed"]) >= 1, star)
        check("overlay is now empty",
              json.loads((root / "Saved" / "BoardTuning" / "testboard.json").read_text()) == {})
        back = c.call("resolve", name="lateralTurnCoefficient", board="testboard")
        check("board is back to its shipped profile value",
              back["effective"] == 5400.0 and back["set_by"] == "profile", back)

        print("\nsandbox: refusals")
        bad_layer = c.call_raw("set_tunable", name="AngularDampingZ", value=1, layer="profile")
        check("cannot write the shipped board profile", bad_layer.get("isError") is True)
        no_board = c.call_raw("set_tunable", name="AngularDampingZ", value=1, layer="overlay")
        check("overlay layer without a board -> isError",
              no_board.get("isError") is True and "needs a board" in no_board["content"][0]["text"])
        unknown = c.call_raw("set_tunable", name="notATunable", value=1, layer="global")
        check("cannot write an unknown coefficient", unknown.get("isError") is True)
        c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    print("=== READ (real project, nothing written) ===")
    read_half(args.project, args.verbose)
    print("\n=== WRITE (throwaway sandbox) ===")
    write_half(args.verbose)
    return report()


if __name__ == "__main__":
    sys.exit(main())
