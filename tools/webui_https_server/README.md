# WebUI HTTPS bridge

Serves the repo's `web/` folder (the same files bundled into the ESP32
firmware) over HTTPS from this PC, so a phone can load the webUI without
ever talking to an ESP32's own HTTP server. The phone's browser then
connects to the ESP32 directly over Web Bluetooth -- this server only
serves static files, it never touches BLE traffic.

This exists because the Web Bluetooth API (`navigator.bluetooth`, used
in `web/script.js` / `web/ble-service.js`) only works in a "secure
context" (HTTPS or localhost). A self-signed cert makes `https://` work;
the phone just needs to accept the certificate warning once.

## One-time setup (this PC)

Already done once, but re-run `gen_cert.sh` (via Git Bash) whenever this
PC's IP address changes (new Wi-Fi network, new VPN/Meshnet IP):

```
bash gen_cert.sh
```

## Start the server

```powershell
.\run_server.ps1            # port 8443 by default
.\run_server.ps1 -Port 9000 # or a custom port
```

Leave it running while using the webUI from the phone.

## Phone-side, first time

1. Make sure the phone can reach this PC (same Wi-Fi, or same NordVPN
   Meshnet).
2. Open `https://<this-PC's-IP>:8443/` in the phone's browser -- e.g.
   `https://192.168.68.114:8443/` (local Wi-Fi) or
   `https://100.124.204.181:8443/` (NordVPN Meshnet). The server prints
   both addresses on startup.
3. The browser will warn about the certificate (self-signed, untrusted).
   Tap **Advanced -> Proceed** (wording varies by browser). This only
   needs to be done once per browser/device -- Chrome remembers the
   exception for that address.
4. Use the webUI as usual -- the BLE connect button talks directly to
   the ESP32 from the phone's browser.

## Updating the UI later

The server always serves whatever is currently on disk in `web/` --
every response is sent with `Cache-Control: no-store`, and the PWA
service worker is deliberately blocked (404) so it can never freeze the
phone on an old cached copy. So after pulling/editing new `web/` files
on the PC:

1. Just reload the page on the phone (no cache to clear, nothing to
   reinstall).
2. If the server itself isn't running, start it again with
   `run_server.ps1`.

If the PC's IP address changed since the cert was generated, re-run
`gen_cert.sh` first (the phone will otherwise get a cert that doesn't
cover the new address).
