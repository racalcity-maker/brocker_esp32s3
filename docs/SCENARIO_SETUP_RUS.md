# Инструкция по настройке сценариев и устройств

Этот материал показывает, как добавить устройства и сценарии **для каждого встроенного шаблона** прямо из веб-интерфейса Broker. В каждой главе описано:

1. Что нажать в UI при создании устройства.
2. Какие поля шаблона заполнить.
3. Какие сценарии привязать или отредактировать.
4. Как быстро проверить результат.

> Перейдите в **Devices → Simple editor**. Для новых устройств используйте **Add device**, после правок не забывайте **Save changes**.

---

## 1. UID Validator (двойные считыватели карт)

**Задача:** Два UID-сканера должны одновременно прислать разрешённые значения, чтобы сработала «успешная» автоматизация.

### Поля устройства
1. Нажмите **Add device**, задайте **Device ID** `pictures_pair`, выберите шаблон **UID Validator**.
2. В блоке **UID slots** добавьте по записи на каждый сканер:
   - Слот 1: `Source ID = pictures/uid/scan1`, `Values = ABC123,DEF456`.
   - Слот 2: `Source ID = pictures/uid/scan2`, `Values = 112233`.
3. В блоке **Success actions**:
   - `MQTT topic = pictures/cmd/green`, `Payload = pass`.
   - `Audio track = /sdcard/audio/pictures_success.mp3`.
4. В блоке **Fail actions**:
   - `MQTT topic = pictures/cmd/red`, `Payload = fail`.
   - `Audio track = /sdcard/audio/pictures_fail.mp3`.

### Сценарии
Шаблон создаёт `uid_success` и `uid_fail`. В разделе **Scenarios** убедитесь, что:
- Шаг 1: `mqtt_publish` с выбранным топиком и нагрузкой.
- Шаг 2: `audio_play` с соответствующим треком.

### Проверка
Опубликуйте UID-пакеты в оба `pictures/uid/scan*`. В логах увидите `[UID] ... event=success`, если оба слота совпали, иначе запустится `uid_fail`.

---

## 2. Signal Hold (лазерная головоломка)

**Задача:** Держать лазер на фоторезисторе 20 секунд. Пока луч есть, играет цикл; по завершении таймера выполняется финальный сценарий.

### Поля устройства
1. Добавьте `laser_guard`, выберите **Signal Hold**.
2. Заполните:
   - `Heartbeat topic = laser/heartbeat`
   - `Reset topic = laser/reset` (???????????: ?????????? ???? ?????????? ????????).
   - Verbose signal logs vkl/otkl cherez Settings -> Diagnostics kogda nuzhno videt' soobshcheniya.
   - `Heartbeat timeout (ms) = 1500`
   - `Required hold (ms) = 20000`
   - `Hold track = /sdcard/audio/laser_loop.mp3`, включите **Loop**.
   - `Complete track = /sdcard/audio/laser_complete.mp3`
   - `Relay topic = laser/relay/cmd`, `Payload ON = ON`, `Payload OFF = OFF`.

### Сценарии
Авто-сценарий `signal_complete` отредактируйте так:
1. `mqtt_publish` (`laser/relay/cmd`, нагрузка `ON`).
2. `audio_play` финального трека.
3. `delay` (например 1000 мс, если реле нужно удерживать).
4. `mqtt_publish` (`laser/relay/cmd`, нагрузка `OFF`).

### Проверка
Шлите MQTT heartbeats (`laser/heartbeat` + любая нагрузка) каждую секунду. Смотрите лог `[Signal] hold=XXms`. При достижении 20000 мс выполнится `signal_complete`. Остановите heartbeat — петля должна встать на паузу.

---

## 3. MQTT Event Trigger

**Задача:** Разные payload’ы в одном топике запускают разные сценарии.

