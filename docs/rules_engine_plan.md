# Rules Engine - Architektura es implementacios terv

## Attekintes

ESPEasy-szeru szabalyrendszer a CLCode01 Zigbee Gateway projekthez.
Event-driven logika szoveges szabalyokkal, ami KIEGESZITI a meglevo
fix ido / delay / kuszobErtek automatizaciot.

## Architektura

```
[Web UI textarea]  --save-->  [NVS: rules text]
                                    |
                                    v
                              [rules_engine.c]
                              +-------------+
                              | Parser       | <- Boot-kor parse-olja a szoveget
                              |   v          |    -> belso rule_t tomb
                              | Evaluator    | <- 10 mp-kent a scheduler hivja
                              |   v          |    + szenzor adat event-kor
                              | Executor     | -> g_cmd_queue, valtozok, timerek
                              +-------------+
```

## Komponensek

### Uj fajlok
- `main/rules_engine.h` - Tipusok, API deklaraciok
- `main/rules_engine.c` - Parser + evaluator + executor + timer kezeles

### Modositott fajlok
| Fajl | Modositas |
|------|-----------|
| `main/CMakeLists.txt` | rules_engine.c hozzaadasa |
| `main/scheduler_task.c` | Rules engine hivasok (timer tick, sensor event, boot, time) |
| `main/nvs_manager.c/h` | Rules text + vars NVS mentes/betoltes |
| `main/wifi_task.c` | GET/POST /api/rules + POST /api/rules/var endpoint |
| `main/ble_handlers.c` | get_rules / set_rules / set_rules_var parancsok |
| `web/index.html` | Szabalyok szekció textarea + valtozo/timer kartyak |
| `web/script.js` | loadRules, saveRules, renderRulesState, BLE mapping |
| `web/style.css` | Rules editor stilus (monospace sotet tema) |

## Adatstrukturak

### rule_t (egy szabaly blokk)
```c
typedef struct {
    rule_event_type_t event_type;   // EVT_SENSOR, EVT_TIMER, EVT_BOOT, EVT_VAR_CHANGED, EVT_TIME
    uint8_t event_param;            // sensor index / timer number / var index
    uint16_t event_time;            // HH*60+MM (csak EVT_TIME-hoz)

    bool has_condition;
    rule_condition_t conditions[3]; // max 3 feltetel
    uint8_t condition_count;
    bool condition_logic_or;        // false=AND, true=OR

    rule_action_t then_actions[4];  // max 4 akcio
    uint8_t then_count;
    rule_action_t else_actions[4];
    uint8_t else_count;
} rule_t;
```

### rule_action_t (egy akcio)
```c
typedef struct {
    rule_action_type_t type;
    uint8_t target_index;
    float value;
    // Kifejezes tamogatas (set, timerSet):
    rule_value_t expr_left;
    int8_t expr_op;           // 0=nincs, '+'=osszeadas, '-'=kivonas
    rule_value_t expr_right;
} rule_action_t;
```

## Memoria haszalat

| Elem | Meret |
|------|-------|
| rule_t x 10 | ~1.5 KB |
| Szoveg NVS-ben | max 2048 byte |
| Valtozok (float x 8) | 32 byte |
| Timer allapotok (8) | 64 byte |
| Parser + Evaluator kod | ~6-8 KB flash |
| **Osszesen RAM** | **~2 KB** |

## API vegpontok

### WiFi HTTP
- `GET /api/rules` - Szabalyok szovege + valtozok + timerek JSON
- `POST /api/rules` - Szabalyszoveg mentes + parse, body: `{"text": "..."}`
- `POST /api/rules/var` - Valtozo ertekadas, body: `{"index": 0, "value": 1.5}`

### BLE parancsok
- `get_rules` - JSON (text + vars + timers)
- `set_rules` - params: `{"text": "..."}`
- `set_rules_var` - params: `{"index": 0, "value": 1.5}`

## Scheduler integracio

A `scheduler_task.c` 10 masodperces ciklusaban:
1. `process_sensor_data()` - szenzor adat utan `rules_engine_on_sensor_data()` hivas
2. `process_sensor_thresholds()` utan `rules_engine_timer_tick()` + `rules_engine_on_time_tick()`
3. `scheduler_task_start()` vegen `rules_engine_init()` + `rules_engine_on_boot()`

## Limitaciok

- Max 10 szabaly blokk
- Max 3 feltetel per if (and/or, nem beagyazott if)
- Max 4 akcio per then/else blokk
- Nincs string valtozo (csak float)
- Nincs beagyazott if/else (csak 1 szintu)
- 2KB max szoveg meret
- 8 timer, 8 valtozo
- Matematikai muveletek: csak + es - a `set` es `timerSet` parancsban
- Timer felbontas: 10 masodperc (a scheduler ciklus miatt)
