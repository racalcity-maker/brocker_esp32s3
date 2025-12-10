#include "dm_templates.h"

#include <string.h>
#include <strings.h>

#include "device_manager_utils.h"

static bool uid_matches(const dm_uid_slot_t *slot, const char *value)
{
    if (!slot || !value || !value[0]) {
        return false;
    }
    for (uint8_t i = 0; i < slot->value_count && i < DM_UID_TEMPLATE_MAX_VALUES; ++i) {
        if (slot->values[i][0] && strcasecmp(slot->values[i], value) == 0) {
            return true;
        }
    }
    return false;
}

static int find_slot_index(const dm_uid_template_t *tpl, const char *source_id)
{
    if (!tpl || !source_id || !source_id[0]) {
        return -1;
    }
    for (uint8_t i = 0; i < tpl->slot_count && i < DM_UID_TEMPLATE_MAX_SLOTS; ++i) {
        const dm_uid_slot_t *slot = &tpl->slots[i];
        if (slot->source_id[0] && strcasecmp(slot->source_id, source_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool slot_marked_ok(const dm_uid_state_t *state, uint8_t index)
{
    uint8_t byte = index / 8;
    uint8_t bit = index % 8;
    return (state->ok_bitmap[byte] >> bit) & 0x01;
}

static void slot_set_ok(dm_uid_state_t *state, uint8_t index)
{
    uint8_t byte = index / 8;
    uint8_t bit = index % 8;
    state->ok_bitmap[byte] |= (uint8_t)(1U << bit);
}

void dm_uid_template_clear(dm_uid_template_t *tpl)
{
    if (!tpl) {
        return;
    }
    memset(tpl, 0, sizeof(*tpl));
}

bool dm_uid_template_set_slot(dm_uid_template_t *tpl, uint8_t index, const char *source_id, const char *label)
{
    if (!tpl || index >= DM_UID_TEMPLATE_MAX_SLOTS || !source_id || !source_id[0]) {
        return false;
    }
    dm_uid_slot_t *slot = &tpl->slots[index];
    memset(slot, 0, sizeof(*slot));
    dm_str_copy(slot->source_id, sizeof(slot->source_id), source_id);
    if (label && label[0]) {
        dm_str_copy(slot->label, sizeof(slot->label), label);
    }
    if (index >= tpl->slot_count) {
        tpl->slot_count = index + 1;
    }
    return true;
}

bool dm_uid_template_add_value(dm_uid_template_t *tpl, uint8_t slot_index, const char *value)
{
    if (!tpl || slot_index >= DM_UID_TEMPLATE_MAX_SLOTS || !value || !value[0]) {
        return false;
    }
    dm_uid_slot_t *slot = &tpl->slots[slot_index];
    if (!slot->source_id[0]) {
        return false;
    }
    if (slot->value_count >= DM_UID_TEMPLATE_MAX_VALUES) {
        return false;
    }
    dm_str_copy(slot->values[slot->value_count], sizeof(slot->values[slot->value_count]), value);
    slot->value_count++;
    return true;
}

void dm_uid_state_reset(dm_uid_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

dm_uid_event_t dm_uid_handle_value(dm_uid_state_t *state,
                                   const dm_uid_template_t *tpl,
                                   const char *source_id,
                                   const char *value)
{
    dm_uid_event_t event = {
        .type = DM_UID_EVENT_NONE,
        .slot = NULL,
    };
    if (!state || !tpl || !source_id || !source_id[0]) {
        return event;
    }
    if (tpl->slot_count == 0 || tpl->slot_count > DM_UID_TEMPLATE_MAX_SLOTS) {
        return event;
    }
    if (state->failed) {
        return event;
    }
    int slot_index = find_slot_index(tpl, source_id);
    if (slot_index < 0) {
        return event;
    }
    const dm_uid_slot_t *slot = &tpl->slots[slot_index];
    event.slot = slot;
    if (!uid_matches(slot, value)) {
        state->failed = true;
        event.type = DM_UID_EVENT_INVALID;
        return event;
    }
    if (slot_marked_ok(state, (uint8_t)slot_index)) {
        event.type = DM_UID_EVENT_DUPLICATE;
        return event;
    }
    slot_set_ok(state, (uint8_t)slot_index);
    state->ok_count++;
    event.type = DM_UID_EVENT_ACCEPTED;
    if (state->ok_count >= tpl->slot_count) {
        event.type = DM_UID_EVENT_SUCCESS;
    }
    return event;
}

bool dm_uid_state_is_complete(const dm_uid_state_t *state, const dm_uid_template_t *tpl)
{
    if (!state || !tpl || tpl->slot_count == 0) {
        return false;
    }
    return (!state->failed) && (state->ok_count >= tpl->slot_count);
}

void dm_signal_template_clear(dm_signal_hold_template_t *tpl)
{
    if (!tpl) {
        return;
    }
    memset(tpl, 0, sizeof(*tpl));
    tpl->heartbeat_timeout_ms = 1000;
}

void dm_signal_state_reset(dm_signal_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

dm_signal_event_t dm_signal_handle_tick(dm_signal_state_t *state,
                                        const dm_signal_hold_template_t *tpl,
                                        uint64_t now_ms)
{
    dm_signal_event_t event = {
        .type = DM_SIGNAL_EVENT_NONE,
        .accumulated_ms = state ? state->accumulated_ms : 0,
    };
    if (!state || !tpl || tpl->required_hold_ms == 0) {
        return event;
    }
    if (state->finished) {
        return event;
    }
    uint64_t timeout = tpl->heartbeat_timeout_ms ? tpl->heartbeat_timeout_ms : 1000;
    if (!state->active) {
        state->active = true;
        state->last_tick_ms = now_ms;
        event.type = DM_SIGNAL_EVENT_START;
        return event;
    }
    uint64_t delta = 0;
    if (now_ms > state->last_tick_ms) {
        delta = now_ms - state->last_tick_ms;
        state->accumulated_ms += (uint32_t)delta;
    }
    state->last_tick_ms = now_ms;
    event.accumulated_ms = state->accumulated_ms;

    if (state->accumulated_ms >= tpl->required_hold_ms) {
        state->finished = true;
        event.type = DM_SIGNAL_EVENT_COMPLETED;
        return event;
    }

    if (delta > timeout) {
        state->active = false;
        event.type = DM_SIGNAL_EVENT_STOP;
        return event;
    }

    event.type = DM_SIGNAL_EVENT_CONTINUE;
    return event;
}
