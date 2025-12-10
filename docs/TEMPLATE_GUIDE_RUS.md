# Руководство по шаблонам и сценариям

Этот документ даёт практические примеры для каждого встроенного шаблона. В каждом разделе описано:

- Как настроить устройство во вкладке Devices.
- Какие сценарии создаёт или использует шаблон.
- Как убедиться, что автоматизация работает корректно.

---

## 1. UID Validator (считыватели карт)

**Цель:** два считывателя должны принять заданные UID. При успехе играем `success.mp3` и отправляем подтверждение по MQTT, при ошибке — `fail.mp3`.

1. **Создание устройства**
   - Device ID: `pictures_pair`
   - Template: `UID Validator`
2. **Слоты**
   - Слот 1: `source_id = pictures/uid/scan1`, `values = ABC123, DEF456`
   - Слот 2: `source_id = pictures/uid/scan2`, `values = 112233`
3. **Действия**
   - Успех: `success_topic = pictures/cmd/success`, `success_payload = ok`, `success_audio_track = /sdcard/audio/success.mp3`
   - Ошибка: `fail_topic = pictures/cmd/fail`, `fail_payload = fail`, `fail_audio_track = /sdcard/audio/fail.mp3`
4. **Сценарии** (создаются автоматически)
   - `uid_success`: публикация MQTT + запуск трека успеха.
   - `uid_fail`: аналогично, но для невалидных карт.
5. **Проверка**
   - Отправьте `ABC123` в `pictures/uid/scan1` и `112233` в `pictures/uid/scan2`. В логах появится `[UID] ... event=success`.
   - Любой другой UID запускает `uid_fail`.

---

## 2. Signal Hold (лазерная головоломка)

**Цель:** если луч держится 20 секунд на фоторезисторе, выключить реле, проиграть `complete.mp3` и опубликовать `done`. Если луч пропал — остановить цикличный трек.

1. **Создание устройства**
   - Device ID: `laser_guard`
   - Template: `Signal Hold`
2. **Поля**
   - `signal_topic = laser/relay/cmd`
   - `signal_payload_on = ON`, `signal_payload_off = OFF`
   - `heartbeat_topic = laser/heartbeat`
   - `required_hold_ms = 20000`
   - `heartbeat_timeout_ms = 1500`
   - `hold_track = /sdcard/audio/hold_loop.mp3`, `hold_track_loop = true`
   - `complete_track = /sdcard/audio/complete.mp3`
3. **Сценарий** (авто)
   - `signal_complete`: отправить ON, проиграть финальный трек, задержаться на `signal_on_ms` при необходимости и отправить OFF.
4. **Логика heartbeats**
   - Каждое сообщение в `laser/heartbeat` добавляет прогресс. Если >1.5 сек тишина — трек ставится на паузу и накопление обнуляется.
5. **Проверка**
   - Публикуйте heartbeat раз в секунду 20 секунд — сценарий выполнится.
   - Пропуск >1.5 сек — пауза и сброс таймера.

---

## 3. MQTT Event Trigger

**Цель:** если RFID шлёт `quest/scanner` с payload `scan`, запускать запрос валидации; payload `reset` — очищать состояние.

1. **Создание устройства**
   - Device ID: `scanner_controller`
   - Template: `MQTT Event Trigger`
2. **Правила**
   - Правило 1: `topic = quest/scanner`, `payload = scan`, `payload_required = true`, `scenario = request_scan`
   - Правило 2: `topic = quest/scanner`, `payload = reset`, `scenario = reset_state`
3. **Сценарии**
   - `request_scan`: Шаг 1 — MQTT `pictures/cmd/scan1` с `scan`. Шаг 2 — `set_flag scan_in_progress=true`.
   - `reset_state`: Шаг 1 — остановить аудио (опционально). Шаг 2 — `set_flag scan_in_progress=false`.
4. **Проверка**
   - Сообщение `scan` → лог `[MQTT trigger] ... scenario=request_scan`.
   - Любой другой payload игнорируется, если `payload_required=true`.

