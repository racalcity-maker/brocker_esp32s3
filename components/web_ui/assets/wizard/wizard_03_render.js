function renderDeviceList() {
  if (!state.list) return;
  const devices = getDevices();
  if (!devices.length) {
    state.list.innerHTML = "<div class='dw-list-empty'>No devices</div>";
    state.selectedDevice = -1;
    state.selectedScenario = -1;
    if (state.detail) {
      state.detail.innerHTML = "<div class='dw-empty'>No devices configured yet.</div>";
    }
    return;
  }
  state.list.innerHTML = devices.map((dev, idx) => {
    const active = idx === state.selectedDevice ? ' active' : '';
    const name = escapeHtml(dev.display_name || dev.name || dev.id || ('Device ' + (idx + 1)));
    const scenarios = (dev.scenarios || []).length;
    return `<div class="dw-device-item${active}" data-device-index="${idx}">
      <span>${name}</span>
      <span class="dw-badge">${scenarios}</span>
    </div>`;
  }).join('');
}

function renderDeviceDetail() {
  if (!state.detail) return;
  const dev = currentDevice();
  if (!dev) {
    state.detail.innerHTML = "<div class='dw-empty'>Select or add a device to edit.</div>";
    return;
  }
  state.detail.innerHTML = `
    <div class="dw-section">
      <h4>Basics</h4>
      <div class="dw-field required">
        <label>Device ID</label>
        <input data-device-field="id" value="${escapeAttr(dev.id || '')}" placeholder="unique-id" data-required="true">
        <div class="dw-hint small">Уникальный ID, используется для запуска сценариев.</div>
      </div>
      <div class="dw-field">
        <label>Display name</label>
        <input data-device-field="display_name" value="${escapeAttr(dev.display_name || dev.name || '')}" placeholder="Visible name">
      </div>
    </div>
    ${renderTemplateSection(dev)}
    ${renderTopicsSection(dev)}
    ${renderScenariosSection(dev)}
  `;
  validateRequiredFields();
}

function validateRequiredFields() {
  if (!state.detail) return;
  const requiredControls = state.detail.querySelectorAll('[data-required="true"]');
  requiredControls.forEach((control) => {
    const field = control.closest('.dw-field');
    if (!field) {
      return;
    }
    const rule = control.dataset.requiredRule || '';
    let valid = true;
    if (rule === 'positive') {
      const num = parseInt(control.value, 10);
      valid = !Number.isNaN(num) && num > 0;
    } else {
      valid = (control.value || '').trim().length > 0;
    }
    field.classList.toggle('invalid', !valid);
  });
}

function renderTemplateSection(dev) {
  const tplType = dev.template?.type || '';
  const options = TEMPLATE_TYPES.map((tpl) => `<option value="${tpl.value}" ${tpl.value === tplType ? 'selected' : ''}>${tpl.label}</option>`).join('');
  let body = "<div class='dw-empty small'>Template is not set.</div>";
  if (tplType === 'uid_validator') {
    body = renderUidTemplate(dev);
  } else if (tplType === 'signal_hold') {
    body = renderSignalTemplate(dev);
  } else if (tplType === 'on_mqtt_event') {
    body = renderMqttTemplate(dev);
  } else if (tplType === 'on_flag') {
    body = renderFlagTemplate(dev);
  } else if (tplType === 'if_condition') {
    body = renderConditionTemplate(dev);
  } else if (tplType === 'interval_task') {
    body = renderIntervalTemplate(dev);
  } else if (tplType === 'sequence_lock') {
    body = renderSequenceTemplate(dev);
  }
  return `
    <div class="dw-section">
      <div class="dw-section-head">
        <h4>Template</h4>
      </div>
      <div class="dw-field">
        <label>Template type</label>
        <select data-template-field="type">${options}</select>
      </div>
      ${body}
    </div>`;
}

