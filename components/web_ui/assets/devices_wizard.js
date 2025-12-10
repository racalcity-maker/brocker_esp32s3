(() => {
const TAB_TYPES = [
  {value: 'audio', label: 'Audio'},
  {value: 'custom', label: 'Custom'},
];
const ACTION_TYPES = ['mqtt_publish','audio_play','audio_stop','set_flag','wait_flags','loop','delay','event','nop'];
const TEMPLATE_TYPES = [
  {value: '', label: 'No template'},
  {value: 'uid_validator', label: 'UID validator'},
  {value: 'signal_hold', label: 'Signal hold'},
];
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
      base.tabs = [];
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
  json: null,
  statusEl: null,
  model: null,
  selectedDevice: -1,
  selectedScenario: -1,
  busy: false,
  dirty: false,
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
      <div class="dw-globals">
        <div class="dw-field">
          <label>Tab limit</label>
          <input type="number" min="1" max="12" id="dw_tab_limit">
        </div>
        <div class="dw-field">
          <label>Schema version</label>
          <input type="number" id="dw_schema" disabled>
        </div>
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
      <div class="dw-json" id="dw_json_preview">No config</div>
      <div class="dw-modal hidden" id="dw_wizard_modal">
        <div class="dw-modal-content" id="dw_wizard_content"></div>
      </div>
    </div>`;

  state.toolbar = document.getElementById('dw_toolbar');
  state.list = document.getElementById('dw_device_list');
  state.detail = document.getElementById('dw_device_detail');
  state.json = document.getElementById('dw_json_preview');
  state.statusEl = document.getElementById('dw_status');
  state.wizardModal = document.getElementById('dw_wizard_modal');
  state.wizardContent = document.getElementById('dw_wizard_content');

  const tabLimit = document.getElementById('dw_tab_limit');
  tabLimit?.addEventListener('input', (ev) => {
    if (!state.model) return;
    const target = ev.target;
    if (!target || typeof target.value === 'undefined') {
      return;
    }
    const val = parseInt(String(target.value), 10);
    if (!isNaN(val)) {
      state.model.tab_limit = Math.max(1, Math.min(12, val));
      markDirty();
    }
  });
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
    }
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

  const runBtn = document.getElementById('device_run_btn');
  runBtn?.addEventListener('click', runScenario);
  const devSelect = document.getElementById('device_run_select');
  devSelect?.addEventListener('change', populateScenarioSelect);
  state.wizardModal?.addEventListener('click', handleWizardClick);
  state.wizardModal?.addEventListener('input', handleWizardInput);
}

function handleDetailClick(ev) {
  const btn = ev.target.closest('[data-action]');
  if (!btn) return;
  const action = btn.dataset.action;
  switch (action) {
    case 'add-tab': addTab(); break;
    case 'remove-tab': removeTab(btn.dataset.index); break;
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
  }
}

function handleDetailInput(ev) {
  const el = ev.target;
  if (!el) return;
  if (el.dataset.deviceField) {
    updateDeviceField(el.dataset.deviceField, el.value);
    return;
  }
  if (el.dataset.tabField) {
    updateTabField(el.dataset.index, el.dataset.tabField, el.value);
    return;
  }
  if (el.dataset.topicField) {
    updateTopicField(el.dataset.index, el.dataset.topicField, el.value);
    return;
  }
  if (el.dataset.scenarioField) {
    updateScenarioField(el.dataset.scenarioField, el.value);
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
      state.selectedDevice = (cfg.devices && cfg.devices.length) ? 0 : -1;
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
  fetch('/api/devices/apply', {
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
      updateRunSelectors();
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
  updateRunSelectors();
}

function renderAll() {
  renderDeviceList();
  renderDeviceDetail();
  renderJson();
  updateToolbar();
  updateGlobals();
  updateRunSelectors();
}

function ensureModel() {
  if (!state.model) {
    state.model = {schema: 1, tab_limit: 12, devices: []};
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
      <div class="dw-field">
        <label>Device ID</label>
        <input data-device-field="id" value="${escapeAttr(dev.id || '')}" placeholder="unique-id">
      </div>
      <div class="dw-field">
        <label>Display name</label>
        <input data-device-field="display_name" value="${escapeAttr(dev.display_name || dev.name || '')}" placeholder="Visible name">
      </div>
    </div>
    ${renderTemplateSection(dev)}
    ${renderTabsSection(dev)}
    ${renderTopicsSection(dev)}
    ${renderScenariosSection(dev)}
  `;
}

function renderTemplateSection(dev) {
  const tplType = dev.template?.type || '';
  const options = TEMPLATE_TYPES.map((tpl) => `<option value="${tpl.value}" ${tpl.value === tplType ? 'selected' : ''}>${tpl.label}</option>`).join('');
  let body = "<div class='dw-empty small'>Template is not set.</div>";
  if (tplType === 'uid_validator') {
    body = renderUidTemplate(dev);
  } else if (tplType === 'signal_hold') {
    body = renderSignalTemplate(dev);
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
      <div class="dw-field"><label>Signal topic</label><input data-template-field="signal" data-subfield="signal_topic" value="${escapeAttr(sig.signal_topic || '')}" placeholder="quest/relay/cmd"></div>
      <div class="dw-field"><label>Payload ON</label><input data-template-field="signal" data-subfield="signal_payload_on" value="${escapeAttr(sig.signal_payload_on || '')}" placeholder="ON"></div>
      <div class="dw-field"><label>Payload OFF</label><input data-template-field="signal" data-subfield="signal_payload_off" value="${escapeAttr(sig.signal_payload_off || '')}" placeholder="OFF"></div>
      <div class="dw-field"><label>Heartbeat topic</label><input data-template-field="signal" data-subfield="heartbeat_topic" value="${escapeAttr(sig.heartbeat_topic || '')}" placeholder="quest/relay/hb"></div>
      <div class="dw-field"><label>Required hold ms</label><input type="number" data-template-field="signal" data-subfield="required_hold_ms" value="${sig.required_hold_ms || 0}"></div>
      <div class="dw-field"><label>Heartbeat timeout ms</label><input type="number" data-template-field="signal" data-subfield="heartbeat_timeout_ms" value="${sig.heartbeat_timeout_ms || 0}"></div>
      <div class="dw-field"><label>Hold track</label><input data-template-field="signal" data-subfield="hold_track" value="${escapeAttr(sig.hold_track || '')}" placeholder="/sdcard/hold.mp3"></div>
      <div class="dw-field"><label>Loop hold track</label><select data-template-field="signal" data-subfield="hold_track_loop"><option value="false" ${sig.hold_track_loop ? '' : 'selected'}>No</option><option value="true" ${sig.hold_track_loop ? 'selected' : ''}>Yes</option></select></div>
      <div class="dw-field"><label>Complete track</label><input data-template-field="signal" data-subfield="complete_track" value="${escapeAttr(sig.complete_track || '')}" placeholder="/sdcard/done.mp3"></div>
    </div>`;
}

function renderTabsSection(dev) {
  const rows = (dev.tabs || []).map((tab, idx) => {
    const options = TAB_TYPES.map(t => `<option value="${t.value}" ${tab.type === t.value ? 'selected' : ''}>${t.label}</option>`).join('');
    return `<tr>
      <td><select data-tab-field="type" data-index="${idx}">${options}</select></td>
      <td><input data-tab-field="label" data-index="${idx}" value="${escapeAttr(tab.label || '')}" placeholder="Label"></td>
      <td><input data-tab-field="extra_payload" data-index="${idx}" value="${escapeAttr(tab.extra_payload || '')}" placeholder="Extra payload"></td>
      <td><button class="danger small" data-action="remove-tab" data-index="${idx}">&times;</button></td>
    </tr>`;
  }).join('');
  return `
    <div class="dw-section">
      <div class="dw-section-head">
        <h4>Tabs</h4>
        <div class="dw-table-actions"><button data-action="add-tab">Add tab</button></div>
      </div>
      <table class="dw-mini-table">
        <thead><tr><th>Type</th><th>Label</th><th>Extra</th><th></th></tr></thead>
        <tbody>${rows || "<tr><td colspan='4' class='muted small'>No tabs</td></tr>"}</tbody>
      </table>
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

function updateDeviceField(field, value) {
  const dev = currentDevice();
  if (!dev) return;
  dev[field] = value;
  markDirty();
}

function updateTabField(indexStr, field, value) {
  const idx = parseInt(indexStr, 10);
  const dev = currentDevice();
  if (!dev || isNaN(idx) || !dev.tabs || !dev.tabs[idx]) return;
  dev.tabs[idx][field] = value;
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
    case 'signal': {
      ensureSignalTemplate(dev);
      if (!dev.template?.signal) return;
      const sub = el.dataset.subfield;
      if (sub === 'required_hold_ms' || sub === 'heartbeat_timeout_ms') {
        dev.template.signal[sub] = parseInt(el.value, 10) || 0;
      } else if (sub === 'hold_track_loop') {
        dev.template.signal[sub] = el.value === 'true';
      } else {
        dev.template.signal[sub] = el.value;
      }
      break;
    }
    default:
      return;
  }
  markDirty();
}

function updateScenarioField(field, value) {
  const scen = currentScenario();
  if (!scen) return;
  scen[field] = value;
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
  if (nextType !== 'uid_validator' && nextType !== 'signal_hold') {
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
    }
  }
  if (nextType === 'uid_validator') {
    ensureUidTemplate(dev);
  } else if (nextType === 'signal_hold') {
    ensureSignalTemplate(dev);
  }
  markDirty();
  renderDeviceDetail();
}

function defaultUidTemplate() {
  return {
    slots: [],
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
    required_hold_ms: 0,
    heartbeat_timeout_ms: 0,
    hold_track: '',
    hold_track_loop: false,
    complete_track: '',
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
}

function ensureSignalTemplate(dev) {
  if (!dev || !dev.template || dev.template.type !== 'signal_hold') {
    return;
  }
  if (!dev.template.signal) {
    dev.template.signal = defaultSignalTemplate();
  }
  const sig = dev.template.signal;
  ['signal_topic','signal_payload_on','signal_payload_off','heartbeat_topic','hold_track','complete_track'].forEach((key) => {
    sig[key] = sig[key] || '';
  });
  sig.required_hold_ms = sig.required_hold_ms || 0;
  sig.heartbeat_timeout_ms = sig.heartbeat_timeout_ms || 0;
  sig.hold_track_loop = !!sig.hold_track_loop;
  sig.signal_on_ms = sig.signal_on_ms || 0;
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
  if (!Array.isArray(dev.scenarios)) {
    dev.scenarios = [];
  }
  dev.scenarios.forEach(normalizeScenario);
}

function normalizeScenario(scen) {
  if (!scen || typeof scen !== 'object') return;
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
    stripTemplateRuntimeFields(snapshot.devices[devIdx]);
  });
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
    display_name: 'New device',
    tabs: [],
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

function addTab() {
  const dev = currentDevice();
  if (!dev) return;
  dev.tabs = dev.tabs || [];
  dev.tabs.push({type: 'custom', label: 'Tab', extra_payload: ''});
  markDirty();
  renderDeviceDetail();
}

function removeTab(indexStr) {
  const idx = parseInt(indexStr, 10);
  const dev = currentDevice();
  if (!dev || isNaN(idx) || !dev.tabs) return;
  dev.tabs.splice(idx, 1);
  markDirty();
  renderDeviceDetail();
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
  const tabLimit = document.getElementById('dw_tab_limit');
  if (tabLimit && state.model) tabLimit.value = state.model.tab_limit ?? 12;
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

function updateRunSelectors() {
  const devSel = document.getElementById('device_run_select');
  const scenSel = document.getElementById('scenario_run_select');
  if (!devSel || !scenSel || !state.model) return;
  devSel.innerHTML = '<option value="">Device</option>';
  const devices = state.model.devices || [];
  if (!devices.length) {
    scenSel.innerHTML = '<option value="">Scenario</option>';
    return;
  }
  devices.forEach((dev, idx) => {
    const opt = document.createElement('option');
    opt.value = idx;
    opt.textContent = dev.display_name || dev.name || dev.id || (`Device ${idx + 1}`);
    devSel.appendChild(opt);
  });
  populateScenarioSelect();
}

let wizardStylesInjected = false;
function injectWizardStyles() {
  if (wizardStylesInjected) return;
  wizardStylesInjected = true;
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

function populateScenarioSelect() {
  const devSel = document.getElementById('device_run_select');
  const scenSel = document.getElementById('scenario_run_select');
  if (!devSel || !scenSel || !state.model) return;
  const idx = parseInt(devSel.value, 10);
  scenSel.innerHTML = '<option value="">Scenario</option>';
  if (isNaN(idx) || !state.model.devices?.[idx]) return;
  (state.model.devices[idx].scenarios || []).forEach((sc, sIdx) => {
    const opt = document.createElement('option');
    opt.value = sIdx;
    opt.textContent = sc.name || sc.id || (`Scenario ${sIdx + 1}`);
    scenSel.appendChild(opt);
  });
}

function runScenario() {
  const devSel = document.getElementById('device_run_select');
  const scenSel = document.getElementById('scenario_run_select');
  const status = document.getElementById('device_run_status');
  if (!devSel || !scenSel || !status) return;
  const devIdx = parseInt(devSel.value, 10);
  const scenIdx = parseInt(scenSel.value, 10);
  if (isNaN(devIdx) || isNaN(scenIdx) || !state.model?.devices?.[devIdx]) {
    status.textContent = 'Select device and scenario';
    status.style.color = '#f87171';
    return;
  }
  const device = state.model.devices[devIdx];
  if (!device) {
    status.textContent = 'Device missing';
    status.style.color = '#f87171';
    return;
  }
  const scenario = device.scenarios?.[scenIdx];
  if (!scenario) {
    status.textContent = 'Scenario missing';
    status.style.color = '#f87171';
    return;
  }
  status.textContent = 'Triggering...';
  status.style.color = '#fbbf24';
  const safeDeviceId = (toSafeString(device.id).trim()) || (toSafeString(device.display_name).trim()) || `device_${devIdx}`;
  const safeScenarioId = (toSafeString(scenario.id).trim()) || (toSafeString(scenario.name).trim()) || `scenario_${scenIdx}`;
  const url = `/api/devices/run?device=${encodeURIComponent(safeDeviceId)}&scenario=${encodeURIComponent(safeScenarioId)}`;
  fetch(url)
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json().catch(() => ({}));
    })
    .then(() => {
      status.textContent = 'Scenario queued';
      status.style.color = '#22c55e';
    })
    .catch(err => {
      status.textContent = 'Failed: ' + err.message;
      status.style.color = '#f87171';
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
    tabs: [],
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
