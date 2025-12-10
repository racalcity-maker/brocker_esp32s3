#include "dm_runtime_signal.h"

#include <string.h>

#include "device_manager_utils.h"

static void fill_signal_payloads(const dm_signal_hold_template_t *tpl,
                                 dm_signal_action_t *action,
                                 bool success)
{
    if (!tpl || !action) {
        return;
    }
    if (!tpl->signal_topic[0]) {
        return;
    }
    dm_str_copy(action->signal_topic, sizeof(action->signal_topic), tpl->signal_topic);
    if (success) {
        if (tpl->signal_payload_on[0]) {
            action->signal_on = true;
            dm_str_copy(action->signal_payload_on, sizeof(action->signal_payload_on), tpl->signal_payload_on);
            action->signal_on_ms = tpl->signal_on_ms;
        }
        if (tpl->signal_payload_off[0]) {
            action->signal_off = true;
            dm_str_copy(action->signal_payload_off, sizeof(action->signal_payload_off), tpl->signal_payload_off);
        }
    } else {
        if (tpl->signal_payload_off[0]) {
            action->signal_off = true;
            dm_str_copy(action->signal_payload_off, sizeof(action->signal_payload_off), tpl->signal_payload_off);
        }
    }
}

void dm_signal_runtime_init(dm_signal_runtime_t *rt, const dm_signal_hold_template_t *tpl)
{
    if (!rt) {
        return;
    }
    if (tpl) {
        memcpy(&rt->config, tpl, sizeof(*tpl));
    } else {
        memset(&rt->config, 0, sizeof(rt->config));
    }
    dm_signal_state_reset(&rt->state);
}

void dm_signal_runtime_set_template(dm_signal_runtime_t *rt, const dm_signal_hold_template_t *tpl)
{
    if (!rt || !tpl) {
        return;
    }
    memcpy(&rt->config, tpl, sizeof(*tpl));
    dm_signal_state_reset(&rt->state);
}

dm_signal_action_t dm_signal_runtime_handle_tick(dm_signal_runtime_t *rt, uint64_t now_ms)
{
    dm_signal_action_t action = {0};
    if (!rt) {
        return action;
    }
    dm_signal_event_t ev = dm_signal_handle_tick(&rt->state, &rt->config, now_ms);
    action.event = ev.type;

    switch (ev.type) {
    case DM_SIGNAL_EVENT_START:
        if (rt->config.hold_track[0]) {
            action.audio_play = true;
            dm_str_copy(action.audio_track, sizeof(action.audio_track), rt->config.hold_track);
        }
        break;
    case DM_SIGNAL_EVENT_STOP:
        action.audio_pause = true;
        break;
    case DM_SIGNAL_EVENT_COMPLETED:
        if (rt->config.complete_track[0]) {
            action.audio_play = true;
            dm_str_copy(action.audio_track, sizeof(action.audio_track), rt->config.complete_track);
        }
        fill_signal_payloads(&rt->config, &action, true);
        break;
    default:
        break;
    }

    return action;
}