function renderUidTemplate(dev) {
  ensureUidTemplate(dev);
  const tpl = dev.template?.uid || {slots: []};
  const slots = (tpl.slots || []).map((slot, idx) => {
    const last = slot.last_value ? escapeAttr(slot.last_value) : '';
    return `
    <div class="dw-slot">
      <div class="dw-slot-head">Slot ${idx + 1}<button class="danger small" data-action="slot-remove" data-index="${idx}">&times;</button></div>
      <div class="dw-field"><label>Source topic</label><input data-template-field="uid-slot" data-subfield="source_id" data-index="${idx}" value="${escapeAttr(slot.source_id || '')}" placeholder="sensor/topic"></div>
      <div class="dw-field"><label>Label</label><input data-template-field="uid-slot" data-subfield="label" data-index="${idx}" value="${escapeAttr(slot.label || '')}" placeholder="Friendly label"></div>
      <div class="dw-field"><label>Allowed values</label><input data-template-field="uid-values" data-index="${idx}" value="${escapeAttr((slot.values || []).join(', '))}" placeholder="uid1, uid2"></div>
      <div class="dw-field dw-slot-last"><label>Last read</label><div class="dw-last-value">${last || '&mdash;'}</div></div>
    </div>`;
  }).join('');
  return `
    <div class="dw-section">
      <h5>Activation</h5>
      <div class="dw-field required">
        <label>Wait for topic</label>
        <input data-template-field="uid-activation" data-subfield="start_topic" value="${escapeAttr(tpl.start_topic || '')}" placeholder="pictures/cmd/scan1" data-required="true">
        <div class="dw-hint small">Когда приходит этот топик (опц. вместе с payload), начинаем проверку UID.</div>
      </div>
      <div class="dw-field">
        <label>Match payload</label>
        <input data-template-field="uid-activation" data-subfield="start_payload" value="${escapeAttr(tpl.start_payload || '')}" placeholder="leave empty for any payload">
      </div>
      <div class="dw-field">
        <label>Broadcast topic</label>
        <input data-template-field="uid-activation" data-subfield="broadcast_topic" value="${escapeAttr(tpl.broadcast_topic || '')}" placeholder="pictures/cmd/scan">
        <div class="dw-hint small">Этот топик отправим сразу после получения стартовой команды.</div>
      </div>
      <div class="dw-field">
        <label>Broadcast payload</label>
        <input data-template-field="uid-activation" data-subfield="broadcast_payload" value="${escapeAttr(tpl.broadcast_payload || '')}">
      </div>
    </div>
    <div class="dw-section">
      <div class="dw-section-head">
        <span>UID slots</span>
        <button data-action="slot-add">Add slot</button>
      </div>
      ${slots || "<div class='dw-empty small'>No slots configured.</div>"}
    </div>
    <div class="dw-section">
      <h5>Success actions</h5>
      <div class="dw-field"><label>MQTT topic</label><input data-template-field="uid-action" data-subfield="success_topic" value="${escapeAttr(tpl.success_topic || '')}" placeholder="quest/ok"></div>
      <div class="dw-field"><label>Payload</label><input data-template-field="uid-action" data-subfield="success_payload" value="${escapeAttr(tpl.success_payload || '')}" placeholder="payload"></div>
      <div class="dw-field"><label>Audio track</label><input data-template-field="uid-action" data-subfield="success_audio_track" value="${escapeAttr(tpl.success_audio_track || '')}" placeholder="/sdcard/ok.mp3"></div>
    </div>
    <div class="dw-section">
      <h5>Fail actions</h5>
      <div class="dw-field"><label>MQTT topic</label><input data-template-field="uid-action" data-subfield="fail_topic" value="${escapeAttr(tpl.fail_topic || '')}" placeholder="quest/fail"></div>
      <div class="dw-field"><label>Payload</label><input data-template-field="uid-action" data-subfield="fail_payload" value="${escapeAttr(tpl.fail_payload || '')}" placeholder="payload"></div>
      <div class="dw-field"><label>Audio track</label><input data-template-field="uid-action" data-subfield="fail_audio_track" value="${escapeAttr(tpl.fail_audio_track || '')}" placeholder="/sdcard/fail.mp3"></div>
    </div>`;
}

