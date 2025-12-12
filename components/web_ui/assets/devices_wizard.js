// NOTE: This bundle is generated from assets/wizard/*.js via build_devices_wizard.py.
//       Edit the source modules rather than the assembled devices_wizard.js.
(() => {
const ACTION_TYPES = ['mqtt_publish','audio_play','audio_stop','set_flag','wait_flags','loop','delay','event','nop'];
const TEMPLATE_TYPES = [
  {value: '', label: 'No template'},
  {value: 'uid_validator', label: 'UID validator'},
  {value: 'signal_hold', label: 'Signal hold'},
  {value: 'on_mqtt_event', label: 'MQTT trigger'},
  {value: 'on_flag', label: 'Flag trigger'},
  {value: 'if_condition', label: 'Conditional scenario'},
  {value: 'interval_task', label: 'Interval task'},
  {value: 'sequence_lock', label: 'Sequence lock'},
];
const MQTT_RULE_LIMIT = 8;
const FLAG_RULE_LIMIT = 8;
const SEQUENCE_STEP_LIMIT = 8;
const WIZARD_TEMPLATES = {
  blank_device: {
    label: 'Blank Device',
    description: 'Start from a minimal empty device and edit details manually.',
    defaults: {
      deviceName: 'New device',
    },
    fields: [],
    build(base) {
      base.scenarios = [];
      base.topics = [];
      return base;
    },
  },
};

const state = {
  root: null,
  toolbar: null,
  list: null,
  detail: null,
  profileList: null,
  profileActions: null,
  json: null,
  jsonWrap: null,
  statusEl: null,
  model: null,
  profiles: [],
  activeProfile: '',
  actionsRoot: null,
  selectedDevice: -1,
  selectedScenario: -1,
  busy: false,
  dirty: false,
  jsonVisible: false,
  wizard: {
    open: false,
    step: 0,
    template: null,
    data: {},
    autoId: true,
  },
  wizardModal: null,
  wizardContent: null,
};

function init() {
  state.root = document.getElementById('device_wizard_root');
  state.actionsRoot = document.getElementById('actions_root');
  if (!state.root) {
    return;
  }
  injectWizardStyles();
  buildShell();
  attachEvents();
  renderAll();
  loadModel();
}

function buildShell() {
  state.root.innerHTML = `
    <div class="dw-root">
      <div class="dw-profile-bar">
        <div class="dw-profile-list" id="dw_profile_list"></div>
        <div class="dw-profile-actions">
          <button class="secondary" data-action="profile-add">Add</button>
          <button class="secondary" data-action="profile-clone">Clone</button>
          <button class="secondary" data-action="profile-download">Download</button>
          <button class="secondary" data-action="profile-rename">Rename</button>
          <button class="danger" data-action="profile-delete">Delete</button>
        </div>
      </div>
      <div class="dw-meta-row">
        <div class="dw-field dw-schema-field">
          <label>Schema version</label>
          <input type="number" id="dw_schema" disabled>
        </div>
        <button class="secondary" data-action="toggle-json">Show JSON</button>
      </div>
      <div class="dw-toolbar" id="dw_toolbar">
        <button class="secondary" data-action="reload">Reload</button>
        <button class="secondary" data-action="add-device">Add device</button>
        <button class="secondary" data-action="open-wizard">Wizard</button>
        <button class="secondary" data-action="clone-device">Clone</button>
        <button class="danger" data-action="delete-device">Delete</button>
        <button class="primary" data-action="save">Save changes</button>
        <span class="dw-status" id="dw_status">Not loaded</span>
      </div>
      <div class="dw-layout">
        <div class="dw-list" id="dw_device_list"></div>
        <div class="dw-detail" id="dw_device_detail"></div>
      </div>
      <div class="dw-json collapsed" id="dw_json_panel">
        <pre class="dw-json-content" id="dw_json_preview">No config</pre>
      </div>
      <div class="dw-modal hidden" id="dw_wizard_modal">
        <div class="dw-modal-content" id="dw_wizard_content"></div>
      </div>
    </div>`;

  state.toolbar = document.getElementById('dw_toolbar');
  state.list = document.getElementById('dw_device_list');
  state.detail = document.getElementById('dw_device_detail');
  state.json = document.getElementById('dw_json_preview');
  state.jsonWrap = document.getElementById('dw_json_panel');
  state.statusEl = document.getElementById('dw_status');
  state.wizardModal = document.getElementById('dw_wizard_modal');
  state.wizardContent = document.getElementById('dw_wizard_content');
  state.profileList = document.getElementById('dw_profile_list');
  state.profileActions = state.root.querySelector('.dw-profile-actions');
}

function attachEvents() {
  state.toolbar?.addEventListener('click', (ev) => {
    const btn = ev.target.closest('[data-action]');
    if (!btn) return;
    switch (btn.dataset.action) {
      case 'reload':
        loadModel();
        break;
      case 'add-device':
        addDevice();
        break;
      case 'clone-device':
        cloneDevice();
        break;
      case 'delete-device':
        deleteDevice();
        break;
      case 'open-wizard':
        openWizard();
        break;
      case 'save':
        saveModel();
        break;
      default:
        break;
    }
  });

  state.profileActions?.addEventListener('click', (ev) => {
    const btn = ev.target.closest('[data-action]');
    if (!btn) return;
    switch (btn.dataset.action) {
      case 'profile-add':
        createProfile();
        break;
      case 'profile-clone':
        cloneProfile();
        break;
      case 'profile-download':
        downloadProfile();
        break;
      case 'profile-rename':
        renameProfile();
        break;
      case 'profile-delete':
        deleteProfile();
        break;
      default:
        break;
    }
  });

  const jsonToggle = state.root.querySelector('[data-action="toggle-json"]');
  jsonToggle?.addEventListener('click', toggleJsonPanel);
  state.profileList?.addEventListener('click', (ev) => {
    const chip = ev.target.closest('[data-profile-id]');
    if (!chip) return;
    activateProfile(chip.dataset.profileId || '');
  });
  state.actionsRoot?.addEventListener('click', (ev) => {
    const btn = ev.target.closest('[data-run-device][data-run-scenario]');
    if (!btn) return;
    runScenario(btn.dataset.runDevice, btn.dataset.runScenario);
  });

  state.list?.addEventListener('click', (ev) => {
    const item = ev.target.closest('[data-device-index]');
    if (!item) return;
    const idx = parseInt(item.dataset.deviceIndex, 10);
    if (!isNaN(idx)) {
      state.selectedDevice = idx;
      state.selectedScenario = -1;
      renderAll();
    }
  });

  state.detail?.addEventListener('input', handleDetailInput);
  state.detail?.addEventListener('change', handleDetailInput);
  state.detail?.addEventListener('click', handleDetailClick);

  state.wizardModal?.addEventListener('click', handleWizardClick);
  state.wizardModal?.addEventListener('input', handleWizardInput);
}

function toggleJsonPanel() {
  state.jsonVisible = !state.jsonVisible;
  if (state.jsonWrap) {
    state.jsonWrap.classList.toggle('collapsed', !state.jsonVisible);
  }
  const btn = state.root?.querySelector('[data-action="toggle-json"]');
  if (btn) {
    btn.textContent = state.jsonVisible ? 'Hide JSON' : 'Show JSON';
  }
  renderJson();
}

function handleDetailClick(ev) {
  const btn = ev.target.closest('[data-action]');
  if (!btn) return;
  const action = btn.dataset.action;
  switch (action) {
    case 'add-topic': addTopic(); break;
    case 'remove-topic': removeTopic(btn.dataset.index); break;
    case 'add-scenario': addScenario(); break;
    case 'remove-scenario': removeScenario(btn.dataset.index); break;
    case 'select-scenario': selectScenario(btn.dataset.index); break;
    case 'add-step': addStep(); break;
    case 'remove-step': removeStep(btn.dataset.index); break;
    case 'step-up': moveStep(btn.dataset.index, -1); break;
    case 'step-down': moveStep(btn.dataset.index, 1); break;
    case 'add-wait-rule': addWaitRule(btn.dataset.stepIndex); break;
    case 'remove-wait-rule': removeWaitRule(btn.dataset.stepIndex, btn.dataset.reqIndex); break;
    case 'slot-add': addTemplateSlot(); break;
    case 'slot-remove': removeTemplateSlot(btn.dataset.index); break;
    case 'mqtt-rule-add': addMqttRule(); break;
    case 'mqtt-rule-remove': removeMqttRule(btn.dataset.index); break;
    case 'flag-rule-add': addFlagRule(); break;
    case 'flag-rule-remove': removeFlagRule(btn.dataset.index); break;
    case 'condition-rule-add': addConditionRule(); break;
    case 'condition-rule-remove': removeConditionRule(btn.dataset.index); break;
    case 'sequence-step-add': addSequenceStep(); break;
    case 'sequence-step-remove': removeSequenceStep(btn.dataset.index); break;
  }
}

function handleDetailInput(ev) {
  const el = ev.target;
  if (!el) return;
  if (el.dataset.deviceField) {
    updateDeviceField(el.dataset.deviceField, el.value);
    return;
  }
  if (el.dataset.topicField) {
    updateTopicField(el.dataset.index, el.dataset.topicField, el.value);
    return;
  }
  if (el.dataset.scenarioField) {
    updateScenarioField(el.dataset.scenarioField, el);
    return;
  }
  if (el.dataset.stepField) {
    updateStepField(el.dataset.index, el.dataset.stepField, el);
    return;
  }
  if (el.dataset.templateField) {
    updateTemplateField(el);
    return;
  }
  if (el.dataset.waitField) {
    updateWaitField(el.dataset.stepIndex, el.dataset.reqIndex, el.dataset.waitField, el);
  }
  validateRequiredFields();
}
function loadModel() {
  setStatus('Loading...', '#fbbf24');
  state.busy = true;
  fetch('/api/devices/config')
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    })
    .then(cfg => {
      state.model = normalizeLoadedConfig(cfg || {});
      state.profiles = Array.isArray(cfg?.profiles) ? cfg.profiles : [];
      state.activeProfile = cfg?.active_profile || cfg?.activeProfile || '';
      if (!state.activeProfile && state.profiles.length) {
        state.activeProfile = state.profiles[0].id;
      }
      state.selectedDevice = (state.model.devices && state.model.devices.length) ? 0 : -1;
      state.selectedScenario = -1;
      state.dirty = false;
      state.busy = false;
      renderAll();
      setStatus('Loaded', '#22c55e');
    })
    .catch(err => {
      console.error(err);
      state.busy = false;
      setStatus('Load failed', '#f87171');
    });
}

