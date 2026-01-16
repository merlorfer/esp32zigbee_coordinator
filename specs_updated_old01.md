# Projekt Specifikáció: ESP32-C6 Zigbee Gateway & Automatizációs Központ

## 1. Kontextus és Környezet
- **Target Mikrovezérlő:** ESP32-C6
- **Framework:** ESP-IDF v5.x + ESP-ZIGBEE-SDK
- **Szerepkör:** Zigbee Coordinator (ZC)
- **Rádiókezelés:** Exkluzív mód (Wi-Fi vagy Zigbee vezérlés prioritás)
- **Build Környezet:**
  - Export: `. D:\Programing\esp-idf\v5.5.1\esp-idf\export.ps1`
  - Target: `idf.py set-target esp32c6`

## 2. Hardver Konfiguráció

### GPIO Kiosztás
- **GPIO 9:** Toggle gomb (Beállítási mód ↔ Automatizációs mód)
- **Beépített LED:** Állapotjelzés (a board beépített LED-je)

### LED Állapotjelzések
| Állapot | LED Viselkedés | Leírás |
|---------|----------------|---------|
| **Normál működés** | Másodpercenként villog | Automatizációs mód aktív, rendszer működik |
| **Wi-Fi AP aktív** | Folyamatos világítás | Beállítási mód, web interfész elérhető |
| **Újraindulás/Óra nincs beállítva** | Gyors villogás (0.5 sec) | Rendszer boot után, RTC nincs inicializálva |
| **Hiba történt** | Speciális minta (pl. 3x gyors villogás, szünet) | Zigbee kommunikációs hiba |

### RTC (Real-Time Clock)
- **Típus:** ESP32-C6 belső RTC
- **Inicializálás:** Webes felületen keresztül manuális beállítás
- **Áramszünet kezelés:** RTC resetelődik 00:00:00-ra, LED gyors villogással jelzi

## 3. Funkcionális Követelmények

### A. Wi-Fi és Konfigurációs Felület (Beállítási Mód)

#### SoftAP Paraméterek
- **SSID:** `ESP32C6_AI_Test`
- **Jelszó:** `12345678`
- **Aktiválás:** GPIO 9 gomb megnyomásával
- **IP Cím:** `192.168.4.1`

#### Webes Felület Funkciók

##### 1. Rendszeróra Beállítás
- **Időbeviteli mező:** `YYYY-MM-DD HH:MM:SS`
- **Kliens oldali frissítés:** JavaScript alapú, másodpercenként frissül
- **Szerver szinkronizáció:** "Óra beállítása" gomb küldés az ESP32-re
- **Visszajelzés:** Sikeres beállítás után a LED normál működésre vált

##### 2. Zigbee Eszközkezelés
- **Permit Join funkció:**
  - Gomb: "Új eszköz hozzáadása" (60 másodperc join időablak)
  - Visszaszámláló timer megjelenítése
  - Automatikus felület frissítés új eszköz csatlakozásakor
  
- **Eszközlista megjelenítés:**
  - Táblázat formátum minden csatlakozott eszközzel
  - Oszlopok:
    - Gyártó (lekérdezett Zigbee attribútum)
    - Eszköz típus (lekérdezett Zigbee attribútum)
    - IEEE cím (64-bit hexadecimális)
    - Endpoint ID
    - Egyedi név (felhasználó által szerkeszthető mező)
    - Műveletek (Szerkesztés, Törlés gombok)

##### 3. Eszköz Automatizáció Beállítása

**Eszközönkénti konfiguráció panel:**

###### Működési Mód Választó (Dropdown)
- **Fix Időpont:** Napi ismétlődő időpontok alapján
- **Delay (Késleltetés):** Wi-Fi AP leállításától számított idő alapján

###### Fix Időpont Mód Beállítások
- **Alapértelmezett:** 1 ON/OFF időpont pár
- **Bővíthetőség:** "Új időpont hozzáadása" gomb (max 5 pár)
- **Időpont bevitel:** 
  - ON idő: `HH:MM` formátum
  - OFF idő: `HH:MM` formátum