function renderSignalTemplate(dev) {
  ensureSignalTemplate(dev);
  const sig = dev.template?.signal || {};
  return `
    <div class="dw-section">
      <h5>Signal control</h5>
      <div class="dw-field required"><label>Signal topic</label><input data-template-field="signal" data-subfield="signal_topic" value="${escapeAttr(sig.signal_topic || '')}" placeholder="quest/relay/cmd" data-required="true"><div class="dw-hint small">Топик для отправки ON/OFF при завершении удержания.</div></div>
      <div class="dw-field"><label>Payload ON</label><input data-template-field="signal" data-subfield="signal_payload_on" value="${escapeAttr(sig.signal_payload_on || '')}" placeholder="ON"></div>
      <div class="dw-field"><label>Payload OFF</label><input data-template-field="signal" data-subfield="signal_payload_off" value="${escapeAttr(sig.signal_payload_off || '')}" placeholder="OFF"></div>
      <div class="dw-field required"><label>Heartbeat topic</label><input data-template-field="signal" data-subfield="heartbeat_topic" value="${escapeAttr(sig.heartbeat_topic || '')}" placeholder="quest/relay/hb" data-required="true"><div class="dw-hint small">Топик, куда устройство шлёт heartbeat пока луч активен.</div></div>
      <div class="dw-field"><label>Reset topic</label><input data-template-field="signal" data-subfield="reset_topic" value="${escapeAttr(sig.reset_topic || '')}" placeholder="laser/reset"><div class="dw-hint small">Опубликуйте любое сообщение сюда, чтобы остановить трек и обнулить прогресс удержания.</div></div>
      <div class="dw-field"><label class="dw-checkbox"><input type="checkbox" data-template-field="signal" data-subfield="debug_logging" ${sig.debug_logging ? 'checked' : ''}>Включить подробные логи</label><div class="dw-hint small">Печатать события heartbeat, паузы трека и публикацию сигналов.</div></div>
      <div class="dw-field required"><label>Required hold ms</label><input type="number" data-template-field="signal" data-subfield="required_hold_ms" value="${sig.required_hold_ms || 0}" data-required="true" data-required-rule="positive"><div class="dw-hint small">Минимальная длительность удержания в миллисекундах.</div></div>
      <div class="dw-field"><label>Heartbeat timeout ms</label><input type="number" data-template-field="signal" data-subfield="heartbeat_timeout_ms" value="${sig.heartbeat_timeout_ms || 0}"></div>
      <div class="dw-field"><label>Hold track</label><input data-template-field="signal" data-subfield="hold_track" value="${escapeAttr(sig.hold_track || '')}" placeholder="/sdcard/hold.mp3"></div>
      <div class="dw-field"><label>Loop hold track</label><select data-template-field="signal" data-subfield="hold_track_loop"><option value="false" ${sig.hold_track_loop ? '' : 'selected'}>No</option><option value="true" ${sig.hold_track_loop ? 'selected' : ''}>Yes</option></select></div>
      <div class="dw-field"><label>Complete track</label><input data-template-field="signal" data-subfield="complete_track" value="${escapeAttr(sig.complete_track || '')}" placeholder="/sdcard/done.mp3"></div>
    </div>`;
}

