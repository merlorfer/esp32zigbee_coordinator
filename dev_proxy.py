"""
dev_proxy.py — Local dev proxy for ESP32-C6 web UI

Usage:
    python dev_proxy.py [ESP_IP] [PORT]

Defaults:
    ESP_IP = 192.168.4.1   (ESP WiFi AP address)
    PORT   = 8080

Serves:
    Static files  → from ./web/ directory (fast, local disk)
    /api/*        → proxied to http://<ESP_IP>/api/... (real ESP)
    /ws           → proxied as WebSocket (if needed)

Open http://localhost:8080 in browser while connected to ESP WiFi AP.
"""

import sys
import os
import http.server
import urllib.request
import urllib.error
import socketserver
import mimetypes
import threading

ESP_IP   = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
PORT     = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
WEB_DIR  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
ESP_BASE = f"http://{ESP_IP}"

TIMEOUT  = 10  # seconds for ESP requests

class ProxyHandler(http.server.BaseHTTPRequestHandler):
    log_lock = threading.Lock()

    def log_message(self, fmt, *args):
        with self.log_lock:
            print(f"  {self.address_string()} {fmt % args}")

    # ------------------------------------------------------------------ #
    # Route decision
    # ------------------------------------------------------------------ #

    def _is_api(self):
        return self.path.startswith("/api/") or self.path == "/api"

    # ------------------------------------------------------------------ #
    # Static file serving
    # ------------------------------------------------------------------ #

    def _serve_static(self):
        # Strip query string
        path = self.path.split("?")[0].split("#")[0]
        if path == "/" or path == "":
            path = "/index.html"

        file_path = os.path.join(WEB_DIR, path.lstrip("/"))
        # Normalise and restrict to web dir
        file_path = os.path.normpath(file_path)
        if not file_path.startswith(os.path.normpath(WEB_DIR)):
            self._send_code(403, "Forbidden")
            return

        if not os.path.isfile(file_path):
            self._send_code(404, f"Not found: {path}")
            return

        mime, _ = mimetypes.guess_type(file_path)
        mime = mime or "application/octet-stream"

        with open(file_path, "rb") as f:
            data = f.read()

        # Inject SW-unregister snippet into HTML so stale service workers
        # from 192.168.4.1 don't intercept requests on localhost.
        if mime == "text/html":
            inject = b"""<script>
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.getRegistrations().then(regs => {
    regs.forEach(r => r.unregister());
  });
}
</script>"""
            data = data.replace(b"<head>", b"<head>" + inject, 1)
            if b"<head>" not in data:
                data = inject + data

        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    # ------------------------------------------------------------------ #
    # API proxy
    # ------------------------------------------------------------------ #

    def _proxy_api(self, body=None):
        url = ESP_BASE + self.path
        headers = {}
        # Forward content-type if present
        ct = self.headers.get("Content-Type")
        if ct:
            headers["Content-Type"] = ct

        req = urllib.request.Request(url, data=body, headers=headers,
                                     method=self.command)
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                data = resp.read()
                self.send_response(resp.status)
                for key, val in resp.headers.items():
                    if key.lower() in ("content-type", "content-length"):
                        self.send_header(key, val)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(data)
        except urllib.error.HTTPError as e:
            data = e.read()
            self.send_response(e.code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)
        except Exception as ex:
            print(f"  [PROXY ERROR] {self.command} {self.path} -> {ESP_BASE}: {ex}")
            msg = f'{{"error":"{ex}"}}'.encode()
            self.send_response(502)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(msg)

    # ------------------------------------------------------------------ #
    # HTTP verb handlers
    # ------------------------------------------------------------------ #

    def do_GET(self):
        if self._is_api():
            self._proxy_api()
        else:
            self._serve_static()

    def do_POST(self):
        if self._is_api():
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else None
            self._proxy_api(body)
        else:
            self._send_code(405, "Method not allowed")

    def do_PUT(self):
        if self._is_api():
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else None
            self._proxy_api(body)
        else:
            self._send_code(405, "Method not allowed")

    def do_DELETE(self):
        if self._is_api():
            self._proxy_api()
        else:
            self._send_code(405, "Method not allowed")

    def do_OPTIONS(self):
        # CORS preflight
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    # ------------------------------------------------------------------ #

    def _send_code(self, code, msg):
        body = msg.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class ThreadedServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    if not os.path.isdir(WEB_DIR):
        print(f"ERROR: web directory not found: {WEB_DIR}")
        sys.exit(1)

    print(f"Dev proxy started")
    print(f"  Static files : {WEB_DIR}")
    print(f"  ESP target   : {ESP_BASE}")
    print(f"  Open in browser: http://localhost:{PORT}")
    print(f"  (connect your PC to the ESP WiFi AP first)")
    print()

    server = ThreadedServer(("", PORT), ProxyHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