- **Törlés:** X gomb minden pár mellett (az első kivételével)
- **Logika:**
  - Ha az időpont elmúlt aznap → másnap ugyanakkor végrehajtás
  - Minden nap automatikusan ismétlődik

Példa konfiguráció:
```
Időpont 1: ON 06:00, OFF 08:00
Időpont 2: ON 18:00, OFF 22:00
Időpont 3: ON 22:30, OFF 23:00
```

###### Delay Mód Beállítások
- **X perc:** Késleltetés a bekapcsolásig (AP kikapcsolásától számítva)
- **Y perc:** Bekapcsolt állapot időtartama
- **Teljes időzítés:** T0 (AP KI) + X perc → BE, majd + Y perc → KI

Példa:
```
AP kikapcsolás: 10:00
X = 30 perc → Eszköz BE: 10:30
Y = 120 perc → Eszköz KI: 12:30
```

##### 4. Wi-Fi AP Leállítási Viselkedés Beállítás
- **Globális RadioButton opció:**
  - ○ **Állapot megtartása:** Eszközök maradnak jelenlegi állapotukban
  - ○ **Összes kikapcsolása:** Minden eszköz OFF parancs kap (alapértelmezett)

##### 5. Mentés és Alkalmazás
- **"Beállítások mentése" gomb:** 
  - Adatok mentése NVS-be
  - JSON formátumú válasz (sikeres/sikertelen)
  - Toast notification visszajelzés
- **"Automatizáció indítása" gomb:**
  - Wi-Fi AP leállítása
  - Zigbee stack aktiválása
  - Scheduler task indítása
  - LED normál működésre vált

##### 6. Hibanapló Megjelenítés
- **Hiba panel (csak ha van hiba):**
  - Vörös háttér, figyelmeztető ikon
  - Eszköz neve és IEEE címe
  - Hibaüzenet szövege
  - Időpont (YYYY-MM-DD HH:MM:SS)
  - "Hibák törlése" gomb
- **Megjelenés:** Oldal tetején, kiemelve
- **Automatikus törlés:** Wi-Fi AP újraindításakor

### B. Zigbee Működés és Automatizációs Logika

#### Támogatott Eszköztípusok
- **Jelenlegi:** ON/OFF Switch (Smart Plug, relay, solenoid valve)
- **Jövőbeli:** Analog/Continuous sensors (pl. vízszint, hőmérséklet)
- **Maximum eszközszám:** 10 darab

#### Erőforrás-allokáció Vezérlés
- **Wi-Fi AP BE állapot:**
  - Zigbee stack felfüggesztése
  - Eszközök kezelése a beállított viselkedés szerint (megtartás/kikapcsolás)
  - Teljes CPU/memória a webszerver számára
  
- **Wi-Fi AP KI állapot:**
  - Teljes erőforrás a Zigbee Coordinator-nak
  - Scheduler Task aktiválása
  - Időzítők elindítása (Delay mód esetén)

#### Parancsküldés és Visszaigazolás
- **ZCL parancsok:** `ZCL_CMD_ON_OFF_ON` és `ZCL_CMD_ON_OFF_OFF`
- **Visszaigazolás ellenőrzés:**
  - `ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID` esemény figyelése
  - Timeout: 5 másodperc
  - Sikertelen próbálkozás → hiba rögzítése

#### Hibakezelés
- **Hiba típusok:**
  - Eszköz nem válaszol (timeout)
  - Negatív visszaigazolás (error status)
  - Eszköz nem elérhető a hálózatban
  
- **Hiba tárolás (NVS):**
  - Eszközönként **1 legutóbbi hiba**
  - Mezők: IEEE cím, timestamp, error message
  - LED speciális hibaminta aktiválása
  
- **Hiba törlés:**
  - Wi-Fi AP kikapcsolásakor minden hiba törlődik
  - Következő hiba felülírja az előzőt

## 4. Rendszer Architektúra

### Task Struktúra

#### WiFi_Task
- **Prioritás:** 5
- **Stack méret:** 4096 byte
- **Futási feltétel:** GPIO 9 gomb által aktiválva
- **Felelősségek:**
  - SoftAP indítás/leállítás
  - HTTP szerver kezelés (libmicrohttpd vagy ESP HTTP Server)
  - JSON API endpoint-ok
  - NVS read/write műveletek