function renderMqttTemplate(dev) {
  ensureMqttTemplate(dev);
  const tpl = dev.template?.mqtt || {rules: []};
  const rules = (tpl.rules || []).map((rule, idx) => {
    const checked = rule.payload_required ? 'checked' : '';
    return `
    <div class="dw-slot">
      <div class="dw-slot-head">Rule ${idx + 1}<button class="danger small" data-action="mqtt-rule-remove" data-index="${idx}">&times;</button></div>
      <div class="dw-field"><label>Name</label><input data-template-field="mqtt-rule" data-subfield="name" data-index="${idx}" value="${escapeAttr(rule.name || '')}" placeholder="Optional"></div>
      <div class="dw-field"><label>Topic</label><input data-template-field="mqtt-rule" data-subfield="topic" data-index="${idx}" value="${escapeAttr(rule.topic || '')}" placeholder="sensor/topic"></div>
      <div class="dw-field"><label>Payload</label><input data-template-field="mqtt-rule" data-subfield="payload" data-index="${idx}" value="${escapeAttr(rule.payload || '')}" placeholder="payload"></div>
      <div class="dw-field dw-checkbox-field"><label><input type="checkbox" data-template-field="mqtt-rule" data-subfield="payload_required" data-index="${idx}" ${checked}>Match payload</label></div>
      <div class="dw-field"><label>Scenario ID</label><input data-template-field="mqtt-rule" data-subfield="scenario" data-index="${idx}" value="${escapeAttr(rule.scenario || '')}" placeholder="scenario_id"></div>
    </div>`;
  }).join('');
  return `
    <div class="dw-section">
      <div class="dw-section-head">
        <span>MQTT rules</span>
        <button data-action="mqtt-rule-add">Add rule</button>
      </div>
      ${rules || "<div class='dw-empty small'>No rules configured.</div>"}
      <div class="dw-hint small">Leave payload empty to match any payload.</div>
    </div>`;
}

function renderFlagTemplate(dev) {
  ensureFlagTemplate(dev);
  const tpl = dev.template?.flag || {rules: []};
  const rules = (tpl.rules || []).map((rule, idx) => {
    return `
    <div class="dw-slot">
      <div class="dw-slot-head">Rule ${idx + 1}<button class="danger small" data-action="flag-rule-remove" data-index="${idx}">&times;</button></div>
      <div class="dw-field"><label>Name</label><input data-template-field="flag-rule" data-subfield="name" data-index="${idx}" value="${escapeAttr(rule.name || '')}" placeholder="Optional"></div>
      <div class="dw-field"><label>Flag</label><input data-template-field="flag-rule" data-subfield="flag" data-index="${idx}" value="${escapeAttr(rule.flag || '')}" placeholder="door_open"></div>
      <div class="dw-field">
        <label>Trigger when</label>
        <select data-template-field="flag-rule" data-subfield="state" data-index="${idx}">
          <option value="true" ${rule.required_state ? 'selected' : ''}>Flag becomes TRUE</option>
          <option value="false" ${!rule.required_state ? 'selected' : ''}>Flag becomes FALSE</option>
        </select>
      </div>
      <div class="dw-field"><label>Scenario ID</label><input data-template-field="flag-rule" data-subfield="scenario" data-index="${idx}" value="${escapeAttr(rule.scenario || '')}" placeholder="scenario_id"></div>
    </div>`;
  }).join('');
  return `
    <div class="dw-section">
      <div class="dw-section-head">
        <span>Flag rules</span>
        <button data-action="flag-rule-add">Add rule</button>
      </div>
      ${rules || "<div class='dw-empty small'>No rules configured.</div>"}
      <div class="dw-hint small">Scenarios must exist on this device and will run when the selected flag changes.</div>
    </div>`;
}