function saveModel() {
  if (!state.model || state.busy) return;
  setStatus('Saving...', '#fbbf24');
  state.busy = true;
  const payload = prepareConfigForSave(state.model);
  const profileQuery = state.activeProfile ? `?profile=${encodeURIComponent(state.activeProfile)}` : '';
  fetch(`/api/devices/apply${profileQuery}`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(payload),
  })
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json().catch(() => ({}));
    })
    .then(() => {
      state.busy = false;
      state.dirty = false;
      setStatus('Saved', '#22c55e');
      renderActions();
    })
    .catch(err => {
      state.busy = false;
      setStatus('Save failed: ' + err.message, '#f87171');
    });
}

function markDirty() {
  state.dirty = true;
  renderJson();
  updateToolbar();
  renderActions();
}

function renderAll() {
  renderDeviceList();
  renderDeviceDetail();
  renderJson();
  updateToolbar();
  updateGlobals();
  renderProfiles();
  renderActions();
}

function ensureModel() {
  if (!state.model) {
    state.model = {schema: 1, devices: []};
  }
  if (!Array.isArray(state.model.devices)) {
    state.model.devices = [];
  }
}

function getDevices() {
  ensureModel();
  return state.model.devices;
}

function currentDevice() {
  ensureModel();
  return state.model.devices[state.selectedDevice] || null;
}

function currentScenario() {
  const dev = currentDevice();
  if (!dev || !Array.isArray(dev.scenarios)) {
    return null;
  }
  return dev.scenarios[state.selectedScenario] || null;
}
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
function ensure(obj, path) {
  let cursor = obj;
  for (const key of path) {
    if (cursor[key] === undefined) cursor[key] = {};
    cursor = cursor[key];
  }
  return cursor;
}

function slugify(text) {
  return String(text || '')
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    || `device_${Date.now().toString(16)}`;
}

function createMqttStep(topic, payload) {
  return {
    type: 'mqtt_publish',
    delay_ms: 0,
    data: {
      mqtt: {
        topic: topic || '',
        payload: payload || '',
        qos: 0,
        retain: false,
      },
    },
  };
}

function createAudioStep(track) {
  return {
    type: 'audio_play',
    delay_ms: 0,
    data: {
      audio: {
        track: track || '',
        blocking: false,
      },
    },
  };
}

function createDelayStep(ms) {
  const parsed = typeof ms === 'number' ? ms : parseInt(ms, 10);
  return {
    type: 'delay',
    delay_ms: Number.isFinite(parsed) ? parsed : 0,
  };
}

function updateDeviceField(field, value) {
  const dev = currentDevice();
  if (!dev) return;
  dev[field] = value;
  if (field === 'display_name') {
    dev.name = value;
  } else if (field === 'name' && !dev.display_name) {
    dev.display_name = value;
  }
  markDirty();
}

function updateTopicField(indexStr, field, value) {
  const idx = parseInt(indexStr, 10);
  const dev = currentDevice();
  if (!dev || isNaN(idx) || !dev.topics || !dev.topics[idx]) return;
  dev.topics[idx][field] = value;
  markDirty();
}

