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

function refreshRequiredIndicators() {
  if (typeof validateRequiredFields === 'function') {
    validateRequiredFields();
  }
}

function refreshTrackPickers() {
  if (!state.detail) {
    return;
  }
  const inputs = state.detail.querySelectorAll('input[list="track_lookup"]');
  if (inputs.length) {
    ensureTrackLookupOptions();
  }
  inputs.forEach((input) => {
    if (input.dataset.pickerBound) {
      return;
    }
    input.dataset.pickerBound = '1';
    input.addEventListener('focus', () => {
      if (typeof input.showPicker === 'function') {
        input.showPicker();
      }
    });
  });
}

const TRACK_LOOKUP_ROOT = '/sdcard';
const TRACK_LOOKUP_MAX_DIRS = 48;
const TRACK_LOOKUP_MAX_TRACKS = 600;
let trackLookupCache = [];
let trackLookupPromise = null;

function ensureTrackLookupOptions(forceReload) {
  if (trackLookupPromise) {
    return trackLookupPromise;
  }
  if (!forceReload && trackLookupCache.length) {
    if (typeof updateTrackLookupOptions === 'function') {
      updateTrackLookupOptions(trackLookupCache);
    }
    return Promise.resolve(trackLookupCache);
  }
  trackLookupPromise = collectTrackLookupEntries(TRACK_LOOKUP_ROOT).then((tracks) => {
    trackLookupCache = tracks;
    if (typeof updateTrackLookupOptions === 'function') {
      updateTrackLookupOptions(trackLookupCache);
    }
    return trackLookupCache;
  }).catch((err) => {
    console.warn('Track lookup failed', err);
    return trackLookupCache;
  }).finally(() => {
    trackLookupPromise = null;
  });
  return trackLookupPromise;
}

async function collectTrackLookupEntries(root) {
  const queue = [root];
  const visited = new Set();
  const tracks = [];
  while (queue.length && visited.size < TRACK_LOOKUP_MAX_DIRS && tracks.length < TRACK_LOOKUP_MAX_TRACKS) {
    const dir = queue.shift();
    if (!dir || visited.has(dir)) {
      continue;
    }
    visited.add(dir);
    const entries = await loadTrackDirectory(dir);
    entries.forEach((entry) => {
      if (!entry || !entry.path) {
        return;
      }
      if (entry.dir) {
        if (!visited.has(entry.path) && queue.length < TRACK_LOOKUP_MAX_DIRS) {
          queue.push(entry.path);
        }
      } else if (tracks.length < TRACK_LOOKUP_MAX_TRACKS) {
        tracks.push(entry);
      }
    });
  }
  return tracks.sort((a, b) => {
    const dirA = trackDirName(a.path);
    const dirB = trackDirName(b.path);
    if (dirA !== dirB) {
      return dirA.localeCompare(dirB);
    }
    return trackBaseName(a.path).localeCompare(trackBaseName(b.path));
  });
}

async function loadTrackDirectory(path) {
  try {
    const res = await fetch(`/api/files?path=${encodeURIComponent(path)}`);
    if (!res.ok) {
      throw new Error(`HTTP ${res.status}`);
    }
    const data = await res.json();
    return Array.isArray(data) ? data : [];
  } catch (err) {
    console.warn('Track lookup dir failed', path, err);
    return [];
  }
}

function trackBaseName(path) {
  if (!path) {
    return '';
  }
  const parts = path.split('/').filter(Boolean);
  return parts.length ? parts[parts.length - 1] : path;
}

function trackDirName(path) {
  if (!path) {
    return '/';
  }
  const idx = path.lastIndexOf('/');
  if (idx < 0) {
    return '/';
  }
  const dir = path.slice(0, idx) || '/';
  return dir || '/';
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
  refreshRequiredIndicators();
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
  refreshRequiredIndicators();
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
