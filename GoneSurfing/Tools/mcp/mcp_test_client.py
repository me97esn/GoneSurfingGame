#!/usr/bin/env python3
"""
The client half of the stdio transport, for driving a server under test.

This is what an MCP client does, in about twenty lines: spawn the server as a
subprocess, write one JSON-RPC object per line to its stdin, read one back off
its stdout. Shared by test_surf_telemetry.py and test_surf_tuning.py.
"""

from __future__ import annotations

import json
import subprocess
import sys


class Client:
    def __init__(self, server_path, project=None, verbose=False):
        cmd = [sys.executable, str(server_path)]
        if project:
            cmd += ["--project", str(project)]
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1,
        )
        self.next_id = 0
        self.verbose = verbose

    def request(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed the stream on " + method +
                               ". stderr: " + self.proc.stderr.read())
        resp = json.loads(line)
        if self.verbose:
            print("  -> " + method + " " + json.dumps(params or {})[:100])
            print("  <- " + json.dumps(resp)[:400])
        return resp

    def notify(self, method):
        # No id, so the server must not answer. If it does, every later read is
        # off by one -- which is why the tests send one.
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def call(self, _tool, **args):
        # Leading underscore so a tool argument literally called "name" does not
        # collide with this parameter.
        resp = self.request("tools/call", {"name": _tool, "arguments": args})
        result = resp.get("result", {})
        text = result.get("content", [{}])[0].get("text", "")
        if result.get("isError"):
            raise AssertionError("tool " + _tool + " errored: " + text)
        return json.loads(text)

    def call_raw(self, _tool, **args):
        """Like call(), but hands back the envelope so a test can assert on isError."""
        return self.request("tools/call", {"name": _tool, "arguments": args})["result"]

    def handshake(self, expect_name):
        init = self.request("initialize", {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "smoke-test", "version": "0"},
        })
        self.notify("notifications/initialized")
        assert init["result"]["serverInfo"]["name"] == expect_name, init
        return init["result"]

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


FAILURES = []


def check(label, condition, detail=""):
    if condition:
        print("  PASS  " + label)
    else:
        print("  FAIL  " + label + "  " + str(detail)[:300])
        FAILURES.append(label)


def report():
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), FAILURES))
        return 1
    print("all checks passed")
    return 0
