# Scenario & Device Setup Instructions

This tutorial explains how to add devices and scenarios for **every built-in template** directly from the Broker web interface. Each section walks through:

1. UI actions to add a device.
2. Template-specific fields to fill in.
3. Scenarios to add or link.
4. A quick way to test the configuration.

> Navigate to **Devices → Simple editor** before starting. Use **Add device** for new entries and **Save changes** after each template is configured.

---

## 1. UID Validator (dual card readers)

**Goal:** Two UID scanners must both report allowed values before the “success” automation fires.

### Device fields
1. Click **Add device**, set **Device ID** to `pictures_pair`, choose template **UID Validator**.
2. Under **UID slots** add one entry per scanner:
   - Slot 1: `Source ID = pictures/uid/scan1`, `Values = ABC123,DEF456`.
   - Slot 2: `Source ID = pictures/uid/scan2`, `Values = 112233`.
3. In **Success actions**:
   - `MQTT topic = pictures/cmd/green`, `Payload = pass`.
   - Select `Audio track = /sdcard/audio/pictures_success.mp3`.
4. In **Fail actions**:
   - `MQTT topic = pictures/cmd/red`, `Payload = fail`.
   - Select `Audio track = /sdcard/audio/pictures_fail.mp3`.

### Scenario hookup
The template auto-creates `uid_success` and `uid_fail`. Open **Scenarios** panel, confirm both contain:
- Step 1: `mqtt_publish` with the topic/payload above.
- Step 2: `audio_play` with the selected track.

### Test
Publish UID payloads to both `pictures/uid/scan*` topics via MQTT. Logs should show `[UID] ... event=success` when both slots match; otherwise `uid_fail` runs.

---

## 2. Signal Hold (laser puzzle)

**Goal:** Hold a laser beam for 20 seconds. While the beam is present, loop a track; when the timer completes, fire a finish scenario.

### Device fields
1. Add device `laser_guard`, choose template **Signal Hold**.
2. Fill the template section:
   - `Heartbeat topic = laser/heartbeat`
   - `Reset topic = laser/reset` *(optional — publish anything here to clear progress)*
   - Enable **Debug logging** if you need detailed heartbeat/audio logs for troubleshooting.
   - `Heartbeat timeout (ms) = 1500`
   - `Required hold (ms) = 20000`
   - `Hold track = /sdcard/audio/laser_loop.mp3`, enable **Loop**.
   - `Complete track = /sdcard/audio/laser_complete.mp3`
   - `Relay topic = laser/relay/cmd`, `Payload ON = ON`, `Payload OFF = OFF`.

### Scenario hookup
The template creates `signal_complete`. Edit it to:
1. Step 1: `mqtt_publish` (`laser/relay/cmd`, payload `ON`).
2. Step 2: `audio_play` complete track.
3. Step 3: `delay` (1000 ms if the relay must stay on briefly).
4. Step 4: `mqtt_publish` (`laser/relay/cmd`, payload `OFF`).

### Test
Send MQTT heartbeats (`laser/heartbeat` + payload `hb`) every second. Watch logs for `[Signal] hold=XXms`. When `20000` ms reached, `signal_complete` fires. Remove the beam (stop heartbeat) and confirm the loop pauses immediately. Publish to `laser/reset` to fully reset the timer/audio.

---

## 3. MQTT Event Trigger

**Goal:** React to specific MQTT commands with different scenarios.

### Device fields
1. Add device `scanner_controller`, select template **MQTT Event Trigger**.
2. Under **Rules** add:
   - Rule 1: `Topic = quest/scanner`, `Payload = scan`, `Require payload = true`, `Scenario = request_scan`.
   - Rule 2: `Topic = quest/scanner`, `Payload = reset`, `Scenario = reset_state`.

### Scenarios
- `request_scan`
  1. Step: `mqtt_publish` topic `pictures/cmd/scan1`, payload `scan`.
  2. Step: `set_flag` name `scan_in_progress`, value `true`.
- `reset_state`
  1. Step: `audio_stop_all`.
  2. Step: `set_flag scan_in_progress false`.

### Test
Publish `scan` to `quest/scanner`. The log `[MQTT trigger] ... scenario=request_scan` should appear. Publish `reset` to see the second scenario.

---

## 4. Flag Trigger

**Goal:** Launch automations whenever a high-level flag toggles.

### Device fields
1. Add device `flag_router`, template **Flag Trigger**.
2. Add rules:
   - Rule 1: `Flag = beam_ok`, `State = true`, `Scenario = laser_ok`.
   - Rule 2: `Flag = pictures_ready`, `State = false`, `Scenario = pictures_abort`.

### Scenarios
- `laser_ok`: play `/sdcard/audio/laser_ok.mp3`, publish MQTT `pictures/cmd/allow`.
- `pictures_abort`: stop audio, publish MQTT `pictures/cmd/abort`, set flag `beam_ok false`.

### Test
Use any scenario or `/api/devices/run` to execute `set_flag beam_ok true`. The log `[Flag trigger] ... laser_ok` confirms activation. Flip `pictures_ready` to `false` to test the abort path.

---

## 5. Conditional Scenario (if_condition)

**Goal:** Evaluate multiple conditions at once, routing to success or fallback scenarios.

### Device fields
1. Add device `condition_router`, choose **Conditional Scenario**.
2. Configure:
   - `Logic mode = All conditions`
   - `Scenario if TRUE = all_clear`
   - `Scenario if FALSE = wait_retry`
3. Add conditions:
   - Condition 1: `Type = Flag`, `Flag name = beam_ok`, `State = true`.
   - Condition 2: `Type = Flag`, `Flag name = uid_complete`, `State = true`.

### Scenarios
- `all_clear`: publish `quest/door/cmd` payload `unlock`.
- `wait_retry`: audio `please_wait.mp3`, optional MQTT `quest/hint`.

### Test
Set both flags true → `all_clear` runs once. Clear any flag → `wait_retry` fires. Watch `[Condition] result=true/false` logs for insight.

---

## 6. Interval Task

**Goal:** Run a scenario on a fixed timer.

### Device fields
1. Add device `heartbeat_task`, template **Interval Task**.
2. Fields: `Interval (ms) = 10000`, `Scenario = heartbeat_tick`, optional `Run immediately = true`.

### Scenario `heartbeat_tick`
1. Step: `mqtt_publish` topic `broker/heartbeat`, payload `tick`.
2. Step: `wait_flags` (Mode `all`, Flag `system_idle=true`) — optional guard.
3. Step: `audio_play /sdcard/audio/beep.mp3`.

### Test
After saving, monitor logs. Every 10 seconds `[Interval] dev=heartbeat_task scenario=heartbeat_tick` appears and MQTT clients receive the heartbeat message.

---

## Saving & Verification Checklist

1. **Save changes** in the Devices tab after editing any template.
2. Switch to **Status → Automation** to verify flags, interval timers, and scenario queues in real time.
3. Use `/api/devices/run?device=<id>&scenario=<name>` for manual firing when testing.
4. If changes do not apply instantly, click **Reload** to force runtime refresh, then retry.

With these steps you can add a device, bind it to the appropriate template, and wire scenarios entirely from the UI without touching firmware code.
