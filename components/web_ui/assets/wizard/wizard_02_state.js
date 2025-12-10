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

