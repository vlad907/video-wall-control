"""
Fake NDI Studio Monitor HTTP API for testing the dashboard off-site (macOS/Linux/Windows).

    python tools/mock_studio_monitor.py 9080 9081 9082 9083

Then run videowall.py with a config whose panels use those ports and
"manage_processes": false (see README).
"""
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SOURCES = [{"name": "RESOLUME-PC (Panel 1)"}, {"name": "RESOLUME-PC (Panel 2)"},
           {"name": "RESOLUME-PC (Panel 3)"}, {"name": "RESOLUME-PC (Panel 4)"},
           {"name": "LAPTOP (Test Pattern)"}]


def make_handler(port):
    state = {"version": 1, "NDI_source": "", "window": {"display_device": "DISPLAY%d" % (port % 10 + 1)},
             "decorations": {"hw_accel": False}}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            print("[%d] %s" % (port, fmt % args))

        def reply(self, obj):
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/v1/sources":
                self.reply(SOURCES)
            elif self.path == "/v1/configuration":
                self.reply(state)
            else:
                self.send_error(404)

        def do_POST(self):
            if self.path != "/v1/configuration":
                return self.send_error(404)
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            print("[%d] POST %s" % (port, json.dumps(body)))
            state["NDI_source"] = body.get("NDI_source", state["NDI_source"])
            state["window"].update(body.get("window", {}))
            state["decorations"].update(body.get("decorations", {}))
            self.reply({})

    return Handler


if __name__ == "__main__":
    ports = [int(p) for p in sys.argv[1:]] or [9080, 9081, 9082, 9083]
    for port in ports:
        server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(port))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        print("Mock Studio Monitor on http://127.0.0.1:%d" % port)
    threading.Event().wait()