function updateTemplateField(el) {
  const dev = currentDevice();
  if (!dev) return;
  const field = el.dataset.templateField;
  switch (field) {
    case 'type':
      setDeviceTemplate(dev, el.value);
      return;
    case 'uid-slot': {
      ensureUidTemplate(dev);
      const idx = parseInt(el.dataset.index, 10);
      if (!dev.template?.uid || Number.isNaN(idx) || !dev.template.uid.slots[idx]) {
        return;
      }
      const sub = el.dataset.subfield;
      dev.template.uid.slots[idx][sub] = el.value;
      break;
    }
    case 'uid-values': {
      ensureUidTemplate(dev);
      const idx = parseInt(el.dataset.index, 10);
      if (!dev.template?.uid || Number.isNaN(idx) || !dev.template.uid.slots[idx]) {
        return;
      }
      dev.template.uid.slots[idx].values = el.value.split(',').map((v) => v.trim()).filter(Boolean);
      break;
    }
    case 'uid-action': {
      ensureUidTemplate(dev);
      if (!dev.template?.uid) return;
      const sub = el.dataset.subfield;
      dev.template.uid[sub] = el.value;
      break;
    }
    case 'uid-activation': {
      ensureUidTemplate(dev);
      if (!dev.template?.uid) return;
      const sub = el.dataset.subfield;
      dev.template.uid[sub] = el.value;
      break;
    }
    case 'signal': {
      ensureSignalTemplate(dev);
      if (!dev.template?.signal) return;
      const sub = el.dataset.subfield;
      if (sub === 'required_hold_ms' || sub === 'heartbeat_timeout_ms') {
        dev.template.signal[sub] = parseInt(el.value, 10) || 0;
      } else if (sub === 'hold_track_loop') {
        dev.template.signal[sub] = el.value === 'true';
      } else if (sub === 'debug_logging') {
        dev.template.signal[sub] = el.type === 'checkbox' ? el.checked : el.value === 'true';
      } else {
        dev.template.signal[sub] = el.value;
      }
      break;
    }
    case 'mqtt-rule': {
      ensureMqttTemplate(dev);
      const tpl = dev.template?.mqtt;
      if (!tpl) return;
      const idx = parseInt(el.dataset.index, 10);
      if (Number.isNaN(idx) || !tpl.rules[idx]) return;
      const sub = el.dataset.subfield;
      if (sub === 'payload_required') {
        tpl.rules[idx].payload_required = el.type === 'checkbox' ? el.checked : el.value === 'true';
      } else {
        tpl.rules[idx][sub] = el.value;
      }
      break;
    }
    case 'flag-rule': {
      ensureFlagTemplate(dev);
      const tpl = dev.template?.flag;
      if (!tpl) return;
      const idx = parseInt(el.dataset.index, 10);
      if (Number.isNaN(idx) || !tpl.rules[idx]) return;
      const sub = el.dataset.subfield;
      if (sub === 'state') {
        tpl.rules[idx].required_state = el.value === 'true';
      } else {
        tpl.rules[idx][sub] = el.value;
      }
      break;
    }
    case 'condition-mode': {
      ensureConditionTemplate(dev);
      if (!dev.template?.condition) return;
      dev.template.condition.mode = el.value === 'any' ? 'any' : 'all';
      break;
    }
    case 'condition-scenario': {
      ensureConditionTemplate(dev);
      if (!dev.template?.condition) return;
      const sub = el.dataset.subfield;
      if (sub === 'true') {
        dev.template.condition.true_scenario = el.value;
      } else if (sub === 'false') {
        dev.template.condition.false_scenario = el.value;
      }
      break;
    }
    case 'condition-rule': {
      ensureConditionTemplate(dev);
      const tpl = dev.template?.condition;
      if (!tpl) return;
      const idx = parseInt(el.dataset.index, 10);
      if (Number.isNaN(idx) || !tpl.rules[idx]) return;
      const sub = el.dataset.subfield;
      if (sub === 'state') {
        tpl.rules[idx].required_state = el.value === 'true';
      } else {
        tpl.rules[idx][sub] = el.value;
      }
      break;
    }
    case 'interval': {
      ensureIntervalTemplate(dev);
      if (!dev.template?.interval) return;
      const sub = el.dataset.subfield;
      if (sub === 'interval_ms') {
        const val = parseInt(el.value, 10);
        const next = Number.isNaN(val) ? 1000 : Math.max(val, 1);
        dev.template.interval.interval_ms = next;
        el.value = next;
      } else if (sub === 'scenario') {
        dev.template.interval.scenario = el.value;
      }
      break;
    }
    case 'sequence-step': {
      ensureSequenceTemplate(dev);
      const tpl = dev.template?.sequence;
      if (!tpl) return;
      const idx = parseInt(el.dataset.index, 10);
      if (Number.isNaN(idx) || !tpl.steps[idx]) return;
      const sub = el.dataset.subfield;
      if (sub === 'payload_required') {
        tpl.steps[idx].payload_required = el.type === 'checkbox' ? el.checked : el.value === 'true';
      } else {
        tpl.steps[idx][sub] = el.value;
      }
      break;
    }
    case 'sequence': {
      ensureSequenceTemplate(dev);
      const tpl = dev.template?.sequence;
      if (!tpl) return;
      const sub = el.dataset.subfield;
      if (sub === 'timeout_ms') {
        const val = parseInt(el.value, 10);
        const next = Number.isNaN(val) ? 0 : Math.max(val, 0);
        tpl.timeout_ms = next;
        el.value = next;
      } else if (sub === 'reset_on_error') {
        tpl.reset_on_error = el.type === 'checkbox' ? el.checked : el.value === 'true';
      } else {
        tpl[sub] = el.value;
      }
      break;
    }
    default:
      return;
  }
  markDirty();
}

function updateScenarioField(field, el) {
  const scen = currentScenario();
  if (!scen || !el) return;
  switch (field) {
    case 'button_enabled': {
      scen.button_enabled = el.type === 'checkbox' ? el.checked : el.value === 'true';
      if (!scen.button_enabled) {
        scen.button_label = '';
      }
      renderDeviceDetail();
      break;
    }
    case 'button_label':
      scen.button_label = el.value;
      break;
    default:
      scen[field] = el.value;
      break;
  }
  markDirty();
}

function updateStepField(indexStr, field, el) {
  const idx = parseInt(indexStr, 10);
  const scen = currentScenario();
  if (!scen || isNaN(idx) || !scen.steps || !scen.steps[idx]) return;
  const step = scen.steps[idx];
  if (field === 'type') {
    step.type = el.value;
    renderDeviceDetail();
    markDirty();
    return;
  }
  if (field.includes('.')) {
    const parts = field.split('.');
    let target = step;
    for (let i = 0; i < parts.length - 1; i++) {
      const key = parts[i];
      if (target[key] === undefined) target[key] = {};
      target = target[key];
    }
    target[parts[parts.length - 1]] = normalizeValue(el.value, el.type);
  } else {
    step[field] = normalizeValue(el.value, el.type);
  }
  markDirty();
}

function updateWaitField(stepIdxStr, reqIdxStr, field, el) {
  const stepIdx = parseInt(stepIdxStr, 10);
  const reqIdx = parseInt(reqIdxStr, 10);
  const scen = currentScenario();
  if (!scen || isNaN(stepIdx) || isNaN(reqIdx)) return;
  const step = scen.steps?.[stepIdx];
  if (!step) return;
  ensure(step, ['data','wait_flags','requirements']);
  const req = step.data.wait_flags.requirements[reqIdx];
  if (!req) return;
  if (field === 'flag') req.flag = el.value;
  if (field === 'state') req.required_state = el.value === 'true';
  markDirty();
}

function setDeviceTemplate(dev, type) {
  if (!dev) return;
  let nextType = type || '';
  const allowed = ['uid_validator',
                   'signal_hold',
                   'on_mqtt_event',
                   'on_flag',
                   'if_condition',
                   'interval_task',
                   'sequence_lock'];
  if (!allowed.includes(nextType)) {
    dev.template = null;
    markDirty();
    renderDeviceDetail();
    return;
  }
  if (!dev.template || dev.template.type !== nextType) {
    if (nextType === 'uid_validator') {
      dev.template = {
        type: nextType,
        uid: defaultUidTemplate(),
      };
    } else if (nextType === 'signal_hold') {
      dev.template = {
        type: nextType,
        signal: defaultSignalTemplate(),
      };
    } else if (nextType === 'on_mqtt_event') {
      dev.template = {
        type: nextType,
        mqtt: defaultMqttTemplate(),
      };
    } else if (nextType === 'on_flag') {
      dev.template = {
        type: nextType,
        flag: defaultFlagTemplate(),
      };
    } else if (nextType === 'if_condition') {
      dev.template = {
        type: nextType,
        condition: defaultConditionTemplate(),
      };
    } else if (nextType === 'interval_task') {
      dev.template = {
        type: nextType,
        interval: defaultIntervalTemplate(),
      };
    } else if (nextType === 'sequence_lock') {
      dev.template = {
        type: nextType,
        sequence: defaultSequenceTemplate(),
      };
    }
  }
  if (nextType === 'uid_validator') {
    ensureUidTemplate(dev);
  } else if (nextType === 'signal_hold') {
    ensureSignalTemplate(dev);
  } else if (nextType === 'on_mqtt_event') {
    ensureMqttTemplate(dev);
  } else if (nextType === 'on_flag') {
    ensureFlagTemplate(dev);
  } else if (nextType === 'if_condition') {
    ensureConditionTemplate(dev);
  } else if (nextType === 'interval_task') {
    ensureIntervalTemplate(dev);
  } else if (nextType === 'sequence_lock') {
    ensureSequenceTemplate(dev);
  }
  markDirty();
  renderDeviceDetail();
}

