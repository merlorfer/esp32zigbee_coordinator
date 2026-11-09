# Hasznalati utmutato — ESP32-C6 Zigbee Gateway

Ez a fajl a teljes rendszer attekintese es belepesi pontja. Minden
reszletes temahoz (soros parancsok, rules engine szintaxis, telefonos
webUI telepites) a tobbi `docs/` fajlra hivatkozik — ott nincs
megismetelve, ami mar le van irva.

---

## 1. Mi ez a rendszer

Az eszkoz egy **ESP32-C6 alapu Zigbee koordinator/gateway**, ami tobb
csatlakozasi modot (WiFi AP, BLE, soros) es tobb automatizacios reteget
(fix idopontos/ciklikus kapcsolas, szenzor-kuszobertek, es egy egyedi
"rules engine" szkript-motor) kombinal, plusz egy helyi (nem Zigbee)
XKC vizszint-erzekelot tud kezelni.

A rendszer **nem futtatja egyszerre a WiFi-t es a Zigbee-t** — ez a
ket "uzemmod" kozotti valasztas az elso dolog, amit erteni kell (lasd
lent).

---

## 2. Uzemmodok

| Mod | Mikor aktiv | Mire hasznalhato |
|-----|-------------|-------------------|
| **Setup mod (WiFi AP)** | Bekapcsolas/ujraindulas utan mindig ez indul elsonek | Idoszinkron (RTC), globalis beallitasok (XKC GPIO/mod, soros interfesz valasztas, log szuro), webUI elerese `192.168.4.1`-rol |
| **Operacios mod (Zigbee + BLE)** | Rovid gombnyomas (vagy a webUI "kilepes setup modbol" vezerloje) utan | Zigbee eszkozok parositasa/vezerlese, BLE-s vezerles/konfiguracio |

**Fontos korlatozasok:**
- Setup (WiFi AP) modban **eszkozt nem lehet parositani** (ehhez Zigbee-nek futnia kell).
- **Setup modba visszaterni csak ujrainditassal lehet** (fizikai reset,
  `reboot` parancs, vagy factory reset) — gombnyomassal vagy parancs-
  csal csak setup -> operacios iranyba lehet valtani, visszafele nem.
- **Hosszu gombnyomas** = Zigbee parositasi mod (permit-join) inditasa — ehhez mar
  operacios modban kell lenni.

