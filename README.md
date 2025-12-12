# Broker Firmware

Firmware for the “Broker” controller: an ESP32-S3 based automation hub that embeds its own MQTT server, device manager, web user interface, and audio/MQTT action engine for escape rooms or interactive exhibits.

---

## Feature Highlights

- **Self-hosted MQTT broker** with QoS0/1, retain messages, last-will support, static ACL table, and a lightweight event bus bridge. Devices connect directly to the ESP32 over Wi-Fi.
- **Web UI** (Status, Audio, Devices, Settings) served from the firmware. Includes a form-based “Simple editor” plus a wizard for creating devices/templates without touching JSON.
- **Device manager & templates** that live on PSRAM, while profiles and backups persist on SD card. Templates include UID validators, laser hold timers, MQTT/flag triggers, interval tasks, and the new sequence lock for ordered MQTT puzzles.
- **Automation engine** capable of MQTT publish, audio play/stop, flag waits, loops, delays, and event bus steps. Multiple worker tasks prevent a single scenario from blocking the rest.
- **Audio subsystem** (I2S) for track playback, pause/resume, loop, and synchronization with scenarios.
- **Documentation** in `docs/` describing device/template setup in EN/RU, plus tests for configuration parsing.
- **Web & MQTT authentication**: Web UI credentials live in NVS (with a hardware reset pin). The broker keeps up to 16 MQTT credential slots (client ID + username + password) and ties each slot to an ACL entry.

---

## Hardware & Software Requirements

- ESP32-S3 board with PSRAM enabled.
- SD card wired as `/sdcard` (SPI or SDMMC) for profile storage and large JSON assets.
- Speaker or amplifier connected to the configured I2S pins.
- ESP-IDF **5.3.x** (tested with 5.3.3) and its Python toolchain.
- MQTT clients (devices) must support username/password logins when broker authentication is enabled.
- Optional: MQTT-capable peripherals (RFID readers, laser heartbeat, relay boards) that will connect to the built-in broker over Wi-Fi.

---

## Building & Flashing

```bash
idf.py set-target esp32s3
idf.py menuconfig      # configure Wi-Fi, MQTT port/ACL, audio pins, SD card mode
idf.py build
idf.py -p COMx flash monitor
```

Useful `menuconfig` sections:

- `Broker Configuration -> Wi-Fi`: STA SSID, password, AP fallback.
- `Broker Configuration -> MQTT`: broker port, keepalive, ACL toggles, and credentials for up to 16 MQTT clients.
- `Broker Configuration -> Audio / I2S`: BCLK, WS, DIN pins, amplifier enable GPIO.
- `Broker Configuration -> Web UI`: HTTP port, static asset compression.
- `Broker Configuration -> Web auth`: default username/password plus the GPIO used to reset credentials in the field.

---

## Runtime Configuration Flow

1. **Wi-Fi**: on first boot the ESP32 brings up its AP and, if STA credentials are set, also tries to join the configured network. The UI shows both AP and STA IPs.
2. **Web UI**: visit `http://<device-ip>/` and open the tabs (Status, Audio, Devices, Settings). Status exposes Wi-Fi, MQTT sessions, automation flags, SD card state.
3. **MQTT broker**: clients connect directly to the ESP32 on the configured port (default 1883). The ACL table in `mqtt_core` restricts publish/subscribe prefixes per client ID.
4. **Profiles**: in the Devices tab use the list on the left to add/clone/delete profiles. Only the active profile stays in PSRAM; everything else is serialized to `/sdcard/.dm_profiles`.
5. **Devices & templates**: add devices via Simple editor or Wizard, choose the template, and fill its fields (slots, heartbeats, MQTT routes). Scenarios and topics appear under the template card.
6. **Saving**: click “Save changes”. The manager validates the JSON, writes it to SD, reinitializes template runtimes, and logs the resulting memory usage.

### Status LED Feedback

- GPIO 48 drives the built-in WS2812 LED to reflect system health.
- Solid green (≈20% brightness) means Wi-Fi is connected and the SD card is mounted and healthy.
- Solid red indicates Wi-Fi isn’t up yet; blinking red warns about a missing or faulty SD card.
- These patterns come from `error_monitor` → `status_led`, so you can reuse the same API if extra states or custom colors are required.

---

## Built-in MQTT Broker

File `components/mqtt_core/mqtt_core.c` implements the server the peripherals connect to:

- Handles CONNECT/SUBSCRIBE/PUBLISH with QoS0/1, retain, and last-will (QoS2/TLS are intentionally omitted to fit the ESP32-S3 profile).
- Keeps up to 16 client sessions with per-client ACL entries (see `k_acl`). Each entry limits publish and subscribe prefixes; extend the table or add dynamic configuration as needed.
- Exposes stats in the Status tab (`mqtt_core_get_client_stats`).
- Bridges events: when a client publishes a topic tied to a template runtime, `dm_template_runtime_handle_mqtt` injects it into the automation engine.

> ACL checks are keyed by client ID, while CONNECT authentication can also require a username/password pair per slot. For untrusted networks configure both the credentials and the ACL entry so clients must match all three fields.

---

## Device Manager & Templates

The manager keeps the active profile in PSRAM while writing snapshots to SD (`components/device_manager`). Each device may use one template plus custom scenarios.

| Template ID | Use case | Key fields |
| ----------- | -------- | ---------- |
| `uid_validator` | Pair/cluster of UID readers that must match configured values. | Slots (source ID + allowed UIDs), success/fail MQTT topics and audio. |
| `signal_hold` | Laser/photoresistor puzzle that accumulates heartbeat time. | Heartbeat topic/timeouts, hold duration, relay topic/payloads, hold/complete tracks. |
| `on_mqtt_event` | Trigger scenarios on incoming MQTT topics/payloads. | Rule list (topic, payload, payload_required, scenario). |
| `on_flag` | React to automation flags toggling. | Flag name, required boolean, scenario per rule. |
| `if_condition` | Evaluate multiple flag requirements and run true/false scenario. | Logic mode (all/any), list of flag requirements, two scenario IDs. |
| `interval_task` | Run a scenario on a fixed period. | Scenario ID, interval in ms, optional “run immediately”. |
| `sequence_lock` | Enforce an ordered list of MQTT triggers. | Steps (topic + optional payload/hints), timeout/reset behavior, success/fail MQTT/audio/scenario outputs. |

Documentation:

- `docs/SCENARIO_SETUP.md` – step-by-step instructions in English for setting up each template in the UI.
- `docs/SCENARIO_SETUP_RUS.md` – the same in Russian.
- `docs/TEMPLATE_GUIDE.md` and `docs/TEMPLATE_GUIDE_RUS.md` – behavioral deep dives with verification steps and troubleshooting tips.

---

## Automation Engine & Scenarios

Scenarios are ordered step lists stored under each device. Supported step types (see `device_action_type_t`):

- `mqtt_publish`: topic, payload, QoS, retain flag.
- `audio_play` / `audio_stop`: track path and blocking mode.
- `set_flag`: set or clear named automation flags.
- `wait_flags`: wait for a set of flags (all/any) with optional timeout.
- `loop`: jump to a previous step with iteration limit.
- `delay`: pause for N milliseconds.
- `event_bus`: post custom events (topic + payload) on the internal bus.
- `nop`: placeholder for manual ordering.

Automation workers pull jobs from a queue, allowing multiple devices to run in parallel. Long audio or waits no longer stall the system because workers are configurable in menuconfig.

---

## Web UI Tour

- **Status**: Wi-Fi state, MQTT server stats, SD health, current flags, automation worker load. Provides form inputs for Wi-Fi/MQTT configuration.
- **Audio**: browse SD tracks, play/pause/stop manually, configure default volume, and test amplifier GPIO.
- **Devices**: two editors:
  - *Simple editor* for direct JSON-backed editing (tabs, topics, scenarios, templates).
  - *Wizard* for a guided flow to add templates quickly.
  JSON preview at the bottom shows what will be saved; you can copy it for backups.
- **Settings**: firmware info, OTA slot, ability to reboot the device, and API links.

### Web UI Authentication

- Default credentials are defined in `menuconfig → Broker Configuration → Web auth` (factory setting: `admin / admin`).
- On first boot the firmware hashes those values and stores them in NVS. Changing defaults requires either erasing NVS or triggering the hardware reset before a reboot.
- To recover access in the field, assign any unused GPIO in the same menu and short it to GND for ~10 seconds while the device is running. The log prints `web auth reset pin triggered`, all sessions are cleared, and the defaults become active again.
- After any password change or reset the firmware deletes existing cookies, so every browser must log in again.

### MQTT Client Authentication

- The broker maintains up to 16 user slots (client ID + username + password). Edit them in the Settings tab (`Broker Configuration → MQTT`) or via `menuconfig`.
- For each slot set all three fields to enable it. Leaving a client ID empty effectively disables that slot and frees the credential.
- Credentials are stored in NVS; `config_store` clamps `user_count` to the number of populated entries so unused slots do not waste RAM.
- Each configured client maps to an ACL entry in `mqtt_core` restricting publish/subscribe prefixes. Expand the ACL table when adding more clients or topics.
- Devices must supply matching credentials in their MQTT CONNECT packet (client ID + username + password). A mismatch causes the broker to drop the CONNECT before any automation template sees the message.
- When the hardware reset pin is triggered both the Web UI password and MQTT credential table revert to defaults, so keep an offline copy of production credentials.