function defaultUidTemplate() {
  return {
    slots: [],
    start_topic: '',
    start_payload: '',
    broadcast_topic: '',
    broadcast_payload: '',
    success_topic: '',
    success_payload: '',
    success_audio_track: '',
    fail_topic: '',
    fail_payload: '',
    fail_audio_track: '',
  };
}

function defaultSignalTemplate() {
  return {
    signal_topic: '',
    signal_payload_on: '',
    signal_payload_off: '',
    signal_on_ms: 0,
    heartbeat_topic: '',
    reset_topic: '',
    debug_logging: false,
    required_hold_ms: 0,
    heartbeat_timeout_ms: 0,
    hold_track: '',
    hold_track_loop: false,
    complete_track: '',
  };
}

function defaultMqttTemplate() {
  return {
    rules: [],
  };
}

function defaultFlagTemplate() {
  return {
    rules: [],
  };
}

function defaultConditionTemplate() {
  return {
    mode: 'all',
    rules: [],
    true_scenario: '',
    false_scenario: '',
  };
}

function defaultIntervalTemplate() {
  return {
    interval_ms: 1000,
    scenario: '',
  };
}

function defaultSequenceTemplate() {
  return {
    steps: [],
    timeout_ms: 0,
    reset_on_error: true,
    success_topic: '',
    success_payload: '',
    success_audio_track: '',
    success_scenario: '',
    fail_topic: '',
    fail_payload: '',
    fail_audio_track: '',
    fail_scenario: '',
  };
}

function ensureUidTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'uid_validator') {
    return;
  }
  if (!dev.template.uid) {
    dev.template.uid = defaultUidTemplate();
  }
  const tpl = dev.template.uid;
  if (!Array.isArray(tpl.slots)) {
    tpl.slots = [];
  }
  tpl.slots.forEach((slot) => {
    slot.source_id = slot.source_id || '';
    slot.label = slot.label || '';
    if (!Array.isArray(slot.values)) {
      slot.values = [];
    }
  });
  ['success_topic','success_payload','success_audio_track','fail_topic','fail_payload','fail_audio_track'].forEach((key) => {
    if (typeof tpl[key] !== 'string') {
      tpl[key] = '';
    }
  });
  tpl.start_topic = tpl.start_topic || '';
  tpl.start_payload = tpl.start_payload || '';
  tpl.broadcast_topic = tpl.broadcast_topic || '';
  tpl.broadcast_payload = tpl.broadcast_payload || '';
}

function ensureSignalTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'signal_hold') {
    return;
  }
  if (!dev.template.signal) {
    dev.template.signal = defaultSignalTemplate();
  }
  const sig = dev.template.signal;
  ['signal_topic','signal_payload_on','signal_payload_off','heartbeat_topic','reset_topic','hold_track','complete_track'].forEach((key) => {
    sig[key] = sig[key] || '';
  });
  sig.required_hold_ms = sig.required_hold_ms || 0;
  sig.heartbeat_timeout_ms = sig.heartbeat_timeout_ms || 0;
  sig.hold_track_loop = !!sig.hold_track_loop;
  sig.signal_on_ms = sig.signal_on_ms || 0;
  sig.debug_logging = !!sig.debug_logging;
}



function ensureMqttTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'on_mqtt_event') {
    return;
  }
  if (!dev.template.mqtt) {
    dev.template.mqtt = defaultMqttTemplate();
  }
  const tpl = dev.template.mqtt;
  if (!Array.isArray(tpl.rules)) {
    tpl.rules = [];
  }
  tpl.rules.forEach((rule) => {
    rule.name = rule.name || '';
    rule.topic = rule.topic || '';
    rule.payload = rule.payload || '';
    rule.scenario = rule.scenario || '';
    rule.payload_required = !!rule.payload_required;
  });
}

function ensureFlagTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'on_flag') {
    return;
  }
  if (!dev.template.flag) {
    dev.template.flag = defaultFlagTemplate();
  }
  const tpl = dev.template.flag;
  if (!Array.isArray(tpl.rules)) {
    tpl.rules = [];
  }
  tpl.rules.forEach((rule) => {
    rule.name = rule.name || '';
    rule.flag = rule.flag || '';
    rule.scenario = rule.scenario || '';
    rule.required_state = rule.required_state !== undefined ? !!rule.required_state : true;
  });
}

function ensureConditionTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'if_condition') {
    return;
  }
  if (!dev.template.condition) {
    dev.template.condition = defaultConditionTemplate();
  }
  const tpl = dev.template.condition;
  tpl.mode = tpl.mode === 'any' ? 'any' : 'all';
  if (!Array.isArray(tpl.rules)) {
    tpl.rules = [];
  }
  tpl.true_scenario = tpl.true_scenario || '';
  tpl.false_scenario = tpl.false_scenario || '';
  tpl.rules.forEach((rule) => {
    rule.flag = rule.flag || '';
    if (rule.required_state === undefined) {
      rule.required_state = true;
    }
  });
}

function ensureIntervalTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'interval_task') {
    return;
  }
  if (!dev.template.interval) {
    dev.template.interval = defaultIntervalTemplate();
  }
  if (typeof dev.template.interval.interval_ms !== 'number') {
    dev.template.interval.interval_ms = 1000;
  }
  dev.template.interval.scenario = dev.template.interval.scenario || '';
}

function ensureSequenceTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'sequence_lock') {
    return;
  }
  if (!dev.template.sequence) {
    dev.template.sequence = defaultSequenceTemplate();
  }
  const seq = dev.template.sequence;
  if (!Array.isArray(seq.steps)) {
    seq.steps = [];
  }
  seq.steps.forEach((step) => {
    step.topic = step.topic || '';
    step.payload = step.payload || '';
    step.payload_required = !!step.payload_required;
    step.hint_topic = step.hint_topic || '';
    step.hint_payload = step.hint_payload || '';
    step.hint_audio_track = step.hint_audio_track || '';
  });
  seq.timeout_ms = seq.timeout_ms || 0;
  if (seq.reset_on_error === undefined) {
    seq.reset_on_error = true;
  }
  ['success_topic','success_payload','success_audio_track','success_scenario','fail_topic','fail_payload','fail_audio_track','fail_scenario'].forEach((key) => {
    if (typeof seq[key] !== 'string') {
      seq[key] = '';
    }
  });
}

function addTemplateSlot() {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'uid_validator') {
    return;
  }
  ensureUidTemplate(dev);
  dev.template.uid.slots.push({source_id: '', label: '', values: []});
  markDirty();
  renderDeviceDetail();
}

function removeTemplateSlot(indexStr) {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'uid_validator') {
    return;
  }
  ensureUidTemplate(dev);
  const idx = parseInt(indexStr, 10);
  if (Number.isNaN(idx) || idx < 0) {
    return;
  }
  dev.template.uid.slots.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
}

function addMqttRule() {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'on_mqtt_event') {
    return;
  }
  ensureMqttTemplate(dev);
  const tpl = dev.template.mqtt;
  if (tpl.rules.length >= MQTT_RULE_LIMIT) {
    setStatus('MQTT rule limit reached', '#fbbf24');
    return;
  }
  tpl.rules.push({
    name: '',
    topic: '',
    payload: '',
    payload_required: true,
    scenario: '',
  });
  markDirty();
  renderDeviceDetail();
}

function removeMqttRule(indexStr) {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'on_mqtt_event') {
    return;
  }
  ensureMqttTemplate(dev);
  const idx = parseInt(indexStr, 10);
  if (Number.isNaN(idx)) {
    return;
  }
  dev.template.mqtt.rules.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
}

function addFlagRule() {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'on_flag') {
    return;
  }
  ensureFlagTemplate(dev);
  const tpl = dev.template.flag;
  if (tpl.rules.length >= FLAG_RULE_LIMIT) {
    setStatus('Flag rule limit reached', '#fbbf24');
    return;
  }
  tpl.rules.push({
    name: '',
    flag: '',
    scenario: '',
    required_state: true,
  });
  markDirty();
  renderDeviceDetail();
}

