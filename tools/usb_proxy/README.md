# USB-C serial proxy

Runs the ESP32-C6 gateway's normal web UI (the same files served from the
device's own WiFi AP in setup mode) through a small local HTTP server that
talks to the device over its USB-C serial connection instead of WiFi or
Bluetooth. Useful because:

- The gateway only serves its web UI over WiFi while in **setup mode**; once
  you switch it into normal Zigbee-operational mode, WiFi is turned off and
  only BLE remains — which can be flaky. The serial link is always available,
  in every mode, and is a direct wired connection.
- It also exposes a small **config export/import** panel (rules text +
  full device/global/variable config to two local files), so a factory reset
  doesn't mean re-entering every automation setting by hand. See
  [Config export/import](#config-export--import) below.

The same `proxy.py` script works unmodified on a Windows PC or on a Linux
host (e.g. an OrangePi permanently wired to the gateway) — only the `--port`
value differs.

## Requirements

- Python 3.8+
- `pyserial` (`pip install -r requirements.txt` or `pip install pyserial`)

## Windows PC

1. Install Python 3 if you don't have it, then install the dependency:
   ```
   pip install -r requirements.txt
   ```
2. Plug the ESP32-C6 into a USB-C port and find its COM port in **Device
   Manager** (under "Ports (COM & LPT)"), e.g. `COM5`. Or let the script try
   to find it itself:
   ```
   python proxy.py --list-ports
   ```
3. Start the proxy:
   ```
   python proxy.py --port COM5
   ```
4. Open `http://localhost:8080` in a browser.

## OrangePi (or any Linux host)

1. Install Python 3 and the dependency:
   ```
   sudo apt install python3-pip
   pip3 install -r requirements.txt
   ```
2. Add your user to the `dialout` group so it can open the serial device
   without root (log out/in — or reboot — for this to take effect):
   ```
   sudo usermod -aG dialout $USER
   ```
3. Find the device path — usually `/dev/ttyUSB0` or `/dev/ttyACM0`:
   ```
   ls /dev/tty*
   # or, after plugging the ESP32 in:
   dmesg | tail
   ```
4. Start the proxy:
   ```
   python3 proxy.py --port /dev/ttyACM0
   ```
5. The server listens on `0.0.0.0:8080` by default, so it's reachable from
   any other device on the same network at `http://<orangepi-ip>:8080` —
   not just `localhost` on the OrangePi itself.

### Keeping it running in the background (optional)

For a permanently-wired OrangePi you may want the proxy to survive reboots.
A minimal systemd unit:

```ini
# /etc/systemd/system/esp32-usb-proxy.service
[Unit]
Description=ESP32-C6 gateway USB proxy
After=network.target

[Service]
ExecStart=/usr/bin/python3 /path/to/tools/usb_proxy/proxy.py --port /dev/ttyACM0
Restart=on-failure
RestartSec=5
User=orangepi

[Install]
WantedBy=multi-user.target
```

Then: `sudo systemctl enable --now esp32-usb-proxy`.

## Important: connecting resets the board

On the ESP32-C6's native USB (USB-Serial-JTAG, the default `serial_interface`
setting), simply **opening the serial port resets the chip** — the same
thing happens if you connect `idf.py monitor` or esptool. `proxy.py` waits
for the device to finish booting before serving requests, but a fresh boot
always comes up in **setup mode** (WiFi AP + BLE), never automatically back
in Zigbee-operational mode, even if that's what was running before you
connected. If the gateway was actively running its automation, switch it
back to Zigbee-operational mode afterwards — either with the physical
button, or from the web UI once the proxy is up (the same control that
normally exits setup mode). Verified against real hardware: the reset is
harmless and non-destructive, it just means one extra step after connecting.

## Common: the port is in use

Only one process can own the serial port at a time. Before flashing new
firmware (`idf.py flash`) or opening a serial monitor, stop the proxy first
(`Ctrl+C`, or `sudo systemctl stop esp32-usb-proxy` if running as a service),
then start it again afterwards.

To check whether the proxy itself is up and talking to the device, load
`http://<host>:8080/api/status` in a browser — a JSON response with
`"success": true` means the serial link is working.

## Config export / import

A small "⚙ Config tools" panel is added to the bottom-right of the page
(this panel only exists when served through the proxy — it is not part of
the firmware's own web UI, since it only works over the USB serial link):

- **Export rules → rules.txt** — downloads the current rules-engine DSL text.
- **Import rules from file...** — uploads a `rules.txt` and replaces the
  active rules with it.
- **Export config → config.json** — downloads every Zigbee/virtual device's
  full configuration (name, automation mode, schedule/delay settings, sensor
  thresholds and links), the global settings (XKC sensor GPIOs, log filter,
  etc.), and the rules-engine variables (persist flag, default, current
  value) as one JSON file.
- **Import config from file...** — restores all of the above from a
  previously exported `config.json`.

Both imports are sent in small chunks behind the scenes (not as one giant
request), so there's no practical size limit tied to the serial link's line
buffer.

**After a factory reset**: import `config.json` first to restore every
device's automation/sensor settings and the global config, then import
`rules.txt` for the automation rules. Note that a factory reset also wipes
the Zigbee network's pairing/security state — physical Zigbee devices still
need to be re-paired (permit-join) afterwards. Because the imported config
is already in place by then, re-pairing the same physical device (same IEEE
address) picks its settings back up automatically instead of starting over
with defaults.
