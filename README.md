# Broker Firmware

Firmware for the “Broker” controller that orchestrates interactive exhibits: card validators, laser/relay puzzles, audio cues, and any MQTT‑aware peripherals. The project targets ESP32‑S3 with PSRAM and an SD card used for profile storage and large JSON assets.

---

## 1. Architecture Overview

| Layer | Purpose |
| ----- | ------- |
| **Device Manager** (`components/device_manager`) | Holds the full configuration (profiles, devices, templates, scenarios). Primary entry point for persistence, profile cloning, and JSON import/export. |
| **Template Runtime** (`template_runtime.c`) | Registers runtime handlers for each template, subscribes to MQTT/flag events, and triggers automation scenarios. Memory is allocated dynamically per template/device pair. |
| **Automation Engine** (`components/automation_engine`) | Executes scenarios as queued jobs. Supports multiple worker tasks, blocking/non‑blocking audio, flag waits, and loops. |
| **Web UI** (`components/web_ui`) | HTTP handlers + static assets for the Simple Editor and Wizard. Talks to Device Manager via `/api/devices/*`. |
| **Supporting components** | `audio_player`, `mqtt_core`, `event_bus`, `config_store`, etc. |

Key design choices:

- Only the active profile is loaded into PSRAM; other profiles live on SD (`/sdcard/.dm_profiles/<id>.bin`).
- Large JSON operations stream from the active configuration while holding the manager lock.
- Template runtimes are linked lists allocated in (S)PSRAM, allowing the firmware to scale with the number of configured templates rather than fixed compile‑time limits.

---

## 2. Templates and Scenarios

Each device can have at most one template, which defines the runtime wiring and default scenarios:

| Template | Use case | Important fields |
| -------- | -------- | ---------------- |
| `uid_validator` | Multiple card readers that must collectively approve a track. | Slots (source topic + allowed UIDs), success/fail MQTT topics, audio tracks, optional signal outputs. |
| `signal_hold` | Laser or sensor that must stay active for N seconds. | Heartbeat topic, hold duration, MQTT signal payloads, audio cues. |
| `on_mqtt_event` | Trigger scenario when a topic/payload matches. | List of rules (`topic`, `payload`, optional QoS, target scenario). |
| `on_flag` | Trigger scenario when an automation flag changes state. | Rules referencing flag name + required boolean. |
| `if_condition` | Evaluate multiple flags and run either the “true” or “false” scenario. | Logic mode (all/any), array of flag requirements, two scenario IDs. |
| `interval_task` | Periodically run a scenario every `interval_ms`. | Scenario ID, interval in milliseconds. |

Scenarios consist of sequential steps (MQTT publish, audio play/stop, set flag, wait for flags, loop, delay, event bus). Automation engine ensures only a configured number of jobs execute in parallel; long blocking steps can delay other devices.

For a full UI walkthrough that shows how to add each template and its scenarios step by step, open `docs/SCENARIO_SETUP.md`. `docs/TEMPLATE_GUIDE.md` complements it with deeper behavior notes and troubleshooting tips.

---

## 3. Building & Flashing

```bash
idf.py set-target esp32s3
idf.py menuconfig      # configure Wi-Fi, MQTT broker, audio pins, SD card
idf.py build
idf.py flash monitor
```

Requirements:

- ESP-IDF **5.3 or newer** (tested with 5.3.3).
- ESP32-S3 board with PSRAM enabled.
- SD card connected as `/sdcard` (SPI or SDMMC) to store profiles and backups.
- Access to an MQTT broker (configure host/port/auth in menuconfig or config store).

Useful menuconfig sections:

- `Broker Configuration → MQTT` for broker URL.
- `Broker Configuration → Audio / I2S` for speaker pins.
- `Broker Configuration → Web UI` for HTTP port and static asset options.

---

## 4. Configuration Workflow

1. **Initial flash**: After flashing, the device advertises over Wi-Fi (if configured) and hosts the Web UI at `http://<device-ip>/devices`.
2. **Profiles**: The Simple Editor shows the list of profiles on the left. Add/clone/delete profiles as needed; every profile keeps its own device list.
3. **Create devices**:
   - Use **Add device** in Simple Editor or run the **Wizard** to generate a device based on templates.
   - Assign a unique device ID. The display name is purely cosmetic.
