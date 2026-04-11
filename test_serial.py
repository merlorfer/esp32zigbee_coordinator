import serial, json, time, sys
from datetime import datetime

ser = serial.Serial('COM14', 115200, timeout=1, dsrdtr=False, rtscts=False)
ser.dtr = False
ser.rts = False
time.sleep(0.3)
ser.reset_input_buffer()

print('Waiting for boot...', flush=True)
deadline = time.time() + 12
booted = False
while time.time() < deadline:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode(errors='ignore').strip()
    if line:
        print(f'  [log] {line}', flush=True)
    if 'initialization complete' in line or 'Setup mode active' in line:
        print('  Boot done.', flush=True)
        booted = True
        break

if not booted:
    print('  (boot timeout, continuing)', flush=True)

time.sleep(0.3)
ser.reset_input_buffer()

def send_cmd(cmd, params=None):
    payload = {'cmd': cmd}
    if params:
        payload['params'] = params
    ser.write((json.dumps(payload) + '\n').encode())
    ser.flush()
    deadline = time.time() + 8
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors='ignore').strip()
        if line:
            print(f'  {line}', flush=True)
        if line.startswith('>>>'):
            return line[3:]
    return None

now = datetime.now()
print(f'[1] set_rtc: {now.strftime("%Y-%m-%d %H:%M:%S")}', flush=True)
r = send_cmd('set_rtc', {'year': now.year, 'month': now.month, 'day': now.day,
                          'hour': now.hour, 'minute': now.minute, 'second': now.second})
print(f'  => {r}', flush=True)

print('[2] switch_mode', flush=True)
r = send_cmd('switch_mode')
print(f'  => {r}', flush=True)

ser.close()
print('Done.', flush=True)