function renderConditionTemplate(dev) {
  ensureConditionTemplate(dev);
  const tpl = dev.template?.condition || {};
  const rules = (tpl.rules || []).map((rule, idx) => `
    <div class="dw-slot">
      <div class="dw-slot-head">Condition ${idx + 1}<button class="danger small" data-action="condition-rule-remove" data-index="${idx}">&times;</button></div>
      <div class="dw-field"><label>Flag</label><input data-template-field="condition-rule" data-subfield="flag" data-index="${idx}" value="${escapeAttr(rule.flag || '')}" placeholder="flag_name"></div>
      <div class="dw-field">
        <label>State</label>
        <select data-template-field="condition-rule" data-subfield="state" data-index="${idx}">
          <option value="true" ${rule.required_state ? 'selected' : ''}>TRUE</option>
          <option value="false" ${!rule.required_state ? 'selected' : ''}>FALSE</option>
        </select>
      </div>
    </div>`).join('');
  return `
    <div class="dw-section">
      <div class="dw-section-head"><span>Condition settings</span></div>
      <div class="dw-field">
        <label>Logic mode</label>
        <select data-template-field="condition-mode">
          <option value="all" ${tpl.mode === 'all' ? 'selected' : ''}>All conditions</option>
          <option value="any" ${tpl.mode === 'any' ? 'selected' : ''}>Any condition</option>
        </select>
      </div>
      <div class="dw-field"><label>Scenario if TRUE</label><input data-template-field="condition-scenario" data-subfield="true" value="${escapeAttr(tpl.true_scenario || '')}" placeholder="scenario_true"></div>
      <div class="dw-field"><label>Scenario if FALSE</label><input data-template-field="condition-scenario" data-subfield="false" value="${escapeAttr(tpl.false_scenario || '')}" placeholder="scenario_false"></div>
    </div>
    <div class="dw-section">
      <div class="dw-section-head">
        <span>Conditions</span>
        <button data-action="condition-rule-add">Add condition</button>
      </div>
      ${rules || "<div class='dw-empty small'>No conditions configured.</div>"}
      <div class="dw-hint small">Conditions evaluate automation flags; scenarios run when result changes.</div>
    </div>`;
}

function renderIntervalTemplate(dev) {
  ensureIntervalTemplate(dev);
  const tpl = dev.template?.interval || {};
  const intervalMs = (tpl.interval_ms && tpl.interval_ms > 0) ? tpl.interval_ms : 1000;
  return `
    <div class="dw-section">
      <div class="dw-section-head"><span>Interval task</span></div>
      <div class="dw-field"><label>Interval (ms)</label><input type="number" min="1" data-template-field="interval" data-subfield="interval_ms" value="${intervalMs}"></div>
      <div class="dw-field"><label>Scenario ID</label><input data-template-field="interval" data-subfield="scenario" value="${escapeAttr(tpl.scenario || '')}" placeholder="scenario_id"></div>
      <div class="dw-hint small">Runs the selected scenario on a fixed interval.</div>
    </div>`;
}

