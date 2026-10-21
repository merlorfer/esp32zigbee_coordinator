"""
proxy.py - USB-C serial <-> HTTP proxy for the ESP32-C6 Zigbee gateway.

Serves the existing web/ frontend (unchanged) and forwards every /api/*
call to the device over its USB serial JSON command protocol - the same
{"cmd": ..., "params": ...} protocol the BLE interface uses (see
main/ble_handlers.c and main/serial_cmd_task.c). This lets you control and
configure the gateway from a normal browser over USB-C, without BLE, on
both Windows and Linux (e.g. an OrangePi permanently wired to the ESP32).

Also exposes a small "Config tools" panel (injected into the served page,
not part of the on-device web/ copy) for exporting/importing the rules
text and the full device/global/variable configuration to local files -
see README.md for the two-file export/import workflow and how it survives
a factory reset.

Usage:
    python proxy.py --port COM5
    python proxy.py --port /dev/ttyUSB0
    python proxy.py --list-ports

Run "python proxy.py --help" for all options. See README.md for the full
setup walkthrough on both Windows and OrangePi/Linux.
"""

import argparse
import json
import mimetypes
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Missing dependency 'pyserial'. Install it with: pip install pyserial", file=sys.stderr)
    sys.exit(1)

# tools/usb_proxy/proxy.py -> ../../web
WEB_DIR = Path(__file__).resolve().parent.parent.parent / "web"
TOOLS_DIR = Path(__file__).resolve().parent

RESPONSE_PREFIX = ">>>"  # matches serial_cmd_task.c's response line prefix
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 10.0
UPLOAD_CHUNK_CHARS = 700  # conservative: stays well under the 2048-byte serial line buffer
ESPRESSIF_USB_VID = "303A"


# ============================================================================
# Serial link
# ============================================================================

class SerialCommandError(RuntimeError):
    pass


class SerialLink:
    """Owns the serial port and serializes one JSON request/response at a time.

    Framing matches serial_cmd_task.c: one JSON object per line in, one line
    out per response prefixed with '>>>' (ESP_LOG output may be interleaved
    on the same port and is simply ignored here).
    """

    def __init__(self, port, baud=DEFAULT_BAUD):
        self._ser = serial.Serial(port, baud, timeout=0.2)
        self._lock = threading.Lock()

    def close(self):
        self._ser.close()

    def call(self, cmd, params=None, timeout=DEFAULT_TIMEOUT):
        payload = json.dumps({"cmd": cmd, "params": params or {}}) + "\n"
        with self._lock:
            self._ser.reset_input_buffer()
            self._ser.write(payload.encode("utf-8"))
            self._ser.flush()

            deadline = time.monotonic() + timeout
            buf = b""
            while time.monotonic() < deadline:
                chunk = self._ser.read(4096)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if line.startswith(RESPONSE_PREFIX.encode("ascii")):
                        raw = line[len(RESPONSE_PREFIX):]
                        try:
                            return json.loads(raw.decode("utf-8", errors="replace"))
                        except json.JSONDecodeError:
                            continue
            raise SerialCommandError(f"No response to '{cmd}' within {timeout:.0f}s (is the device connected and running?)")

    def upload(self, target, text, chunk_chars=UPLOAD_CHUNK_CHARS):
        """Sends a (potentially large) text payload via upload_begin/upload_chunk/
        upload_commit instead of one oversized command line. Splitting by
        character (codepoint) count -- not raw byte offsets -- guarantees each
        chunk is valid UTF-8 on its own; the firmware just concatenates the
        raw bytes back together, so the split point never matters."""
        size = len(text.encode("utf-8"))
        resp = self.call("upload_begin", {"target": target, "size": size})
        if resp.get("status") != "ok":
            return resp

        for i in range(0, len(text), chunk_chars):
            piece = text[i:i + chunk_chars]
            resp = self.call("upload_chunk", {"data": piece})
            if resp.get("status") != "ok":
                self.call("upload_abort")
                return resp

        return self.call("upload_commit", {}, timeout=max(timeout_for_commit(size), DEFAULT_TIMEOUT))