function removeFlagRule(indexStr) {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'on_flag') {
    return;
  }
  ensureFlagTemplate(dev);
  const idx = parseInt(indexStr, 10);
  if (Number.isNaN(idx)) {
    return;
  }
  dev.template.flag.rules.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
}

function addConditionRule() {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'if_condition') {
    return;
  }
  ensureConditionTemplate(dev);
  const tpl = dev.template.condition;
  if (tpl.rules.length >= FLAG_RULE_LIMIT) {
    setStatus('Condition limit reached', '#fbbf24');
    return;
  }
  tpl.rules.push({
    flag: '',
    required_state: true,
  });
  markDirty();
  renderDeviceDetail();
}

function removeConditionRule(indexStr) {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'if_condition') {
    return;
  }
  ensureConditionTemplate(dev);
  const idx = parseInt(indexStr, 10);
  if (Number.isNaN(idx)) {
    return;
  }
  dev.template.condition.rules.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
}

function addSequenceStep() {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'sequence_lock') {
    return;
  }
  ensureSequenceTemplate(dev);
  const tpl = dev.template.sequence;
  if (tpl.steps.length >= SEQUENCE_STEP_LIMIT) {
    setStatus('Step limit reached', '#fbbf24');
    return;
  }
  tpl.steps.push({
    topic: '',
    payload: '',
    payload_required: false,
    hint_topic: '',
    hint_payload: '',
    hint_audio_track: '',
  });
  markDirty();
  renderDeviceDetail();
}

function removeSequenceStep(indexStr) {
  const dev = currentDevice();
  if (!dev || dev.template?.type !== 'sequence_lock') {
    return;
  }
  ensureSequenceTemplate(dev);
  const idx = parseInt(indexStr, 10);
  if (Number.isNaN(idx)) {
    return;
  }
  dev.template.sequence.steps.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
}

function addWaitRule(stepIdxStr) {
  const idx = parseInt(stepIdxStr, 10);
  const scen = currentScenario();
  if (!scen || isNaN(idx)) return;
  const step = scen.steps?.[idx];
  if (!step) return;
  ensure(step, ['data','wait_flags','requirements']);
  step.data.wait_flags.requirements.push({flag: '', required_state: true});
  renderDeviceDetail();
  markDirty();
}

function removeWaitRule(stepIdxStr, reqIdxStr) {
  const stepIdx = parseInt(stepIdxStr, 10);
  const reqIdx = parseInt(reqIdxStr, 10);
  const scen = currentScenario();
  if (!scen || isNaN(stepIdx) || isNaN(reqIdx)) return;
  const step = scen.steps?.[stepIdx];
  if (!step) return;
  ensure(step, ['data','wait_flags','requirements']);
  step.data.wait_flags.requirements.splice(reqIdx, 1);
  renderDeviceDetail();
  markDirty();
}

function normalizeValue(value, type) {
  if (type === 'number') {
    const num = parseInt(value, 10);
    return isNaN(num) ? 0 : num;
  }
  if (value === 'true') return true;
  if (value === 'false') return false;
  return value;
}

function normalizeLoadedConfig(cfg) {
  const model = cfg && typeof cfg === 'object' ? cfg : {devices: []};
  if (!Array.isArray(model.devices)) {
    model.devices = [];
  }
  model.devices.forEach(normalizeDevice);
  return model;
}

function normalizeDevice(dev) {
  if (!dev || typeof dev !== 'object') return;
  if (!dev.display_name) {
    dev.display_name = dev.name || dev.id || '';
  }
  if (!dev.name && dev.display_name) {
    dev.name = dev.display_name;
  }
  if (!Array.isArray(dev.scenarios)) {
    dev.scenarios = [];
  }
  dev.scenarios.forEach(normalizeScenario);
  if (dev.template && dev.template.type === 'signal_hold') {
    ensureSignalTemplate(dev);
  }
}

function normalizeScenario(scen) {
  if (!scen || typeof scen !== 'object') return;
  scen.button_enabled = !!scen.button_enabled;
  if (typeof scen.button_label !== 'string') {
    scen.button_label = '';
  }
  if (!Array.isArray(scen.steps)) {
    scen.steps = [];
  }
  scen.steps.forEach(normalizeStepForEditing);
}

function normalizeStepForEditing(step) {
  if (!step || typeof step !== 'object') return;
  step.data = step.data || {};
  switch (step.type) {
    case 'mqtt_publish': {
      const mqtt = step.data.mqtt = step.data.mqtt || {};
      if (mqtt.topic === undefined) mqtt.topic = step.topic || '';
      if (mqtt.payload === undefined) mqtt.payload = step.payload || '';
      if (mqtt.qos === undefined) mqtt.qos = typeof step.qos === 'number' ? step.qos : 0;
      if (mqtt.retain === undefined) mqtt.retain = !!step.retain;
      break;
    }
    case 'audio_play': {
      const audio = step.data.audio = step.data.audio || {};
      if (audio.track === undefined) audio.track = step.track || '';
      if (audio.blocking === undefined) audio.blocking = !!step.blocking;
      break;
    }
    case 'set_flag': {
      const flag = step.data.flag = step.data.flag || {};
      if (flag.flag === undefined) flag.flag = step.flag || '';
      if (flag.value === undefined) flag.value = step.value !== undefined ? !!step.value : false;
      break;
    }
    case 'wait_flags': {
      const waitFlags = step.data.wait_flags = step.data.wait_flags || {};
      if (waitFlags.mode === undefined) waitFlags.mode = (step.wait && step.wait.mode) || 'all';
      if (waitFlags.timeout_ms === undefined) {
        waitFlags.timeout_ms = (step.wait && step.wait.timeout_ms) ? step.wait.timeout_ms : 0;
      }
      const requirements = Array.isArray(waitFlags.requirements)
        ? waitFlags.requirements
        : (step.wait && Array.isArray(step.wait.requirements) ? step.wait.requirements : []);
      waitFlags.requirements = requirements.map(req => ({
        flag: req.flag || '',
        required_state: req.required_state !== undefined ? !!req.required_state : !!req.state,
      }));
      break;
    }
    case 'loop': {
      const loop = step.data.loop = step.data.loop || {};
      if (loop.target_step === undefined) {
        loop.target_step = (step.loop && typeof step.loop.target_step === 'number') ? step.loop.target_step : 0;
      }
      if (loop.max_iterations === undefined) {
        loop.max_iterations = (step.loop && typeof step.loop.max_iterations === 'number') ? step.loop.max_iterations : 0;
      }
      break;
    }
    case 'event': {
      const event = step.data.event = step.data.event || {};
      if (event.event === undefined) event.event = step.event || '';
      if (event.topic === undefined) event.topic = step.topic || '';
      if (event.payload === undefined) event.payload = step.payload || '';
      break;
    }
    default:
      break;
  }
}
function prepareConfigForSave(model) {
  const snapshot = JSON.parse(JSON.stringify(model || {}));
  if (!Array.isArray(snapshot.devices)) {
    snapshot.devices = [];
  }
  snapshot.devices.forEach((dev, devIdx) => {
    if (!dev || typeof dev !== 'object') return;
    if (!Array.isArray(dev.scenarios)) {
      snapshot.devices[devIdx].scenarios = [];
      return;
    }
    snapshot.devices[devIdx].scenarios = dev.scenarios.map(serializeScenarioForSave);
    const displayName = toSafeString(dev.display_name || dev.name || dev.id || 'Device');
    snapshot.devices[devIdx].display_name = displayName;
    snapshot.devices[devIdx].name = displayName;
    stripTemplateRuntimeFields(snapshot.devices[devIdx]);
    if (snapshot.devices[devIdx].tabs) {
      delete snapshot.devices[devIdx].tabs;
    }
  });
  if (snapshot.tab_limit !== undefined) {
    delete snapshot.tab_limit;
  }
  return snapshot;
}

