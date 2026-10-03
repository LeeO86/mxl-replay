#!/usr/bin/env python3
"""Minimal IS-04 Registration and Query APIs for the replay integration test."""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from urllib.parse import urlparse


class Registry:
    def __init__(self, log_path):
        self.resources = {}
        self.log_path = log_path

    def note(self, line):
        with open(self.log_path, "a", encoding="utf-8") as handle:
            handle.write(line + "\n")


def handler_for(registry, query):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            return

        def _send(self, status, body=b"", content_type="application/json"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_GET(self):
            path = urlparse(self.path).path
            if query and path.startswith("/x-nmos/query/"):
                parts = path.strip("/").split("/")
                if len(parts) >= 5 and parts[3] == "nodes":
                    node_id = parts[4]
                    if node_id in registry.resources.get("node", {}):
                        self._send(200, json.dumps({"id": node_id}).encode())
                        return
                self._send(404, b"{}")
                return
            self._send(200, b"{}")

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length else b""
            path = urlparse(self.path).path
            if path.rstrip("/").endswith("/resource"):
                try:
                    doc = json.loads(raw.decode() or "{}")
                except json.JSONDecodeError:
                    self._send(400, b"{}")
                    return
                resource_type = doc.get("type", "")
                data = doc.get("data") or {}
                resource_id = data.get("id", "")
                registry.resources.setdefault(resource_type, {})[resource_id] = data
                registry.note(f"POST {resource_type} {resource_id}")
                self._send(201, b"{}")
                return
            if "/health/nodes/" in path:
                self._send(200, b"{}")
                return
            self._send(200, b"{}")

        def do_DELETE(self):
            path = urlparse(self.path).path
            registry.note("DELETE " + path)
            parts = path.strip("/").split("/")
            if "resource" in parts:
                index = parts.index("resource")
                if len(parts) >= index + 3:
                    resource_type = parts[index + 1]
                    resource_id = parts[index + 2]
                    registry.resources.get(resource_type, {}).pop(resource_id, None)
            self._send(204, b"")

    return Handler


def serve(port, registry, query):
    server = ThreadingHTTPServer(("127.0.0.1", port), handler_for(registry, query))
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def main():
    registration_port = int(sys.argv[1])
    query_port = int(sys.argv[2])
    log_path = sys.argv[3]
    registry = Registry(log_path)
    serve(registration_port, registry, False)
    serve(query_port, registry, True)
    print(f"registry {registration_port} query {query_port}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        return


if __name__ == "__main__":
    main()