def timeout_for_commit(size_bytes):
    """A config import re-creates every device in NVS; give it more time
    the bigger the payload is instead of a single fixed timeout."""
    return max(DEFAULT_TIMEOUT, 5.0 + size_bytes / 2000.0)


# ============================================================================
# HTTP endpoint -> serial cmd/params translation
#
# Mirrors web/script.js's endpointToCommand() (used for its BLE transport)
# so the exact same REST surface works here. Kept in the same order except
# that /api/devices/virtual is matched before the generic /api/devices/<ieee>
# fallback -- script.js checks it last, after a catch-all delete_device
# pattern that would otherwise shadow it.
# ============================================================================

IEEE_RE = re.compile(r"^/api/devices/(0x[0-9A-Fa-f]+)")


def extract_ieee(path):
    m = IEEE_RE.match(path)
    return m.group(1) if m else None


def endpoint_to_cmd(method, path, body):
    body = body or {}

    if path == "/api/status":
        return "get_status", {}
    if path == "/api/devices" and method == "GET":
        return "get_devices", {}
    if path == "/api/devices/virtual":
        if method == "POST":
            return "add_virtual_device", body
    if path == "/api/rtc/set":
        parts = re.split(r"[- :]", body["datetime"])
        return "set_rtc", {
            "year": int(parts[0]), "month": int(parts[1]), "day": int(parts[2]),
            "hour": int(parts[3]), "minute": int(parts[4]), "second": int(parts[5]),
        }
    if path.startswith("/api/devices/") and path.endswith("/config") and method == "POST":
        params = dict(body)
        params["ieee_addr"] = extract_ieee(path)
        return "set_device_config", params
    if path.startswith("/api/devices/") and method == "POST" and body.get("cmd"):
        params = dict(body)
        params.setdefault("ieee_addr", extract_ieee(path))
        return "control_device", params
    if path.startswith("/api/devices/") and method == "DELETE":
        return "delete_device", {"ieee_addr": extract_ieee(path)}
    if path == "/api/zigbee/permit-join":
        return "permit_join", (body if body else {"duration": 60})
    if path == "/api/config":
        return ("set_global_settings", body) if body else ("get_global_settings", {})
    if path == "/api/reboot":
        return "reboot", {}
    if path == "/api/wifi/shutdown":
        return "switch_mode", {}
    if path == "/api/factory-reset":
        return "factory_reset", {}
    if path == "/api/rules":
        return ("set_rules", body) if body else ("get_rules", {})
    if path == "/api/rules/timers":
        return "get_rules_timers", {}
    if path == "/api/rules/var":
        return "set_rules_var", body
    if path == "/api/rules/varconfig":
        return "set_rules_varconfig", body
    if path == "/api/rules/reset":
        return "reset_rules", {}
    if path == "/api/rules/exec":
        return "exec_rules_cmd", (body if body else {})
    if path == "/api/logs/live":
        return "get_logs_live", {"lines": 50}

    return None, None


def normalize_response(resp):
    """Matches script.js's bleRequest() normalization exactly, so the
    frontend sees the same {success:...} shape it already expects from its
    BLE code path -- this proxy is, from the page's point of view, just
    another transport."""
    if not isinstance(resp, dict):
        return {"success": False, "message": "Invalid response from device"}
    status = resp.get("status")
    if status == "ok":
        out = dict(resp)
        out["success"] = True
        return out
    if status == "error":
        return {"success": False, "message": resp.get("message", "error")}
    return resp


# ============================================================================
# HTTP server
# ============================================================================

