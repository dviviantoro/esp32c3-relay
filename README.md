# KITB-Relay

ESP32-C3 (Lolin C3 Mini) relay pulse controller driven by a Raspberry Pi over USB CDC.
Relays: GPIO6 = relay 1, GPIO7 = relay 2 (active-high; flip `RELAY_ON` in `src/main.cpp` if not).

## Wire protocol v1
Newline-terminated flat JSON, max 127 bytes per line. Serial carries protocol only (no logs).

| Request | Replies |
|---|---|
| `{"id":N,"cmd":"pulse","relay":1,"ms":1000}` (`ms` 1-5000, default 1000) | `{"id":N,"ok":true,"evt":"ack"}` then `{"id":N,"ok":true,"evt":"done","relay":1,"ms":1000}` |
| `{"id":N,"cmd":"ping"}` | `{"id":N,"ok":true,"evt":"pong","proto":1,"fw":"1.1.0","uptime_ms":..,"relays":[0,0],"busy":false,"last_done":..,"errs":..,"drops":..,"reset":".."}` |
| anything invalid | `{"id":N,"ok":false,"err":CODE}` (`id` 0 if unreadable) |

Errors: `BAD_JSON BAD_ID UNKNOWN_CMD BAD_RELAY BAD_MS BUSY LINE_TOO_LONG`.
`id` is required, 1..2^32-1.

**Retries:** resend a pulse with the *same* id. The firmware remembers the last 8 pulse ids for 10 s
and replays `ack`/`done` instead of firing again (same id with different content -> `BAD_ID`).
Use fresh ids per new pulse (the Python client starts at a random id).

**Health check:** ping every 2-5 s; 3 misses = offline. `uptime_ms` going backwards or `reset` != `usb`/`power_on`
means the device rebooted. `drops` counts replies discarded because the host wasn't reading.

## Safety
Task watchdog (2 s) resets the MCU on a hang. GPIOs then float, so fit pull-downs (or fail-safe
relay wiring) so a reset never leaves a relay energised. Pulses are hard-capped at 5 s.

## Dev
- Firmware: `pio run -t upload`
- Parser tests (host, with ASan/UBSan + fuzz): `pio test -e native`
- Pi client: `pip install pyserial pytest && pytest pi/tests`; `python pi/test_relay.py /dev/ttyACM0`

## Message field reference

### Requests (Pi -> ESP32)
| Field | Type | Used by | Required | Description |
|---|---|---|---|---|
| `id` | uint32, 1..4294967295 | all | **yes** | Request identifier chosen by the Pi. Echoed in every reply so replies can be matched to requests; also the key for retry de-duplication. `0`, missing, negative or non-integer -> `BAD_ID`. |
| `cmd` | string | all | yes | `"pulse"` or `"ping"`. Anything else -> `UNKNOWN_CMD`. |
| `relay` | uint, 1..2 | pulse | yes | Which relay to energise (1 = GPIO6, 2 = GPIO7). Otherwise `BAD_RELAY`. |
| `ms` | uint, 1..5000 | pulse | no (default 1000) | Pulse duration in milliseconds. Otherwise `BAD_MS`. |

Unknown extra keys with scalar values are ignored. Nested objects/arrays are rejected (`BAD_JSON`).

### Replies (ESP32 -> Pi)
| Field | Type | Present in | Description |
|---|---|---|---|
| `id` | uint32 | all | Echo of the request `id`; `0` when the request line was unreadable (bad JSON, too long). |
| `ok` | bool | all | `true` = success/event, `false` = error. |
| `evt` | string | `ok:true` | `"ack"` (pulse accepted, relay now on), `"done"` (pulse finished, relay off), `"pong"` (ping answer). |
| `err` | string | `ok:false` | Error code, see list above. |
| `relay` | uint | `done` | Relay that was pulsed. |
| `ms` | uint | `done` | Duration that was applied. |
| `proto` | uint | `pong` | Protocol version (currently 1). |
| `fw` | string | `pong` | Firmware version. |
| `uptime_ms` | uint64 | `pong` | Milliseconds since boot. Decreasing value = device rebooted. |
| `relays` | [uint, uint] | `pong` | Live state of relay 1 and 2 (1 = energised). |
| `busy` | bool | `pong` | `true` if any relay is currently pulsing. |
| `last_done` | uint32 | `pong` | `id` of the most recently finished pulse (0 = none since boot). Lets the Pi learn the outcome if it missed a `done`. |
| `errs` | uint32 | `pong` | Count of error replies sent since boot. |
| `drops` | uint32 | `pong` | Replies discarded because the USB TX buffer was full. |
| `reset` | string | `pong` | Last reset cause: `power_on`, `usb`, `software`, `panic`, `task_wdt`, `int_wdt`, `wdt`, `brownout`, `deepsleep`, `unknown`. |

## Folders
- `pi/` - production client for the Raspberry Pi (`relay_client.py`, tests, `requirements.txt`).
- `macbook/` - manual test CLI for a MacBook; imports `pi/test_relay.py` so it tests the same code.
  ```
  python3 -m venv .venv && source .venv/bin/activate
  pip install -r macbook/requirements.txt
  python macbook/test_relay.py ping | pulse 1 1000 | health | demo
  ```
