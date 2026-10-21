#!/usr/bin/env python3
"""
Serves the repo's web/ directory (the same webUI bundled into the ESP32
firmware's SPIFFS image) over HTTPS from this PC, so a phone can load the
webUI as a secure context without ever talking to the ESP32's own HTTP
server. The phone's browser then connects to the ESP32 directly over
Web Bluetooth (navigator.bluetooth in script.js/ble-service.js) -- this
server never touches that traffic, it only serves static files.

Usage:
    python serve.py [port]      (default port: 8443)

Requires certs/cert.pem + certs/key.pem -- run gen_cert.sh first (or
again, if this PC's IP address changed).
"""
import http.server
import os
import ssl
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", "web"))
CERT_FILE = os.path.join(SCRIPT_DIR, "certs", "cert.pem")
KEY_FILE = os.path.join(SCRIPT_DIR, "certs", "key.pem")
CA_FILE = os.path.join(SCRIPT_DIR, "certs", "ca-cert.pem")


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def send_response_only(self, code, message=None):
        super().send_response_only(code, message)
        # Always serve the current files on disk -- never let the phone's
        # browser cache a stale copy of the webUI across updates.
        self.send_header("Cache-Control", "no-store, must-revalidate")

    def do_GET(self):
        # One-time download so the phone can install this bridge's CA as a
        # trusted root -- without that, Chrome treats the page as served
        # over an untrusted connection and refuses to offer a real "Install
        # app" (only "Create shortcut"), even if the cert warning was
        # clicked through.
        if self.path == "/root-ca.pem":
            if os.path.isfile(CA_FILE):
                with open(CA_FILE, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/x-x509-ca-cert")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_response(404)
                self.end_headers()
            return
        super().do_GET()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8443

    if not (os.path.isfile(CERT_FILE) and os.path.isfile(KEY_FILE)):
        print(f"Missing {CERT_FILE} / {KEY_FILE} -- run gen_cert.sh first.")
        sys.exit(1)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), NoCacheHandler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    print(f"Serving {WEB_DIR}")
    print(f"Listening on https://0.0.0.0:{port}/")
    print("Open this from the phone, e.g.:")
    print(f"  https://100.124.204.181:{port}/   (NordVPN Meshnet)")
    print(f"  https://192.168.68.114:{port}/    (local Wi-Fi)")
    print("Ctrl+C to stop.")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
