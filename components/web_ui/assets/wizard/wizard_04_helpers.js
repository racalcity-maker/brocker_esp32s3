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
  const allowed = ['uid_validator', 'signal_hold', 'on_mqtt_event', 'on_flag'];
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

