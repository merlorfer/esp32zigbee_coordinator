# Rules Engine - Hasznalati utmutato

## Alapok

A szabalyok szoveges formaban keszulnek a web UI "Szabalyok" szekcioban.
Minden szabaly egy `on ... do ... endon` blokkbol all.

```
on <esemeny> do
  <parancsok>
endon
```

---

## Esemenyek (triggerek)

| Szintaxis | Mikor fut | Pelda |
|-----------|-----------|-------|
| `on <EszkozNev> do` | Szenzor uj meresi adatot kuld | `on Temperature do` |
| `on timer <N> do` | Timer N lejart | `on timer 1 do` |
| `on boot do` | Rendszer indulaskor | `on boot do` |
| `on var<N> do` | Valtozo erteke megvaltozott | `on var1 do` |
| `on time <HH:MM> do` | Adott idopont elerve (percre pontos) | `on time 18:00 do` |

---

## Feltetelek

```
on Temperature do
  if [Temperature] < 18 and [hour] >= 6
    on Heater
  else
    off Heater
  endif
endon
```

### Osszehasonlito operatorok

| Operator | Jelentes |
|----------|----------|
| `<` | Kisebb |
| `>` | Nagyobb |
| `<=` | Kisebb vagy egyenlo |
| `>=` | Nagyobb vagy egyenlo |
| `=` | Egyenlo |
| `!=` | Nem egyenlo |

### Logikai operatorok

- `and` - ES kapcsolat (minden feltetelnek igaznak kell lennie)
- `or` - VAGY kapcsolat (legalabb egynek igaznak kell lennie)
- `not` - Tagadas (feltetel elott)

**FONTOS:** Egy `if` sorban max 3 feltetel lehet, es csak `and` VAGY `or`
hasznalhato (nem keverhetok).

```
if [Temperature] < 25 and not [var1] = 1
```

---

## Ertekek feltetelekben `[...]`

| Hivatkozas | Jelentes | Pelda ertek |
|-----------|----------|-------------|
| `[<EszkozNev>]` | Szenzor aktualis erteke | `[Temperature]` -> 22.5 |
| `[var1]`..`[var8]` | Felhasznaloi valtozok (float) | `[var1]` -> 1.0 |
| `[hour]` | Aktualis ora (0-23) | `[hour]` -> 18 |
| `[minute]` | Aktualis perc (0-59) | `[minute]` -> 30 |
| `[uptime]` | Bekapcsolas ota eltelt perc | `[uptime]` -> 1440 |

---

## Parancsok (akciok)

### Eszkoz vezerles

```
on Pump            // Eszkoz bekapcsolasa
off Pump           // Eszkoz kikapcsolasa
toggle Pump        // Eszkoz allapot valtas
```

### Timer

```
timerSet 1 300           // Timer 1 beallitasa 300 masodpercre
timerSet 2 [var1]        // Timer 2 beallitasa valtozo ertekevel
timerSet 3 [var1] + 60   // Timer 3 = valtozo + 60 mp
timerCancel 1            // Timer 1 megszakitasa
```

**Timer szamok:** 1-tol 8-ig.
**Timer felbontas:** ~10 masodperc (a scheduler ciklus alapjan).

### Valtozo beallitas

```
set var1 1                     // Fix ertek
set var1 0
set var1 [var1] + 1            // Noveles
set var1 [var1] - 1            // Csokkentes
set var1 [Temperature] - 5     // Szenzor ertek alapjan
set var2 [var1] + [var3]       // Ket valtozo osszege
```

**Valtozok:** `var1`-tol `var8`-ig. Mindegyik float tipusu.
Valtozo ertekmodositaskor `on var<N> do` esemeny triggerel.

**Persist konfiguracio:** Minden valtozo konfiguralhato, hogy ujrainditas utan
megorizze az erteket, vagy alapertekre alljon vissza (lasd lent).

### Komment

```
// Ez egy komment, a parser athagyja
```

---

## !!! FONTOS FORMAZASI SZABALYOK !!!

### 1. Eszkoznev NEM tartalmazhat szokozoket

**HIBAS:** `on Water Pump do` -> parse hiba, a "Pump"-ot kulon szonak veszi

