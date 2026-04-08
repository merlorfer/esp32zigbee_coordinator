# ESP32-C6 Gateway — USB Soros Parancssori Interfész

## Áttekintés

Az ESP32-C6 az USB Serial JTAG porton (Linux: `/dev/ttyACM0`, Windows: `COM9`) JSON parancsokat fogad.
A parancsok ugyanazok mint a BLE interfészen.

**Protokoll:**
- Küldés: egysoros JSON + `\n` (115200 baud)
- Fogadás: a válaszsorok `>>>` prefixszel érkeznek
- A többi sor ESP log — szűrhető (formátum: `I (timestamp) TAG: üzenet`)

**Megjegyzés Windows alatt:** a COM port megnyitása az ESP32-t reseteli (USB CDC driver sajátossága). Linux alatt ez nem történik meg.

---

## Gyors példák

```bash
# Linux (Orange Pi)
echo '{"cmd":"get_status"}' > /dev/ttyACM0
cat /dev/ttyACM0 | grep '^>>>'

# Python (platformfüggetlen)
python esp_setup.py COM9
```

---

## Parancsok

### Rendszer

#### `get_status`
Rendszer állapot lekérdezése.

```json
{"cmd":"get_status"}
```
Válasz: RTC idő, WiFi/BLE/Zigbee módok, eszközszám, memória.

---

#### `switch_mode`
WiFi ↔ Zigbee üzemmód váltás (mint a gomb rövid nyomás).

```json
{"cmd":"switch_mode"}
```
Válasz: `{"success":true,"message":"Uzemmod valtas..."}`

---

#### `reboot`
Újraindítás.

```json
{"cmd":"reboot"}
```

---

#### `factory_reset`
Gyári visszaállítás — minden eszköz és konfiguráció törlése.

```json
{"cmd":"factory_reset"}
```

---

#### `set_rtc`
Valós idejű óra beállítása.

```json
{"cmd":"set_rtc","params":{"year":2026,"month":3,"day":30,"hour":20,"minute":44,"second":0}}
```
Válasz: `{"status":"ok","message":"RTC time set"}`

---

#### `get_global_settings`
Globális konfiguráció lekérdezése.

```json
{"cmd":"get_global_settings"}
```
Válasz: WiFi on-behavior, XKC beállítások, log szűrő, rules_enabled, stb.

---

#### `set_global_settings`
Globális konfiguráció módosítása (csak a megadott mezők változnak).

```json
{"cmd":"set_global_settings","params":{"rules_enabled":true}}
{"cmd":"set_global_settings","params":{"rules_enabled":false}}
{"cmd":"set_global_settings","params":{"log_zigbee_only":true}}
{"cmd":"set_global_settings","params":{"local_xkc_enabled":true,"local_xkc_gpio_lower":2,"local_xkc_gpio_upper":3}}
```
Válasz: `{"status":"ok","message":"Global settings updated"}`

---

#### `permit_join`
Zigbee párosítási mód engedélyezése.

```json
{"cmd":"permit_join"}
{"cmd":"permit_join","params":{"duration":60}}
```
Alapértelmezett időtartam: 60 mp. Válasz: `{"status":"ok"}`

---

### Eszközök

#### `get_devices`
Összes párosított Zigbee eszköz listája.

```json
{"cmd":"get_devices"}
```
Válasz: eszközök tömbje (IEEE cím, név, endpoint, típus, állapot, stb.)

---

#### `control_device`
Eszköz közvetlen vezérlése.

```json
{"cmd":"control_device","params":{"ieee_addr":"0xA4C138FF7D97628D","endpoint":1,"cmd":"on"}}
{"cmd":"control_device","params":{"ieee_addr":"0xA4C138FF7D97628D","endpoint":1,"cmd":"off"}}
{"cmd":"control_device","params":{"ieee_addr":"0xA4C138FF7D97628D","endpoint":1,"cmd":"toggle"}}
```
Válasz: `{"status":"ok","message":"Command sent"}`

---

#### `set_device_config`
Eszköz nevének, automatizálásának beállítása.

