"""Minimal Splunk HEC-style receiver for local runs, tests and benchmarks.

Accepts ``POST /services/collector/event`` with ``Authorization: Splunk <token>`` and a body of
one or more concatenated JSON objects (the real HEC batching format). ``GET /stats`` returns
counters and the most recent events so demos and tests can verify delivery.
"""
from __future__ import annotations

import argparse
import json
import os
import threading
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_TOKEN = "dev-token-not-secret"


class HecState:
    def __init__(self, token: str, keep: int = 100):
        self.token = token
        self.lock = threading.Lock()
        self.events_received = 0
        self.requests = 0
        self.rejected = 0
        self.recent: deque = deque(maxlen=keep)
        self.fail_next = 0  # test hook: answer the next N requests with 503

    def stats(self) -> dict:
        with self.lock:
            return {
                "events_received": self.events_received,
                "requests": self.requests,
                "rejected": self.rejected,
                "recent": list(self.recent),
            }


def parse_concatenated_json(body: bytes) -> list:
    """Parse ``{...}{...}`` / newline separated JSON objects. Raises ValueError on bad input."""
    text = body.decode("utf-8")
    dec = json.JSONDecoder()
    out, i, n = [], 0, len(text)
    while i < n:
        while i < n and text[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        obj, i = dec.raw_decode(text, i)
        out.append(obj)
    return out


def make_handler(state: HecState):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):  # keep benchmarks quiet
            pass

        def _reply(self, code: int, payload: dict):
            data = json.dumps(payload).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/stats":
                self._reply(200, state.stats())
            elif self.path in ("/services/collector/health", "/healthz"):
                self._reply(200, {"text": "HEC is healthy", "code": 17})
            else:
                self._reply(404, {"text": "not found", "code": 404})

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            if self.path not in ("/services/collector/event", "/services/collector"):
                return self._reply(404, {"text": "not found", "code": 404})
            if self.headers.get("Authorization") != f"Splunk {state.token}":
                with state.lock:
                    state.rejected += 1
                return self._reply(401, {"text": "Invalid token", "code": 4})
            with state.lock:
                if state.fail_next > 0:
                    state.fail_next -= 1
                    return self._reply(503, {"text": "Server is busy", "code": 9})
            try:
                events = parse_concatenated_json(body)
                if not events or any("event" not in e for e in events):
                    raise ValueError("missing event field")
            except (ValueError, UnicodeDecodeError):
                with state.lock:
                    state.rejected += 1
                return self._reply(400, {"text": "Invalid data format", "code": 6})
            with state.lock:
                state.requests += 1
                state.events_received += len(events)
                state.recent.extend(events[-5:])
            self._reply(200, {"text": "Success", "code": 0})

    return Handler


def make_server(host: str, port: int, token: str) -> tuple[ThreadingHTTPServer, HecState]:
    state = HecState(token)
    server = ThreadingHTTPServer((host, port), make_handler(state))
    server.daemon_threads = True
    return server, state


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=os.environ.get("HOTPATH_MOCK_HEC_HOST", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("HOTPATH_MOCK_HEC_PORT", "8088")))
    args = ap.parse_args(argv)
    token = os.environ.get("HOTPATH_HEC_TOKEN", DEFAULT_TOKEN)
    server, _ = make_server(args.host, args.port, token)
    print(f"mock HEC listening on {args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