function renderSequenceTemplate(dev) {
  ensureSequenceTemplate(dev);
  const tpl = dev.template?.sequence || {};
  const steps = (tpl.steps || []).map((step, idx) => {
    const checked = step.payload_required ? 'checked' : '';
    return `
      <div class="dw-slot">
        <div class="dw-slot-head">Step ${idx + 1}<button class="danger small" data-action="sequence-step-remove" data-index="${idx}">&times;</button></div>
        <div class="dw-field"><label>MQTT topic</label><input data-template-field="sequence-step" data-subfield="topic" data-index="${idx}" value="${escapeAttr(step.topic || '')}" placeholder="sensor/topic"></div>
        <div class="dw-field"><label>Payload</label><input data-template-field="sequence-step" data-subfield="payload" data-index="${idx}" value="${escapeAttr(step.payload || '')}" placeholder="payload"></div>
        <div class="dw-field dw-checkbox-field"><label><input type="checkbox" data-template-field="sequence-step" data-subfield="payload_required" data-index="${idx}" ${checked}>Require exact payload</label></div>
        <div class="dw-field"><label>Hint topic</label><input data-template-field="sequence-step" data-subfield="hint_topic" data-index="${idx}" value="${escapeAttr(step.hint_topic || '')}" placeholder="hint/topic"></div>
        <div class="dw-field"><label>Hint payload</label><input data-template-field="sequence-step" data-subfield="hint_payload" data-index="${idx}" value="${escapeAttr(step.hint_payload || '')}" placeholder="payload"></div>
        <div class="dw-field"><label>Hint audio track</label><input data-template-field="sequence-step" data-subfield="hint_audio_track" data-index="${idx}" value="${escapeAttr(step.hint_audio_track || '')}" placeholder="/sdcard/hint.mp3"></div>
      </div>`;
  }).join('');
  return `
    <div class="dw-section">
      <div class="dw-section-head">
        <span>Sequence steps</span>
        <button data-action="sequence-step-add">Add step</button>
      </div>
      ${steps || "<div class='dw-empty small'>No steps defined.</div>"}
      <div class="dw-hint small">Steps advance when matching MQTT messages arrive in order.</div>
    </div>
    <div class="dw-section">
      <div class="dw-field"><label>Timeout (ms)</label><input type="number" min="0" step="100" data-template-field="sequence" data-subfield="timeout_ms" value="${tpl.timeout_ms || 0}" placeholder="0 = no timeout"></div>
      <div class="dw-field"><label class="dw-checkbox"><input type="checkbox" data-template-field="sequence" data-subfield="reset_on_error" ${tpl.reset_on_error !== false ? 'checked' : ''}>Reset on wrong step</label></div>
    </div>
    <div class="dw-section">
      <h5>Success actions</h5>
      <div class="dw-field"><label>MQTT topic</label><input data-template-field="sequence" data-subfield="success_topic" value="${escapeAttr(tpl.success_topic || '')}" placeholder="quest/sequence/success"></div>
      <div class="dw-field"><label>Payload</label><input data-template-field="sequence" data-subfield="success_payload" value="${escapeAttr(tpl.success_payload || '')}" placeholder="payload"></div>
      <div class="dw-field"><label>Audio track</label><input data-template-field="sequence" data-subfield="success_audio_track" value="${escapeAttr(tpl.success_audio_track || '')}" placeholder="/sdcard/success.mp3"></div>
      <div class="dw-field"><label>Scenario ID</label><input data-template-field="sequence" data-subfield="success_scenario" value="${escapeAttr(tpl.success_scenario || '')}" placeholder="scenario_success"></div>
    </div>
    <div class="dw-section">
      <h5>Fail actions</h5>
      <div class="dw-field"><label>MQTT topic</label><input data-template-field="sequence" data-subfield="fail_topic" value="${escapeAttr(tpl.fail_topic || '')}" placeholder="quest/sequence/fail"></div>
      <div class="dw-field"><label>Payload</label><input data-template-field="sequence" data-subfield="fail_payload" value="${escapeAttr(tpl.fail_payload || '')}" placeholder="payload"></div>
      <div class="dw-field"><label>Audio track</label><input data-template-field="sequence" data-subfield="fail_audio_track" value="${escapeAttr(tpl.fail_audio_track || '')}" placeholder="/sdcard/fail.mp3"></div>
      <div class="dw-field"><label>Scenario ID</label><input data-template-field="sequence" data-subfield="fail_scenario" value="${escapeAttr(tpl.fail_scenario || '')}" placeholder="scenario_fail"></div>
      <div class="dw-hint small">Failure actions run when timeout expires or an unexpected topic arrives.</div>
    </div>`;
}

function renderTopicsSection(dev) {
  const rows = (dev.topics || []).map((topic, idx) => `
    <tr>
      <td><input data-topic-field="name" data-index="${idx}" value="${escapeAttr(topic.name || '')}" placeholder="Name"></td>
      <td><input data-topic-field="topic" data-index="${idx}" value="${escapeAttr(topic.topic || '')}" placeholder="mqtt/topic"></td>
      <td><button class="danger small" data-action="remove-topic" data-index="${idx}">&times;</button></td>
    </tr>`).join('');
  return `
    <div class="dw-section">
      <div class="dw-section-head">
        <h4>Topics</h4>
        <div class="dw-table-actions"><button data-action="add-topic">Add topic</button></div>
      </div>
      <table class="dw-mini-table">
        <thead><tr><th>Name</th><th>Topic</th><th></th></tr></thead>
        <tbody>${rows || "<tr><td colspan='3' class='muted small'>No topics</td></tr>"}</tbody>
      </table>
    </div>`;
}