#### Zigbee_Task
- **Prioritás:** 10 (magasabb mint WiFi)
- **Stack méret:** 8192 byte
- **Futási feltétel:** Wi-Fi AP leállított állapot
- **Felelősségek:**
  - Coordinator inicializáció
  - Device discovery és binding
  - ZCL parancsok küldése
  - Callback kezelés (join, command response)
  - Error detection és logging

#### Scheduler_Task
- **Prioritás:** 8
- **Stack méret:** 4096 byte
- **Futási feltétel:** RTC inicializálva és Wi-Fi AP KI
- **Felelősségek:**
  - RTC óra tick kezelése
  - Fix időpont egyezések ellenőrzése (minutánként)
  - Delay timerek kezelése (FreeRTOS Software Timers)
  - Automatizációs parancsok triggere

#### Button_Task
- **Prioritás:** 12 (legmagasabb - interrupt jellegű)
- **Stack méret:** 2048 byte
- **Implementáció:** `switch_driver.c` integráció
- **Felelősségek:**
  - GPIO 9 interrupt kezelés
  - Debounce (50ms)
  - Mód váltás (Wi-Fi ↔ Zigbee)
  - LED állapot váltás

#### LED_Task
- **Prioritás:** 3 (alacsony - indikáció)
- **Stack méret:** 2048 byte
- **Felelősségek:**
  - LED GPIO vezérlés
  - Villogási minták implementálása
  - Állapot üzenet queue-ból olvasás

### Szinkronizáció és Kommunikáció
- **Event Groups:** Mód váltás jelzésére (WiFi_Mode_Bit, Zigbee_Mode_Bit)
- **Queues:**
  - `cmd_queue`: Scheduler → Zigbee parancsok (ON/OFF)
  - `led_queue`: Állapot üzenetek → LED Task
  - `error_queue`: Zigbee hiba → WiFi/NVS
- **Mutexek:** NVS access, device list modification

## 5. Adatstruktúra és Tárolás

### NVS Namespace-ek
- **`config`:** Globális beállítások
- **`devices`:** Eszköz lista
- **`errors`:** Hibanaplók
- **`rtc`:** Óra állapot (opcionális backup)

### Device Configuration Structure

```c
#define MAX_DEVICE_NAME_LEN 32
#define MAX_MANUFACTURER_LEN 32
#define MAX_MODEL_LEN 32
#define MAX_TIME_PAIRS 5

typedef struct {
    uint8_t hour;
    uint8_t minute;
} time_point_t;

typedef struct {
    time_point_t on_time;
    time_point_t off_time;
} time_pair_t;

typedef enum {
    MODE_FIXED_TIME = 0,
    MODE_DELAY = 1
} automation_mode_t;

typedef struct {
    // Zigbee identifikáció
    uint64_t ieee_addr;               // 64-bit IEEE address
    uint8_t endpoint;                 // Endpoint ID (általában 1)
    
    // Eszköz információk (Zigbee attribútumokból)
    char manufacturer[MAX_MANUFACTURER_LEN];  // ZCL_MANUFACTURER_NAME
    char model[MAX_MODEL_LEN];                // ZCL_MODEL_IDENTIFIER
    
    // Felhasználói beállítások
    char custom_name[MAX_DEVICE_NAME_LEN];    // Webes felületen szerkeszthető
    bool enabled;                              // Automatizáció engedélyezve
    
    // Üzemmód
    automation_mode_t mode;
    
    // Fix időpont mód
    uint8_t time_pair_count;          // 1-5 közötti érték
    time_pair_t time_pairs[MAX_TIME_PAIRS];
    
    // Delay mód
    uint16_t delay_on_minutes;        // X perc AP kikapcsolás után
    uint16_t delay_duration_minutes;  // Y perc bekapcsolt állapot
    
    // Állapotkövetés
    bool current_state;               // true=ON, false=OFF
    uint32_t last_command_timestamp;  // utolsó parancs unix timestamp
    
} device_config_t;
```

