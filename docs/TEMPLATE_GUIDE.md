# Template & Scenario Guide

This guide walks through concrete examples for every built‑in template. Each section explains the setup (UI fields), the resulting automation scenario, and how to verify the behavior.

---

## 1. UID Validator (card readers)

**Goal:** Two card readers must accept specific UIDs; on success play “success.mp3” and publish MQTT confirmation, otherwise play “fail.mp3”.

1. **Device creation**
   - Device ID: `pictures_pair`
   - Template: `UID Validator`
2. **Slots**
   - Slot 1: `source_id = pictures/uid/scan1`, `values = ABC123, DEF456`
   - Slot 2: `source_id = pictures/uid/scan2`, `values = 112233`
3. **Actions**
   - Success: `success_topic = pictures/cmd/success`, `success_payload = ok`, `success_audio_track = /sdcard/audio/success.mp3`
   - Fail: `fail_topic = pictures/cmd/fail`, `fail_payload = fail`, `fail_audio_track = /sdcard/audio/fail.mp3`
4. **Scenarios** (auto-generated)
   - `uid_success`: MQTT publish + audio play.
   - `uid_fail`: MQTT publish + audio play.
5. **Verification**
   - Publish `ABC123` on `pictures/uid/scan1` and `112233` on `pictures/uid/scan2`: device logs `[UID] ... event=success`.
   - Any other UID triggers `uid_fail`.

---

## 2. Signal Hold (laser puzzle)

**Goal:** When the laser hits a photoresistor for 20 seconds, turn off the relay, play “complete.mp3”, and publish MQTT “done”. Pause the hold track when the beam drops.

1. **Device creation**
   - Device ID: `laser_guard`
   - Template: `Signal Hold`
2. **Fields**
   - `signal_topic = laser/relay/cmd`
   - `signal_payload_on = ON`, `signal_payload_off = OFF`
   - `heartbeat_topic = laser/heartbeat`
   - `required_hold_ms = 20000`
   - `heartbeat_timeout_ms = 1500`
   - `hold_track = /sdcard/audio/hold_loop.mp3`, `hold_track_loop = true`
   - `complete_track = /sdcard/audio/complete.mp3`
3. **Scenario** (auto-generated)
   - `signal_complete`: publish ON, play complete track, delay by `signal_on_ms` if set, then publish OFF.
4. **Heartbeat flow**
   - Every MQTT message on `laser/heartbeat` marks progress; if >1.5s without heartbeat, runtime pauses audio and resets accumulation.
5. **Verification**
   - Publish heartbeat once per second for 20s → scenario triggers.
   - Drop heartbeat (>1.5s) → audio pauses, progress resets.

---

## 3. MQTT Event Trigger

**Goal:** When an RFID reader publishes `quest/scanner` with payload `scan`, request validation; when payload `reset`, run a cleanup scenario.

1. **Device creation**
   - Device ID: `scanner_controller`
   - Template: `MQTT Event Trigger`
2. **Rules**
   - Rule 1: `topic = quest/scanner`, `payload = scan`, `payload_required = true`, `scenario = request_scan`
   - Rule 2: `topic = quest/scanner`, `payload = reset`, `scenario = reset_state`
3. **Scenarios**
   - `request_scan`: Step 1 = MQTT publish `pictures/cmd/scan1` with payload `scan`. Step 2 = set flag `scan_in_progress=true`.
   - `reset_state`: Step 1 = cancel audio (optional). Step 2 = set flag `scan_in_progress=false`.
4. **Verification**
   - Publish `scan` → see log `[MQTT trigger] ... scenario=request_scan`.
   - Publish any other payload → ignored unless `payload_required=false`.

---

## 4. Flag Trigger

**Goal:** When laser flag `beam_ok` becomes true, run scenario `laser_ok`. When flag `pictures_ready` becomes false, trigger `pictures_abort`.

1. **Device creation**
   - Device ID: `flag_router`
   - Template: `Flag Trigger`
2. **Rules**
   - Rule 1: `flag=beam_ok`, `state=true`, `scenario=laser_ok`
   - Rule 2: `flag=pictures_ready`, `state=false`, `scenario=pictures_abort`
3. **Scenarios example**
   - `laser_ok`: audio play `/sdcard/audio/laser_ok.mp3`, MQTT `pictures/cmd/allow`.
   - `pictures_abort`: stop audio, MQTT `pictures/cmd/abort`.
4. **Triggering flags**
   - Add scenario steps elsewhere to `set_flag beam_ok true` or `false`.
5. **Verification**
   - When automation sets `beam_ok` to true, log `[Flag trigger] ... scenario=laser_ok`.

---

## 5. Conditional Scenario (If Condition)

**Goal:** If `beam_ok` AND `uid_complete` are true, run `all_clear`; otherwise run `wait_retry`.

1. **Device creation**
   - Device ID: `condition_router`
   - Template: `Conditional Scenario`
2. **Fields**
   - `Logic mode = All conditions`
   - `Scenario if TRUE = all_clear`
   - `Scenario if FALSE = wait_retry`
   - Conditions:
     - Condition 1: `flag=beam_ok`, `state=true`
     - Condition 2: `flag=uid_complete`, `state=true`
3. **Scenarios**
   - `all_clear`: MQTT `quest/door/cmd` payload `unlock`.
   - `wait_retry`: Audio `please_wait.mp3`.
4. **Behavior**
   - Whenever either flag changes, runtime evaluates all conditions; if result toggles, appropriate scenario runs.
5. **Verification**
   - Set both flags true → `all_clear`.
   - Set any flag false → `wait_retry`.

---

## 6. Interval Task

**Goal:** Every 10 seconds, ping a heartbeat topic and play a short beep if the system is idle.

1. **Device creation**
   - Device ID: `heartbeat_task`
   - Template: `Interval Task`
2. **Fields**
   - `interval_ms = 10000`
   - `scenario = heartbeat_tick`
3. **Scenario `heartbeat_tick`**
   - Step 1: MQTT publish `broker/heartbeat` payload `tick`.
   - Step 2: Conditional step `wait_flags`: Mode = `all`, requirements = `system_idle=true`, timeout=0; only if idle proceed to step 3.
   - Step 3: Audio play `/sdcard/audio/beep.mp3` (non-blocking).
4. **Verification**
   - Observe logs `[Interval] dev=heartbeat_task scenario=heartbeat_tick` every 10 seconds.
   - Ensure MQTT broker receives `broker/heartbeat`.

---

## Web UI Tips for Template Setup

- Use the **Wizard** to bootstrap devices quickly; templates map 1:1 to wizard cards.
- The JSON preview at the bottom of the Simple Editor reflects all template fields (`template.uid`, `template.signal`, etc.), useful for manual tweaks.
- After modifying templates, click “Reload” to ensure runtime state matches the saved configuration.

---

## Automation Testing Checklist

| Step | Action |
| ---- | ------ |
| 1 | Enable verbose logging (`esp_log_level_set("template_runtime", ESP_LOG_DEBUG)`). |
| 2 | Use MQTT client (e.g., `mosquitto_pub`) to mimic device topics described above. |
| 3 | Validate automation flags via logs (`automation: flag <name> set to <value>`). |
| 4 | Trigger scenarios manually via `/api/devices/run?device=<id>&scenario=<id>` for sanity checks. |

With these examples, you can mix templates: e.g., UID success sets a flag consumed by `if_condition`, while an interval task reminds players every few seconds.