function renderScenariosSection(dev) {
  const items = (dev.scenarios || []).map((sc, idx) => {
    const active = idx === state.selectedScenario ? ' active' : '';
    const title = escapeHtml(sc.name || sc.id || ('Scenario ' + (idx + 1)));
    return `<div class="dw-scenario-item${active}" data-action="select-scenario" data-index="${idx}">
      <span>${title}</span>
      <span class="dw-badge">${(sc.steps || []).length}</span>
    </div>`;
  }).join('');
  return `
    <div class="dw-section">
      <div class="dw-scenarios">
        <div class="dw-scenario-list">
          <div class="dw-scenario-actions">
            <button data-action="add-scenario">Add</button>
            <button class="danger" data-action="remove-scenario" data-index="${state.selectedScenario}">Delete</button>
          </div>
          ${items || "<div class='dw-list-empty'>No scenarios</div>"}
        </div>
        <div class="dw-scenario-detail">
          ${renderScenarioDetail()}
        </div>
      </div>
    </div>`;
}

function renderScenarioDetail() {
  const scen = currentScenario();
  if (!scen) {
    return "<div class='dw-empty'>Select a scenario to edit.</div>";
  }
  const steps = (scen.steps || []).map((step, idx) => renderStep(step, idx)).join('');
  return `
    <div class="dw-field">
      <label>Scenario ID</label>
      <input data-scenario-field="id" value="${escapeAttr(scen.id || '')}" placeholder="scenario_id">
    </div>
    <div class="dw-field">
      <label>Scenario name</label>
      <input data-scenario-field="name" value="${escapeAttr(scen.name || '')}" placeholder="Display name">
    </div>
    <div class="dw-field dw-checkbox-field">
      <label><input type="checkbox" data-scenario-field="button_enabled" ${scen.button_enabled ? 'checked' : ''}>Show in Actions tab</label>
    </div>
    <div class="dw-field">
      <label>Button label</label>
      <input data-scenario-field="button_label" value="${escapeAttr(scen.button_label || scen.name || scen.id || '')}" placeholder="Friendly label" ${scen.button_enabled ? '' : 'disabled'}>
    </div>
    <div class="dw-step-footer">
      <button class="secondary" data-action="add-step">Add step</button>
      <span class="dw-note">Steps execute sequentially; use loops and waits for advanced flows.</span>
    </div>
    <div>${steps || "<div class='dw-empty'>No steps</div>"}</div>
  `;
}

function renderStep(step, idx) {
  const options = ACTION_TYPES.map((type) => `<option value="${type}" ${type === step.type ? 'selected' : ''}>${type}</option>`).join('');
  return `
    <div class="dw-step-card">
      <div class="dw-step-head">
        <h5>Step ${idx + 1}</h5>
        <div class="dw-step-head-controls">
          <button data-action="step-up" data-index="${idx}">&uarr;</button>
          <button data-action="step-down" data-index="${idx}">&darr;</button>
          <button class="danger" data-action="remove-step" data-index="${idx}">Delete</button>
        </div>
      </div>
      <div class="dw-step-body">
        <div class="dw-field">
          <label>Type</label>
          <select data-step-field="type" data-index="${idx}">${options}</select>
        </div>
        <div class="dw-field">
          <label>Delay ms</label>
          <input type="number" data-step-field="delay_ms" data-index="${idx}" value="${step.delay_ms || 0}">
        </div>
        ${renderStepFields(step, idx)}
      </div>
    </div>`;
}