### Error Log Structure

```c
#define MAX_ERROR_MSG_LEN 64

typedef struct {
    uint64_t ieee_addr;               // Melyik eszköz
    uint32_t timestamp;               // Unix timestamp
    char error_message[MAX_ERROR_MSG_LEN];
    bool active;                      // true ha megjelenítendő
} device_error_t;
```

### Global Configuration Structure

```c
typedef struct {
    bool wifi_shutdown_behavior;      // true=megtartás, false=kikapcsolás
    uint8_t device_count;             // Jelenleg csatlakozott eszközök száma
    bool rtc_initialized;             // RTC be van-e állítva
    uint32_t last_rtc_set;            // Utolsó RTC beállítás timestamp
} global_config_t;
```

### NVS Kulcsok

| Namespace | Kulcs | Típus | Leírás |
|-----------|-------|-------|--------|
| `config` | `global` | blob | `global_config_t` |
| `config` | `wifi_behavior` | u8 | Wi-Fi shutdown behavior |
| `devices` | `count` | u8 | Eszközök száma |
| `devices` | `dev_0` ... `dev_9` | blob | `device_config_t` tömbök |
| `errors` | `err_0` ... `err_9` | blob | `device_error_t` tömbök |

## 6. API Specifikáció

### HTTP Endpoints

#### GET `/api/status`
**Válasz:**
```json
{
  "rtc_initialized": true,
  "current_time": "2025-01-16 14:30:45",
  "device_count": 3,
  "wifi_active": true,
  "zigbee_active": false
}
```

#### POST `/api/rtc/set`
**Kérés:**
```json
{
  "datetime": "2025-01-16 14:30:45"
}
```
**Válasz:**
```json
{
  "success": true,
  "message": "RTC beállítva"
}
```

#### POST `/api/zigbee/permit-join`
**Kérés:**
```json
{
  "duration": 60
}
```
**Válasz:**
```json
{
  "success": true,
  "expires_at": "2025-01-16 14:31:45"
}
```

#### GET `/api/devices`
**Válasz:**
```json
{
  "devices": [
    {
      "ieee_addr": "0x00124B001F2A3B4C",
      "endpoint": 1,
      "manufacturer": "IKEA",
      "model": "TRADFRI control outlet",
      "custom_name": "Nappali lámpa",
      "enabled": true,
      "mode": "fixed_time",
      "time_pairs": [
        {"on": "06:00", "off": "08:00"},
        {"on": "18:00", "off": "22:00"}
      ],
      "current_state": false
    }
  ]
}
```

#### POST `/api/devices/{ieee_addr}/config`
**Kérés:**
```json
{
  "custom_name": "Konyha konnektor",
  "enabled": true,
  "mode": "delay",
  "delay_on_minutes": 30,
  "delay_duration_minutes": 120
}
```

#### DELETE `/api/devices/{ieee_addr}`
**Válasz:**
```json
{
  "success": true,
  "message": "Eszköz törölve"
}
```

#### GET `/api/errors`
**Válasz:**
```json
{
  "errors": [
    {
      "ieee_addr": "0x00124B001F2A3B4C",
      "device_name": "Nappali lámpa",
      "timestamp": "2025-01-16 14:25:30",
      "message": "Eszköz nem válaszol (timeout)"
    }
  ]
}
```

#### POST `/api/errors/clear`
**Válasz:**
```json
{
  "success": true,
  "cleared_count": 2
}
```

#### POST `/api/wifi/shutdown`
**Kérés:**
```json
{
  "behavior": "power_off"  // vagy "maintain_state"
}
```
**Válasz:**
```json
{
  "success": true,
  "message": "Wi-Fi leállítása megkezdve",
  "zigbee_starting": true
}
```

## 7. Fejlesztési Fázisok

### Fázis 1: Alapinfrastruktúra (1-2 nap)
- [ ] Projekt generálás ESP-IDF példából
- [ ] GPIO és LED driver implementáció
- [ ] Button Task és debounce
- [ ] Event Group és Queue struktúrák
- [ ] NVS inicializálás és alapfunkciók

