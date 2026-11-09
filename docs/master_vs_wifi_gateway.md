# Emlekezteto: `master` es `wifi-gateway` agak kulonbsege

Allapot felvetele: 2026-11-09. A cel az volt, hogy a `wifi-gateway` bekeruljon
a `master`-be, es csak a `master` maradjon. A probalkozott merge-et **megszakitottuk**
(`git merge --abort`), mert elobb ki kell dolgozni a tobbkimenetu dugaljsor
(power strip) megoldast. Ez a fajl rogziti, hol tart a ket ag, hogy a merge
kesobb gyorsan elvegezheto legyen.

## Roviden

- `wifi-gateway` (`671ab3a`) **40 committal van a `master` elott**, 35 fajl,
  +2997 / -331 sor.
- A `master` (`ec19277`) 3 olyan commitot tartalmaz, ami `wifi-gateway`-ben
  mas commit-azonositoval, de **azonos tartalommal** mar benne van.
- Egy fajlban van valodi konfliktus: `main/ble_handlers.c`.

## A `master` 3 egyedi commitja (tartalmilag mar a wifi-gateway-ben is megvan)

| Commit | Tartalom |
|--------|----------|
| `ef44f6c` | Tobbkimenetu dugaljsor (EP2-EP5) automatizacio-mentesi hiba: uj `device_manager_update_by_endpoint()`, endpoint-alapu mentes a `ble_handlers.c`-ben, a `script.js` kuldi az `endpoint`-ot |
| `afc11f5` | Felrevezeto fallback-trigger javitasa a Zigbee eszkozfelfedezesben, ha nehany endpoint mar sikeresen felkerult |
| `ec19277` | Teves figyelmeztetes javitasa, ha nem-HA endpoint (pl. ZGP EP242) az utolso callback |

A merge-kiserletben ellenorizve: `zigbee_task.c`, `device_manager.c`,
`device_manager.h`, `web/script.js` a merge utan **byte-azonos** a wifi-gateway
verziojaval, `device_manager_update_by_endpoint` nincs duplan definialva. Ezekbol
a fajlokbol tehat semmi uj nem kerulne a masterbe.

## Mit hozna a wifi-gateway a masterbe

- Soros config export/import (`export_config`, `import_config`, `upload_*`),
  USB soros proxy (`tools/usb_proxy`).
- HTTPS webUI hid telefonhoz (`tools/webui_https_server`, PWA telepites),
  service worker network-first.
- Analog XKC mod + feszultseg a logban, csak valtozaskor logol; az XKC virtualis
  eszkoz megmarad GPIO/mod valtaskor.
- Rules engine: `log "szoveg"` akcio, az `if` elotti akciok feltetel nelkul
  futnak (`pre_actions`), automatikus ujraparse eszkoz torlese / import utan.
- UART0 soros interfesz GPIO16/17-en (volt GPIO20/21).
- Multi-cluster szenzor szerkesztes javitas (`ble_handlers.c`).
- Build szkriptek az uj ESP-IDF 5.5.5 kornyezetre (`build_now.ps1`, `flash_now.ps1`).
- Dokumentacio: `docs/hasznalat.md`, `docs/webui_app_update_on_phone.md`,
  `docs/rules_engine_usage.md` frissitesek, `todo.md`, `ebb_and_flow_260913.rul`.

## Az egyetlen konfliktus: `main/ble_handlers.c` (`handle_set_device_config`)

- `master`: a regi, inline mezoalkalmazas (~150 sor) + az `ef44f6c` endpoint-alapu mentese.
- `wifi-gateway`: a mezoalkalmazas a kozos `apply_device_fields_from_json()`
  fuggvenybe lett szervezve; a mentes mar ott is helyes (szenzor: tipus szerint,
  nem-szenzor: endpoint szerint, `ble_handlers.c` ~558. sor).
- **Javasolt feloldas a merge-nel**: a wifi-gateway oldalat kell megtartani; a
  master 156 soros kulonbsege a regi inline kod, amit a segedfuggveny lefed.
  Merge utan forditas es ellenorzes szukseges.

## Kesobb kidolgozando: tobbkimenetu dugaljsor

A dugaljsor-megoldast **meg ki kell dolgozni**; ez a merge elott/kozben tisztazando:

- A `master` `ef44f6c` javitasa **csak a BLE/soros utat** (`ble_handlers.c`) javitotta.
- A natív WiFi AP HTTP utvonal (`main/wifi_task.c`, `api_device_config_post_handler`)
  tovabbra is **hibas** dugaljsorra: a mentes mindig `device_manager_update_by_type()`
  alapjan tortenik, ezert mindig az elso kimenet kerul felulirasra. Reszletes
  leiras es javaslat: [`todo.md`](../todo.md).
- A ket ag mindket utvonalat (BLE/soros es WiFi AP) azonos modon kell kezelje:
  szenzor -> `(ieee, device_type)`, nem-szenzor -> `(ieee, endpoint)`.
- A merge-et celszeru a dugaljsor-megoldas elkeszulte utan elvegezni (vagy
  legalabb a `todo.md` hibaval egyutt tudatosan elfogadni).

## Egyeb agak

Az `origin`-on (es helyben) mas agak is vannak: `corrections`, `funcexp01`, `rules`,
`usbconnection`, valamint helyi `orangepi`, `thermostat`, `without-bind`,
`connection-with-binding`. Ezekhez nem nyultunk; a "csak a master maradjon"
cel kitero ezekre mindenkeppen kulon dontest igenyel.

## A merge elvegzese kesobb (lepesek)

1. `git checkout master`
2. `git merge wifi-gateway`
3. `main/ble_handlers.c` konfliktus: a wifi-gateway oldalat tartani
   (`git checkout --theirs main/ble_handlers.c`), majd `git add`.
4. Forditas (`build_now.ps1`), ha rendben: commit + push.
5. Csak ezutan, kulon jovahagyassal: `git branch -d wifi-gateway` es
   `git push origin --delete wifi-gateway`.