function renderStepFields(step, idx) {
  const type = step.type || 'nop';
  switch (type) {
    case 'mqtt_publish':
      ensure(step, ['data','mqtt']);
      return `
        <div class="dw-field"><label>Topic</label><input data-step-field="data.mqtt.topic" data-index="${idx}" value="${escapeAttr(step.data.mqtt.topic || '')}"></div>
        <div class="dw-field"><label>Payload</label><input data-step-field="data.mqtt.payload" data-index="${idx}" value="${escapeAttr(step.data.mqtt.payload || '')}"></div>
        <div class="dw-field"><label>QoS</label><input type="number" data-step-field="data.mqtt.qos" data-index="${idx}" value="${step.data.mqtt.qos || 0}"></div>
        <div class="dw-field"><label>Retain</label><select data-step-field="data.mqtt.retain" data-index="${idx}"><option value="false" ${step.data.mqtt.retain?'':'selected'}>False</option><option value="true" ${step.data.mqtt.retain?'selected':''}>True</option></select></div>`;
    case 'audio_play':
      ensure(step, ['data','audio']);
      return `
        <div class="dw-field"><label>Track path</label><input data-step-field="data.audio.track" data-index="${idx}" value="${escapeAttr(step.data.audio.track || '')}"></div>
        <div class="dw-field"><label>Blocking</label><select data-step-field="data.audio.blocking" data-index="${idx}"><option value="false" ${step.data.audio.blocking?'':'selected'}>No</option><option value="true" ${step.data.audio.blocking?'selected':''}>Yes</option></select></div>`;
    case 'set_flag':
      ensure(step, ['data','flag']);
      return `
        <div class="dw-field"><label>Flag name</label><input data-step-field="data.flag.flag" data-index="${idx}" value="${escapeAttr(step.data.flag.flag || '')}"></div>
        <div class="dw-field"><label>Value</label><select data-step-field="data.flag.value" data-index="${idx}"><option value="true" ${step.data.flag.value?'selected':''}>True</option><option value="false" ${step.data.flag.value?'':'selected'}>False</option></select></div>`;
    case 'wait_flags':
      ensure(step, ['data','wait_flags']);
      ensure(step.data.wait_flags, ['requirements']);
      const reqs = (step.data.wait_flags.requirements || []).map((req, rIdx) => `
        <div class="dw-wait-row">
          <input placeholder="flag" data-wait-field="flag" data-step-index="${idx}" data-req-index="${rIdx}" value="${escapeAttr(req.flag || '')}">
          <select data-wait-field="state" data-step-index="${idx}" data-req-index="${rIdx}">
            <option value="true" ${req.required_state?'selected':''}>True</option>
            <option value="false" ${req.required_state?'':'selected'}>False</option>
          </select>
          <button data-action="remove-wait-rule" data-step-index="${idx}" data-req-index="${rIdx}">&times;</button>
        </div>`).join('');
      return `
        <div class="dw-field"><label>Mode</label><select data-step-field="data.wait_flags.mode" data-index="${idx}"><option value="all" ${step.data.wait_flags.mode==='all'?'selected':''}>All</option><option value="any" ${step.data.wait_flags.mode==='any'?'selected':''}>Any</option></select></div>
        <div class="dw-field"><label>Timeout ms</label><input type="number" data-step-field="data.wait_flags.timeout_ms" data-index="${idx}" value="${step.data.wait_flags.timeout_ms || 0}"></div>
        <div class="dw-field"><label>Requirements</label>
          <div class="dw-wait-req">${reqs || "<div class='dw-empty small'>No requirements</div>"}</div>
          <button class="secondary" data-action="add-wait-rule" data-step-index="${idx}">Add requirement</button>
        </div>`;
    case 'loop':
      ensure(step, ['data','loop']);
      return `
        <div class="dw-field"><label>Target step</label><input type="number" data-step-field="data.loop.target_step" data-index="${idx}" value="${step.data.loop.target_step || 0}"></div>
        <div class="dw-field"><label>Max iterations</label><input type="number" data-step-field="data.loop.max_iterations" data-index="${idx}" value="${step.data.loop.max_iterations || 0}"></div>`;
    case 'event':
      ensure(step, ['data','event']);
      return `
        <div class="dw-field"><label>Event</label><input data-step-field="data.event.event" data-index="${idx}" value="${escapeAttr(step.data.event.event || '')}"></div>
        <div class="dw-field"><label>Topic</label><input data-step-field="data.event.topic" data-index="${idx}" value="${escapeAttr(step.data.event.topic || '')}"></div>
        <div class="dw-field"><label>Payload</label><input data-step-field="data.event.payload" data-index="${idx}" value="${escapeAttr(step.data.event.payload || '')}"></div>`;
    default:
      return '';
  }
}
