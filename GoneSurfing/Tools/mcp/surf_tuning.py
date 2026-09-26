#!/usr/bin/env python3
"""
surf-tuning -- an MCP server over the surfing coefficients.

WHY THIS EXISTS
    USurfTuningSubsystem holds ~190 float coefficients, and the value any one of
    them actually rides at is decided by four files stacked on top of each
    other. Answering "what is yawFwdOffFaceFloor on the shortboard, and who set
    it" means reading a C++ header, a board profile, a global override file and
    a per-board overlay, and knowing which wins. Answering "why is it that
    value" means finding the comment above the declaration, which is where this
    project keeps the reasoning -- the A/B that justified it, the date, the spec.

    These tools do that reading, and can write the two layers that are safe to
    write.

HOW MCP WORKS
    See mcp_lite.py -- the protocol lives there, shared with surf_telemetry.py.
    This file is the tuning half.

THE FOUR LAYERS (most specific wins)
    1. compiled default     Source/GoneSurfing/SurfTuningSubsystem.h
    2. board profile        Content/Boards/<n>-<id>.json  "tuning": {...}
    3. global override      Saved/TuningOverrides.json
    4. board overlay        Saved/BoardTuning/<id>.json

    effective = overlay ?? global ?? profile ?? default

    But "baseline" -- what an edit is diffed against, and what the in-game
    reset button returns to -- skips the overlay:

    baseline  = global ?? profile ?? default

    That asymmetry is deliberate in the C++ (USurfTuningSubsystem::GetBaseline):
    resetting a value the global file pins returns to the global value, not the
    board's, which is the honest answer to "undo my change". The overlay is
    written sparsely against the baseline, so deleting it returns a board to
    exactly its shipped feel.

PROJECT ROOT
    --project arg, else $GONESURFING_PROJECT, else walked up from this file.
    Layers 1 and 2 are in git; layers 3 and 4 live under the gitignored Saved/,
    so a fresh worktree has the header and the profiles but no live tuning.
    Whichever tree you point at is the tree whose header is parsed -- a branch
    that added coefficients will show them and one that did not will not.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
from datetime import datetime
from pathlib import Path

import mcp_lite
from mcp_lite import dumps, tool  # dumps re-exported: handy from a REPL

SERVER_INFO = {"name": "surf-tuning", "version": "0.1.0"}

HEADER_REL = Path("Source") / "GoneSurfing" / "SurfTuningSubsystem.h"
PROFILES_REL = Path("Content") / "Boards"
GLOBAL_REL = Path("Saved") / "TuningOverrides.json"
OVERLAY_DIR_REL = Path("Saved") / "BoardTuning"


def resolve_root(cli_project: str | None) -> Path:
    if cli_project:
        return Path(cli_project).resolve()
    env = os.environ.get("GONESURFING_PROJECT")
    if env:
        return Path(env).resolve()
    return Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------------
# Layer 1 -- compiled defaults, parsed out of the C++ header
# ---------------------------------------------------------------------------

# Reflection is the runtime source of truth, but nothing here is running an
# engine. The header is the next best thing and it carries something reflection
# does not: the comment block above each declaration, which in this project is
# where the reasoning for the number lives.
_FLOAT_DECL = re.compile(r"^float\s+(\w+)\s*(?:=\s*([^;]+?))?\s*;")
_CATEGORY = re.compile(r'Category\s*=\s*"([^"]+)"')


def _parse_float_literal(text: str | None) -> float:
    if text is None:
        return 0.0
    return float(text.strip().rstrip("fF").rstrip() or 0)


_header_cache: dict[tuple, dict] = {}


def tunables() -> dict[str, dict]:
    """{name: {default, category, doc, line}} from the header."""
    path = ROOT / HEADER_REL
    if not path.exists():
        raise FileNotFoundError(
            f"no tuning header at {path}. Is the project root right? (resolved to {ROOT})")
    key = (str(path), path.stat().st_mtime_ns)
    if key in _header_cache:
        return _header_cache[key]

    out: dict[str, dict] = {}
    doc: list[str] = []
    category: str | None = None
    # A float only counts as a tunable if it is a UPROPERTY: the subsystem
    # collects them with TFieldIterator<FFloatProperty>, which walks reflected
    # properties only, so a plain private member (SaveDebounceSeconds) is not
    # tunable no matter how float it looks.
    reflected = False

    for n, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = raw.strip()
        if line.startswith("//"):
            doc.append(line[2:].strip())
            continue
        if line.startswith("UPROPERTY("):
            reflected = True
            m = _CATEGORY.search(line)
            if m:
                category = m.group(1)
            continue  # comments may sit either side of the macro; keep the block
        m = _FLOAT_DECL.match(line)
        if m and reflected:
            # Drop pure separator lines ("--- Phase 1: damping ---") from the
            # front of the block but keep any real prose that followed them.
            body = [d for d in doc if d.strip("- ")]
            out[m.group(1)] = {
                "default": _parse_float_literal(m.group(2)),
                "category": category or "",
                "doc": " ".join(body).strip(),
                "line": n,
            }
            doc, category, reflected = [], None, False
            continue
        if line and not line.startswith(("#", "{", "}")):
            doc, category, reflected = [], None, False  # other code ends the block

    _header_cache.clear()
    _header_cache[key] = out
    return out


# ---------------------------------------------------------------------------
# Layers 2-4 -- the JSON files
# ---------------------------------------------------------------------------

def _read_json(path: Path) -> dict:
    if not path.exists():
        return {}
    try:  # UE writes these, sometimes with a BOM
        return json.loads(path.read_text(encoding="utf-8-sig") or "{}")
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path} is not valid JSON: {exc}") from exc


def profiles() -> dict[str, dict]:
    """{board_id: profile dict} from Content/Boards/*.json."""
    out = {}
    d = ROOT / PROFILES_REL
    if d.exists():
        for p in sorted(d.glob("*.json")):
            data = _read_json(p)
            board_id = data.get("id") or p.stem
            data["_path"] = p
            out[board_id] = data
    return out


def resolve_board(board: str | None) -> str | None:
    """Accept the board id ('shortboard') or its filename stem ('5-shortboard')."""
    if not board:
        return None
    known = profiles()
    if board in known:
        return board
    for board_id, data in known.items():
        if Path(data["_path"]).stem == board:
            return board_id
    raise ValueError(f"unknown board {board!r}. Known: {', '.join(sorted(known)) or '(none)'}")


def global_overrides() -> dict:
    return {k: v for k, v in _read_json(ROOT / GLOBAL_REL).items() if isinstance(v, (int, float))}


def overlay_path(board_id: str) -> Path:
    return ROOT / OVERLAY_DIR_REL / f"{board_id}.json"


def overlay(board_id: str | None) -> dict:
    if not board_id:
        return {}
    return {k: v for k, v in _read_json(overlay_path(board_id)).items()
            if isinstance(v, (int, float))}


def profile_tuning(board_id: str | None) -> dict:
    if not board_id:
        return {}
    return profiles().get(board_id, {}).get("tuning", {}) or {}


def stack(name: str, board_id: str | None) -> dict:
    """The whole four-layer story for one coefficient."""
    t = tunables()
    if name not in t:
        near = [k for k in t if name.lower() in k.lower()][:8]
        raise KeyError(f"no tunable named {name!r}"
                       + (f". Did you mean: {', '.join(near)}?" if near else
                          " -- try search_tunables to find it."))
    default = t[name]["default"]
    prof = profile_tuning(board_id).get(name)
    glob = global_overrides().get(name)
    ovl = overlay(board_id).get(name)

    effective, winner = default, "default"
    for value, layer in ((prof, "profile"), (glob, "global"), (ovl, "overlay")):
        if value is not None:
            effective, winner = value, layer
    # Baseline deliberately skips the overlay -- see the module docstring.
    baseline, baseline_layer = default, "default"
    for value, layer in ((prof, "profile"), (glob, "global")):
        if value is not None:
            baseline, baseline_layer = value, layer

    return {
        "name": name, "board": board_id, "category": t[name]["category"],
        "layers": {"default": default, "profile": prof, "global": glob, "overlay": ovl},
        "effective": effective, "set_by": winner,
        "baseline": baseline, "baseline_from": baseline_layer,
    }


def _file_note(path: Path) -> dict:
    if not path.exists():
        return {"path": str(path), "exists": False}
    st = path.stat()
    return {"path": str(path), "exists": True,
            "modified": datetime.fromtimestamp(st.st_mtime).strftime("%Y-%m-%d %H:%M")}


def _layer_path(layer: str, board_id: str | None) -> Path:
    """Validate the layer and return the file it writes.

    Nothing validates a tool's arguments for us here, and the failure mode of
    not checking was silent and bad: an unexpected layer name fell through to
    "not overlay, therefore global" and wrote the global file.
    """
    if layer == "overlay":
        if not board_id:
            raise ValueError("layer 'overlay' needs a board -- pass board, or use layer 'global'.")
        return overlay_path(board_id)
    if layer == "global":
        return ROOT / GLOBAL_REL
    if layer in ("profile", "board", "default"):
        raise ValueError(
            f"layer {layer!r} is not writable here. The compiled defaults live in the C++ header "
            "and the board profiles under Content/Boards are board identity -- both belong in a "
            "commit, not in a tool call. Writable layers: 'overlay', 'global'.")
    raise ValueError(f"unknown layer {layer!r}. Writable layers: 'overlay', 'global'.")


def shadowing_overlays(name: str) -> dict:
    """Boards whose overlay pins `name`, and therefore ignore the global file."""
    return {b: overlay(b)[name] for b in profiles() if name in overlay(b)}


WRITE_WARNING = ("Values are read when a board is applied, so switch boards or relaunch "
                 "for this to take effect. If the game is running, its own debounced save "
                 "will overwrite this file from memory -- write between sessions.")


def _write_layer(path: Path, mutate) -> dict:
    """Read-modify-write one layer file, keeping a .bak of what was there."""
    before = _read_json(path)
    after = dict(before)
    mutate(after)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        shutil.copyfile(path, path.with_suffix(".json.bak"))
    path.write_text(json.dumps(after, indent=2) + "\n", encoding="utf-8", newline="\n")
    return {"keys_before": len(before), "keys_after": len(after),
            "backup": str(path.with_suffix(".json.bak")) if before else None}


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@tool(
    "list_boards",
    "The installed boards, their identity and how much tuning each carries: how "
    "many coefficients the shipped profile pins, whether a live per-board "
    "overlay exists and when it was last written. Also reports the global "
    "override file, which applies to every board. Start here.",
    {"type": "object", "properties": {}},
)
def list_boards() -> dict:
    out = []
    for board_id, data in sorted(profiles().items(), key=lambda kv: str(kv[1]["_path"])):
        out.append({
            "id": board_id,
            "display_name": data.get("displayName"),
            "tagline": data.get("tagline"),
            "ratings": data.get("ratings"),
            "assist_level": data.get("assistLevel"),
            "profile": {"file": Path(data["_path"]).name,
                        "tuning_keys": len(data.get("tuning") or {})},
            "overlay": {**_file_note(overlay_path(board_id)),
                        "keys": len(overlay(board_id))},
        })
    # Files in BoardTuning/ that are not exactly <id>.json are inert -- the game
    # only ever loads the exact name -- but they look live in a directory listing.
    inert = []
    d = ROOT / OVERLAY_DIR_REL
    if d.exists():
        live = {f"{b}.json" for b in profiles()}
        inert = sorted(p.name for p in d.glob("*.json") if p.name not in live)
    return {
        "project_root": str(ROOT),
        "header": str(ROOT / HEADER_REL),
        "tunable_count": len(tunables()),
        "boards": out,
        "global_overrides": {**_file_note(ROOT / GLOBAL_REL), "keys": len(global_overrides())},
        "inactive_overlay_files": inert,
    }


@tool(
    "search_tunables",
    "Find coefficients by name, category or by what the comment above them "
    "says. The header comments are where this project records WHY a number is "
    "what it is -- the A/B that justified it, the date, the spec -- so "
    "searching them is often faster than searching the code. Pass a board to "
    "see what that board actually rides at rather than the compiled default.",
    {
        "type": "object",
        "properties": {
            "query": {"type": "string",
                      "description": "Substring matched against name and comment. Omit to list a whole category."},
            "board": {"type": "string", "description": "Board id, e.g. 'shortboard'."},
            "category": {"type": "string",
                         "description": "Exact or partial category, e.g. 'Damping', 'Tuning|WaveMass'."},
            "changed_only": {"type": "boolean", "default": False,
                             "description": "Only coefficients some layer moves off the compiled default."},
            "limit": {"type": "integer", "default": 25},
        },
    },
)
def search_tunables(query: str = "", board: str = None, category: str = None,
                    changed_only: bool = False, limit: int = 25) -> dict:
    board_id = resolve_board(board)
    q = (query or "").lower()
    cat = (category or "").lower()
    hits = []
    for name, meta in tunables().items():
        if cat and cat not in meta["category"].lower():
            continue
        in_name = q in name.lower()
        if q and not in_name and q not in meta["doc"].lower():
            continue
        st = stack(name, board_id)
        if changed_only and st["set_by"] == "default":
            continue
        doc = meta["doc"]
        hits.append({
            "name": name,
            "category": meta["category"],
            "default": st["layers"]["default"],
            "effective": st["effective"],
            "set_by": st["set_by"],
            "why": (doc[:200] + "...") if len(doc) > 200 else doc,
            "_rank": (0 if in_name else 1, name.lower()),
        })
    hits.sort(key=lambda h: h.pop("_rank"))
    return {"board": board_id, "matched": len(hits), "returned": min(len(hits), limit),
            "results": hits[:limit],
            "note": None if len(hits) <= limit else
            f"{len(hits) - limit} more; narrow with category or a longer query."}


@tool(
    "resolve",
    "The full four-layer story for one coefficient: what each of the compiled "
    "default, board profile, global override and board overlay says, which one "
    "wins, and what the in-game reset button would return to (the baseline, "
    "which deliberately skips the overlay). Includes the complete header "
    "comment and which other boards pin the same coefficient.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string", "description": "Exact coefficient name."},
            "board": {"type": "string", "description": "Board id; omit for the no-board case."},
        },
        "required": ["name"],
    },
)
def resolve(name: str, board: str = None) -> dict:
    board_id = resolve_board(board)
    st = stack(name, board_id)
    meta = tunables()[name]
    also = {b: t[name] for b, t in ((b, profile_tuning(b)) for b in profiles()) if name in t}
    return {
        **st,
        "why": meta["doc"] or None,
        "declared_at": f"{HEADER_REL.as_posix()}:{meta['line']}",
        "set_by_other_boards": also or None,
        "files": {
            "global": _file_note(ROOT / GLOBAL_REL),
            "overlay": _file_note(overlay_path(board_id)) if board_id else None,
            "profile": str(profiles()[board_id]["_path"]) if board_id in profiles() else None,
        },
    }


@tool(
    "set_tunable",
    "Write a coefficient into the board overlay (Saved/BoardTuning/<id>.json) "
    "or the global override file (Saved/TuningOverrides.json). The shipped "
    "board profiles under Content/Boards are NOT writable here -- those are "
    "board identity and belong in a commit. Keeps a .bak of the file it "
    "changes. Values are read when a board is applied, so a switch or relaunch "
    "is needed; and a running game will overwrite these files from memory, so "
    "write between sessions.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "value": {"type": "number"},
            "board": {"type": "string",
                      "description": "Board id. Required for layer 'overlay'."},
            "layer": {"type": "string", "enum": ["overlay", "global"],
                      "description": "Defaults to 'overlay' when a board is given, else 'global'."},
        },
        "required": ["name", "value"],
    },
)
def set_tunable(name: str, value: float, board: str = None, layer: str = None) -> dict:
    if name not in tunables():
        stack(name, None)  # raises with the did-you-mean list
    board_id = resolve_board(board)
    layer = layer or ("overlay" if board_id else "global")
    path = _layer_path(layer, board_id)

    before = stack(name, board_id)
    written = _write_layer(path, lambda d: d.__setitem__(name, value))
    after = stack(name, board_id)
    result = {
        "name": name, "board": board_id, "layer": layer, "file": str(path),
        "effective_before": before["effective"], "effective_after": after["effective"],
        "baseline": after["baseline"], "baseline_from": after["baseline_from"],
        **written,
        "warning": WRITE_WARNING,
    }
    if layer == "global":
        blocked = shadowing_overlays(name)
        if blocked:
            result["shadowed_on_boards"] = {
                "boards": blocked,
                "explain": ("These boards pin this coefficient in their own overlay, which wins "
                            "over the global file, so this write does not reach them. Revert it "
                            "there, or set it per board."),
            }
    if after["set_by"] != layer:
        result["shadowed"] = (
            f"Written to '{layer}', but '{after['set_by']}' still wins for this coefficient "
            f"(effective {after['effective']}). Layer order is overlay > global > profile > default.")
    if abs(value - after["baseline"]) < 1e-9:
        result["note"] = ("This equals the baseline, so the game's next sparse save will drop "
                          "the key entirely -- which is the same as reverting it.")
    return result


@tool(
    "revert",
    "Remove a coefficient from a writable layer, so the layer beneath it takes "
    "over again. Pass name '*' to clear the whole layer -- for a board overlay "
    "that returns the board to exactly its shipped feel. Keeps a .bak.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string", "description": "Coefficient name, or '*' for the whole layer."},
            "board": {"type": "string"},
            "layer": {"type": "string", "enum": ["overlay", "global"],
                      "description": "Defaults to 'overlay' when a board is given, else 'global'."},
        },
        "required": ["name"],
    },
)
def revert(name: str, board: str = None, layer: str = None) -> dict:
    board_id = resolve_board(board)
    layer = layer or ("overlay" if board_id else "global")
    path = _layer_path(layer, board_id)
    current = _read_json(path)

    if name == "*":
        removed = sorted(current)
        written = _write_layer(path, lambda d: d.clear())
    else:
        if name not in current:
            return {"name": name, "layer": layer, "file": str(path), "removed": [],
                    "note": f"'{name}' was not set in the {layer} layer; nothing to remove."}
        removed = [name]
        written = _write_layer(path, lambda d: d.pop(name, None))

    out = {"board": board_id, "layer": layer, "file": str(path),
           "removed": removed, **written, "warning": WRITE_WARNING}
    if name != "*":
        st = stack(name, board_id)
        out["effective_now"] = st["effective"]
        out["set_by"] = st["set_by"]
    return out


@tool(
    "diff",
    "Where two boards actually differ, or where one board differs from the "
    "compiled defaults. Compares effective values across every coefficient and "
    "reports only what differs, with the layer each side got its value from -- "
    "so a difference that comes from a stray overlay is distinguishable from "
    "one that is the board's shipped identity. Pass 'defaults' for either side.",
    {
        "type": "object",
        "properties": {
            "a": {"type": "string", "description": "Board id, or 'defaults'."},
            "b": {"type": "string", "description": "Board id, or 'defaults'."},
            "category": {"type": "string", "description": "Restrict to one category."},
        },
        "required": ["a", "b"],
    },
)
def diff(a: str, b: str, category: str = None) -> dict:
    def side(x):
        return None if x.lower() in ("defaults", "default", "none") else resolve_board(x)

    a_id, b_id = side(a), side(b)
    cat = (category or "").lower()
    rows = []
    for name, meta in tunables().items():
        if cat and cat not in meta["category"].lower():
            continue
        sa, sb = stack(name, a_id), stack(name, b_id)
        if abs(sa["effective"] - sb["effective"]) < 1e-9:
            continue
        rows.append({
            "name": name, "category": meta["category"],
            "a": sa["effective"], "a_from": sa["set_by"],
            "b": sb["effective"], "b_from": sb["set_by"],
            "ratio": round(sb["effective"] / sa["effective"], 3) if sa["effective"] else None,
        })
    rows.sort(key=lambda r: (r["category"], r["name"]))
    return {"a": a_id or "defaults", "b": b_id or "defaults",
            "compared": len(tunables()), "differing": len(rows), "differences": rows}


ROOT = resolve_root(None)


def main() -> None:
    global ROOT
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--project", help="Path to the GoneSurfing project directory.")
    args = ap.parse_args()
    ROOT = resolve_root(args.project)
    mcp_lite.serve(SERVER_INFO, f"project root = {ROOT}")


if __name__ == "__main__":
    main()