A pontos parancsok (`switch_mode`, `reboot`, `factory_reset`,
`permit_join`, `get_status`) leirasa: **[serial_commands.md](serial_commands.md#rendszer)**
(ugyanezek BLE-n keresztul is elerhetok, azonos JSON formatummal).

---

## 3. Csatlakozasi lehetosegek

| Mod | Mikor erheto el | Hasznalat |
|-----|------------------|-----------|
| **WiFi AP** (`192.168.4.1`) | Csak setup modban | A gep sajat webUI-ja, bongeszobol kozvetlenul |
| **BLE** | Csak operacios modban | JSON parancsprotokoll; a webUI is ezt hasznalja Web Bluetooth-tal, de hosszabb tavon instabil lehet |
| **Soros** (USB Serial JTAG vagy UART0 GPIO16/17) | Minden modban elerheto | Ugyanaz a JSON parancsprotokoll mint BLE-n, `>>>` valasz-prefixszel — lasd **[serial_commands.md](serial_commands.md)** |
| **USB soros proxy (PC/OrangePi)** | Minden modban, soros kapcsolaton at | A sajat webUI-t szolgaltatja ki a gyari firmware helyett, soros kapcsolaton keresztul vezerelve az eszkozt — hasznos, mert a soros kapcsolat mindig elerheto, BLE/WiFi nem mindig. Teljes leiras: **[tools/usb_proxy/README.md](../tools/usb_proxy/README.md)** |
| **HTTPS webUI telepites telefonra** | Operacios modban (BLE-hez kell) | A telefon bongezoje kozvetlenul BLE-n csatlakozik az eszkozhoz, de a webUI fajlokat (HTML/JS/CSS) egy gepi szerver szolgaltatja ki HTTPS-en — ez kell ahhoz, hogy a telefon telepithesse a webUI-t PWA-kent. Teljes leiras: **[webui_app_update_on_phone.md](webui_app_update_on_phone.md)**, szerver eszkoz: **[tools/webui_https_server/README.md](../tools/webui_https_server/README.md)** |

---

## 4. Zigbee eszkozok kezelese

1. Operacios modban hosszu gombnyomassal (vagy `permit_join` parancs-
   csal) inditsd a parositasi modot.
2. Az uj eszkoz automatikusan felkerul a listara — nevet, bekapcsolt
   allapotat a webUI-n vagy a `set_device_config` / `control_device`
   parancsokkal lehet beallitani (**[serial_commands.md#eszkozok](serial_commands.md#eszközök)**).
3. **Tobb-bejegyzesu eszkozok** (pl. homerseklet+paratartalom egy
   szenzoron, vagy tobb-kimenetu dugaljsor): ezeket a rendszer `(ieee_addr,
   device_type)` vagy `(ieee_addr, endpoint)` alapjan kulonbozteti meg,
   csaladonkent eltero logikaval. Ismert, meg nem javitott hiba van ezzel
   kapcsolatban a natiiv WiFi AP felulet dugaljsor-szerkesztesenel —
   lasd **[todo.md](../todo.md)** a projekt gyokereben.

---

## 5. Beepitett eszkoz-automatizacio (rules engine nelkul)

Minden Zigbee kapcsolhato eszkoznek (pl. relé/aktuator) **ket beepitett
mod** kozul lehet valasztani:

- **Fix idopontos mod**: akar 5 db be/ki idoablak naponta.
- **Ciklikus (delay) mod**: 3-fazisu ciklus — OFF1 (kesleltetes) -> ON
  (tartas) -> OFF2 (szunet) -> ujra kezdi.

A **szenzor eszkozoknek** (homerseklet, paratartalom, vizszint, szivargas)
kuszobertek-alapu automatizacio allithato: also/felso kuszob, hiszterezis,
kesleltetes, es "linked device" — melyik aktuatort kapcsolja be/ki a
kuszob at- illetve vissza-lepesekor. Idotullepesre (nincs friss adat)
kulon hiba-aktuator es allapot allithato.

**Ez a reteg a rules engine-nel FUGGETLENUL, azzal PARHUZAMOSAN fut** —
lasd a reszletes magyarazatot a rules engine dokumentacio vegen.

---

## 6. Rules engine — egyedi automatizacios szabalyok

Egy ESPEasy-szeru, szoveges DSL (sajat szintaxis: `on ... do ... endon`,
`if/else`, valtozok, timerek, naplo-iras), amivel a fenti beepitett
automatizaciokon tul tetszoleges esemeny-vezerelt logika irhato.

**Teljes hasznalati leiras (szintaxis, minden parancs, formazasi
szabalyok, buktatok, peldak): [rules_engine_usage.md](rules_engine_usage.md)**

Architekturalis/fejlesztoi hatter (miert igy lett megtervezve):
**[rules_engine_plan.md](rules_engine_plan.md)**

---

## 7. Lokalis XKC vizszint-erzekelo

Egy, a Zigbee-tol fuggetlen, kozvetlenul az ESP32-C6 GPIO-jara kotott
vizszint-erzekelo, ami egy virtualis eszkozkent jelenik meg a tobbi
Zigbee eszkoz kozott (igy a fenti automatizacio es a rules engine is
latja/vezerelheti).

Ket erzekelesi mod kozul lehet valasztani a globalis beallitasokban:
- **Digitalis**: sima GPIO szint (be/kikapcsolt allapot).
- **Analog**: ADC-feszultseg + allithato kuszob (mV) — ideiglenes
  megoldas szintillesztő nelkuli bekotesnel, ahol a digitalis szint
  nem eleg megbizhato.

A log csak **valtozaskor** ir (nincs ismetlodo "keepalive" szemetelés
a naploban, sem digitalis, sem analog modban).

**Eszkoz torlese a szabalyok mellett**: a szabalyok automatikusan
ujraparse-olodnak, kezi ujramentes nem kell — reszletek:
**[rules_engine_usage.md, 8. pont](rules_engine_usage.md#8-eszkoz-torlese-utan-a-szabalyok-automatikusan-frissulnek)**.

---

## 8. Naplozas (log)

- A log egy RAM gyorsitotarban (elo nezet) es egy FAT-partition-ban
  (historikus, ~30 mp-enkent irva) is elerheto.
- Egyszerre csak **egy** fizikai soros interfeszen latszik a log kimenet
  (USB Serial JTAG VAGY UART0 GPIO16/17, a globalis `serial_interface`
  beallitastol fuggoen) — de a webUI/BLE/soros `get_logs_live` /
  `get_logs_history` parancsok mindig mukodnek, fuggetlenul ettol.
- A naplo torolheto (`DELETE /api/logs` vagy a webUI gombjaval).
- A rules engine-bol egyedi szoveg irhato a naplo**ba** `log "szoveg"`
  paranccsal — lasd **[rules_engine_usage.md](rules_engine_usage.md)**,
  "Log uzenet" szakasz.

Pontos parancsok: **[serial_commands.md, Logok](serial_commands.md#logok)**.

---

## 9. Konfiguracio mentese es visszaallitasa (export/import)

A teljes konfiguracio (minden eszkoz automatizacios/szenzor beallitasa,
globalis beallitasok, rules engine szabalyok es valtozok) exportalhato/
importalhato — ez **kizarolag soros kapcsolaton** mukodik (BLE-n
szandekosan nincs implementalva, mert a BLE hosszabb tavon instabil
lehet nagyobb adatatvitelhez).

A gyakorlatban a legegyszerubb mod erre a **USB soros proxy** eszkoz
"⚙ Config tools" panelje (export/import gombok rules.txt es config.json
fajlokhoz) — teljes leiras: **[tools/usb_proxy/README.md, "Config export / import" szakasz](../tools/usb_proxy/README.md#config-export--import)**.

Hasznos factory reset utan: eloszor `config.json` import (visszaallitja
az eszkoz-beallitasokat), majd `rules.txt` import — igy a Zigbee
eszkozok ujraparositasa utan (a parositasi/biztonsagi allapotot a
factory reset is torli) a korabbi nevek/beallitasok automatikusan
visszakerulnek.

---

## 10. Ismert korlatozasok

- **Tobb-kimenetu dugaljsor szerkesztese natiiv WiFi AP modban**: a
  mentes rossz kimenetre irhat. Reszletek es tervezett javitas:
  **[todo.md](../todo.md)**.
- **Setup (WiFi) modban nem lehet Zigbee eszkozt parositani** — csak
  idoszinkronra es globalis beallitasokra hasznalhato ott.
- **Setup modba visszaterni csak ujrainditassal lehet.**

---

## Gyors referencia — melyik dokumentumot mikor nezd

| Kerdes | Dokumentum |
|--------|------------|
| Milyen JSON parancsok vannak (BLE/soros)? | [serial_commands.md](serial_commands.md) |
| Hogyan irjak a rules engine szabalyokat? | [rules_engine_usage.md](rules_engine_usage.md) |
| Hogyan tudom a webUI-t telepiteni/frissiteni a telefonon? | [webui_app_update_on_phone.md](webui_app_update_on_phone.md) |
| Hogyan hasznalom a webUI-t PC-rol/OrangePi-rol soros kapcsolaton? | [tools/usb_proxy/README.md](../tools/usb_proxy/README.md) |
| Hogyan allitom be a HTTPS hidat a telefon-telepiteshez? | [tools/webui_https_server/README.md](../tools/webui_https_server/README.md) |
| Mi az a meg nem javitott hiba, amire figyelni kell? | [todo.md](../todo.md) |
