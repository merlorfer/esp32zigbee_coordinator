"""
chunk_flash.py — Flash large firmware in chunks to work around
the 10-second ROM bootloader watchdog on ESP32-C6 rev v0.2.

Run on Orange Pi:
    python3 chunk_flash.py
"""

import subprocess
import os
import sys
import time
import tempfile

PORT = "/dev/ttyACM0"
BAUD = 460800
ESPTOOL = os.path.expanduser("~/.local/bin/esptool")
FIRMWARE = os.path.expanduser("~/esp32firmware/esp32c6_zigbee_gateway.bin")
APP_OFFSET = 0x10000
CHUNK_SIZE = 128 * 1024  # 128KB — smaller chunks for unreliable connections
MAX_RETRIES = 5


def flash_chunk(chunk_file, offset):
    cmd = [
        ESPTOOL,
        "--chip", "esp32c6",
        "--port", PORT,
        "--baud", str(BAUD),
        "write-flash",
        "--flash-mode", "dio",
        "--flash-freq", "80m",
        "--flash-size", "4MB",
        "--no-compress",
        hex(offset), chunk_file,
    ]
    print(f"  esptool offset={hex(offset)} size={os.path.getsize(chunk_file)} bytes")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    if "Hash of data verified" in output and "Wrote" in output:
        return True
    print(f"  Output: {output[-300:]}")
    return False


def main():
    if not os.path.exists(FIRMWARE):
        print(f"ERROR: {FIRMWARE} not found")
        sys.exit(1)

    with open(FIRMWARE, "rb") as f:
        data = f.read()

    total = len(data)
    num_chunks = (total + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"Firmware: {total} bytes, splitting into {num_chunks} chunks of {CHUNK_SIZE} bytes")
    print(f"Port: {PORT}, Baud: {BAUD}")
    print()

    tmpdir = tempfile.mkdtemp()

    try:
        for i in range(num_chunks):
            start = i * CHUNK_SIZE
            end = min(start + CHUNK_SIZE, total)
            chunk_data = data[start:end]
            chunk_file = os.path.join(tmpdir, f"chunk_{i:03d}.bin")
            with open(chunk_file, "wb") as f:
                f.write(chunk_data)

            offset = APP_OFFSET + start
            print(f"[{i+1}/{num_chunks}] Flashing bytes {start}-{end} → offset {hex(offset)} ({len(chunk_data)} bytes)")

            success = False
            for attempt in range(1, MAX_RETRIES + 1):
                # Kill any process holding the port
                subprocess.run(["fuser", "-k", PORT], capture_output=True)
                time.sleep(0.5)

                try:
                    if flash_chunk(chunk_file, offset):
                        print(f"  OK (attempt {attempt})")
                        success = True
                        break
                    else:
                        print(f"  Failed attempt {attempt}, waiting for chip reset...")
                        time.sleep(3)
                except subprocess.TimeoutExpired:
                    print(f"  Timeout on attempt {attempt}")
                    time.sleep(3)

            if not success:
                print(f"ERROR: chunk {i} failed after {MAX_RETRIES} attempts")
                sys.exit(1)

            # Small delay between chunks to let chip reset cycle complete
            if i < num_chunks - 1:
                print(f"  Waiting for chip reset before next chunk...")
                time.sleep(4)

        print()
        print("All chunks flashed successfully!")

    finally:
        import shutil
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    main()