---

## Persistence & Memory Strategy

- Configurations live in PSRAM (active profile only). `device_manager` allocates descriptors dynamically and frees them during reloads.
- Profiles are serialized to `/sdcard/.dm_profiles/<id>.bin`; JSON exports go to `/sdcard/device_manager.json`.
- `dm_template_runtime_reset` frees per-template linked lists before registering runtimes, preventing leaks when the UI reloads a configuration.
- Large JSON responses (status, files, config export) stream in chunks to minimize RAM spikes.

---

## REST API & Assets

Key HTTP endpoints (all under `/api`):

| Endpoint | Method | Description |
| -------- | ------ | ----------- |
| `/api/status` | GET | Wi-Fi, MQTT, SD, automation stats. |
| `/api/devices/config` | GET | Active configuration JSON. |
| `/api/devices/apply` | POST | Apply JSON payload (entire config or specific profile). |
| `/api/devices/profile/*` | POST | Create, rename, delete, or activate profiles. |
| `/api/devices/run` | GET | Trigger scenario (`device`, `scenario` query params). |
| `/api/templates.js` | GET | JS payload for the wizard/editor. |
| `/api/config/mqtt`, `/api/config/wifi` | GET/POST | Update router/broker parameters. |

`components/web_ui` hosts the HTTP handlers plus the `build_devices_wizard.py` script that assembles JS/CSS assets at build time.

---

## Logging & Debugging

- Use `idf.py monitor --timestamp` to correlate button presses, MQTT events, and scenario logs.
- Raise log verbosity for modules when debugging:
  - `ESP_LOG_LEVEL_DEBUG("template_runtime", ...)`
  - `esp_log_level_set("automation", ESP_LOG_DEBUG);`
  - `esp_log_level_set("mqtt_core", ESP_LOG_DEBUG);`
- Device Manager emits `dm_copy size=<bytes>` messages when reloading; monitor PSRAM/internal heap there.
- Web UI handlers log HTTP errors (400/500) to help diagnose malformed JSON or SD failures.

---

## Tests

Unit tests live next to their components and are wrapped by standalone test apps in `tests/`. Currently available:

- `tests/device_manager`: exercises JSON parsing (topics, scenarios, capacity limits).

Run a test suite:

```bash
cd tests/device_manager
idf.py set-target esp32s3
idf.py -p COMx flash monitor
```

Unity prints pass/fail to the serial console. Add more tests by copying the pattern and importing the component’s `test/*.c` files.

---

## Documentation Map

- `docs/SCENARIO_SETUP.md` / `_RUS.md`: hands-on UI instructions for every template.
- `docs/TEMPLATE_GUIDE.md` / `_RUS.md`: behavioral reference and verification steps.
- `docs/ARCHITECTURE.md`: system overview, data flow diagram, component responsibilities, and CI notes.
- `docs/` is the place for future guides (hardware wiring, troubleshooting, etc.).

---

## Repository Layout

```
components/
  automation_engine/      Scenario workers, step executor.
  audio_player/           I2S playback helpers.
  config_store/           Wi-Fi, MQTT, audio configuration.
  device_manager/         Core config, parser, profiles, templates, runtimes.
  event_bus/              Lightweight event dispatcher.
  mqtt_core/              Built-in MQTT broker.
  web_ui/                 HTTP handlers + asset builder.
docs/                     Template guides and how-tos.
main/                     `app_main`, Wi-Fi/bootstrap logic.
tests/                    Standalone test applications (Unity-based).
```

---

## Contributing & Roadmap Ideas

- When adding templates or automation steps, touch `dm_templates`, `template_runtime`, the web UI editors, and documentation simultaneously.
- Keep configurations ASCII-only unless a file already uses UTF-8 text.
- Ideas/TODOs often discussed:
  - Authentication for MQTT and the Web UI.
  - Streaming JSON responses for all HTTP handlers to shrink RAM spikes further.
  - Additional templates (combined triggers, state machines) and wizard cards.
  - Extended diagnostics (profiling automation queue latency).

Pull requests and issues are welcome. Please run `idf.py build` and relevant tests before submitting changes. Continuous improvement of documentation and templates helps integrators understand the system faster.
