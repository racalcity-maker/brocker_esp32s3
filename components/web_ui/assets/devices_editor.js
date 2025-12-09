// Visual editor (profiles + basic device editor)
(() => {
  const TAB_OPTIONS = [
    {value: 'audio', label: 'Audio'},
    {value: 'pictures', label: 'Pictures'},
    {value: 'laser', label: 'Laser'},
    {value: 'robot', label: 'Robot'},
    {value: 'custom', label: 'Custom'},
  ];
  const DEVICE_IDS = {
    PICTURES: 'pictures_core',
    LASER: 'laser_core',
  };
  const SCENARIO_IDS = {
    PICTURES_OK: 'pictures_ok',
    PICTURES_FAIL: 'pictures_fail',
    LASER_TRIGGER: 'laser_trigger',
  };
  const DEFAULT_TRACKS = {
    PICTURES_OK: '/sdcard/pictures/ok.mp3',
    PICTURES_FAIL: '/sdcard/pictures/fail.mp3',
    LASER_RELAY: '/sdcard/laser/relayOn.mp3',
  };
  const state = {
    root: null,
    profileList: null,
    deviceList: null,
    detail: null,
    statusEl: null,
    saveBtn: null,
    varList: null,
    varMenuEl: null,
    varMenuTarget: null,
    varMenuAnchor: null,
    profiles: [],
    activeProfile: '',
    model: null,
    devices: [],
    selectedDevice: -1,
    selectedScenario: -1,
    dirty: false,
    variables: [],
    validation: createEmptyValidation(),
    draggingStep: null,
  };

  function initEditor() {
    state.root = document.getElementById('device_editor_root');
    if (!state.root) return;
    state.root.innerHTML = `
      <div class="de-root">
        <div class="de-toolbar">
          <button data-action="refresh">Reload</button>
          <button data-action="create">Add profile</button>
          <button data-action="clone">Clone</button>
          <button data-action="rename">Rename</button>
          <button data-action="delete">Delete</button>
          <button data-action="save" class="primary" disabled>Save changes</button>
          <span class="dw-status" id="de_status">Ready</span>
        </div>
        <div class="de-profiles" id="de_profile_list"></div>
        <div class="de-variables">
          <div class="de-vars-head">
            <span>Variables</span>
            <button data-action="refresh-vars">Reload vars</button>
          </div>
          <div class="de-vars-list" id="de_var_list"><div class="de-empty">No variables loaded</div></div>
        </div>
        <div class="de-main">
          <div class="de-device-list" id="de_device_list"></div>
          <div class="de-detail" id="de_detail_panel">
            <div class="de-empty">Select a profile to load devices.</div>
          </div>
        </div>
      </div>`;
    state.profileList = document.getElementById('de_profile_list');
    state.deviceList = document.getElementById('de_device_list');
    state.detail = document.getElementById('de_detail_panel');
    state.statusEl = document.getElementById('de_status');
    state.saveBtn = state.root.querySelector('[data-action="save"]');
    state.varList = document.getElementById('de_var_list');

    state.root.addEventListener('click', handleToolbarClick);
    state.deviceList.addEventListener('click', handleDeviceClick);
    state.detail.addEventListener('click', handleDetailClick);
    state.detail.addEventListener('input', handleDetailInput);
    state.detail.addEventListener('dragstart', handleDetailDragStart);
    state.detail.addEventListener('dragover', handleDetailDragOver);
    state.detail.addEventListener('drop', handleDetailDrop);
    state.detail.addEventListener('dragend', handleDetailDragEnd);
    state.varMenuEl = document.createElement('div');
    state.varMenuEl.className = 'de-var-menu hidden';
    state.varMenuEl.addEventListener('click', handleVarMenuClick);
    state.root.appendChild(state.varMenuEl);
    document.addEventListener('click', handleGlobalVarMenuClick);
    loadProfiles();
    loadVariables();
  }

  function handleToolbarClick(ev) {
    const btn = ev.target.closest('[data-action]');
    if (!btn) return;
    switch (btn.dataset.action) {
      case 'refresh':
        loadProfiles();
        break;
      case 'create':
        createProfilePrompt();
        break;
      case 'clone':
        cloneProfilePrompt();
        break;
      case 'rename':
        renameProfilePrompt();
        break;
      case 'delete':
        deleteProfilePrompt();
        break;
      case 'save':
        saveProfileConfig();
        break;
      case 'refresh-vars':
        loadVariables();
        break;
      default:
        break;
    }
  }

  function setStatus(text, color) {
    if (!state.statusEl) return;
    state.statusEl.textContent = text;
    if (color) {
      state.statusEl.style.color = color;
    } else {
      state.statusEl.style.color = '#94a3b8';
    }
  }

  function setDirty(flag) {
    state.dirty = !!flag;
    refreshValidation();
  }

  function markDirty() {
    if (!state.dirty) {
      setDirty(true);
    } else {
      refreshValidation();
    }
  }

  function loadProfiles() {
    setStatus('Loading...', '#fbbf24');
    fetch('/api/devices/config')
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then((cfg) => {
        state.profiles = (cfg.profiles || []).map((p) => ({
          id: p.id,
          name: p.name || p.id,
          active: !!p.active,
        }));
        state.activeProfile = cfg.active_profile || (state.profiles[0] && state.profiles[0].id) || '';
        renderProfiles();
        loadProfileConfig(state.activeProfile);
      })
      .catch((err) => {
        console.error(err);
        setStatus('Load failed: ' + err.message, '#f87171');
      });
  }

  function renderProfiles() {
    if (!state.profileList) return;
    state.profileList.innerHTML = '';
    if (!state.profiles.length) {
      state.profileList.innerHTML = '<div class="de-profile">No profiles</div>';
      return;
    }
    state.profiles.forEach((p) => {
      const div = document.createElement('div');
      const active = p.id === state.activeProfile;
      div.className = 'de-profile' + (active ? ' active' : '');
      div.textContent = p.name || p.id;
      div.onclick = () => activateProfile(p.id);
      state.profileList.appendChild(div);
    });
  }

  function loadVariables() {
    if (!state.varList) return;
    state.varList.innerHTML = '<div class="de-empty">Loading...</div>';
    fetch('/api/devices/variables')
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then((vars) => {
        state.variables = Array.isArray(vars) ? vars : [];
        renderVariables();
      })
      .catch((err) => {
        console.error(err);
        state.variables = [];
        renderVariables(`Failed: ${err.message}`);
      });
  }

  function renderVariables(error) {
    if (!state.varList) return;
    if (error) {
      state.varList.innerHTML = `<div class="de-empty">${escapeHtml(error)}</div>`;
      return;
    }
    if (!state.variables.length) {
      state.varList.innerHTML = '<div class="de-empty">No variables defined</div>';
      return;
    }
    state.varList.innerHTML = state.variables.map((v) => {
      return `<div class="de-var" title="${escapeHtml(v.description || '')}" data-var-key="${escapeHtml(v.key)}">
        ${escapeHtml(v.key)}
      </div>`;
    }).join('');
    state.varList.querySelectorAll('.de-var').forEach((el) => {
      el.onclick = () => insertVariable(el.dataset.varKey);
    });
  }

  function loadProfileConfig(profileId) {
    if (!profileId) {
      state.model = null;
      state.devices = [];
      state.selectedDevice = -1;
      state.selectedScenario = -1;
      renderDevices();
      renderDeviceDetail();
      setDirty(false);
      return;
    }
    setStatus('Loading profile...', '#fbbf24');
    fetch(`/api/devices/config?profile=${encodeURIComponent(profileId)}`)
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then((cfg) => {
        state.model = cfg;
        state.devices = cfg.devices || [];
        return hydrateDevicesFromSystem().catch(() => {}).then(() => cfg);
      })
      .then(() => {
        state.selectedDevice = state.devices.length ? 0 : -1;
        state.selectedScenario = 0;
        renderDevices();
        renderDeviceDetail();
        setDirty(false);
      })
      .catch((err) => {
        console.error(err);
        state.model = null;
        state.devices = [];
        state.selectedDevice = -1;
        state.selectedScenario = -1;
        renderDevices();
        renderDeviceDetail();
        setDirty(false);
        setStatus('Profile load failed: ' + err.message, '#f87171');
      });
  }

  function renderDevices() {
    if (!state.deviceList) return;
    if (!state.devices.length) {
      state.deviceList.innerHTML = '<div class="de-empty">No devices</div>';
      return;
    }
    state.deviceList.innerHTML = state.devices.map((dev, idx) => {
      const title = escapeHtml(dev.display_name || dev.name || dev.id || `Device ${idx + 1}`);
      const scenarios = (dev.scenarios || []).length;
      const tabs = (dev.tabs || []).length;
      const active = idx === state.selectedDevice ? ' active' : '';
      const invalid = deviceHasError(idx) ? ' invalid' : '';
      return `<div class="de-device-item${active}${invalid}" data-device-index="${idx}">
        <span class="title">${title}</span>
        <span class="meta">${tabs} tab(s) | ${scenarios} scenario(s)</span>
      </div>`;
    }).join('');
  }

  function renderDeviceDetail() {
    if (!state.detail) return;
    closeVariableMenu();
    const dev = state.devices[state.selectedDevice];
    if (!dev) {
      state.detail.innerHTML = '<div class="de-empty">Select a device.</div>';
      return;
    }
    const scenarios = dev.scenarios || [];
    const deviceIdx = state.selectedDevice;
    if (state.selectedScenario >= scenarios.length) {
      state.selectedScenario = scenarios.length ? 0 : -1;
    }
    const scenario = state.selectedScenario >= 0 ? scenarios[state.selectedScenario] : null;
    const deviceNameClass = buildInputClass(`device.${deviceIdx}.display_name`);
    let html = `
      <div class="de-device-header">
        <label>Display name</label>
        <input class="${deviceNameClass}" data-device-field="display_name" value="${escapeHtml(dev.display_name || '')}" placeholder="Display name">
        <div class="de-meta">ID: ${escapeHtml(dev.id || '')}</div>
      </div>`;
    html += renderValidationSummary();
    html += '<div class="de-device-sections">';
    html += renderTabsSection(dev, deviceIdx);
    html += renderTopicsSection(dev, deviceIdx);
    html += '</div>';
    html += '<div class="de-scenario-list">';
    html += '<div class="de-scenario-actions"><button data-action="add-scenario">Add scenario</button></div>';
    if (!scenarios.length) {
      html += '<div class="de-empty">No scenarios yet.</div>';
    } else {
      scenarios.forEach((sc, idx) => {
        const active = idx === state.selectedScenario ? ' active' : '';
        const invalid = scenarioHasError(deviceIdx, idx) ? ' invalid' : '';
        const name = escapeHtml(sc.name || sc.id || `Scenario ${idx + 1}`);
        html += `<div class="de-scenario-item${active}${invalid}" data-action="select-scenario" data-scenario-index="${idx}">
          <span>${name}</span>
          <button data-action="remove-scenario" data-scenario-index="${idx}">&times;</button>
        </div>`;
      });
    }
    html += '</div>';
    if (scenario) {
      html += renderScenarioDetail(scenario, deviceIdx);
    } else {
      html += '<div class="de-empty">Select or add a scenario.</div>';
    }
    state.detail.innerHTML = html;
  }

  function renderScenarioDetail(scenario, deviceIdx) {
    const idx = state.selectedScenario;
    const scenarioNameClass = buildInputClass(`device.${deviceIdx}.scenario.${idx}.name`);
    let html = `
      <div class="de-scenario-detail">
        <div class="dw-field"><label>Scenario ID</label><input value="${escapeHtml(scenario.id || '')}" readonly></div>
        <div class="dw-field"><label>Scenario name</label>
          <input class="${scenarioNameClass}" data-scenario-field="name" data-scenario-index="${idx}" value="${escapeHtml(scenario.name || '')}">
        </div>`;
    html += '<div class="de-step-toolbar"><button data-action="add-step">Add step</button></div>';
    const steps = scenario.steps || [];
    if (!steps.length) {
      html += '<div class="de-empty">No steps</div>';
    } else {
      html += '<div class="de-step-list">';
      steps.forEach((step, sIdx) => {
        html += renderStepCard(step, sIdx, deviceIdx, idx);
      });
      html += '</div>';
    }
    html += '</div>';
    return html;
  }

  function renderTabsSection(dev, deviceIdx) {
    const tabs = dev.tabs || [];
    const rows = tabs.map((tab, idx) => {
      const options = TAB_OPTIONS.map((opt) => `<option value="${opt.value}" ${tab.type === opt.value ? 'selected' : ''}>${opt.label}</option>`).join('');
      const labelClass = buildInputClass(`device.${deviceIdx}.tab.${idx}.label`);
      const extraClass = buildInputClass(`device.${deviceIdx}.tab.${idx}.extra_payload`);
      return `<tr>
        <td><select data-tab-field="type" data-tab-index="${idx}">${options}</select></td>
        <td><input class="${labelClass}" data-tab-field="label" data-tab-index="${idx}" value="${escapeHtml(tab.label || '')}" placeholder="Label"></td>
        <td><input class="${extraClass}" data-tab-field="extra_payload" data-tab-index="${idx}" value="${escapeHtml(tab.extra_payload || '')}" placeholder="Extra payload"></td>
        <td><button class="danger" data-action="remove-tab" data-tab-index="${idx}">Delete</button></td>
      </tr>`;
    }).join('');
    return `<div class="de-section">
      <div class="de-section-head">
        <span>Tabs (${tabs.length})</span>
        <button data-action="add-tab">Add tab</button>
      </div>
      <table class="de-table">
        <thead><tr><th>Type</th><th>Label</th><th>Extra</th><th></th></tr></thead>
        <tbody>${rows || "<tr><td colspan='4' class='de-empty'>No tabs</td></tr>"}</tbody>
      </table>
    </div>`;
  }

  function renderTopicsSection(dev, deviceIdx) {
    const topics = dev.topics || [];
    const rows = topics.map((topic, idx) => {
      const nameClass = buildInputClass(`device.${deviceIdx}.topic.${idx}.name`);
      const topicClass = buildInputClass(`device.${deviceIdx}.topic.${idx}.topic`);
      return `<tr>
        <td><input class="${nameClass}" data-topic-field="name" data-topic-index="${idx}" value="${escapeHtml(topic.name || '')}" placeholder="Name"></td>
        <td><input class="${topicClass}" data-topic-field="topic" data-topic-index="${idx}" value="${escapeHtml(topic.topic || '')}" placeholder="mqtt/topic"></td>
        <td><button class="danger" data-action="remove-topic" data-topic-index="${idx}">Delete</button></td>
      </tr>`;
    }).join('');
    return `<div class="de-section">
      <div class="de-section-head">
        <span>Topics (${topics.length})</span>
        <button data-action="add-topic">Add topic</button>
      </div>
      <table class="de-table">
        <thead><tr><th>Name</th><th>Topic</th><th></th></tr></thead>
        <tbody>${rows || "<tr><td colspan='3' class='de-empty'>No topics</td></tr>"}</tbody>
      </table>
    </div>`;
  }

  function renderStepCard(step, idx, deviceIdx, scenarioIdx) {
    const typeOptions = ['mqtt_publish', 'audio_play', 'audio_stop', 'set_flag', 'wait_flags', 'delay', 'loop', 'event']
      .map((type) => `<option value="${type}" ${step.type === type ? 'selected' : ''}>${type}</option>`).join('');
    const invalid = stepHasError(deviceIdx, scenarioIdx, idx) ? ' invalid' : '';
    const typeClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.type`);
    const delayClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.delay_ms`);
    return `
      <div class="de-step-card${invalid}" data-step-index="${idx}">
        <div class="de-step-head">
          <div class="de-step-head-left">
            <span class="de-drag-handle" data-step-drag="${idx}" draggable="true" title="Перетащите для изменения порядка"></span>
            <strong>Step ${idx + 1}</strong>
          </div>
          <button data-action="remove-step" data-step-index="${idx}">Delete</button>
        </div>
        <div class="de-step-body">
          <div class="dw-field"><label>Type</label><select class="${typeClass}" data-step-field="type" data-step-index="${idx}">${typeOptions}</select></div>
          <div class="dw-field"><label>Delay ms</label><input class="${delayClass}" type="number" data-step-field="delay_ms" data-step-index="${idx}" value="${step.delay_ms || 0}"></div>
          ${renderStepExtraFields(step, idx, deviceIdx, scenarioIdx)}
        </div>
      </div>`;
  }

  function renderStepExtraFields(step, idx, deviceIdx, scenarioIdx) {
    switch (step.type) {
      case 'mqtt_publish': {
        const topic = escapeHtml(step.data?.mqtt?.topic || '');
        const payload = escapeHtml(step.data?.mqtt?.payload || '');
        const topicClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.mqtt.topic`);
        const payloadClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.mqtt.payload`);
        const topicInput = renderVarInput(topicClass, `data-step-field="data.mqtt.topic" data-step-index="${idx}"`, topic, "mqtt/topic");
        const payloadInput = renderVarInput(payloadClass, `data-step-field="data.mqtt.payload" data-step-index="${idx}"`, payload, "payload");
        return `
          <div class="dw-field"><label>Topic</label>${topicInput}</div>
          <div class="dw-field"><label>Payload</label>${payloadInput}</div>`;
      }
      case 'audio_play': {
        const track = escapeHtml(step.data?.audio?.track || '');
        const trackClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.audio.track`);
        const trackInput = renderVarInput(trackClass, `data-step-field="data.audio.track" data-step-index="${idx}"`, track, "/sdcard/...");
        return `<div class="dw-field"><label>Track</label>${trackInput}</div>`;
      }
      case 'set_flag': {
        const flag = escapeHtml(step.data?.flag?.flag || '');
        const value = step.data?.flag?.value ? 'true' : 'false';
        const flagClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.flag.flag`);
        return `
          <div class="dw-field"><label>Flag</label><input class="${flagClass}" data-step-field="data.flag.flag" data-step-index="${idx}" value="${flag}" placeholder="flag"></div>
          <div class="dw-field"><label>Value</label>
            <select data-step-field="data.flag.value" data-step-index="${idx}">
              <option value="true" ${value === 'true' ? 'selected' : ''}>true</option>
              <option value="false" ${value === 'false' ? 'selected' : ''}>false</option>
            </select>
          </div>`;
      }
      case 'wait_flags': {
        const mode = step.data?.wait_flags?.mode || 'all';
        const timeout = step.data?.wait_flags?.timeout_ms || 0;
        const timeoutClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.wait_flags.timeout_ms`);
        return `
          <div class="dw-field"><label>Mode</label>
            <select data-step-field="data.wait_flags.mode" data-step-index="${idx}">
              <option value="all" ${mode === 'all' ? 'selected' : ''}>all</option>
              <option value="any" ${mode === 'any' ? 'selected' : ''}>any</option>
            </select>
          </div>
          <div class="dw-field"><label>Timeout ms</label><input class="${timeoutClass}" type="number" data-step-field="data.wait_flags.timeout_ms" data-step-index="${idx}" value="${timeout}"></div>`;
      }
      case 'loop': {
        const target = step.data?.loop?.target_step || 0;
        const max = step.data?.loop?.max_iterations || 0;
        const targetClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.loop.target_step`);
        const maxClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.loop.max_iterations`);
        return `
          <div class="dw-field"><label>Target step</label><input class="${targetClass}" type="number" data-step-field="data.loop.target_step" data-step-index="${idx}" value="${target}"></div>
          <div class="dw-field"><label>Max iterations</label><input class="${maxClass}" type="number" data-step-field="data.loop.max_iterations" data-step-index="${idx}" value="${max}"></div>`;
      }
      case 'event': {
        const event = escapeHtml(step.data?.event?.event || '');
        const topic = escapeHtml(step.data?.event?.topic || '');
        const payload = escapeHtml(step.data?.event?.payload || '');
        const eventClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.event.event`);
        const topicClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.event.topic`);
        const payloadClass = buildInputClass(`device.${deviceIdx}.scenario.${scenarioIdx}.step.${idx}.data.event.payload`);
        const eventInput = renderVarInput(eventClass, `data-step-field="data.event.event" data-step-index="${idx}"`, event, "event");
        const topicInput = renderVarInput(topicClass, `data-step-field="data.event.topic" data-step-index="${idx}"`, topic, "optional");
        const payloadInput = renderVarInput(payloadClass, `data-step-field="data.event.payload" data-step-index="${idx}"`, payload, "optional");
        return `
          <div class="dw-field"><label>Event</label>${eventInput}</div>
          <div class="dw-field"><label>Topic</label>${topicInput}</div>
          <div class="dw-field"><label>Payload</label>${payloadInput}</div>`;
      }
      default:
        return '<div class="de-empty">No extra fields</div>';
    }
  }

  function handleDeviceClick(ev) {
    const item = ev.target.closest('[data-device-index]');
    if (!item) return;
    const idx = parseInt(item.dataset.deviceIndex, 10);
    if (Number.isNaN(idx) || idx === state.selectedDevice) return;
    state.selectedDevice = idx;
    state.selectedScenario = 0;
    renderDevices();
    renderDeviceDetail();
  }

  function handleDetailClick(ev) {
    const btn = ev.target.closest('[data-action]');
    if (!btn) return;
    switch (btn.dataset.action) {
      case 'select-scenario':
        selectScenario(parseInt(btn.dataset.scenarioIndex, 10));
        break;
      case 'add-scenario':
        addScenario();
        break;
      case 'remove-scenario':
        removeScenario(parseInt(btn.dataset.scenarioIndex, 10));
        break;
      case 'add-step':
        addStep();
        break;
      case 'remove-step':
        removeStep(parseInt(btn.dataset.stepIndex, 10));
        break;
      case 'add-tab':
        addTab();
        break;
      case 'remove-tab':
        removeTab(parseInt(btn.dataset.tabIndex, 10));
        break;
      case 'add-topic':
        addTopic();
        break;
      case 'remove-topic':
        removeTopic(parseInt(btn.dataset.topicIndex, 10));
        break;
      case 'open-var-menu': {
        ev.stopPropagation();
        const input = btn.previousElementSibling;
        if (input) {
          openVariableMenu(input, btn);
        }
        break;
      }
      default:
        break;
    }
  }

  function handleDetailInput(ev) {
    const target = ev.target;
    if (!target) return;
    if (target.dataset.deviceField) {
      updateDeviceField(target.dataset.deviceField, target.value);
      return;
    }
    if (target.dataset.scenarioField) {
      updateScenarioField(target.dataset.scenarioField, target.value);
      return;
    }
    if (target.dataset.tabField) {
      updateTabField(parseInt(target.dataset.tabIndex, 10), target.dataset.tabField, target.value);
      return;
    }
    if (target.dataset.topicField) {
      updateTopicField(parseInt(target.dataset.topicIndex, 10), target.dataset.topicField, target.value);
      return;
    }
    if (target.dataset.stepField) {
      updateStepField(parseInt(target.dataset.stepIndex, 10), target.dataset.stepField, target.value, target);
    }
  }

  function handleDetailDragStart(ev) {
    const handle = ev.target.closest('[data-step-drag]');
    if (!handle) {
      return;
    }
    const idx = parseInt(handle.dataset.stepDrag, 10);
    if (Number.isNaN(idx)) {
      return;
    }
    state.draggingStep = idx;
    const card = handle.closest('.de-step-card');
    clearDragIndicators(true);
    if (card) {
      card.classList.add('dragging');
    }
    if (ev.dataTransfer) {
      ev.dataTransfer.effectAllowed = 'move';
      ev.dataTransfer.setData('text/plain', String(idx));
    }
  }

  function handleDetailDragOver(ev) {
    if (state.draggingStep === null || state.draggingStep === undefined) {
      return;
    }
    const card = ev.target.closest('.de-step-card');
    const list = ev.target.closest('.de-step-list');
    if (!card && !list) {
      return;
    }
    ev.preventDefault();
    if (ev.dataTransfer) {
      ev.dataTransfer.dropEffect = 'move';
    }
    if (card) {
      clearDragIndicators();
      card.classList.add('drag-over');
    }
  }

  function handleDetailDrop(ev) {
    if (state.draggingStep === null || state.draggingStep === undefined) {
      return;
    }
    const list = state.detail ? state.detail.querySelector('.de-step-list') : null;
    const from = state.draggingStep;
    const card = ev.target.closest('.de-step-card');
    let to = null;
    if (card) {
      const targetIdx = parseInt(card.dataset.stepIndex, 10);
      if (Number.isNaN(targetIdx)) {
        return;
      }
      const rect = card.getBoundingClientRect();
      const before = (ev.clientY - rect.top) < rect.height / 2;
      to = before ? targetIdx : targetIdx + 1;
    } else if (list) {
      to = list.children.length;
    } else {
      return;
    }
    ev.preventDefault();
    reorderStep(from, to);
    state.draggingStep = null;
    clearDragIndicators(true);
  }

  function handleDetailDragEnd() {
    state.draggingStep = null;
    clearDragIndicators(true);
  }

  function updateDeviceField(field, value) {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return;
    dev[field] = value;
    markDirty();
    renderDevices();
  }

  function selectScenario(index) {
    if (Number.isNaN(index)) return;
    state.selectedScenario = index;
    renderDeviceDetail();
  }

  function updateScenarioField(field, value) {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return;
    const scen = (dev.scenarios || [])[state.selectedScenario];
    if (!scen) return;
    scen[field] = value;
    markDirty();
    renderDeviceDetail();
  }

  function updateTabField(index, field, value) {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return;
    dev.tabs = dev.tabs || [];
    if (Number.isNaN(index) || !dev.tabs[index]) return;
    if (field === 'type') {
      dev.tabs[index].type = value;
    } else {
      dev.tabs[index][field] = value;
    }
    markDirty();
    renderDevices();
  }

  function updateTopicField(index, field, value) {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return;
    dev.topics = dev.topics || [];
    if (Number.isNaN(index) || !dev.topics[index]) return;
    dev.topics[index][field] = value;
    markDirty();
    renderDevices();
  }

  function addScenario() {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return;
    dev.scenarios = dev.scenarios || [];
    const scenario = createEmptyScenario();
    dev.scenarios.push(scenario);
    state.selectedScenario = dev.scenarios.length - 1;
    markDirty();
    renderDeviceDetail();
    renderDevices();
  }

  function addTab() {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return;
    dev.tabs = dev.tabs || [];
    dev.tabs.push(createDefaultTab());
    markDirty();
    renderDevices();
    renderDeviceDetail();
  }

  function removeTab(index) {
    const dev = state.devices[state.selectedDevice];
    if (!dev || Number.isNaN(index) || !dev.tabs) return;
    dev.tabs.splice(index, 1);
    markDirty();
    renderDevices();
    renderDeviceDetail();
  }

  function addTopic() {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return;
    dev.topics = dev.topics || [];
    dev.topics.push(createDefaultTopic());
    markDirty();
    renderDevices();
    renderDeviceDetail();
  }

  function removeTopic(index) {
    const dev = state.devices[state.selectedDevice];
    if (!dev || Number.isNaN(index) || !dev.topics) return;
    dev.topics.splice(index, 1);
    markDirty();
    renderDevices();
    renderDeviceDetail();
  }

  function removeScenario(index) {
    const dev = state.devices[state.selectedDevice];
    if (!dev || !dev.scenarios || Number.isNaN(index)) return;
    dev.scenarios.splice(index, 1);
    state.selectedScenario = Math.min(state.selectedScenario, dev.scenarios.length - 1);
    markDirty();
    renderDeviceDetail();
    renderDevices();
  }

  function addStep() {
    const scen = getCurrentScenario();
    if (!scen) return;
    scen.steps = scen.steps || [];
    scen.steps.push(createDefaultStep());
    markDirty();
    renderDeviceDetail();
  }

  function removeStep(index) {
    const scen = getCurrentScenario();
    if (!scen || Number.isNaN(index)) return;
    scen.steps.splice(index, 1);
    markDirty();
    renderDeviceDetail();
  }

  function reorderStep(from, to) {
    const scen = getCurrentScenario();
    if (!scen || !Array.isArray(scen.steps)) {
      return;
    }
    const steps = scen.steps;
    if (Number.isNaN(from) || Number.isNaN(to)) {
      return;
    }
    if (from < 0 || from >= steps.length) {
      return;
    }
    let target = to;
    if (target < 0) {
      target = 0;
    }
    if (target > steps.length) {
      target = steps.length;
    }
    if (target === from || target === from + 1) {
      return;
    }
    const [step] = steps.splice(from, 1);
    if (!step) {
      return;
    }
    let insertIndex = target;
    if (insertIndex > steps.length) {
      insertIndex = steps.length;
    }
    if (insertIndex < 0) {
      insertIndex = 0;
    }
    if (from < target) {
      insertIndex -= 1;
    }
    if (insertIndex < 0) {
      insertIndex = 0;
    }
    steps.splice(insertIndex, 0, step);
    markDirty();
    renderDeviceDetail();
  }

  function clearDragIndicators(removeDragging = false) {
    if (!state.detail) {
      return;
    }
    const selector = removeDragging ? '.de-step-card.drag-over, .de-step-card.dragging' : '.de-step-card.drag-over';
    const cards = state.detail.querySelectorAll(selector);
    cards.forEach((card) => {
      card.classList.remove('drag-over');
      if (removeDragging) {
        card.classList.remove('dragging');
      }
    });
  }

  function updateStepField(stepIdx, path, value, target) {
    const scen = getCurrentScenario();
    if (!scen || Number.isNaN(stepIdx)) return;
    const step = (scen.steps || [])[stepIdx];
    if (!step) return;
    if (path === 'type') {
      step.type = value;
      if (value === 'mqtt_publish' && !step.data?.mqtt) step.data = {mqtt: {topic: '', payload: ''}};
      if (value === 'audio_play' && !step.data?.audio) step.data = {audio: {track: ''}};
      if (value === 'set_flag' && !step.data?.flag) step.data = {flag: {flag: '', value: true}};
      if (value === 'wait_flags' && !step.data?.wait_flags) {
        step.data = {wait_flags: {mode: 'all', timeout_ms: 0}};
      }
      if (value === 'loop' && !step.data?.loop) {
        step.data = {loop: {target_step: 0, max_iterations: 0}};
      }
      if (value === 'event' && !step.data?.event) {
        step.data = {event: {event: '', topic: '', payload: ''}};
      }
      markDirty();
      renderDeviceDetail();
      return;
    }
    if (path === 'delay_ms') {
      step.delay_ms = parseInt(value, 10) || 0;
      markDirty();
      return;
    }
    const segments = path.split('.');
    let cursor = step;
    for (let i = 0; i < segments.length - 1; i++) {
      const key = segments[i];
      cursor[key] = cursor[key] || {};
      cursor = cursor[key];
    }
    const last = segments[segments.length - 1];
    if (target?.type === 'number') {
      cursor[last] = parseInt(value, 10) || 0;
    } else if (value === 'true' || value === 'false') {
      cursor[last] = value === 'true';
    } else {
      cursor[last] = value;
    }
    markDirty();
  }

  function createEmptyScenario() {
    return {
      id: `scenario_${Date.now().toString(16)}`,
      name: 'Scenario',
      steps: [createDefaultStep()],
    };
  }

  function createDefaultStep() {
    return {
      type: 'mqtt_publish',
      delay_ms: 0,
      data: {mqtt: {topic: '', payload: ''}},
    };
  }

  function createDefaultTab() {
    return {
      type: 'custom',
      label: 'Tab',
      extra_payload: '',
    };
  }

  function createDefaultTopic() {
    return {
      name: 'topic',
      topic: '',
    };
  }

  function getCurrentScenario() {
    const dev = state.devices[state.selectedDevice];
    if (!dev) return null;
    return (dev.scenarios || [])[state.selectedScenario];
  }

  function createProfilePrompt(cloneId) {
    const id = prompt('New profile id:', `profile_${Date.now().toString(16)}`);
    if (!id) return;
    const name = prompt('Display name (optional):', id) || id;
    let url = `/api/devices/profile/create?id=${encodeURIComponent(id)}&name=${encodeURIComponent(name)}`;
    if (cloneId) {
      url += `&clone=${encodeURIComponent(cloneId)}`;
    }
    setStatus('Creating profile...', '#fbbf24');
    fetch(url, {method: 'POST'})
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json().catch(() => ({}));
      })
      .then(() => loadProfiles())
      .catch((err) => setStatus('Create failed: ' + err.message, '#f87171'));
  }

  function cloneProfilePrompt() {
    if (!state.activeProfile) {
      alert('Select a profile first');
      return;
    }
    createProfilePrompt(state.activeProfile);
  }

  function renameProfilePrompt() {
    if (!state.activeProfile) {
      alert('Select a profile first');
      return;
    }
    const name = prompt('New name for profile:', state.activeProfile);
    if (!name) return;
    setStatus('Renaming profile...', '#fbbf24');
    fetch(`/api/devices/profile/rename?id=${encodeURIComponent(state.activeProfile)}&name=${encodeURIComponent(name)}`, {method: 'POST'})
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json().catch(() => ({}));
      })
      .then(() => loadProfiles())
      .catch((err) => setStatus('Rename failed: ' + err.message, '#f87171'));
  }

  function deleteProfilePrompt() {
    if (!state.activeProfile) {
      alert('Select a profile first');
      return;
    }
    if (!confirm(`Delete profile ${state.activeProfile}?`)) return;
    setStatus('Deleting profile...', '#fbbf24');
    fetch(`/api/devices/profile/delete?id=${encodeURIComponent(state.activeProfile)}`, {method: 'POST'})
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json().catch(() => ({}));
      })
      .then(() => loadProfiles())
      .catch((err) => setStatus('Delete failed: ' + err.message, '#f87171'));
  }

  function activateProfile(id) {
    if (!id || id === state.activeProfile) return;
    setStatus('Switching profile...', '#fbbf24');
    fetch(`/api/devices/profile/activate?id=${encodeURIComponent(id)}`, {method: 'POST'})
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json().catch(() => ({}));
      })
      .then(() => {
        state.activeProfile = id;
        renderProfiles();
        loadProfileConfig(id);
      })
      .catch((err) => setStatus('Activate failed: ' + err.message, '#f87171'));
  }

  function saveProfileConfig() {
    if (!state.model || !state.activeProfile || !state.dirty) return;
    setStatus('Saving...', '#fbbf24');
    fetch(`/api/devices/apply?profile=${encodeURIComponent(state.activeProfile)}`, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(state.model),
    })
      .then((r) => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json().catch(() => ({}));
      })
      .then(() => {
        setDirty(false);
        setStatus('Saved', '#22c55e');
      })
      .catch((err) => setStatus('Save failed: ' + err.message, '#f87171'));
  }

  function refreshValidation() {
    state.validation = validateDevices(state.devices);
    updateSaveState();
  }

  function updateSaveState() {
    const hasErrors = validationIsActive() && !!(state.validation && state.validation.messages.length);
    if (state.saveBtn) {
      state.saveBtn.disabled = !state.dirty || hasErrors;
    }
    if (hasErrors) {
      const count = state.validation.messages.length;
      const label = count === 1 ? 'error' : 'errors';
      setStatus(`Fix ${count} ${label}`, '#f87171');
    } else if (state.dirty) {
      setStatus('Unsaved changes', '#fbbf24');
    } else if (state.model) {
      setStatus('Profile loaded', '#22c55e');
    } else {
      setStatus('Ready');
    }
  }

  function renderValidationSummary() {
    if (!validationIsActive() || !state.validation || state.validation.messages.length === 0) {
      return '';
    }
    const focus = state.selectedDevice;
    const scoped = state.validation.messages.filter((msg) => msg.deviceIdx === focus);
    const list = (scoped.length ? scoped : state.validation.messages).slice(0, 5);
    const remaining = (scoped.length ? scoped : state.validation.messages).length - list.length;
    const items = list.map((msg) => `<li>${escapeHtml(msg.text)}</li>`).join('');
    const more = remaining > 0 ? `<li>...and ${remaining} more issue(s)</li>` : '';
    return `<div class="de-alert"><strong>Validation errors</strong><ul>${items}${more}</ul></div>`;
  }

  function validateDevices(devices) {
    const result = createEmptyValidation();
    if (!Array.isArray(devices)) {
      return result;
    }
    devices.forEach((dev, deviceIdx) => {
      if (!dev) {
        return;
      }
      const deviceLabel = (dev.display_name || dev.name || dev.id || '').trim() || `Device ${deviceIdx + 1}`;
      if ((!dev.display_name || !dev.display_name.trim()) && (!dev.id || !dev.id.trim())) {
        addValidationError(result, deviceIdx, null, null, `device.${deviceIdx}.display_name`, `${deviceLabel}: device needs an identifier`);
      }
      (dev.tabs || []).forEach((tab, tabIdx) => {
        if (!tab || !tab.label || !tab.label.trim()) {
          addValidationError(result, deviceIdx, null, null, `device.${deviceIdx}.tab.${tabIdx}.label`, `${deviceLabel}: tab #${tabIdx + 1} needs a label`);
        }
      });
      (dev.topics || []).forEach((topic, topicIdx) => {
        if (!topic || !topic.name || !topic.name.trim()) {
          addValidationError(result, deviceIdx, null, null, `device.${deviceIdx}.topic.${topicIdx}.name`, `${deviceLabel}: topic #${topicIdx + 1} needs a name`);
        }
        if (!topic || !topic.topic || !topic.topic.trim()) {
          addValidationError(result, deviceIdx, null, null, `device.${deviceIdx}.topic.${topicIdx}.topic`, `${deviceLabel}: topic #${topicIdx + 1} needs an MQTT topic`);
        }
      });
      const scenarios = dev.scenarios || [];
      scenarios.forEach((scenario, scenarioIdx) => {
        if (!scenario) {
          return;
        }
        const scenarioLabel = (scenario.name || scenario.id || '').trim() || `Scenario ${scenarioIdx + 1}`;
        if ((!scenario.name || !scenario.name.trim()) && (!scenario.id || !scenario.id.trim())) {
          addValidationError(result, deviceIdx, scenarioIdx, null, `device.${deviceIdx}.scenario.${scenarioIdx}.name`, `${deviceLabel}: scenario #${scenarioIdx + 1} needs a name or id`);
        }
        const steps = scenario.steps || [];
        steps.forEach((step, stepIdx) => {
          validateStep(result, step, deviceIdx, scenarioIdx, stepIdx, deviceLabel, scenarioLabel);
        });
      });
    });
    return result;
  }

  function validateStep(result, step, deviceIdx, scenarioIdx, stepIdx, deviceLabel, scenarioLabel) {
    if (!step) {
      addValidationError(result, deviceIdx, scenarioIdx, stepIdx, null, `${deviceLabel}/${scenarioLabel}: step ${stepIdx + 1} is empty`);
      return;
    }
    const base = `device.${deviceIdx}.scenario.${scenarioIdx}.step.${stepIdx}`;
    if (!step.type) {
      addValidationError(result, deviceIdx, scenarioIdx, stepIdx, `${base}.type`, `${deviceLabel}/${scenarioLabel}: step ${stepIdx + 1} needs a type`);
      return;
    }
    switch (step.type) {
      case 'mqtt_publish': {
        const topic = (step.data?.mqtt?.topic || '').trim();
        if (!topic) {
          addValidationError(result, deviceIdx, scenarioIdx, stepIdx, `${base}.data.mqtt.topic`, `${deviceLabel}/${scenarioLabel}: step ${stepIdx + 1} missing MQTT topic`);
        }
        break;
      }
      case 'audio_play': {
        const track = (step.data?.audio?.track || '').trim();
        if (!track) {
          addValidationError(result, deviceIdx, scenarioIdx, stepIdx, `${base}.data.audio.track`, `${deviceLabel}/${scenarioLabel}: step ${stepIdx + 1} missing audio track`);
        }
        break;
      }
      case 'set_flag': {
        const flag = (step.data?.flag?.flag || '').trim();
        if (!flag) {
          addValidationError(result, deviceIdx, scenarioIdx, stepIdx, `${base}.data.flag.flag`, `${deviceLabel}/${scenarioLabel}: step ${stepIdx + 1} needs a flag name`);
        }
        break;
      }
      case 'event': {
        const ev = (step.data?.event?.event || '').trim();
        if (!ev) {
          addValidationError(result, deviceIdx, scenarioIdx, stepIdx, `${base}.data.event.event`, `${deviceLabel}/${scenarioLabel}: step ${stepIdx + 1} needs an event name`);
        }
        break;
      }
      default:
        break;
    }
  }

  function addValidationError(result, deviceIdx, scenarioIdx, stepIdx, path, message) {
    if (!result) {
      return;
    }
    if (path) {
      result.fields.add(path);
    }
    if (typeof deviceIdx === 'number' && deviceIdx >= 0) {
      result.deviceErrors.add(deviceIdx);
    }
    if (typeof scenarioIdx === 'number' && scenarioIdx >= 0) {
      result.scenarioErrors.add(`${deviceIdx}:${scenarioIdx}`);
    }
    if (typeof stepIdx === 'number' && stepIdx >= 0) {
      result.stepErrors.add(`${deviceIdx}:${scenarioIdx}:${stepIdx}`);
    }
    if (message) {
      result.messages.push({
        text: message,
        deviceIdx,
        scenarioIdx,
        stepIdx,
      });
    }
  }

  function createEmptyValidation() {
    return {
      fields: new Set(),
      deviceErrors: new Set(),
      scenarioErrors: new Set(),
      stepErrors: new Set(),
      messages: [],
    };
  }

  function deviceHasError(idx) {
    if (!validationIsActive()) {
      return false;
    }
    return !!(state.validation && state.validation.deviceErrors && state.validation.deviceErrors.has(idx));
  }

  function scenarioHasError(deviceIdx, scenarioIdx) {
    const key = `${deviceIdx}:${scenarioIdx}`;
    if (!validationIsActive()) {
      return false;
    }
    return !!(state.validation && state.validation.scenarioErrors && state.validation.scenarioErrors.has(key));
  }

  function stepHasError(deviceIdx, scenarioIdx, stepIdx) {
    const key = `${deviceIdx}:${scenarioIdx}:${stepIdx}`;
    if (!validationIsActive()) {
      return false;
    }
    return !!(state.validation && state.validation.stepErrors && state.validation.stepErrors.has(key));
  }

  function hasFieldError(path) {
    if (!validationIsActive()) {
      return false;
    }
    return !!(path && state.validation && state.validation.fields && state.validation.fields.has(path));
  }

  function buildInputClass(path, baseClass = '') {
    const classes = [];
    if (baseClass) {
      classes.push(baseClass);
    }
    if (hasFieldError(path)) {
      classes.push('de-input-error');
    }
    return classes.join(' ').trim();
  }

  function validationIsActive() {
    return !!state.dirty;
  }

  function hydrateDevicesFromSystem() {
    const tasks = [];
    if (getDeviceById(DEVICE_IDS.PICTURES)) {
      tasks.push(fetch('/api/pictures/config')
        .then((r) => {
          if (!r.ok) throw new Error('pictures');
          return r.json();
        })
        .then((cfg) => applyPicturesDefaults(cfg))
        .catch(() => applyPicturesDefaults(null)));
    }
    if (getDeviceById(DEVICE_IDS.LASER)) {
      tasks.push(fetch('/api/laser/config')
        .then((r) => {
          if (!r.ok) throw new Error('laser');
          return r.json();
        })
        .then((cfg) => applyLaserDefaults(cfg))
        .catch(() => applyLaserDefaults(null)));
    }
    if (!tasks.length) {
      return Promise.resolve();
    }
    return Promise.all(tasks);
  }

  function applyPicturesDefaults(cfg) {
    const dev = getDeviceById(DEVICE_IDS.PICTURES);
    if (!dev) return;
    const okTrack = (cfg && cfg.ok) || DEFAULT_TRACKS.PICTURES_OK;
    const failTrack = (cfg && cfg.fail) || DEFAULT_TRACKS.PICTURES_FAIL;
    ensureScenarioDefaults(dev, SCENARIO_IDS.PICTURES_OK, [
      {
        index: 0,
        type: 'mqtt_publish',
        fields: {
          'data.mqtt.topic': 'pictures/lightsgreen',
          'data.mqtt.payload': 'ok',
        },
      },
      {
        index: 1,
        type: 'mqtt_publish',
        fields: {
          'data.mqtt.topic': 'robot/speak',
          'data.mqtt.payload': okTrack,
        },
      },
      {
        index: 2,
        type: 'audio_play',
        fields: {
          'data.audio.track': okTrack,
        },
      },
    ]);
    ensureScenarioDefaults(dev, SCENARIO_IDS.PICTURES_FAIL, [
      {
        index: 0,
        type: 'mqtt_publish',
        fields: {
          'data.mqtt.topic': 'pictures/lightsred',
          'data.mqtt.payload': 'fail',
        },
      },
      {
        index: 1,
        type: 'mqtt_publish',
        fields: {
          'data.mqtt.topic': 'robot/speak',
          'data.mqtt.payload': failTrack,
        },
      },
      {
        index: 2,
        type: 'audio_play',
        fields: {
          'data.audio.track': failTrack,
        },
      },
    ]);
  }

  function applyLaserDefaults(cfg) {
    const dev = getDeviceById(DEVICE_IDS.LASER);
    if (!dev) return;
    const relayTrack = (cfg && cfg.relay) || DEFAULT_TRACKS.LASER_RELAY;
    ensureScenarioDefaults(dev, SCENARIO_IDS.LASER_TRIGGER, [
      {
        index: 0,
        type: 'mqtt_publish',
        fields: {
          'data.mqtt.topic': 'relay/relayOn',
          'data.mqtt.payload': 'on',
        },
      },
      {
        index: 1,
        type: 'mqtt_publish',
        fields: {
          'data.mqtt.topic': 'robot/laser/relayOn.mp3',
          'data.mqtt.payload': relayTrack,
        },
      },
      {
        index: 2,
        type: 'audio_play',
        fields: {
          'data.audio.track': relayTrack,
        },
      },
    ]);
  }

  function ensureScenarioDefaults(device, scenarioId, defaults) {
    if (!device || !Array.isArray(device.scenarios)) {
      return;
    }
    const scenario = findScenarioById(device, scenarioId);
    if (!scenario || !scenario.steps) {
      return;
    }
    defaults.forEach((def) => {
      const step = scenario.steps[def.index];
      if (!step) {
        return;
      }
      if (def.type && !step.type) {
        step.type = def.type;
      }
      if (def.fields) {
        Object.keys(def.fields).forEach((path) => {
          setStepFieldIfEmpty(step, path, def.fields[path]);
        });
      }
    });
  }

  function getDeviceById(id) {
    if (!id || !Array.isArray(state.devices)) {
      return null;
    }
    return state.devices.find((dev) => (dev.id && dev.id === id) || (dev.display_name && dev.display_name === id)) || null;
  }

  function findScenarioById(device, scenarioId) {
    if (!device || !Array.isArray(device.scenarios)) {
      return null;
    }
    return device.scenarios.find((sc) => (sc.id && sc.id === scenarioId) || (sc.name && sc.name === scenarioId)) || null;
  }

  function setStepFieldIfEmpty(step, path, value) {
    if (!step || !path) {
      return;
    }
    const segments = path.split('.');
    let cursor = step;
    for (let i = 0; i < segments.length - 1; i++) {
      const key = segments[i];
      if (cursor[key] === undefined || cursor[key] === null || typeof cursor[key] !== 'object') {
        cursor[key] = {};
      }
      cursor = cursor[key];
    }
    const last = segments[segments.length - 1];
    const current = cursor[last];
    if (current !== undefined && current !== null && current !== '') {
      return;
    }
    cursor[last] = value;
  }

  function renderVarInput(inputClass, attrs, value, placeholder) {
    return `<div class="de-input-wrapper">
      <input class="${inputClass}" ${attrs} value="${value}" placeholder="${placeholder}">
      <button type="button" class="de-var-inline-btn" data-action="open-var-menu" title="Вставить переменную">&#123;&#125;</button>
    </div>`;
  }

  function openVariableMenu(input, anchor) {
    if (!state.varMenuEl || !state.root) {
      return;
    }
    if (state.varMenuTarget === input && state.varMenuAnchor === anchor && !state.varMenuEl.classList.contains('hidden')) {
      closeVariableMenu();
      return;
    }
    if (!state.variables || !state.variables.length) {
      setStatus('Нет доступных переменных', '#fbbf24');
      return;
    }
    state.varMenuTarget = input;
    state.varMenuAnchor = anchor;
    state.varMenuEl.innerHTML = state.variables.map((v) => `<div class="de-var-menu-item" data-var-key="${escapeHtml(v.key)}">
      <strong>${escapeHtml(v.key)}</strong>
      <span>${escapeHtml(v.description || '')}</span>
    </div>`).join('');
    const rootRect = state.root.getBoundingClientRect();
    const anchorRect = anchor.getBoundingClientRect();
    state.varMenuEl.style.left = `${anchorRect.left - rootRect.left}px`;
    state.varMenuEl.style.top = `${anchorRect.bottom - rootRect.top + 6}px`;
    state.varMenuEl.classList.remove('hidden');
  }

  function closeVariableMenu() {
    if (state.varMenuEl) {
      state.varMenuEl.classList.add('hidden');
    }
    state.varMenuTarget = null;
    state.varMenuAnchor = null;
  }

  function handleVarMenuClick(ev) {
    const item = ev.target.closest('.de-var-menu-item');
    if (!item) {
      return;
    }
    ev.stopPropagation();
    const key = item.dataset.varKey;
    if (!key) {
      return;
    }
    insertVariableIntoInput(state.varMenuTarget, key);
    closeVariableMenu();
  }

  function handleGlobalVarMenuClick(ev) {
    if (!state.varMenuEl || state.varMenuEl.classList.contains('hidden')) {
      return;
    }
    if (state.varMenuEl.contains(ev.target)) {
      return;
    }
    if (state.varMenuAnchor && state.varMenuAnchor.contains(ev.target)) {
      return;
    }
    closeVariableMenu();
  }

  function insertVariableIntoInput(input, key) {
    if (!input || !key) {
      return;
    }
    const token = `{{${key}}}`;
    const start = input.selectionStart ?? input.value.length;
    const end = input.selectionEnd ?? start;
    const value = input.value || '';
    input.value = value.slice(0, start) + token + value.slice(end);
    const pos = start + token.length;
    input.selectionStart = pos;
    input.selectionEnd = pos;
    const event = new Event('input', {bubbles: true});
    input.dispatchEvent(event);
    input.focus();
  }

  function escapeHtml(text) {
    if (text === null || text === undefined) return '';
    return String(text).replace(/[&<>"']/g, (c) => ({
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;',
    }[c] || c));
  }

  window.addEventListener('load', initEditor);
})();

function insertVariable(key) {
  if (!key) return;
  const el = document.activeElement;
  if (!el || (el.tagName !== 'INPUT' && el.tagName !== 'TEXTAREA')) {
    alert(`Focus an input to insert {{${key}}}`);
    return;
  }
  const token = `{{${key}}}`;
  const start = el.selectionStart ?? el.value.length;
  const end = el.selectionEnd ?? start;
  const value = el.value || '';
  el.value = value.slice(0, start) + token + value.slice(end);
  el.selectionStart = el.selectionEnd = start + token.length;
  const event = new Event('input', {bubbles: true});
  el.dispatchEvent(event);
}