### Поля устройства
1. Добавьте `scanner_controller`, шаблон **MQTT Event Trigger**.
2. В разделе **Rules**:
   - Правило 1: `Topic = quest/scanner`, `Payload = scan`, `Require payload = true`, `Scenario = request_scan`.
   - Правило 2: `Topic = quest/scanner`, `Payload = reset`, `Scenario = reset_state`.

### Сценарии
- `request_scan`
  1. `mqtt_publish` → `pictures/cmd/scan1`, нагрузка `scan`.
  2. `set_flag` `scan_in_progress = true`.
- `reset_state`
  1. `audio_stop_all`.
  2. `set_flag scan_in_progress = false`.

### Проверка
Опубликуйте `scan` в `quest/scanner` и увидите `[MQTT trigger] ... scenario=request_scan`. Payload `reset` вызовет второй сценарий.

---

## 4. Flag Trigger

**Задача:** Реагировать на смену автоматизационных флагов.

### Поля устройства
1. Добавьте `flag_router`, выберите **Flag Trigger**.
2. Правила:
   - `flag = beam_ok`, `state = true`, `scenario = laser_ok`.
   - `flag = pictures_ready`, `state = false`, `scenario = pictures_abort`.

### Сценарии
- `laser_ok`: воспроизвести `/sdcard/audio/laser_ok.mp3`, отправить MQTT `pictures/cmd/allow`.
- `pictures_abort`: остановить звук, отправить MQTT `pictures/cmd/abort`, сбросить флаг `beam_ok`.

### Проверка
Любым сценарием установите `beam_ok = true` → лог `[Flag trigger] ... laser_ok`. Сбросьте `pictures_ready` → выполнится `pictures_abort`.

---

## 5. Conditional Scenario (if_condition)

**Задача:** Проверять несколько условий одновременно и запускать сценарий «да» или «нет».

### Поля устройства
1. Создайте `condition_router`, шаблон **Conditional Scenario**.
2. Настройки:
   - `Logic mode = All conditions`
   - `Scenario if TRUE = all_clear`
   - `Scenario if FALSE = wait_retry`
3. Условия:
   - №1: `Type = Flag`, `Flag name = beam_ok`, `State = true`.
   - №2: `Type = Flag`, `Flag name = uid_complete`, `State = true`.

### Сценарии
- `all_clear`: отправить MQTT `quest/door/cmd` с нагрузкой `unlock`.
- `wait_retry`: проиграть `please_wait.mp3`, при необходимости отправить подсказку.

### Проверка
Установите оба флага в `true` → сработает `all_clear`. Сбросьте хотя бы один → выполнится `wait_retry`. Логи `[Condition] result=true/false` подскажут текущее состояние.

---

## 6. Interval Task

**Задача:** Периодически запускать сценарий по таймеру.

### Поля устройства
1. Добавьте `heartbeat_task`, выберите **Interval Task**.
2. Поля: `Interval (ms) = 10000`, `Scenario = heartbeat_tick`, при необходимости `Run immediately = true`.

### Сценарий `heartbeat_tick`
1. `mqtt_publish` → `broker/heartbeat`, нагрузка `tick`.
2. `wait_flags` (режим `all`, флаг `system_idle=true`) — опциональная защита.
3. `audio_play /sdcard/audio/beep.mp3`.

### Проверка
После сохранения каждые 10 секунд в логах появится `[Interval] dev=heartbeat_task scenario=heartbeat_tick`, а MQTT-клиент получит сообщение.

---

## Чек-лист сохранения и отладки

1. После любых правок нажимайте **Save changes** на вкладке Devices.
2. В разделе **Status → Automation** можно отслеживать флаги, таймеры и очередь сценариев.
3. Для ручной проверки используйте `/api/devices/run?device=<id>&scenario=<name>`.
4. Если изменения не вступили в силу, нажмите **Reload**, дождитесь логов об обновлении и повторите тест.

Теперь всё добавляется и настраивается из интерфейса, без перепрошивки и правок прошивки.
