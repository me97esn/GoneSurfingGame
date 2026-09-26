#!/usr/bin/env python3
"""Listener for the phone's SEND panel (specs/share-trace-from-phone.md).

The game POSTs a ride trace CSV to /trace with X-Trace-Name = its filename; this saves it under
Saved/InputTraces/fromphone/ and appends a line to fromphone/arrivals.log. A subfolder, not the
InputTraces root: the game's retention prune (surf.traces.prune) sweeps the root and would delete a
phone trace once it aged past the grace period, and the PC's own recordings live there.

    python Tools/TraceShareServer.py            # port 8765, project = this repo
    python Tools/TraceShareServer.py --port 9000 --project E:/path/to/GoneSurfing

Reachable over Tailscale from anywhere: bind is 0.0.0.0, and Windows Firewall needs the port open
on the Tailscale interface once (Tools/TraceShareServer.ps1 does that and prints the address).

Stdlib only, like the MCP servers next to it. GET / answers with a one-line status so a browser
on the phone can confirm the route before a ride is wasted on it.
"""
from __future__ import annotations

import argparse
import re
import sys
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAX_BYTES = 32 * 1024 * 1024          # the longest trace seen is ~1.5 MB; anything near this is not a trace
NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,120}\.csv$")


def make_handler(dest: Path):
    dest.mkdir(parents=True, exist_ok=True)
    log = dest / "arrivals.log"

    class Handler(BaseHTTPRequestHandler):
        server_version = "TraceShare/1"

        def _reply(self, code: int, text: str) -> None:
            body = (text + "\n").encode()
            self.send_response(code)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802 - http.server naming
            n = len(list(dest.glob("*.csv")))
            self._reply(200, f"TraceShare listening. {n} trace(s) in {dest}")

        def do_POST(self) -> None:  # noqa: N802
            if self.path.rstrip("/") != "/trace":
                self._reply(404, "POST /trace")
                return
            name = self.headers.get("X-Trace-Name", "")
            if not NAME_RE.match(name):
                self._reply(400, f"bad X-Trace-Name: {name!r}")
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if length <= 0 or length > MAX_BYTES:
                self._reply(413 if length > MAX_BYTES else 400, f"bad Content-Length: {length}")
                return
            data = self.rfile.read(length)
            if len(data) != length:
                self._reply(400, f"short body: {len(data)} of {length}")
                return

            path = dest / name
            existed = path.exists()
            path.write_bytes(data)          # a re-send of the same ride is a refresh: overwrite

            device = self.headers.get("X-Trace-Device", "?")
            rows = data.count(b"\n")
            stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            line = f"{stamp}  {name}  {len(data):>9} B  {rows:>6} lines  from {device}  {self.client_address[0]}" \
                   + ("  (overwrote)" if existed else "")
            with log.open("a", encoding="utf-8") as fh:
                fh.write(line + "\n")
            print(line, flush=True)
            self._reply(200, f"saved {name} ({len(data)} bytes)")

        def log_message(self, fmt, *args):  # quiet: arrivals are printed above, probes are noise
            pass

    return Handler


def main() -> int:
    here = Path(__file__).resolve().parent.parent   # Tools/ -> project root
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--project", type=Path, default=here, help="GoneSurfing project dir (has Saved/)")
    args = ap.parse_args()

    dest = args.project / "Saved" / "InputTraces" / "fromphone"
    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(dest))
    print(f"TraceShare: POST /trace on port {args.port} -> {dest}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