---

## 4. Flag Trigger

**Цель:** когда флаг `beam_ok` переходит в true, выполнить `laser_ok`. Когда `pictures_ready` = false — `pictures_abort`.

1. **Создание устройства**
   - Device ID: `flag_router`
   - Template: `Flag Trigger`
2. **Правила**
   - `flag=beam_ok`, `state=true`, `scenario=laser_ok`
   - `flag=pictures_ready`, `state=false`, `scenario=pictures_abort`
3. **Примеры сценариев**
   - `laser_ok`: запустить `/sdcard/audio/laser_ok.mp3`, опубликовать `pictures/cmd/allow`.
   - `pictures_abort`: остановить аудио, отправить `pictures/cmd/abort`.
4. **Как менять флаги**
   - Добавьте шаги `set_flag` в других сценариях, которые управляют этими флагами.
5. **Проверка**
   - При установке `beam_ok` в true появится `[Flag trigger] ... scenario=laser_ok`.

---

## 5. Conditional Scenario (If Condition)

**Цель:** если `beam_ok` И `uid_complete` равны true — выполнить `all_clear`, иначе `wait_retry`.

1. **Создание устройства**
   - Device ID: `condition_router`
   - Template: `Conditional Scenario`
2. **Поля**
   - `Logic mode = All conditions`
   - `Scenario if TRUE = all_clear`
   - `Scenario if FALSE = wait_retry`
   - Условия:
     - `flag=beam_ok`, `state=true`
     - `flag=uid_complete`, `state=true`
3. **Сценарии**
   - `all_clear`: MQTT `quest/door/cmd` с `unlock`.
   - `wait_retry`: проиграть `please_wait.mp3`.
4. **Поведение**
   - При изменении любого флага все условия пересчитываются; при смене результата запускается соответствующий сценарий.
5. **Проверка**
   - Оба флага true → `all_clear`.
   - Любой false → `wait_retry`.

---

## 6. Interval Task

**Цель:** каждые 10 секунд посылать heartbeat и, если система свободна, проигрывать короткий звук.

1. **Создание устройства**
   - Device ID: `heartbeat_task`
   - Template: `Interval Task`
2. **Поля**
   - `interval_ms = 10000`
   - `scenario = heartbeat_tick`
3. **Сценарий `heartbeat_tick`**
   - Шаг 1: MQTT `broker/heartbeat` с payload `tick`.
   - Шаг 2: `wait_flags` (режим `all`, условие `system_idle=true`, `timeout=0`) — опционно.
   - Шаг 3: воспроизвести `/sdcard/audio/beep.mp3` (неблокирующий).
4. **Проверка**
   - В логах каждые 10 секунд: `[Interval] dev=heartbeat_task scenario=heartbeat_tick`.
   - MQTT брокер получает сообщения `broker/heartbeat`.

---

## Советы по настройке в веб-интерфейсе

- Используйте **Wizard** для быстрого создания устройства — каждая карточка мастера соответствует одному шаблону.
- JSON-просмотр внизу Simple Editor показывает все поля (`template.uid`, `template.signal` и т. д.), удобно для ручных правок.
- После изменения шаблонов нажмите **Reload**, чтобы рантайм перечитал конфиг.

---

## Чек-лист тестирования автоматизации

| Шаг | Действие |
| --- | -------- |
| 1 | Включите подробные логи: `esp_log_level_set("template_runtime", ESP_LOG_DEBUG)`. |
| 2 | Используйте MQTT-клиент (например, `mosquitto_pub`) для имитации устройств. |
| 3 | Проверяйте флаги автоматизации в логах (`automation: flag <имя> set to <значение>`). |
| 4 | Запускайте сценарии вручную через `/api/devices/run?device=<id>&scenario=<id>`. |

Комбинируйте шаблоны: успешный UID может ставить флаг для `if_condition`, а `interval_task` будет напоминать участникам каждые несколько секунд.
