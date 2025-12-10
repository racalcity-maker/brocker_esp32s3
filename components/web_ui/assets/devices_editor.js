(() => {
  const LIMITS = {
    devices: 12,
    uidSlots: 8,
  };

  const state = {
    root: null,
    sidebar: null,
    detail: null,
    statusEl: null,
    saveBtn: null,
    profiles: [],
    activeProfile: '',
    devices: [],
    model: null,
    selectedDevice: -1,
    dirty: false,
    templates: [
      {id: 'uid_validator', label: 'UID validator'},
      {id: 'signal_hold', label: 'Signal hold'},
    ],
  };

  function init() {
    state.root = document.getElementById('device_editor_root');
    if (!state.root) {
      return;
    }
    state.root.innerHTML = [
      "<div class='dx-root'>",
      "  <div class='dx-toolbar'>",
      "    <div class='dx-actions'>",
      "      <button data-action='reload'>Reload</button>",
      "      <button data-action='profile-add'>Add profile</button>",
      "      <button data-action='profile-clone'>Clone</button>",
      "      <button data-action='profile-rename'>Rename</button>",
      "      <button data-action='profile-delete'>Delete</button>",
      "    </div>",
      "    <div class='dx-right'>",
      "      <button class='primary' data-action='save' disabled>Save changes</button>",
      "      <span id='dx_status' class='dx-status'>Ready</span>",
      "    </div>",
      "  </div>",
      "  <div class='dx-body'>",
      "    <div class='dx-sidebar' id='dx_sidebar'></div>",
      "    <div class='dx-detail' id='dx_detail'><div class='dx-empty'>Select a device.</div></div>",
      "  </div>",
      "</div>",
    ].join('');
    state.sidebar = document.getElementById('dx_sidebar');
    state.detail = document.getElementById('dx_detail');
    state.statusEl = document.getElementById('dx_status');
    state.saveBtn = state.root.querySelector('[data-action=\"save\"]');
    state.root.addEventListener('click', handleClick);
    state.root.addEventListener('input', handleInput);
    loadProfiles();
  }

  function setStatus(text, mode) {
    if (!state.statusEl) {
      return;
    }
    state.statusEl.textContent = text;
    state.statusEl.dataset.mode = mode || '';
  }

  function handleClick(ev) {
    const button = ev.target.closest('[data-action]');
    if (button) {
      switch (button.dataset.action) {
        case 'reload':
          loadProfiles();
          break;
        case 'profile-add':
          createProfile();
          break;
        case 'profile-clone':
          cloneProfile();
          break;
        case 'profile-rename':
          renameProfile();
          break;
        case 'profile-delete':
          deleteProfile();
          break;
        case 'save':
          saveProfile();
          break;
        case 'device-add':
          addDevice();
          break;
        case 'device-remove':
          removeDevice();
          break;
        case 'slot-add':
          addSlot();
          break;
        case 'slot-remove':
          removeSlot(parseInt(button.dataset.index, 10));
          break;
        default:
          break;
      }
      return;
    }
    const deviceItem = ev.target.closest('[data-device-index]');
    if (deviceItem) {
      selectDevice(parseInt(deviceItem.dataset.deviceIndex, 10));
      return;
    }
    const profileChip = ev.target.closest('[data-profile-id]');
    if (profileChip) {
      activateProfile(profileChip.dataset.profileId);
    }
  }

  function handleInput(ev) {
    const target = ev.target;
    if (!target) {
      return;
    }
    const dev = state.devices[state.selectedDevice];
    if (!dev) {
      return;
    }
    if (target.dataset.field === 'display_name') {
      dev.display_name = target.value;
    } else if (target.dataset.field === 'id') {
      dev.id = target.value;
    } else if (target.dataset.field === 'template') {
      assignTemplate(dev, target.value);
    } else if (target.dataset.field === 'uid-slot') {
      const idx = parseInt(target.dataset.index, 10);
      const field = target.dataset.subfield;
      dev.template.uid.slots[idx][field] = target.value;
    } else if (target.dataset.field === 'uid-values') {
      const idx = parseInt(target.dataset.index, 10);
      dev.template.uid.slots[idx].values = target.value.split(',').map((v) => v.trim()).filter(Boolean);
    } else if (target.dataset.field === 'uid-action') {
      dev.template.uid[target.dataset.subfield] = target.value;
    } else if (target.dataset.field === 'signal') {
      dev.template.signal[target.dataset.subfield] = target.value;
    }
    markDirty();
    renderSidebar();
  }

  function loadProfiles() {
    setStatus('Loading...', 'info');
    fetch('/api/devices/config')
      .then((r) => {
        if (!r.ok) {
          throw new Error('HTTP ' + r.status);
        }
        return r.json();
      })
      .then((cfg) => {
        state.model = cfg;
        state.profiles = cfg.profiles || [];
        state.activeProfile = cfg.active_profile || (state.profiles[0] && state.profiles[0].id) || '';
        state.devices = cfg.devices || [];
        state.selectedDevice = state.devices.length ? 0 : -1;
        state.dirty = false;
        renderSidebar();
        renderDetail();
        updateSaveState();
        setStatus('Profile loaded', 'success');
      })
      .catch((err) => {
        console.error(err);
        setStatus('Load failed: ' + err.message, 'error');
      });
  }

  function renderSidebar() {
    if (!state.sidebar) {
      return;
    }
    const rows = [];
    rows.push("<div class='dx-profile-bar'>");
    rows.push("<div class='dx-profile-list'>");
    if (state.profiles.length) {
      state.profiles.forEach((profile) => {
        const active = profile.id === state.activeProfile ? ' active' : '';
        rows.push("<div class='dx-profile-chip" + active + "' data-profile-id='" +
          profile.id + "'>" + escapeHtml(profile.name || profile.id) + '</div>');
      });
    } else {
      rows.push("<div class='dx-empty'>No profiles</div>");
    }
    rows.push('</div>');
    rows.push("<button data-action='device-add'>Add device</button>");
    rows.push('</div>');
    if (!state.devices.length) {
      rows.push("<div class='dx-empty'>No devices configured.</div>");
    } else {
      state.devices.forEach((dev, idx) => {
        const active = idx === state.selectedDevice ? ' active' : '';
        const title = escapeHtml(dev.display_name || dev.id || ('Device ' + (idx + 1)));
        const template = dev.template ? dev.template.type : 'none';
        rows.push("<div class='dx-device" + active + "' data-device-index='" + idx + "'>");
        rows.push("<div class='dx-title'>" + title + '</div>');
        rows.push("<div class='dx-meta'>" + escapeHtml(template) + '</div>');
        rows.push('</div>');
      });
    }
    state.sidebar.innerHTML = rows.join('');
  }

  function renderDetail() {
    if (!state.detail) {
      return;
    }
    const dev = state.devices[state.selectedDevice];
    if (!dev) {
      state.detail.innerHTML = "<div class='dx-empty'>Select a device.</div>";
      return;
    }
    const html = [];
    html.push("<div class='dx-field'><label>Name</label><input data-field='display_name' value='" +
      escapeHtml(dev.display_name || '') + "'></div>");
    html.push("<div class='dx-field'><label>ID</label><input data-field='id' value='" +
      escapeHtml(dev.id || '') + "'></div>");
    html.push("<div class='dx-field'><label>Template</label><select data-field='template'>");
    html.push("<option value=''>None</option>");
    state.templates.forEach((tpl) => {
      const selected = dev.template && dev.template.type === tpl.id ? ' selected' : '';
      html.push("<option value='" + tpl.id + "'" + selected + '>' + escapeHtml(tpl.label) + '</option>');
    });
    html.push('</select></div>');
    if (dev.template && dev.template.type === 'uid_validator') {
      html.push(renderUidTemplate(dev));
    } else if (dev.template && dev.template.type === 'signal_hold') {
      html.push(renderSignalTemplate(dev));
    } else {
      html.push("<div class='dx-empty'>Assign template to configure behavior.</div>");
    }
    html.push("<button class='danger' data-action='device-remove'>Delete device</button>");
    state.detail.innerHTML = html.join('');
  }

  function assignTemplate(dev, type) {
    if (!type) {
      dev.template = null;
      renderDetail();
      return;
    }
    if (type === 'uid_validator') {
      dev.template = {
        type: type,
        uid: {
          slots: [],
          success_topic: '',
          success_payload: '',
          success_audio_track: '',
          fail_topic: '',
          fail_payload: '',
          fail_audio_track: '',
        },
      };
    } else if (type === 'signal_hold') {
      dev.template = {
        type: type,
        signal: {
          signal_topic: '',
          signal_payload_on: '',
          signal_payload_off: '',
          heartbeat_topic: '',
          required_hold_ms: 0,
          heartbeat_timeout_ms: 0,
        },
      };
    } else {
      dev.template = null;
    }
    renderDetail();
    markDirty();
  }

  function renderUidTemplate(dev) {
    const tpl = dev.template.uid || {slots: []};
    tpl.slots = tpl.slots || [];
    const html = [];
    html.push("<div class='dx-section'><div class='dx-section-head'>UID slots<button data-action='slot-add'>Add slot</button></div>");
    if (!tpl.slots.length) {
      html.push("<div class='dx-empty'>No slots configured.</div>");
    } else {
      tpl.slots.forEach((slot, idx) => {
        slot.values = slot.values || [];
        html.push("<div class='dx-slot'>");
        html.push("<div class='dx-slot-head'>Slot " + (idx + 1) +
          "<button data-action='slot-remove' data-index='" + idx + "'>&times;</button></div>");
        html.push("<div class='dx-field'><label>Source topic</label><input data-field='uid-slot' data-subfield='source_id' data-index='" +
          idx + "' value='" + escapeHtml(slot.source_id || '') + "'></div>");
        html.push("<div class='dx-field'><label>Label</label><input data-field='uid-slot' data-subfield='label' data-index='" +
          idx + "' value='" + escapeHtml(slot.label || '') + "'></div>");
        html.push("<div class='dx-field'><label>Values</label><input data-field='uid-values' data-index='" +
          idx + "' value='" + escapeHtml((slot.values || []).join(', ')) + "' placeholder='uid1, uid2'></div>");
        const last = slot.last_value ? escapeHtml(slot.last_value) : '';
        html.push("<div class='dx-field dx-slot-last'><label>Last read</label><div class='dx-last-value'>" +
          (last || '&mdash;') + "</div></div>");
        html.push('</div>');
      });
    }
    html.push('</div>');
    html.push("<div class='dx-section'><div class='dx-section-head'>Success actions</div>");
    html.push(actionInput('uid-action', 'success_topic', tpl.success_topic || '', 'Topic'));
    html.push(actionInput('uid-action', 'success_payload', tpl.success_payload || '', 'Payload'));
    html.push(actionInput('uid-action', 'success_audio_track', tpl.success_audio_track || '', 'Audio track'));
    html.push('</div>');
    html.push("<div class='dx-section'><div class='dx-section-head'>Fail actions</div>");
    html.push(actionInput('uid-action', 'fail_topic', tpl.fail_topic || '', 'Topic'));
    html.push(actionInput('uid-action', 'fail_payload', tpl.fail_payload || '', 'Payload'));
    html.push(actionInput('uid-action', 'fail_audio_track', tpl.fail_audio_track || '', 'Audio track'));
    html.push('</div>');
    return html.join('');
  }

  function renderSignalTemplate(dev) {
    const sig = dev.template.signal || {};
    const html = [];
    html.push("<div class='dx-section'><div class='dx-section-head'>Signal control</div>");
    html.push(actionInput('signal', 'signal_topic', sig.signal_topic || '', 'Topic'));
    html.push(actionInput('signal', 'signal_payload_on', sig.signal_payload_on || '', 'Payload ON'));
    html.push(actionInput('signal', 'signal_payload_off', sig.signal_payload_off || '', 'Payload OFF'));
    html.push(actionInput('signal', 'heartbeat_topic', sig.heartbeat_topic || '', 'Heartbeat topic'));
    html.push(actionInput('signal', 'required_hold_ms', sig.required_hold_ms || '', 'Hold ms'));
    html.push(actionInput('signal', 'heartbeat_timeout_ms', sig.heartbeat_timeout_ms || '', 'Timeout ms'));
    html.push('</div>');
    return html.join('');
  }

  function actionInput(field, name, value, label) {
    return "<div class='dx-field'><label>" + label +
      "</label><input data-field='" + field + "' data-subfield='" + name + "' value='" +
      escapeHtml(String(value || '')) + "'></div>";
  }

  function selectDevice(idx) {
    if (idx < 0 || idx >= state.devices.length) {
      return;
    }
    state.selectedDevice = idx;
    renderSidebar();
    renderDetail();
  }

  function addDevice() {
    if (state.devices.length >= LIMITS.devices) {
      setStatus('Device limit reached', 'warn');
      return;
    }
    const dev = {
      id: 'device_' + Date.now().toString(16),
      display_name: 'Device',
      template: null,
      tabs: [],
      topics: [],
      scenarios: [],
    };
    state.devices.push(dev);
    state.selectedDevice = state.devices.length - 1;
    markDirty();
    renderSidebar();
    renderDetail();
  }

  function removeDevice() {
    if (state.selectedDevice < 0) {
      return;
    }
    state.devices.splice(state.selectedDevice, 1);
    state.selectedDevice = Math.min(state.selectedDevice, state.devices.length - 1);
    markDirty();
    renderSidebar();
    renderDetail();
  }

  function addSlot() {
    const dev = state.devices[state.selectedDevice];
    if (!dev || !dev.template || dev.template.type !== 'uid_validator') {
      return;
    }
    if (dev.template.uid.slots.length >= LIMITS.uidSlots) {
      setStatus('Slot limit reached', 'warn');
      return;
    }
    dev.template.uid.slots.push({source_id: '', label: '', values: []});
    markDirty();
    renderDetail();
  }

  function removeSlot(idx) {
    const dev = state.devices[state.selectedDevice];
    if (!dev || !dev.template || dev.template.type !== 'uid_validator') {
      return;
    }
    dev.template.uid.slots.splice(idx, 1);
    markDirty();
    renderDetail();
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
    fetch(url, {method: 'POST'}).then(() => loadProfiles());
  }

  function cloneProfile() {
    if (!state.activeProfile) {
      return;
    }
    createProfile(state.activeProfile);
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
      '&name=' + encodeURIComponent(name), {method: 'POST'}).then(() => loadProfiles());
  }

  function deleteProfile() {
    if (!state.activeProfile) {
      return;
    }
    if (!confirm('Delete profile ' + state.activeProfile + '?')) {
      return;
    }
    fetch('/api/devices/profile/delete?id=' + encodeURIComponent(state.activeProfile), {method: 'POST'})
      .then(() => loadProfiles());
  }

  function activateProfile(id) {
    if (!id || id === state.activeProfile) {
      return;
    }
    state.activeProfile = id;
    renderSidebar();
    fetch('/api/devices/profile/activate?id=' + encodeURIComponent(id), {method: 'POST'})
      .then(() => loadProfiles());
  }

  function saveProfile() {
    if (!state.model || !state.dirty) {
      return;
    }
    state.model.devices = state.devices;
    const payload = prepareModelForSave(state.model);
    fetch('/api/devices/apply?profile=' + encodeURIComponent(state.activeProfile || ''), {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(payload),
    })
      .then((r) => {
        if (!r.ok) {
          throw new Error('HTTP ' + r.status);
        }
        return r.json().catch(() => ({}));
      })
      .then(() => {
        state.dirty = false;
        updateSaveState();
        setStatus('Saved', 'success');
      })
      .catch((err) => setStatus('Save failed: ' + err.message, 'error'));
  }

  function markDirty() {
    state.dirty = true;
    updateSaveState();
  }

  function updateSaveState() {
    if (state.saveBtn) {
      state.saveBtn.disabled = !state.dirty;
    }
  }

  function escapeHtml(text) {
    if (text === null || text === undefined) {
      return '';
    }
    return String(text).replace(/[&<>\"']/g, (c) => ({
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;',
    }[c] || c));
  }

  window.addEventListener('load', init);

  function prepareModelForSave(model) {
    const clone = JSON.parse(JSON.stringify(model || {}));
    if (Array.isArray(clone.devices)) {
      clone.devices.forEach(stripRuntimeFields);
    }
    return clone;
  }

  function stripRuntimeFields(dev) {
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
})();
