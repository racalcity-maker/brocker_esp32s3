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

