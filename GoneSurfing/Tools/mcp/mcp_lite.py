#!/usr/bin/env python3
"""
mcp_lite -- the MCP protocol, and nothing else.

An MCP server is a subprocess. The client (Claude Code) writes JSON-RPC 2.0
requests to its stdin, one compact JSON object per line, and reads responses the
same way off its stdout. That is the entire "stdio transport".

The handshake and the three calls that matter:

    client -> initialize                 "what are you, what version"
    server -> {protocolVersion, capabilities, serverInfo}
    client -> notifications/initialized  (a notification: no id, no reply)
    client -> tools/list                 "what can you do"
    server -> [{name, description, inputSchema}, ...]
    client -> tools/call {name, arguments}
    server -> {content: [{type: "text", text: "..."}]}

That is it. Everything a server adds on top is domain logic.

THE ONE RULE: stdout belongs to the protocol. A stray print() corrupts the
stream and the client drops the connection. Diagnostics go to stderr.

No dependencies on purpose -- the point is that the protocol stays visible.

To write a server:

    import mcp_lite
    from mcp_lite import tool

    @tool("add", "Add two numbers.", {
        "type": "object",
        "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
        "required": ["a", "b"],
    })
    def add(a, b):
        return {"sum": a + b}

    mcp_lite.serve({"name": "calculator", "version": "0.1.0"})
"""

from __future__ import annotations

import json
import re
import sys

PROTOCOL_FALLBACK = "2024-11-05"

TOOLS: dict[str, dict] = {}


def log(msg: str, prefix: str = "mcp") -> None:
    """Diagnostics go to stderr. Never stdout -- see THE ONE RULE above."""
    print(f"[{prefix}] {msg}", file=sys.stderr, flush=True)


def send(msg: dict) -> None:
    sys.stdout.write(json.dumps(msg, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def reply(req_id, result: dict) -> None:
    send({"jsonrpc": "2.0", "id": req_id, "result": result})


def fail(req_id, code: int, message: str) -> None:
    send({"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}})


def tool(name: str, description: str, schema: dict):
    """Register a function as an MCP tool.

    `description` and `inputSchema` are the entire contract the model sees --
    they are the prompt for this tool, so they carry the units and the caveats.
    """
    def deco(fn):
        TOOLS[name] = {"name": name, "description": description,
                       "inputSchema": schema, "fn": fn}
        return fn
    return deco


# Numbers-only arrays collapsed onto one line. json.dumps(indent=2) would put
# every element of a [t, speed, pitch] row on its own line, which turns a
# 40-sample window into 280 lines of mostly punctuation.
_NUM = r"-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?|null"
_NUM_ARRAY = re.compile(r"\[\s*((?:" + _NUM + r")(?:\s*,\s*(?:" + _NUM + r"))*)\s*\]")


def dumps(obj) -> str:
    text = json.dumps(obj, indent=2, default=str)
    return _NUM_ARRAY.sub(lambda m: "[" + re.sub(r"\s+", " ", m.group(1)).strip() + "]", text)


def handle(msg: dict, server_info: dict) -> None:
    method = msg.get("method")
    req_id = msg.get("id")

    # Notifications have no id and must never be answered.
    if req_id is None:
        return

    if method == "initialize":
        # Echo the client's protocol version when it looks like one we can
        # speak; the handshake is otherwise version-agnostic for plain tools.
        asked = (msg.get("params") or {}).get("protocolVersion")
        version = asked if isinstance(asked, str) and asked[:2] == "20" else PROTOCOL_FALLBACK
        reply(req_id, {
            "protocolVersion": version,
            "capabilities": {"tools": {}},
            "serverInfo": server_info,
        })
    elif method == "ping":
        reply(req_id, {})
    elif method == "tools/list":
        reply(req_id, {"tools": [
            {k: t[k] for k in ("name", "description", "inputSchema")}
            for t in TOOLS.values()
        ]})
    elif method == "tools/call":
        params = msg.get("params") or {}
        entry = TOOLS.get(params.get("name"))
        if entry is None:
            fail(req_id, -32602, f"unknown tool: {params.get('name')}")
            return
        try:
            result = entry["fn"](**(params.get("arguments") or {}))
            reply(req_id, {"content": [{"type": "text", "text": dumps(result)}]})
        except Exception as exc:  # a tool error is a result, not a protocol error
            reply(req_id, {
                "content": [{"type": "text", "text": f"{type(exc).__name__}: {exc}"}],
                "isError": True,
            })
    else:
        fail(req_id, -32601, f"method not found: {method}")


def serve(server_info: dict, ready_note: str = "") -> None:
    # Windows text mode would turn every \n into \r\n. Harmless for most
    # clients, but the framing is line-based, so keep it exactly as written.
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    sys.stdin.reconfigure(encoding="utf-8")
    log(f"ready. {ready_note}".strip(), server_info.get("name", "mcp"))
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            log(f"dropped unparseable line: {line[:120]}", server_info.get("name", "mcp"))
            continue
        handle(msg, server_info)
