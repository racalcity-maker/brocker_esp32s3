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

