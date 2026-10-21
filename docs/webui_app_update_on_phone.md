# WebUI telepítés és frissítés telefonon (HTTPS híd a gépről)

## Mi ez és miért kell

A telefon böngészőjében a BLE-kapcsolat a webUI JavaScript kódjából (`web/script.js` / `web/ble-service.js`) a Web Bluetooth API-n (`navigator.bluetooth`) keresztül megy, közvetlenül a telefon és az ESP32 között. Ennek az API-nak viszont **"secure context"** kell (HTTPS, vagy localhost) — sima `http://` címről (pl. az ESP32 sajat 192.168.4.1 AP-jarol) a bongeszo ezt nem engedi.

A `tools/webui_https_server/` egy kis szerver, ami **ezen a gepen** szolgalja ki a `web/` mappa tartalmat (ugyanaz, ami a firmware-be is be van agyazva) HTTPS-en, sajat (helyi) tanusitvannyal. A telefon ehhez a gephez csatlakozik a feluletert, a BLE-forgalom viszont utana kozvetlenul a telefon <-> ESP32 kozott megy, a gep mar nem latja.

---

## Egyszeri beallitas (uj gepen, vagy ha a gep IP-je megvaltozik)

```
cd tools/webui_https_server
bash gen_cert.sh
```

Ez letrehoz egy helyi CA-t (`certs/ca-cert.pem`) es egy ettol alairt szerver-tanusitvanyt (`certs/cert.pem` + `certs/key.pem`), a gep aktualis IP-cimeire (helyi WiFi + VPN/Meshnet, ha van) ervenyesen. **Ha mar letezik CA, ujrafuttataskor azt megtartja**, csak a szerver-tanusitvanyt generalja ujra — ez fontos, mert ami miatt a telefon megbizik a CA-ban, az nem valtozik, tehat nem kell ujra telepiteni a telefonon.

---

## Szerver inditasa

```powershell
cd tools\webui_https_server
.\run_server.ps1            # alapertelmezett port: 8443
.\run_server.ps1 -Port 9000 # vagy mas port
```

A szerver kiirja, milyen cimeken erheto el (pl. `https://192.168.68.114:8443/` helyi WiFi-n, `https://100.x.x.x:8443/` VPN/Meshnet-en).

---

## Telepites a telefonon (elso alkalom)

1. **Nyisd meg** a `https://<gep-IP>:8443/` cimet a telefon bongeszojeben (ugyanazon a halozaton/VPN-en kell lennie, mint a gepnek).
2. Ekkor meg **"nem biztonsagos" figyelmeztetest** fog adni (ismeretlen tanusitvany) — kattints at rajta ("Specialis" -> "Folytatas", a pontos szoveg bongeszofuggo). Ez a lepes csak addig kell, amig a CA nincs telepitve a telefonon.
3. **Tolsd le a CA-tanusitvanyt**: nyisd meg a `https://<gep-IP>:8443/root-ca.pem` cimet. (Ugyanugy johet meg egyszer a fenti figyelmeztetes, ugyanugy kattints at rajta.)
4. **Telepitsd a CA-t a telefon Beallitasaibol** — FONTOS: ezt **nem** lehet kozvetlenul a letoltott fajlra kattintva megcsinalni (Android ezt blokkolja, "Can't install CA certificates" uzenettel, csak bezaras gombbal). A helyes ut:
   - Nyisd meg a telefon **Beallitasok** appjat.
   - A tetejen levo **kereso**be ird be: `tanusitvany telepitese` vagy `CA certificate` (a pontos menu gyartonkent/Android-verziomkent mas: pl. Pixel-en *Biztonsag -> Specialis -> Titkositas es hitelesito adatok*, Samsung-on *Biztonsag es adatvedelem -> Tovabbi biztonsagi beallitasok*).
   - Valaszd: **"Tanusitvany telepitese"** -> **"CA-tanusitvany"** (nem a "VPN es alkalmazas" opciot).
   - Itt is jon egy figyelmeztetes ("Telepites ennek ellenere" / "Install anyway" gombbal) — ezen az uton mar **lehet** folytatni.
   - Tallozd be a letoltott `root-ca.pem` fajlt (Letoltesek mappa), adj neki egy tetszoleges nevet (pl. "ESP32 WebUI CA"), erosisd meg.
   - Ha a telefonon nincs beallitva PIN/minta/jelszo a zarkepernyon, ezt eloszor be fogja kerni — CA-tanusitvany telepitesehez ez kotelezo.
   - Android ezutan egy allando ertesitest mutathat ("a halozati forgalom megfigyelheto") — ez a szokasos, elvart uzenet sajat CA telepitesekor, artalmatlan.
5. **Tolsd ujra** a `https://<gep-IP>:8443/` oldalt. Ha meg mindig piros/figyelmezteto ikont latsz (pedig inkognito ablakban mar rendben van): a normal Chrome-profil megjegyezte a korabbi "nem biztonsagos" dontest erre a cimre. Javitas:
   - Chrome -> a cim melletti ikon (vagy Beallitasok -> Webhelybeallitasok) -> keresd meg ezt a cimet -> **"Torles es visszaallitas"**.
   - Zard be teljesen a Chrome-ot (recent apps-bol kihuzni, vagy App info -> Eroltetett leallitas), majd nyisd meg ujra.
   - Ha ez sem eleg: Chrome -> Beallitasok -> Adatvedelem es biztonsag -> Bongeszesi adatok torlese -> idotartam "Minden ido", "Cookie-k es webhelyadatok" bepipalva -> Torles.
6. Most mar a cim mellett nem szabad figyelmeztetest latnod. Chrome menu (⋮) -> **"Telepites alkalmazaskent"** (nem csak "Parancsikon letrehozasa").

---

## Frissites kesobb (amikor a `web/` fajlok valtoznak)

**Nincs kulon teendo a telefonon.** A service worker (`web/service-worker.js`) "halozat elsokent" strategiat hasznal: amikor a telefon el tudja erni a szervert, mindig a lemezen levo aktualis fajlokat tolti be, es csak akkor esik vissza a korabban elmentett valtozatra, ha eppen nincs elereshez (offline).

Ha tehat frissult a `web/` mappa tartalma:
1. Gyozodj meg rola, hogy a szerver fut (`run_server.ps1`).
2. A telefonon egyszeruen **told ujra / nyisd meg ujra** a telepitett appot (vagy a lapot) — a friss tartalmat fogja mutatni, kulon cache-torles vagy ujratelepites nelkul.

**Ha kozben a gep IP-cime megvaltozott** (uj WiFi-halozat, uj VPN-cim): futtasd ujra a `gen_cert.sh`-t (uj szerver-tanusitvanyt general, a meglevo CA-t ujrahasznalja) — a telefonon **nem** kell ujra semmit telepiteni, mert az uj tanusitvanyt ugyanaz a mar megbizhatonak jelzett CA irja ala.

---

## Osszefoglalo / gyors emlekezteto

| Mikor | Mit kell tenni |
|---|---|
| Uj gep, vagy uj IP-cim | `bash gen_cert.sh`, majd (uj gepnel) a telefonon ujra a CA-telepites |
| Szerver inditasa | `run_server.ps1` a `tools/webui_https_server` mappabol |
| Elso telepites a telefonon | ld. fenti "Telepites a telefonon" szakasz |
| `web/` fajlok frissitese | nincs teendo a telefonon, csak toltsd ujra az oldalt/appot |
| Meg mindig "nem biztonsagos" a CA telepitese utan | Chrome site-adatainak torlese arra a cimre + Chrome ujrainditasa |
