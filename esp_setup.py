"""
esp_setup.py — ESP32 serial setup script
Sends commands via USB serial (>>>-prefixed responses).

Usage:
    python esp_setup.py [COM9]
"""

import sys
import json
import time
import os
from datetime import datetime

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
BAUD = 115200
RESPONSE_TIMEOUT = 8  # seconds

RULES_FILE = os.path.join(os.path.dirname(__file__), "test_rules.rul")


def send_cmd(ser, cmd, params=None):
    payload = {"cmd": cmd}
    if params is not None:
        payload["params"] = params
    line = json.dumps(payload, ensure_ascii=False) + "\n"
    print(f"    >> {line.strip()}")
    ser.write(line.encode("utf-8"))
    ser.flush()

    deadline = time.time() + RESPONSE_TIMEOUT
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="ignore").strip()
        if not text:
            continue
        if text.startswith(">>>"):
            print(f"    << {text}")
            try:
                return json.loads(text[3:])
            except json.JSONDecodeError:
                return {"raw": text[3:]}
        else:
            print(f"    [log] {text}")
    return None


def check(result, label):
    if result is None:
        print(f"  {label}: TIMEOUT — nincs valasz")
        return False
    ok = result.get("status") == "ok" or result.get("success") is True
    if ok:
        extras = {k: v for k, v in result.items() if k not in ("status", "success")}
        print(f"  {label}: OK  {extras if extras else ''}")
        return True
    else:
        print(f"  {label}: HIBA — {result}")
        return False


def main():
    if not os.path.exists(RULES_FILE):
        print(f"ERROR: Nem talalhato: {RULES_FILE}")
        sys.exit(1)

    with open(RULES_FILE, "r", encoding="utf-8") as f:
        rules_text = f.read()

    print(f"Port: {PORT}  ({BAUD} baud)")
    print(f"Szabaly fajl: {RULES_FILE}  ({len(rules_text)} byte)")
    print()

    try:
        ser = serial.Serial(PORT, BAUD, timeout=1, dsrdtr=False, rtscts=False)
        ser.dtr = False
        ser.rts = False
    except serial.SerialException as e:
        print(f"ERROR: Nem sikerult megnyitni {PORT}: {e}")
        sys.exit(1)

    time.sleep(0.3)
    ser.reset_input_buffer()

    # Wait for boot to complete before sending any commands
    print("Varakozas ESP boot befejezesere...")
    deadline = time.time() + 10
    booted = False
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="ignore").strip()
        if text:
            print(f"    [log] {text}")
        if "initialization complete" in text or "Setup mode active" in text:
            print("    ESP boot kesz.")
            booted = True
            time.sleep(0.5)
            break
    if not booted:
        print("    (boot uzenet nem erkezett, folytatom)")
    ser.reset_input_buffer()
    print()

    now = datetime.now()
    print(f"[1] RTC beallitasa: {now.strftime('%Y-%m-%d %H:%M:%S')}")
    result = send_cmd(ser, "set_rtc", {
        "year":   now.year,
        "month":  now.month,
        "day":    now.day,
        "hour":   now.hour,
        "minute": now.minute,
        "second": now.second,
    })
    check(result, "set_rtc")

    print(f"[2] Szabalyok engedelyezese")
    result = send_cmd(ser, "set_global_settings", {"rules_enabled": True})
    check(result, "set_global_settings")

    print(f"[3] Szabalyok feltoltese ({rules_text.count(chr(10))+1} sor)")
    result = send_cmd(ser, "set_rules", {"text": rules_text})
    if check(result, "set_rules"):
        print(f"     rule_count: {result.get('rule_count', '?')}")

    print(f"[4] Zigbee uzemmodra valtas")
    result = send_cmd(ser, "switch_mode")
    check(result, "switch_mode")

    print(f"     Varakozas Zigbee inditasra (8 mp)...")
    time.sleep(8)

    print(f"[5] var1 = 0 (szabaly inditasa)")
    result = send_cmd(ser, "set_rules_var", {"index": 0, "value": 0})
    check(result, "set_rules_var")

    ser.close()
    print()
    print("Kesz.")


if __name__ == "__main__":
    main()