function serializeScenarioForSave(scen) {
  const next = Object.assign({}, scen);
  if (!Array.isArray(scen.steps)) {
    next.steps = [];
  } else {
    next.steps = scen.steps.map(serializeStepForSave);
  }
  return next;
}

function serializeStepForSave(step) {
  const safe = step || {};
  const out = {
    type: safe.type || 'nop',
    delay_ms: toInt(safe.delay_ms),
  };
  switch (out.type) {
    case 'mqtt_publish': {
      const mqtt = (safe.data && safe.data.mqtt) || {};
      out.topic = mqtt.topic || '';
      out.payload = mqtt.payload || '';
      out.qos = clamp(intOrDefault(mqtt.qos), 0, 2);
      out.retain = !!mqtt.retain;
      break;
    }
    case 'audio_play': {
      const audio = (safe.data && safe.data.audio) || {};
      out.track = audio.track || '';
      out.blocking = !!audio.blocking;
      break;
    }
    case 'set_flag': {
      const flag = (safe.data && safe.data.flag) || {};
      out.flag = flag.flag || '';
      out.value = !!flag.value;
      break;
    }
    case 'wait_flags': {
      const wf = (safe.data && safe.data.wait_flags) || {};
      const wait = {
        mode: wf.mode === 'any' ? 'any' : 'all',
        timeout_ms: toInt(wf.timeout_ms),
        requirements: [],
      };
      (wf.requirements || []).forEach(req => {
        if (!req) return;
        wait.requirements.push({
          flag: req.flag || '',
          state: req.required_state !== undefined ? !!req.required_state : !!req.state,
        });
      });
      out.wait = wait;
      break;
    }
    case 'loop': {
      const loop = (safe.data && safe.data.loop) || {};
      out.loop = {
        target_step: toInt(loop.target_step),
        max_iterations: toInt(loop.max_iterations),
      };
      break;
    }
    case 'event': {
      const event = (safe.data && safe.data.event) || {};
      out.event = event.event || '';
      out.topic = event.topic || '';
      out.payload = event.payload || '';
      break;
    }
    case 'audio_stop':
    case 'delay':
    case 'nop':
    default:
      break;
  }
  return out;
}

function stripTemplateRuntimeFields(dev) {
  if (!dev || !dev.template) {
    return;
  }
  if (dev.template.uid && Array.isArray(dev.template.uid.slots)) {
    dev.template.uid.slots.forEach((slot) => {
      if (slot && Object.prototype.hasOwnProperty.call(slot, 'last_value')) {
        delete slot.last_value;
      }
    });
  }
}

function toInt(value) {
  const n = parseInt(value, 10);
  return isNaN(n) ? 0 : n;
}

function intOrDefault(value, fallback) {
  const n = parseInt(value, 10);
  if (isNaN(n)) {
    return typeof fallback === 'number' ? fallback : 0;
  }
  return n;
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function addDevice() {
  ensureModel();
  state.model.devices.push({
    id: `device_${Date.now().toString(16)}`,
    name: 'New device',
    display_name: 'New device',
    topics: [],
    scenarios: [],
  });
  state.selectedDevice = state.model.devices.length - 1;
  state.selectedScenario = -1;
  markDirty();
  renderAll();
}

function cloneDevice() {
  const dev = currentDevice();
  if (!dev) return;
  ensureModel();
  const copy = JSON.parse(JSON.stringify(dev));
  copy.id = (dev.id || 'device') + '_copy';
  copy.display_name = (dev.display_name || dev.name || dev.id || 'Device') + ' copy';
  state.model.devices.splice(state.selectedDevice + 1, 0, copy);
  state.selectedDevice += 1;
  state.selectedScenario = -1;
  markDirty();
  renderAll();
}

function deleteDevice() {
  ensureModel();
  if (state.selectedDevice < 0 || !state.model.devices.length) return;
  state.model.devices.splice(state.selectedDevice, 1);
  if (!state.model.devices.length) {
    state.selectedDevice = -1;
    state.selectedScenario = -1;
  } else {
    state.selectedDevice = Math.min(state.selectedDevice, state.model.devices.length - 1);
    state.selectedScenario = -1;
  }
  markDirty();
  renderAll();
}

function addTopic() {
  const dev = currentDevice();
  if (!dev) return;
  dev.topics = dev.topics || [];
  dev.topics.push({name: 'topic', topic: ''});
  markDirty();
  renderDeviceDetail();
}

function removeTopic(indexStr) {
  const idx = parseInt(indexStr, 10);
  const dev = currentDevice();
  if (!dev || isNaN(idx) || !dev.topics) return;
  dev.topics.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
}

function addScenario() {
  const dev = currentDevice();
  if (!dev) return;
  dev.scenarios = dev.scenarios || [];
  dev.scenarios.push({id: `scenario_${Date.now().toString(16)}`, name: 'Scenario', steps: []});
  state.selectedScenario = dev.scenarios.length - 1;
  markDirty();
  renderDeviceDetail();
}

function removeScenario(indexStr) {
  const dev = currentDevice();
  if (!dev || !dev.scenarios || !dev.scenarios.length) return;
  const idx = parseInt(indexStr, 10);
  const target = isNaN(idx) ? state.selectedScenario : idx;
  if (target < 0) return;
  dev.scenarios.splice(target, 1);
  state.selectedScenario = Math.min(target, (dev.scenarios.length - 1));
  markDirty();
  renderDeviceDetail();
}

function selectScenario(indexStr) {
  const idx = parseInt(indexStr, 10);
  if (isNaN(idx)) return;
  state.selectedScenario = idx;
  renderDeviceDetail();
}

function addStep() {
  const scen = currentScenario();
  if (!scen) return;
  scen.steps = scen.steps || [];
  scen.steps.push({type: 'mqtt_publish', delay_ms: 0, data: {mqtt: {topic: '', payload: '', qos: 0, retain: false}}});
  markDirty();
  renderDeviceDetail();
}

function removeStep(indexStr) {
  const idx = parseInt(indexStr, 10);
  const scen = currentScenario();
  if (!scen || isNaN(idx) || !scen.steps) return;
  scen.steps.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
}

function moveStep(indexStr, delta) {
  const idx = parseInt(indexStr, 10);
  const scen = currentScenario();
  if (!scen || isNaN(idx) || !scen.steps || !scen.steps[idx]) return;
  const target = idx + delta;
  if (target < 0 || target >= scen.steps.length) return;
  const [step] = scen.steps.splice(idx, 1);
  scen.steps.splice(target, 0, step);
  markDirty();
  renderDeviceDetail();
}

function renderJson() {
  if (!state.json) return;
  if (!state.model) {
    state.json.textContent = 'No config loaded';
    return;
  }
  state.json.textContent = JSON.stringify(state.model, null, 2);
  if (state.jsonWrap) {
    state.jsonWrap.classList.toggle('collapsed', !state.jsonVisible);
  }
}

function updateToolbar() {
  const saveBtn = state.toolbar?.querySelector('[data-action="save"]');
  if (saveBtn) saveBtn.disabled = !state.dirty || state.busy;
  const cloneBtn = state.toolbar?.querySelector('[data-action="clone-device"]');
  if (cloneBtn) cloneBtn.disabled = state.selectedDevice < 0;
  const deleteBtn = state.toolbar?.querySelector('[data-action="delete-device"]');
  if (deleteBtn) deleteBtn.disabled = state.selectedDevice < 0;
}

function updateGlobals() {
  const schema = document.getElementById('dw_schema');
  if (schema && state.model) schema.value = state.model.schema ?? state.model.schema_version ?? 1;
}

function toSafeString(value) {
  if (value === null || value === undefined) {
    return '';
  }
  return String(value);
}

function escapeHtml(text) {
  const str = toSafeString(text);
  return str.replace(/[&<>"']/g, (c) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
  }[c] || c));
}