class ProxyHandler(BaseHTTPRequestHandler):
    server_version = "ESP32C6UsbProxy/1.0"
    log_lock = threading.Lock()

    def log_message(self, fmt, *args):
        with self.log_lock:
            print(f"  {self.address_string()} {fmt % args}")

    # -- routing ------------------------------------------------------------

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/proxy-tools.js":
            self._serve_tools_asset("proxy-tools.js", "application/javascript")
        elif path == "/api/proxy/rules.txt":
            self._export_rules()
        elif path == "/api/proxy/config.json":
            self._export_config()
        elif path.startswith("/api/"):
            self._handle_api("GET", path, None)
        else:
            self._serve_static(path)

    def do_POST(self):
        path = self.path.split("?")[0]
        length = int(self.headers.get("Content-Length", 0) or 0)
        raw_body = self.rfile.read(length) if length else b""

        if path == "/api/proxy/rules.txt":
            self._import_rules(raw_body)
        elif path == "/api/proxy/config.json":
            self._import_config(raw_body)
        elif path.startswith("/api/"):
            body = None
            if raw_body:
                try:
                    body = json.loads(raw_body.decode("utf-8"))
                except json.JSONDecodeError:
                    body = None
            self._handle_api("POST", path, body)
        else:
            self._send_json(405, {"success": False, "message": "Method not allowed"})

    def do_DELETE(self):
        path = self.path.split("?")[0]
        if path.startswith("/api/"):
            self._handle_api("DELETE", path, None)
        else:
            self._send_json(405, {"success": False, "message": "Method not allowed"})

    def do_OPTIONS(self):
        self.send_response(200)
        self._add_cors_headers()
        self.send_header("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    # -- /api/* generic relay -------------------------------------------------

    def _handle_api(self, method, path, body):
        cmd, params = endpoint_to_cmd(method, path, body)
        if cmd is None:
            self._send_json(404, {"success": False, "message": f"Unknown endpoint: {method} {path}"})
            return
        try:
            resp = self.server.serial_link.call(cmd, params)
        except SerialCommandError as e:
            self._send_json(504, {"success": False, "message": str(e)})
            return
        except serial.SerialException as e:
            self._send_json(503, {"success": False, "message": f"Serial link error: {e}"})
            return
        self._send_json(200, normalize_response(resp))

    # -- config export/import (serial-only feature, not part of web/) -------

    def _export_rules(self):
        try:
            resp = self.server.serial_link.call("get_rules")
        except (SerialCommandError, serial.SerialException) as e:
            self._send_json(504, {"success": False, "message": str(e)})
            return
        text = resp.get("text", "") if resp.get("status") == "ok" else ""
        data = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Disposition", 'attachment; filename="rules.txt"')
        self.send_header("Content-Length", str(len(data)))
        self._add_cors_headers()
        self.end_headers()
        self.wfile.write(data)

    def _import_rules(self, raw_body):
        text = raw_body.decode("utf-8", errors="replace")
        try:
            resp = self.server.serial_link.upload("rules", text)
        except (SerialCommandError, serial.SerialException) as e:
            self._send_json(504, {"success": False, "message": str(e)})
            return
        self._send_json(200, normalize_response(resp))

    def _export_config(self):
        try:
            resp = self.server.serial_link.call("export_config")
        except (SerialCommandError, serial.SerialException) as e:
            self._send_json(504, {"success": False, "message": str(e)})
            return
        data = json.dumps(resp, indent=2, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Disposition", 'attachment; filename="config.json"')
        self.send_header("Content-Length", str(len(data)))
        self._add_cors_headers()
        self.end_headers()
        self.wfile.write(data)

    def _import_config(self, raw_body):
        text = raw_body.decode("utf-8", errors="replace")
        try:
            resp = self.server.serial_link.upload("config", text)
        except (SerialCommandError, serial.SerialException) as e:
            self._send_json(504, {"success": False, "message": str(e)})
            return
        self._send_json(200, normalize_response(resp))

    # -- static file serving --------------------------------------------------

    def _serve_static(self, path):
        if path in ("/", ""):
            path = "/index.html"

        file_path = (WEB_DIR / path.lstrip("/")).resolve()
        if WEB_DIR.resolve() not in file_path.parents and file_path != WEB_DIR.resolve():
            self._send_plain(403, "Forbidden")
            return
        if not file_path.is_file():
            self._send_plain(404, f"Not found: {path}")
            return

        mime, _ = mimetypes.guess_type(str(file_path))
        mime = mime or "application/octet-stream"
        data = file_path.read_bytes()

        if mime == "text/html":
            # Set the usb-proxy marker as early as possible (right after <head>)
            # so script.js -- which runs near the end of <body>, well before
            # this file's own </body> injection point -- can already see it.
            # A DOM query for the proxy-tools.js tag itself would not work here:
            # classic <script> tags execute synchronously as the parser reaches
            # them, so anything added later in the document (like the tag right
            # before </body>) does not exist yet when script.js's top-level code
            # runs.
            marker = b'<head><script>window.__usbProxy = true;</script>'
            if b"<head>" in data:
                data = data.replace(b"<head>", marker, 1)

            inject = b'<script src="/proxy-tools.js"></script>\n</body>'
            if b"</body>" in data:
                data = data.replace(b"</body>", inject, 1)

        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self._add_cors_headers()
        self.end_headers()
        self.wfile.write(data)

    def _serve_tools_asset(self, name, mime):
        file_path = TOOLS_DIR / name
        if not file_path.is_file():
            self._send_plain(404, "Not found")
            return
        data = file_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self._add_cors_headers()
        self.end_headers()
        self.wfile.write(data)

    # -- small helpers ----------------------------------------------------

    def _add_cors_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")

    def _send_json(self, code, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self._add_cors_headers()
        self.end_headers()
        self.wfile.write(data)

    def _send_plain(self, code, msg):
        data = msg.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self._add_cors_headers()
        self.end_headers()
        self.wfile.write(data)


class ProxyServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ============================================================================
# CLI
# ============================================================================

def guess_esp32_port():
    for p in serial.tools.list_ports.comports():
        if p.vid is not None and f"{p.vid:04X}" == ESPRESSIF_USB_VID:
            return p.device
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="Serial port (e.g. COM5 on Windows, /dev/ttyUSB0 or /dev/ttyACM0 on Linux). "
                                        "If omitted, tries to auto-detect an Espressif USB device.")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"Serial baud rate (default {DEFAULT_BAUD})")
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind address (default 0.0.0.0, i.e. reachable from the LAN)")
    parser.add_argument("--http-port", type=int, default=8080, help="HTTP port to serve on (default 8080)")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports and exit")
    args = parser.parse_args()

    if args.list_ports:
        for p in serial.tools.list_ports.comports():
            print(f"{p.device}  {p.description}")
        return

    if not WEB_DIR.is_dir():
        print(f"ERROR: web directory not found: {WEB_DIR}", file=sys.stderr)
        sys.exit(1)

    port = args.port or guess_esp32_port()
    if not port:
        print("ERROR: no --port given and auto-detection found no Espressif USB device.", file=sys.stderr)
        print("Run 'python proxy.py --list-ports' to see what's available.", file=sys.stderr)
        sys.exit(1)

    print(f"Opening serial port {port} @ {args.baud} baud ...")
    try:
        link = SerialLink(port, args.baud)
    except serial.SerialException as e:
        print(f"ERROR: could not open serial port {port}: {e}", file=sys.stderr)
        sys.exit(1)

    # On boards with native USB (USB-Serial-JTAG, the ESP32-C6 default),
    # simply opening the port resets the chip -- same as connecting a
    # serial monitor or esptool would. Wait here for the device to finish
    # booting instead of letting the first browser request hit a timeout.
    # A fresh boot always comes up in setup mode (WiFi AP + BLE); switch
    # it back to normal Zigbee-operational mode from the web UI or with
    # the physical button afterwards if it was running normally before.
    print("Waiting for device to finish booting...")
    for attempt in range(15):
        try:
            link.call("get_status", timeout=2)
            print("Device is responding.")
            break
        except SerialCommandError:
            time.sleep(1)
    else:
        print("WARNING: device did not respond within ~15s of opening the port. "
              "It may still be booting, or the port may be wrong.", file=sys.stderr)

    server = ProxyServer((args.host, args.http_port), ProxyHandler)
    server.serial_link = link

    print(f"Serving {WEB_DIR}")
    print(f"Web UI:  http://localhost:{args.http_port}  (also reachable on your LAN IP)")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        link.close()


if __name__ == "__main__":
    main()