4. **Configure templates**:
   - Fill in template-specific sections (slots, heartbeat topics, MQTT rules).
   - For UID templates, `Slots` define MQTT topics from each reader; values are comma-separated UID strings.
5. **Define scenarios**:
   - Add topics (logical bindings) that will invoke device scenarios.
   - Under “Scenarios” add steps; use the toolbar to add MQTT publish, audio play/stop, set flag, wait, loop, delay, or event bus actions.
6. **Save**: Press “Save changes” to persist. Device Manager saves the active profile to SD (JSON snapshot + binary profile file) and reloads runtimes.
7. **Test**: Use the “Run scenario” dropdown at the bottom of the page to manually trigger a scenario to ensure it executes correctly.

Tips:

- For long-running puzzles, prefer non-blocking audio and avoid large `delay` values—use `interval_task` or `wait_flags`.
- Use automation flags to coordinate between multiple devices (e.g., a laser puzzle sets `laser_ok=true`, which the picture controller awaits).
- The Web UI’s JSON preview (bottom pane) shows the raw configuration that will be saved.

---

## 5. Runtime & Memory Notes

- `device_manager_lock_config()` / `device_manager_unlock_config()` must wrap any function that reads or writes the configuration (automation engine, web UI, export handlers).
- Template runtimes now allocate per device; `dm_template_runtime_reset()` frees all lists before re-registering templates.
- Persistence routines (`persist_locked`, `device_manager_sync_file`, export) operate directly on the locked `s_config`, avoiding full copies of `device_manager_config_t`.
- If PSRAM is limited, reduce `DEVICE_MANAGER_MAX_DEVICES`, tab/topic limits, or audio buffer sizes in `dm_limits.h` / menuconfig.

---

## 6. API & Web UI Endpoints

| Endpoint | Method | Description |
| -------- | ------ | ----------- |
| `/api/devices/config` | GET | Returns current configuration JSON (active profile). |
| `/api/devices/apply` | POST | Apply new configuration (body = JSON). Optional `?profile=<id>`. |
| `/api/devices/profile/*` | POST | `create`, `rename`, `delete`, `activate` operations. |
| `/api/devices/run` | GET | Trigger scenario (`device` & `scenario` query parameters). |
| `/api/devices/templates` | GET | Serve JS assets for wizard/editor. |

All handlers live in `components/web_ui/web_ui_devices.c`; authentication can be implemented via HTTP middleware if required.

---

## 7. Logging & Debugging

- Enable verbose logging for `template_runtime` and `automation` to trace scenario queues.
- `dm_profiles` logs profile load/save errors (missing SD card, corrupt file).
- `web_ui` logs HTTP requests and any JSON parsing failures.
- Use `idf.py monitor` with `--timestamp` to correlate inputs (button presses) with scenario execution.

Common issues:

| Symptom | Possible cause |
| ------- | --------------- |
| Scenario executes 3–5 seconds after trigger | Automation worker busy (long audio/loop). Increase worker count or refactor scenario steps. |
| Configuration not saving | SD card missing or write-protected; check `dm_storage` logs. |
| UID last value not showing in UI | Template runtime not registered (template removed / device ID mismatch). Check `template_runtime` logs. |

---

## 8. Repository Layout

```
components/
  automation_engine/        Scenario runtime and worker tasks.
  audio_player/             Audio playback abstraction (I2S).
  config_store/             Key-value storage for system settings.
  device_manager/
    device_manager.c        Core manager (locking, persistence).
    device_manager_parse.c  JSON -> config.
    device_manager_export.c Config -> JSON.
    template_runtime.c      UID, signal, MQTT, flag, condition, interval runtimes.
  event_bus/                Lightweight event distribution.
  mqtt_core/                MQTT client wrapper used by templates.
  web_ui/                   HTTP handlers and assets builder.
main/                       Entry point, Wi-Fi init, top-level tasks.
README.md                   (this file)
```

---

## 9. Contribution Guidelines

1. Use clang-format for C files (style shipped in repo).
2. Run `idf.py build` before submitting PRs.
3. When adding templates or scenario step types, update:
   - `dm_templates.h` / `dm_template_registry`.
   - `template_runtime`.
   - Web UI editors (JS) and wizard builder.
   - README (this document) if the user-facing features change.
4. For new devices/peripherals, extend the wizard state machine; use `components/web_ui/assets/build_devices_wizard.py`.

Questions, bug reports, and feature requests are welcome via issues or pull requests.