### Fázis 2: Wi-Fi és Webszerver (2-3 nap)
- [ ] SoftAP konfiguráció
- [ ] HTTP szerver beállítás
- [ ] JSON API endpoint implementáció
- [ ] RTC beállítás és szinkronizáció
- [ ] Alapvető HTML/CSS/JS frontend

### Fázis 3: Zigbee Coordinator (3-4 nap)
- [ ] Zigbee stack inicializálás
- [ ] Permit Join funkció
- [ ] Device discovery és attribute query
- [ ] ZCL ON/OFF parancs implementáció
- [ ] Command response callback kezelés

### Fázis 4: Scheduler és Automatizáció (2-3 nap)
- [ ] RTC tick kezelés
- [ ] Fix időpont ellenőrzés
- [ ] Delay timer implementáció
- [ ] Parancs queue és végrehajtás
- [ ] State machine minden eszközhöz

### Fázis 5: Hibakezelés és UI finomítás (2-3 nap)
- [ ] Error detection és logging
- [ ] Retry mechanizmus
- [ ] Hibanapló UI komponens
- [ ] LED hiba animáció
- [ ] Teljes weboldal responsive design

### Fázis 6: Tesztelés és Optimalizáció (3-5 nap)
- [ ] Unit tesztek (Scheduler, NVS)
- [ ] Integrációs tesztek (Wi-Fi ↔ Zigbee váltás)
- [ ] Memória profiling
- [ ] Energiafogyasztás optimalizálás
- [ ] Dokumentáció írás

## 8. Kritikus Megfontolások

### Biztonság
- [ ] Wi-Fi jelszó erősségének ellenőrzése
- [ ] HTTPS támogatás megfontolása (ESP-TLS)
- [ ] NVS titkosítás (flash encryption)
- [ ] Zigbee hálózat zárása (install code használat)

### Megbízhatóság
- [ ] Watchdog timer minden task-hoz
- [ ] NVS corruption detection
- [ ] Graceful degradation (partial function ha egy eszköz hibás)
- [ ] Logging és diagnosztika (ESP Log UART)

### Skálázhatóság
- [ ] Dinamikus eszköz limit kezelés (ha 10-nél több is kell)
- [ ] Jövőbeli eszköztípusok plugin architektúra
- [ ] OTA firmware update előkészítés

## 9. Tesztelési Checklist

### Funkcionális Tesztek
- [ ] GPIO 9 gomb minden állapotváltás
- [ ] LED minták minden szcenárióban
- [ ] RTC beállítás és időzítési pontosság
- [ ] Zigbee join és 10 eszköz kezelése
- [ ] Fix időpont minden időzóna (00:00, 12:00, 23:59)
- [ ] Delay számítás különböző időtartamokkal
- [ ] Wi-Fi shutdown viselkedés mindkét opcióval
- [ ] Hibajelzés és napló működés

### Határesetek
- [ ] Áramszünet közben
- [ ] RTC beállítás nélküli működés
- [ ] Eszköz leválasztás működés közben
- [ ] Egyidejű több eszköz parancs (race condition)
- [ ] NVS tele írás (memória limit)
- [ ] Nagyon hosszú delay értékek (overflow ellenőrzés)

### Teljesítmény
- [ ] Weboldal betöltési idő (<2 sec)
- [ ] API válaszidő (<500 ms)
- [ ] Zigbee parancs késleltetés (<1 sec)
- [ ] Memóriahasználat monitoring (heap, stack)
- [ ] CPU load méri task-onként

## 10. Dokumentáció Követelmények

### Felhasználói Dokumentáció
- Gyors telepítési útmutató
- Webes felület használati leírás képekkel
- Hibaelhárítási FAQ
- Zigbee eszköz kompatibilitási lista

### Fejlesztői Dokumentáció
- Architektúra diagram (task-ok, adatfolyam)
- API referencia (minden endpoint példákkal)
- NVS adatstruktúra leírás
- Build és debug útmutató
- Kód kommentek (Doxygen formátum)

---

**Utolsó frissítés:** 2025-01-16  
**Verzió:** 2.0  
**Státusz:** Részletesen specifikált, kész implementációra