**HELYES:** `on Water_Pump do`

Az eszkoz `custom_name` (Egyedi nev) mezojet a web UI-ban kell beallitani.
Hasznaljon aláhuzast (`_`) szokoz helyett.

### 2. Az `on` parancs vs. `on <esemeny> do` trigger

A parser a kontextusbol donti el:
- `on ... do` sor: TRIGGER (szabaly elejen)
- `on EszkozNev` sor (if/else blokkon belul): BEKAPCSOLAS parancs

**Ne nevezzen el eszkozt "do", "timer", "boot", "time", "var1".."var8" nevvel!**
Ezek foglalt szavak a trigger parserehez.

### 3. Beagyazott if NEM tamogatott

```
// HIBAS - nem mukodik:
on Temperature do
  if [Temperature] < 18
    if [hour] >= 6
      on Heater
    endif
  endif
endon

// HELYES - hasznaljon "and"-ot:
on Temperature do
  if [Temperature] < 18 and [hour] >= 6
    on Heater
  endif
endon
```

### 4. Egy if-ben csak AND vagy csak OR

```
// HIBAS - keverek:
if [Temperature] < 18 and [hour] >= 6 or [var1] = 1

// HELYES - valasszon egyet:
if [Temperature] < 18 and [hour] >= 6 and [var1] = 1
```

### 5. Max limitek

| Elem | Limit |
|------|-------|
| Szabaly blokkok szama | 10 |
| Feltetelek egy `if`-ben | 3 |
| Akciok egy then/else blokkban | 4 |
| Timerek | 8 (timer 1..8) |
| Valtozok | 8 (var1..var8) |
| Szabaly szoveg osszmerete | 2048 byte |

### 6. Kis- es nagybetu

A kulcsszavak **nem** kis-nagybetu-erzekenyek:
`on`, `ON`, `On` mind mukodik.
`timerSet`, `timerset`, `TIMERSET` mind mukodik.

Az **eszkoznevek** viszont **pontos egyezest** kovetelnek
(a `custom_name` mezovel osszehasonlitva):
`Temperature` != `temperature`

### 7. Valtozo-valtozas rekurzio

Ha egy `set var1 ...` utasitas triggereli az `on var1 do` szabalyt,
es az ujra modositja `var1`-et, az ismet triggerelohet.
A motor max 3 szintu rekurziot enged, utana leall
(vegtelen hurok vedelem).

---

## Pelda: Ontozes idoablakkal

```
// Vizszint alacsony -> 5 perc varakozas, utana ontozes
on WaterLevel do
  if [WaterLevel] < 1 and [hour] >= 6 and [hour] < 22
    timerSet 1 300
  endif
  if [WaterLevel] >= 1
    timerCancel 1
    off Pump
  endif
endon

// 5 perc eltelt, meg mindig alacsony -> Pump bekapcs 10 percre
on timer 1 do
  if [WaterLevel] < 1
    on Pump
    timerSet 2 600
  endif
endon

// 10 perc ontozes utan -> Pump kikapcs
on timer 2 do
  off Pump
endon
```

## Pelda: Valtozo mint feltetel

```
// Valtozo beallitasa: "futesi szezon"
on Temperature do
  if [Temperature] < 20 and [hour] >= 5 and [hour] < 23
    set var1 1
  else
    set var1 0
  endif
endon

// Valtozo alapjan aktuator vezerles
on var1 do
  if [var1] = 1
    on Heater
  else
    off Heater
  endif
endon
```

## Pelda: Idopontra inditott akcio

```
on time 06:00 do
  on Light
endon

on time 22:00 do
  off Light
endon
```

## Pelda: Szamlalo valtozoval

```
// Minden szenzor esemenyre noveljuk a szamlalot
on Temperature do
  set var2 [var2] + 1
endon

// Boot-kor nullazzuk
on boot do
  set var2 0
  set var1 0
endon
```

## Pelda: if / else hasznalata

Az `else` ag akkor fut le, ha az `if` feltetel **hamis**.

```
// Homerseklet alapu futes vezerlese:
// - Ha hideg ES megfelelo napszak -> futes be
// - Egyebkent -> futes ki
on Temperature do
  if [Temperature] < 18 and [hour] >= 6
    on Heater
  else
    off Heater
  endif
endon
```

