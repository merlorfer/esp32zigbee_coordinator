# dev_proxy.py — Használati útmutató

## Mi ez?

A `dev_proxy.py` egy helyi Python webszerver, ami lehetővé teszi, hogy a web UI-t a
**gépedről** töltsd be (`localhost:8080`) ahelyett, hogy az ESP32-re égett SPIFFS képből
szolgálná ki. Az `/api/*` hívások továbbra is az igazi ESP-re mennek át (proxy).

```
Böngésző → localhost:8080
              ├─ statikus fájlok (index.html, script.js, ...)  ← helyi ./web/ mappa
              └─ /api/* kérések  ─────────────────────────────→ http://192.168.4.1/api/...
```

**Mire jó:**
- Web UI fájlokat (`web/index.html`, `web/script.js`) szerkeszted és azonnal látod a
  változást böngésző-frissítéssel — nem kell minden módosítás után újrafordítani és
  felégni az ESP-t.
- Gyorsabb fejlesztési ciklus: mentés → F5, kész.

---

## Előfeltételek

- Python 3.8+ (standard lib elég, nincs külső csomag)
- A gép csatlakozzon az ESP WiFi AP-jához (`ESP32C6_AI_Test`, jelszó: `12345678`)

---

## Indítás

A projekt gyökérkönyvtárából:

```bash
python dev_proxy.py
```

Alapértelmezett értékek:
| Paraméter | Alapértelmezett |
|-----------|----------------|
| ESP IP    | `192.168.4.1`  |
| Port      | `8080`         |

**Egyéni ESP IP vagy port:**
```bash
python dev_proxy.py 192.168.4.1 8080
python dev_proxy.py 192.168.1.50 9000
```

---

## Megnyitás böngészőben

```
http://localhost:8080
```

A proxy indításakor kiírja a pontos URL-t a konzolra.

---

## Amit automatikusan kezel

- **Service Worker törlés:** ha korábban a `192.168.4.1`-en volt megnyitva az oldal,
  a böngésző cache-elt service worker-t tarthat. A proxy automatikusan beilleszt egy
  kis JavaScript snippetet az HTML-be, ami törli ezeket, így nem akadályozzák a
  helyi fejlesztést.
- **CORS:** az API válaszokhoz hozzáadja az `Access-Control-Allow-Origin: *` fejlécet.
- **Összes HTTP metódus:** GET, POST, PUT, DELETE, OPTIONS — mind proxyzva van az ESP-re.

---

## Leállítás

`Ctrl+C` a terminálban.

---

## Tipikus fejlesztési munkafolyamat

1. ESP-t egyszer felégeted (firmware + SPIFFS) a normál módszerrel.
2. Csatlakozol az ESP WiFi AP-jához.
3. Elindítod a proxyt: `python dev_proxy.py`
4. Megnyitod: `http://localhost:8080`
5. Szerkeszted a `web/index.html` vagy `web/script.js` fájlt.
6. Böngészőben F5 — azonnal látod a változást.
7. Ha elégedett vagy az eredménnyel, normál build + flash (`build_now.ps1` +
   `flash_now.ps1`) hogy az ESP-n is frissüljön a SPIFFS.