function escapeAttr(text) {
  return escapeHtml(text);
}

function setStatus(text, color) {
  if (!state.statusEl) return;
  state.statusEl.textContent = text;
  if (color) state.statusEl.style.color = color;
}

function renderProfiles() {
  if (!state.profileList) return;
  const profiles = Array.isArray(state.profiles) ? state.profiles : [];
  if (!profiles.length) {
    state.profileList.innerHTML = "<div class='dw-empty small'>No profiles</div>";
    return;
  }
  state.profileList.innerHTML = profiles.map((profile) => {
    const active = profile.id === state.activeProfile ? ' active' : '';
    const label = escapeHtml(profile.name || profile.id);
    return `<div class="dw-profile-chip${active}" data-profile-id="${escapeAttr(profile.id)}">${label}</div>`;
  }).join('');
}

function renderActions() {
  if (!state.actionsRoot) return;
  const devices = state.model?.devices || [];
  const groups = devices.map((dev) => {
    const scenarios = (dev.scenarios || []).filter(sc => sc && sc.button_enabled && sc.id);
    if (!scenarios.length || !dev.id) {
      return '';
    }
    const title = escapeHtml(dev.display_name || dev.name || dev.id);
    const buttons = scenarios.map((sc) => {
      const label = escapeHtml(sc.button_label || sc.name || sc.id);
      return `<button class="dw-action-btn" data-run-device="${escapeAttr(dev.id)}" data-run-scenario="${escapeAttr(sc.id)}">${label}</button>`;
    }).join('');
    return `<div class="dw-action-group">
      <div class="dw-action-title">${title}</div>
      <div class="dw-action-buttons">${buttons}</div>
    </div>`;
  }).filter(Boolean).join('');
  state.actionsRoot.innerHTML = groups || "<div class='dw-empty'>No action buttons configured.</div>";
}

function createProfile(cloneId) {
  const id = prompt('Profile id:', 'profile_' + Date.now().toString(16));
  if (!id) {
    return;
  }
  const name = prompt('Display name:', id) || id;
  let url = '/api/devices/profile/create?id=' + encodeURIComponent(id) +
    '&name=' + encodeURIComponent(name);
  if (cloneId) {
    url += '&clone=' + encodeURIComponent(cloneId);
  }
  fetch(url, {method: 'POST'})
    .then(() => loadModel())
    .catch(err => setStatus('Profile create failed: ' + err.message, '#f87171'));
}

function cloneProfile() {
  if (!state.activeProfile) {
    return;
  }
  createProfile(state.activeProfile);
}

function downloadProfile() {
  const id = state.activeProfile || (state.profiles[0]?.id) || '';
  const url = id ? `/api/devices/profile/download?profile=${encodeURIComponent(id)}` : '/api/devices/profile/download';
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.style.display = 'none';
  document.body.appendChild(anchor);
  anchor.click();
  setTimeout(() => {
    document.body.removeChild(anchor);
  }, 0);
}

function renameProfile() {
  if (!state.activeProfile) {
    return;
  }
  const name = prompt('New profile name:', state.activeProfile);
  if (!name) {
    return;
  }
  fetch('/api/devices/profile/rename?id=' + encodeURIComponent(state.activeProfile) +
    '&name=' + encodeURIComponent(name), {method: 'POST'})
    .then(() => loadModel())
    .catch(err => setStatus('Rename failed: ' + err.message, '#f87171'));
}

function deleteProfile() {
  if (!state.activeProfile) {
    return;
  }
  if (!confirm('Delete profile ' + state.activeProfile + '?')) {
    return;
  }
  fetch('/api/devices/profile/delete?id=' + encodeURIComponent(state.activeProfile), {method: 'POST'})
    .then(() => loadModel())
    .catch(err => setStatus('Delete failed: ' + err.message, '#f87171'));
}

function activateProfile(id) {
  if (!id || id === state.activeProfile) {
    return;
  }
  state.activeProfile = id;
  renderProfiles();
  fetch('/api/devices/profile/activate?id=' + encodeURIComponent(id), {method: 'POST'})
    .then(() => loadModel())
    .catch(err => setStatus('Activate failed: ' + err.message, '#f87171'));
}

function injectWizardStyles() {
  if (document.getElementById('dw_wizard_styles')) return;
  const style = document.createElement('style');
  style.id = 'dw_wizard_styles';
  style.textContent = `
    .dw-modal{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;background:rgba(15,23,42,0.6);z-index:50;}
    .dw-modal.hidden{display:none;}
    .dw-modal .dw-modal-content{background:#0f172a;color:#e2e8f0;border:1px solid #1f2937;border-radius:12px;max-width:720px;width:90%;max-height:90vh;overflow:auto;box-shadow:0 25px 50px -12px rgba(0,0,0,.5);}
    .dw-wizard{padding:24px;display:flex;flex-direction:column;gap:16px;}
    .dw-wizard h3{margin:0;font-size:1.3rem;}
    .dw-wizard-body{display:flex;flex-direction:column;gap:16px;}
    .dw-wizard-fields .dw-field{margin-bottom:12px;}
    .dw-wizard-templates{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;}
    .dw-wizard-template{border:1px solid #1f2937;border-radius:8px;padding:12px;cursor:pointer;background:#111827;transition:all .2s;}
    .dw-wizard-template.selected{border-color:#38bdf8;background:#0d253b;}
    .dw-wizard-summary{background:#0b1120;border:1px solid #1f2937;border-radius:8px;padding:12px;font-family:monospace;font-size:0.85rem;max-height:280px;overflow:auto;}
    .dw-wizard-nav{display:flex;justify-content:space-between;align-items:center;}
    .dw-wizard-nav .right{display:flex;gap:8px;}
    .dw-wizard-note{font-size:0.85rem;color:#94a3b8;}
  `;
  document.head.appendChild(style);
}

function runScenario(deviceId, scenarioId) {
  const devId = (toSafeString(deviceId).trim()) || (toSafeString(currentDevice()?.id).trim());
  const scenId = (toSafeString(scenarioId).trim()) || (toSafeString(currentScenario()?.id).trim());
  if (!devId || !scenId) {
    setStatus('Select a scenario to run', '#f87171');
    return;
  }
  const url = `/api/devices/run?device=${encodeURIComponent(devId)}&scenario=${encodeURIComponent(scenId)}`;
  setStatus('Triggering scenario...', '#fbbf24');
  fetch(url)
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json().catch(() => ({}));
    })
    .then(() => {
      setStatus('Scenario queued', '#22c55e');
    })
    .catch(err => {
      setStatus('Run failed: ' + err.message, '#f87171');
    });
}
function openWizard(templateId) {
  state.wizard.open = true;
  state.wizard.step = 0;
  state.wizard.autoId = true;
  state.wizard.data = {deviceName: '', deviceId: ''};
  state.wizard.template = null;
  if (templateId && WIZARD_TEMPLATES[templateId]) {
    applyWizardTemplate(templateId);
    state.wizard.step = 1;
  }
  renderWizard();
}

function closeWizard() {
  state.wizard.open = false;
  state.wizard.step = 0;
  state.wizard.template = null;
  state.wizard.data = {};
  state.wizardModal?.classList.add('hidden');
}

function applyWizardTemplate(templateId) {
  const tpl = WIZARD_TEMPLATES[templateId];
  if (!tpl) {
    state.wizard.template = null;
    return;
  }
  state.wizard.template = templateId;
  state.wizard.data = {deviceName: tpl.defaults?.deviceName || '', deviceId: ''};
  state.wizard.autoId = true;
  if (tpl.defaults) {
    Object.keys(tpl.defaults).forEach((key) => {
      state.wizard.data[key] = tpl.defaults[key];
    });
  }
  state.wizard.data.deviceId = slugify(state.wizard.data.deviceName || templateId);
}