```json
{"cmd":"set_device_config","params":{"ieee_addr":"0xA4C138FF7D97628D","name":"Lampa1"}}
```

---

#### `delete_device`
Eszköz törlése.

```json
{"cmd":"delete_device","params":{"ieee_addr":"0xA4C138FF7D97628D"}}
```

---

#### `configure_sensor_thresholds`
Szenzor küszöbértékek beállítása.

```json
{"cmd":"configure_sensor_thresholds","params":{"ieee_addr":"0x...","endpoint":1,"threshold_low":20.0,"threshold_high":25.0}}
```

---

#### `link_sensor_devices`
Szenzor és vezérelt eszköz összekapcsolása.

```json
{"cmd":"link_sensor_devices","params":{"sensor_ieee":"0x...","sensor_ep":1,"device_ieee":"0x...","device_ep":1}}
```

---

### Szabályok (Rules Engine)

#### `get_rules`
Aktuális szabályszöveg és változók lekérdezése.

```json
{"cmd":"get_rules"}
```
Válasz: `{"status":"ok","text":"on boot do...","variables":[...],"timers":[...]}`

---

#### `set_rules`
Szabályszöveg feltöltése és mentése NVS-be.

```json
{"cmd":"set_rules","params":{"text":"on boot do\n    set var1 0\nendon"}}
```
Válasz: `{"status":"ok","rule_count":1}` vagy `{"status":"error","message":"Parse hiba szövege"}`

---

#### `reset_rules`
Összes szabály törlése.

```json
{"cmd":"reset_rules"}
```

---

#### `get_rules_timers`
Timerek aktuális állapota (aktív-e, hány másodperc van hátra).

```json
{"cmd":"get_rules_timers"}
```
Válasz: `{"timers":[{"id":1,"active":true,"remaining":15},{"id":2,"active":false,"remaining":0},...]}`

---

#### `set_rules_var`
Változó értékének beállítása (0-alapú index: var1=0, var2=1, ...).

```json
{"cmd":"set_rules_var","params":{"index":0,"value":0}}
{"cmd":"set_rules_var","params":{"index":1,"value":20.0}}
```
Válasz: `{"status":"ok"}`

---

#### `set_rules_varconfig`
Változó konfigurálása: alapérték és perzisztencia.

```json
{"cmd":"set_rules_varconfig","params":{"index":0,"persist":false,"default":1.0}}
{"cmd":"set_rules_varconfig","params":{"index":1,"persist":true,"default":20.0}}
```
- `persist: false` → bootkor visszaáll az alapértékre
- `persist: true` → bootkor az utolsó értéket veszi fel

Válasz: `{"status":"ok"}`

---

### Logok

#### `get_logs_live`
RAM-ban lévő élő log buffer lekérdezése.

```json
{"cmd":"get_logs_live"}
{"cmd":"get_logs_live","params":{"lines":50}}
```
Válasz: `{"status":"ok","log":"..."}` (utolsó N sor)

---

## Python példa (Orange Pi)

```python
import serial, json, time

def send_cmd(port, cmd, params=None):
    ser = serial.Serial(port, 115200, timeout=3)
    payload = {"cmd": cmd}
    if params:
        payload["params"] = params
    ser.write((json.dumps(payload) + "\n").encode())
    deadline = time.time() + 5
    while time.time() < deadline:
        line = ser.readline().decode(errors="ignore").strip()
        if line.startswith(">>>"):
            ser.close()
            return json.loads(line[3:])
    ser.close()
    return None

# Példák
print(send_cmd("/dev/ttyACM0", "get_status"))
print(send_cmd("/dev/ttyACM0", "set_rules_var", {"index": 0, "value": 0}))
print(send_cmd("/dev/ttyACM0", "set_global_settings", {"rules_enabled": True}))
```

---

## Válasz formátumok

| Eset | Mező | Érték |
|------|------|-------|
| Sikeres | `status` | `"ok"` |
| Sikeres (switch_mode, reboot) | `success` | `true` |
| Hiba | `status` | `"error"` |
| Hiba részlet | `message` | hibaüzenet szövege |