Masik pelda: valtozo beallitasa if/else-szel, majd a valtozo
triggereli a tenyleges eszkozparancsot:

```
// 1. Szabaly: valtozo beallitasa a feltetelek alapjan
on Temperature do
  if [Temperature] < 20 and [hour] >= 5 and [hour] < 23
    set var1 1    // "futesi szezon" aktiv
  else
    set var1 0    // "futesi szezon" inaktiv
  endif
endon

// 2. Szabaly: eszkoz vezerlese a valtozo alapjan
on var1 do
  if [var1] = 1
    on Heater
  else
    off Heater
  endif
endon
```

**Hasznos minta**: Az elso szabaly "dontesi logika" (szamitja az allapotot),
a masodik "vegrehajtas" (a tenyleges kapcsolast csinalja). Igy a logika
es a vezErles szet van valasztva, es a valtozo allapotat mas szabalyok
is lathatjak `[var1]` hivatkozassal.

---

## Pelda: Timer dinamikus idovel

```
on WaterLevel do
  if [WaterLevel] < 1
    timerSet 1 [var3]
  endif
endon
```
Itt a `var3` erteket elozetesen be kell allitani (pl. a web UI-bol
vagy egy masik szaballyal), es az adja meg a timer idotartamat masodpercben.

---

## Valtozo persist konfiguracio

Minden valtozo (var1..var8) konfiguralhato, hogy ujrainditas utan hogyan viselkedjen.

### Persist modok

| Mod | Ikon | Ujrainditas utan |
|-----|------|-----------------|
| **Persist** (alapert.) | 💾 | Az utolso erteket tartja meg (NVS-bol toltodik) |
| **Nem persist** | 🔄 | Az elore beallitott alapertekre all vissza |

### Konfiguracio a web UI-bol

A "Szabalyok" szekcioban minden valtozo karten a **⚙** gombbal:
1. Megadhato, hogy **persist** vagy **nem persist** legyen a valtozo
2. Nem persist eseten megadhato az **alapertek** (ezt kapja indulaskor)

### Tipikus hasznalati esetek

**Persist valtozok** (alapertelmezett) — beallitasok, amik megmaradnak:
```
var4 = 300   // Ontozesi idotartam (mp) — legyen 300 mp alapbol
             // Felhasznalo atat allitja, meg kell maradnia
```

**Nem persist valtozok** — allapotok, amik resetelodjenek:
```
var1 = 0    // "Pump fut" flag — indulaskor legyen 0 (nem fut)
var2 = 0    // Szamlalo — indulaskor nullazodjon
```

### Pelda: Ontozesi idotartam konfiguralhato valtozoval

```
// var4 = ontozesi idotartam masodpercben (persist, alapert: 300)
// var5 = pump allapot (nem persist, alapert: 0)

on WaterLevel do
  if [WaterLevel] < 1
    on Pump
    set var5 1
    timerSet 2 [var4]   // var4 adja meg a pump futasi idot
  endif
  if [WaterLevel] >= 1
    timerCancel 2
    off Pump
    set var5 0
  endif
endon

on timer 2 do
  off Pump
  set var5 0
endon
```

A `var4` erteket a web UI-bol lehet modositani, es NVS-ben megmarad.
A `var5` minden indulasko 0-ra all (pump nem fut allapot).

---

## Hasznos tudnivalok

- A szabalyok az NVS-ben tarolodnak, ujrainditas utan megmaradnak
- A valtozok (var1..var8) alapertelmezetten persist (NVS-bol toltodnak)
- A valtozo persist viselkedese a web UI ⚙ gombjaval konfiguralhato
- Nem persist valtozok indulaskor a beallitott alaperteket kapjak
- A timerek NEM persistalodnak (ujrainditaskor nullazodnak)
- A `boot` esemeny minden Zigbee mod inditaskor lefut
- A szenzor triggerek csak akkor futnak, amikor uj meres erkezik
- A `time` trigger percre pontos (percenkent egyszer ellenorizve)
- A timer felbontas ~10 mp (a scheduler ciklus miatt)
- A szabalyok a meglevo threshold/delay automatizacio MELLETT futnak
