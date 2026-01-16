# Projekt Specifikáció: ESP32-C6 Zigbee Gateway & Automatizációs Központ

## 1. Kontextus és Környezet
- **Target Mikrovezérlő:** ESP32-C6
- **Framework:** ESP-IDF v5.x + ESP-ZIGBEE-SDK
- **Szerepkör:** Zigbee Coordinator (ZC)
- **Rádiókezelés:** Exkluzív mód (Wi-Fi vagy Zigbee vezérlés prioritás).

## 2. Funkcionális Követelmények

### A. Wi-Fi és Konfigurációs Felület (Beállítási fázis)
- **SoftAP:** `ESP32C6_AI_Test` (12345678). Csak beállításkor aktív.
- **Fizikai Vezérlés (GPIO 9):** Toggle kapcsoló a "Beállítási mód" (Wi-Fi ON) és az "Automatizációs mód" (Wi-Fi OFF) között.
- **Webes Felület:**
    - Dinamikus rendszeróra (JS alapú frissítés).
    - **Eszközkezelés:** Új Zigbee eszközök hozzáadása (Permit Join).
    - **Eszközlista:** Minden eszközhöz egyedi üzemmód (Fix időpont VAGY Időtartam/Delay).
    - **Automatizáció Mentése:** Az adatok mentése után a Wi-Fi kikapcsolható a gombbal az üzembe helyezéshez.

### B. Zigbee és Automatizációs Logika (Működési fázis)
- **Erőforrás optimalizáció:** - Ha a Wi-Fi AP BE van kapcsolva: Minden Zigbee eszköz KIKAPCSOL, a hálózat "pihen", hogy a webes konfiguráció zavartalan legyen.
    - Ha a Wi-Fi AP KI van kapcsolva: A mikrovezérlő minden erőforrását a Zigbee stack-nek és az időzítőknek szenteli.
- **Vezérlési módok eszközönként:**
    1. **Fix Időpont:** HH:MM-kor BE, majd HH:MM-kor KI kapcsolás (RTC alapján).
    2. **Időtartam (Delay):** Az AP kikapcsolásának pillanatában elindul a visszaszámlálás (X perc múlva BE, Y perc múlva KI).
- **Adattárolás:** Minden beállítás (eszközlista, módok, időpontok) az NVS-be kerül mentésre.

## 3. Rendszer Architektúra
- **WiFi_Task:** Csak akkor fut, ha a gombbal aktiválták. Kezeli a webszervert és a JSON API-t.
- **Zigbee_Task:** Koordinátori feladatok, parancsküldés a kapcsolóknak.
- **Scheduler_Task:** A központi "agy". Kezeli az RTC-t és a szoftveres timereket. Wi-Fi leálláskor aktiválja a Delay-alapú folyamatokat.
- **Button_Task:** `switch_driver.c` forráskód integrálása a fizikai módváltáshoz.

## 4. Build és Telepítés
- Környezet: `esp-zigbee-sdk` integráció szükséges.
- Export: `. D:\Programing\esp-idf\v5.5.1\esp-idf\export.ps1`
- Target: `idf.py set-target esp32c6`