function renderWizard() {
  if (!state.wizardModal || !state.wizardContent) return;
  if (!state.wizard.open) {
    state.wizardModal.classList.add('hidden');
    state.wizardContent.innerHTML = '';
    return;
  }
  state.wizardModal.classList.remove('hidden');
  const stepTitle = getWizardStepTitle(state.wizard.step);
  const total = wizardTotalSteps();
  const isLast = state.wizard.step >= total - 1;
  const nextLabel = isLast ? 'Create device' : 'Next';
  const nextAction = isLast ? 'apply' : 'next';
  const nextDisabled = wizardForwardDisabled();
  const body = renderWizardStepBody();
  state.wizardContent.innerHTML = `
    <div class="dw-wizard">
      <div>
        <h3>${stepTitle}</h3>
        <div class="dw-wizard-note">${getWizardStepNote(state.wizard.step)}</div>
      </div>
      <div class="dw-wizard-body">
        ${body}
      </div>
      <div class="dw-wizard-nav">
        <button data-wizard-action="close">Cancel</button>
        <div class="right">
          <button data-wizard-action="prev" ${state.wizard.step === 0 ? 'disabled' : ''}>Back</button>
          <button data-wizard-action="${nextAction}" ${nextDisabled ? 'disabled' : ''}>${nextLabel}</button>
        </div>
      </div>
    </div>`;
  if (state.wizard.step === total - 1) {
    updateWizardSummary();
  }
}

function renderWizardStepBody() {
  switch (state.wizard.step) {
    case 0:
      return renderWizardTemplateSelection();
    case 1:
      return renderWizardBasics();
    case 2:
      return renderWizardTemplateFields();
    case 3:
      return `<div class="dw-wizard-summary" id="dw_wizard_summary"></div>`;
    default:
      return '';
  }
}

function renderWizardTemplateSelection() {
  const cards = Object.entries(WIZARD_TEMPLATES).map(([id, tpl]) => {
    const active = state.wizard.template === id ? ' selected' : '';
    return `<div class="dw-wizard-template${active}" data-wizard-action="select-template" data-template-id="${id}">
      <strong>${escapeHtml(tpl.label)}</strong>
      <p class="dw-wizard-note">${escapeHtml(tpl.description)}</p>
    </div>`;
  }).join('');
  return `<div class="dw-wizard-templates">${cards}</div>`;
}

function renderWizardBasics() {
  return `
    <div class="dw-wizard-fields">
      <div class="dw-field">
        <label>Device name</label>
        <input data-wizard-field="deviceName" value="${escapeAttr(state.wizard.data.deviceName || '')}" placeholder="Friendly name">
      </div>
      <div class="dw-field">
        <label>Device ID</label>
        <input data-wizard-field="deviceId" value="${escapeAttr(state.wizard.data.deviceId || '')}" placeholder="unique_id">
        <div class="dw-wizard-note">Used internally &mdash; only letters/numbers.</div>
      </div>
    </div>`;
}

function renderWizardTemplateFields() {
  if (!state.wizard.template) {
    return `<div class="dw-wizard-note">Select a template first.</div>`;
  }
  const tpl = WIZARD_TEMPLATES[state.wizard.template];
  if (!tpl?.fields?.length) {
    return `<div class="dw-wizard-note">No additional settings for this template.</div>`;
  }
  const fields = tpl.fields.map((field) => `
    <div class="dw-field">
      <label>${escapeHtml(field.label)}</label>
      <input data-wizard-field="${field.name}" value="${escapeAttr(state.wizard.data[field.name] || '')}" placeholder="${escapeAttr(field.placeholder || '')}">
    </div>`).join('');
  return `<div class="dw-wizard-fields">${fields}</div>`;
}

function updateWizardSummary() {
  const summaryEl = document.getElementById('dw_wizard_summary');
  if (!summaryEl) return;
  const device = buildDeviceFromWizard();
  if (!device) {
    summaryEl.textContent = 'Select template and fill required fields.';
    return;
  }
  summaryEl.textContent = JSON.stringify(device, null, 2);
}

function getWizardStepTitle(step) {
  switch (step) {
    case 0: return 'Choose a template';
    case 1: return 'Basics';
    case 2: return 'Template settings';
    case 3: return 'Summary';
    default: return 'Wizard';
  }
}

function getWizardStepNote(step) {
  switch (step) {
    case 0: return 'Pick a ready-to-go configuration.';
    case 1: return 'Set the device name and identifier.';
    case 2: return 'Customize actions for this template.';
    case 3: return 'Review the generated device before adding.';
    default: return '';
  }
}

function wizardTotalSteps() {
  return 4;
}

function wizardForwardDisabled() {
  if (state.wizard.step === 0) {
    return !state.wizard.template;
  }
  if (state.wizard.step === 1) {
    return !(state.wizard.data.deviceName && state.wizard.data.deviceId);
  }
  return false;
}

function handleWizardClick(ev) {
  if (!state.wizard.open) return;
  if (ev.target === state.wizardModal) {
    closeWizard();
    return;
  }
  const btn = ev.target.closest('[data-wizard-action]');
  if (!btn) return;
  const action = btn.dataset.wizardAction;
  switch (action) {
    case 'close':
      closeWizard();
      break;
    case 'next':
      wizardNext();
      break;
    case 'prev':
      wizardPrev();
      break;
    case 'apply':
      wizardApply();
      break;
    case 'select-template':
      selectWizardTemplate(btn.dataset.templateId);
      break;
    default:
      break;
  }
}

function handleWizardInput(ev) {
  if (!state.wizard.open) return;
  const target = ev.target;
  if (!target?.dataset?.wizardField) return;
  const field = target.dataset.wizardField;
  state.wizard.data[field] = target.value;
  if (field === 'deviceName' && state.wizard.autoId) {
    const slug = slugify(target.value);
    state.wizard.data.deviceId = slug;
    const idInput = state.wizardModal?.querySelector('input[data-wizard-field="deviceId"]');
    if (idInput) idInput.value = slug;
  }
  if (field === 'deviceId') {
    state.wizard.autoId = false;
  }
  if (state.wizard.step === wizardTotalSteps() - 1) {
    updateWizardSummary();
  }
}

function selectWizardTemplate(templateId) {
  if (!templateId || !WIZARD_TEMPLATES[templateId]) {
    return;
  }
  applyWizardTemplate(templateId);
  state.wizard.step = 1;
  renderWizard();
}

function wizardNext() {
  if (wizardForwardDisabled()) {
    return;
  }
  const total = wizardTotalSteps();
  state.wizard.step = Math.min(state.wizard.step + 1, total - 1);
  renderWizard();
}

function wizardPrev() {
  state.wizard.step = Math.max(state.wizard.step - 1, 0);
  renderWizard();
}

function wizardApply() {
  const device = buildDeviceFromWizard();
  if (!device) {
    return;
  }
  ensureModel();
  state.model.devices.push(device);
  state.selectedDevice = state.model.devices.length - 1;
  state.selectedScenario = -1;
  closeWizard();
  renderAll();
  markDirty();
  setStatus('Device added via wizard', '#22c55e');
}

function buildDeviceFromWizard() {
  const tpl = WIZARD_TEMPLATES[state.wizard.template];
  if (!tpl) {
    return null;
  }
  const name = (state.wizard.data.deviceName || tpl.defaults?.deviceName || 'Device').trim();
  const id = (state.wizard.data.deviceId || slugify(name)).trim();
  const base = {
    id: id || slugify(name),
    display_name: name || 'Device',
    topics: [],
    scenarios: [],
  };
  if (typeof tpl.build === 'function') {
    return tpl.build(base, state.wizard.data);
  }
  return base;
}
window.addEventListener('load', init);
})();
