# TODO: fix multi-outlet power strip editing over the native WiFi AP HTTP API

## Status

**Not fixed yet — deliberately deferred.** This is a planning note for a later
session. The user doesn't currently use a multi-outlet power strip, so this
isn't urgent, but it's a real, confirmed-by-code-reading bug and should be
fixed before anyone relies on editing a power strip's per-outlet settings
while the gateway is in WiFi AP / setup mode.

## Background

This is the sibling bug to the one already fixed in `main/ble_handlers.c`
(`handle_set_device_config`, commits `c8e442c` on `wifi-gateway` and
`346c278` on `export-import-params`): a device edit request can be looked up
and saved by the wrong key when a single Zigbee IEEE address maps to more
than one `device_config_t` entry.

There are two families of "one IEEE address, several entries" devices in
this codebase, and each needs a **different** disambiguator:

- **Multi-cluster sensors** (e.g. a combined temperature+humidity sensor):
  both entries share the same `endpoint`, so only `device_type` tells them
  apart.
- **Multi-outlet power strips**: each outlet is a separate `endpoint` with
  the *same* `device_type` (`DEVICE_TYPE_ON_OFF_LIGHT`), so only `endpoint`
  tells them apart.

The BLE/serial JSON path (`ble_handlers.c`) was fixed to try `device_type`
first for sensors and fall back to `endpoint` otherwise — see that function
for the exact pattern to mirror here.

## The bug (native WiFi AP HTTP path)

File: `main/wifi_task.c`, function `api_device_config_post_handler`
(registered for `POST /api/devices/<ieee>/config`).

**Fetch** (already correct for sensors, not relevant to power strips since
it only disambiguates by `device_type`):

```c
// Check for device_type to disambiguate multi-endpoint sensors
device_config_t dev;
bool found = false;
cJSON *dtype_obj = cJSON_GetObjectItem(root, "device_type");
if (cJSON_IsString(dtype_obj)) {
    device_type_t dtype = sensor_type_from_string(dtype_obj->valuestring);
    found = (device_manager_get_by_type(ieee_addr, dtype, &dev) == ESP_OK);
}
if (!found) {
    if (device_manager_get(ieee_addr, &dev) != ESP_OK) {
        ...
    }
}
```

**Save** (this is the actual bug):

```c
device_manager_update_by_type(ieee_addr, dev.device_type, &dev);
```

This is **unconditional** — every save goes through `device_manager_update_by_type()`,
which matches by `(ieee_addr, device_type)` only. For a multi-outlet power
strip, every outlet shares the same `device_type`
(`DEVICE_TYPE_ON_OFF_LIGHT`), so this always writes to whichever outlet
entry comes first in `s_devices[]` — never the one the user actually opened
in the edit form, unless it happens to be that first entry.

Net effect: editing outlet #2 or #3 of a power strip while connected over
the native WiFi AP page would silently overwrite outlet #1's config instead
(name, schedule, everything), while outlet #2/#3 stays unchanged and looks
"stuck" — the same class of symptom already seen and fixed for the
temperature+humidity sensor case.

Note this is the **opposite** shape of the original BLE-path bug: there,
`endpoint` was wrongly preferred over `device_type` (breaking sensors).
Here, `device_type` is the *only* thing used for the save (breaking power
strips). Both stem from the same root cause: an IEEE address can map to
several `device_config_t` entries, disambiguated by *different* fields
depending on device family, and the code picks one fixed field instead of
picking the right one per family.

## Why this wasn't hit by the just-fixed bug report

The user's reported bug (temp+humidity sensor edits landing on the wrong
entry) went through the BLE/serial/USB-proxy path (`ble_handlers.c`), not
this native WiFi AP HTTP path. For *sensors specifically*, this HTTP path
was already correct (type-first fetch, type-only save — right for sensors,
just wrong for power strips). The two paths had independently-written,
inconsistent disambiguation logic; only the BLE path was exercised by the
user's report.

## Proposed fix

Mirror the pattern already applied in `ble_handlers.c`'s
`handle_set_device_config` / `apply_device_fields_from_json`:

1. **Fetch**: if `device_type` is present *and* `is_sensor_device(dtype)`,
   look up by `(ieee_addr, device_type)` first. Otherwise (non-sensor, i.e.
   `DEVICE_TYPE_ON_OFF_LIGHT` / `DEVICE_TYPE_VIRTUAL`), look up by
   `(ieee_addr, endpoint)` if `endpoint` was supplied in the request body
   (it currently isn't parsed out of the POST body in this handler at all —
   that also needs adding, mirroring `cJSON *ep_obj = cJSON_GetObjectItem(root, "endpoint")`
   plus a call to `device_manager_find_by_ieee_and_endpoint()`). Fall back to
   `device_manager_get(ieee_addr, &dev)` (IEEE-only) as a last resort, same
   as today.
2. **Save**: `if (is_sensor_device(dev.device_type)) { device_manager_update_by_type(...); } else if (endpoint was supplied) { device_manager_update_by_endpoint(...); } else { device_manager_update_by_type(...); }`.
3. Apply the same fix to **both** `wifi-gateway` and `export-import-params`
   branches (cherry-pick across, same as the BLE-path fix — check for
   conflicts since `export-import-params` may have since touched nearby
   code).
4. **Verify**: pair (or simulate via `export_config`/`import_config`) a
   multi-outlet power strip so it has >1 `device_config_t` entries sharing
   one IEEE address and `device_type`, then edit outlet #2's name via the
   native WiFi AP page (`POST /api/devices/<ieee>/config` with an
   `endpoint` field) and confirm outlet #1 is untouched and outlet #2 is
   the one that actually changed. Also re-confirm the BLE/serial path
   still handles this same power-strip case correctly (it already does,
   per the earlier fix, but worth a regression check since it's the same
   function family).

## Files likely touched

- `main/wifi_task.c` — `api_device_config_post_handler` (fetch + save logic)

No changes expected to be needed in `main/ble_handlers.c`, `main/device_manager.c`,
or `web/script.js` — the frontend already sends both `device_type` and
`endpoint` in the save payload ([web/script.js:971-972](web/script.js#L971-L972)); this is purely a backend
lookup/save-key fix.
