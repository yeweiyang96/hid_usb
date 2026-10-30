#include "hid_bridge.h"
#include <string.h>

static bool available(const HidBridge *b) {
    return b->network_up && b->usb_available && b->acknowledged_epoch == b->epoch;
}

static void invalidate(HidBridge *b, uint32_t reason, uint32_t now_ms) {
    HidLease *l = &b->lease;
    if (l->active) {
        if (reason) {
            b->diagnostics.snapshot.stop=(HidDiagnosticStop){reason,now_ms,b->epoch,l->session,l->target_seq};
            b->diagnostics.snapshot.validity |= HID_VALID_STOP;
            b->diagnostics.release_context=(HidDiagnosticEvent){b->epoch,l->session,l->target_seq,now_ms};
            b->diagnostics.release_context_valid=true;
            b->diagnostics.release_context_submitted=false;
        }
        l->closed_epoch = b->epoch;
        l->closed_session = l->session;
        l->release_pending = true;
    }
    l->active = l->desired_pending = l->hold_started = false;
    b->motion.active = b->motion.frame_pending = false;
    l->modifier = 0;
    memset(l->keys, 0, sizeof(l->keys));
    ++b->epoch;
    b->rejected += b->count;
    b->count = b->head = 0;
    /* In-flight/engine state is cleared only by the owning core's publish. */
}

void hid_bridge_init(HidBridge *b) {
    memset(b, 0, sizeof(*b));
    critical_section_init(&b->lock);
    b->epoch = 1;
    b->lease.boot = 1; /* Tests; production replaces this before either core runs. */
}

void hid_bridge_network(HidBridge *b, bool up, bool force_abort) {
    critical_section_enter_blocking(&b->lock);
    if ((!up && b->network_up) || force_abort) invalidate(b,HID_STOP_WIFI,b->clock_ms);
    b->network_up = up;
    critical_section_exit(&b->lock);
}

bool hid_bridge_submit(HidBridge *b, const HidCommand *command) {
    critical_section_enter_blocking(&b->lock);
    /* Emergency release is a cancellation barrier, not a queued action. */
    if (command->type == CMD_RELEASE_ALL) {
        invalidate(b,HID_STOP_LEGACY_RELEASE,b->clock_ms);
        ++b->accepted;
        critical_section_exit(&b->lock);
        return true;
    }
    bool ok = available(b) && !b->lease.active && !b->lease.release_pending &&
        b->count < HID_QUEUE_CAPACITY;
    if (ok) {
        b->commands[(b->head + b->count) % HID_QUEUE_CAPACITY] = *command;
        ++b->count;
        ++b->accepted;
    } else {
        ++b->rejected;
    }
    critical_section_exit(&b->lock);
    return ok;
}

void hid_bridge_cancel(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    invalidate(b,HID_STOP_INTERNAL,b->clock_ms);
    critical_section_exit(&b->lock);
}

void hid_bridge_reject(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    ++b->rejected;
    critical_section_exit(&b->lock);
}

void hid_bridge_network_heartbeat(HidBridge *b, uint32_t now_ms) {
    critical_section_enter_blocking(&b->lock);
    b->network_heartbeat_ms = now_ms;
    critical_section_exit(&b->lock);
}

uint32_t hid_bridge_epoch(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    uint32_t epoch = b->epoch;
    critical_section_exit(&b->lock);
    return epoch;
}

HidBridgeStatus hid_bridge_status(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    bool busy = b->count || b->in_flight || b->engine_busy || b->acknowledged_epoch != b->epoch ||
        b->lease.active || b->lease.release_pending;
    HidBridgeStatus result = {
        .flags = b->usb_flags | (busy ? 8u : 0u) | (b->network_up ? 16u : 0u),
        .queued = b->count,
        .available = available(b),
        .accepted = b->accepted,
        .rejected = b->rejected,
        .epoch = b->epoch,
        .acknowledged_epoch = b->acknowledged_epoch,
        .network_heartbeat_ms = b->network_heartbeat_ms
    };
    critical_section_exit(&b->lock);
    return result;
}

bool hid_bridge_take(HidBridge *b, uint32_t epoch, HidCommand *command) {
    critical_section_enter_blocking(&b->lock);
    bool ok = available(b) && !b->lease.active && !b->lease.release_pending &&
        epoch == b->epoch && b->count && !b->in_flight && !b->engine_busy;
    if (ok) {
        *command = b->commands[b->head];
        b->head = (uint8_t)((b->head + 1u) % HID_QUEUE_CAPACITY);
        --b->count;
        b->in_flight = true;
    }
    critical_section_exit(&b->lock);
    return ok;
}

static void publish_internal(HidBridge *b, uint32_t epoch, uint8_t flags,
                         bool usb_available, bool busy, uint32_t rejected_delta, uint64_t now_us, bool timed) {
    uint32_t now_ms=(uint32_t)(now_us/1000u);
    uint32_t current=(uint32_t)now_us;
    critical_section_enter_blocking(&b->lock);
    if (timed) {
        b->clock_ms=now_ms;
        HidDiagnostics *d=&b->diagnostics;
        if (d->core0_seen) {
            uint32_t gap=current-d->core0_last_us;
            if (gap>d->snapshot.core0_max_gap_us) d->snapshot.core0_max_gap_us=gap;
        }
        d->core0_last_us=current; d->core0_seen=true;
    }
    b->rejected += rejected_delta;
    b->usb_flags = flags & 7u;
    if (!usb_available && b->usb_available) {
        ++b->diagnostics.snapshot.usb_errors;
        b->diagnostics.pending=false; /* Disconnect cannot be reported as completion. */
        b->diagnostics.tracking_blocked=false;
        invalidate(b,HID_STOP_USB_UNAVAILABLE,b->clock_ms);
    }
    b->usb_available = usb_available;
    if (epoch == b->epoch) {
        b->acknowledged_epoch = epoch;
        b->engine_busy = busy;
        b->in_flight = busy;
    }
    critical_section_exit(&b->lock);
}

void hid_bridge_publish(HidBridge *b, uint32_t epoch, uint8_t flags,
                         bool usb_available, bool busy, uint32_t rejected_delta) {
    publish_internal(b,epoch,flags,usb_available,busy,rejected_delta,0,false);
}
void hid_bridge_publish_at(HidBridge *b, uint32_t epoch, uint8_t flags,
                            bool usb_available, bool busy, uint32_t rejected_delta, uint64_t now_us) {
    publish_internal(b,epoch,flags,usb_available,busy,rejected_delta,now_us,true);
}

bool hid_bridge_output_begin(HidBridge *b, uint32_t epoch, bool release) {
    critical_section_enter_blocking(&b->lock);
    if (!release && (!available(b) || epoch != b->epoch || b->lease.active || b->lease.release_pending)) {
        critical_section_exit(&b->lock);
        return false;
    }
    return true;
}

void hid_bridge_output_end(HidBridge *b) {
    critical_section_exit(&b->lock);
}


static bool reached(uint32_t now, uint32_t deadline) {
    return (uint32_t)(now - deadline) < 0x80000000u;
}

/* Called for every packet and Core 0 tick, so a packet arriving after expiry
 * cannot keep an expired session alive before the next scheduled USB tick. */
static void lease_expire(HidBridge *b, uint32_t now) {
    HidLease *l = &b->lease;
    if (l->active && reached(now, l->expiry_ms)) {
        invalidate(b,HID_STOP_EXPIRED,now);
    } else if (l->active && b->motion.active && reached(now,b->motion.not_after_ms)) {
        invalidate(b,HID_STOP_WINDOW_END,now);
    } else if (l->active && l->hold_started && reached(now, l->hold_deadline_ms)) {
        l->modifier = 0;
        memset(l->keys, 0, 6);
        l->hold_started = false;
        l->hold_expired = true;
        l->desired_pending = true;
    }
}

static bool lease_can_open(const HidBridge *b) {
    return available(b) && !b->count && !b->in_flight && !b->engine_busy &&
        !b->lease.active && !b->lease.release_pending && !b->lease.desired_pending;
}

static HidLeaseStatus lease_status(const HidBridge *b, uint32_t now, uint32_t token, uint8_t result) {
    const HidLease *l = &b->lease;
    bool pending = l->release_pending || l->desired_pending || b->acknowledged_epoch != b->epoch ||
        b->engine_busy || b->in_flight;
    HidLeaseStatus s = {0};
    s.result = result; s.token = token; s.boot = l->boot; s.epoch = b->epoch;
    s.session = l->session; s.accepted_seq = l->accepted_seq; s.applied_seq = l->applied_seq;
    s.uptime_ms = now; s.modifier = l->actual_modifier; memcpy(s.keys, l->actual_keys, 6);
    s.flags = (b->usb_available ? LEASE_USB : 0) | (b->network_up ? LEASE_WIFI : 0) |
        (l->active ? LEASE_ACTIVE : 0) | (pending ? LEASE_PENDING : 0) |
        (l->hold_expired ? LEASE_HOLD_EXPIRED : 0) | (lease_can_open(b) ? LEASE_CAN_OPEN : 0);
    if (l->active && !reached(now, l->expiry_ms)) s.lease_remaining_ms = (uint16_t)(l->expiry_ms - now);
    if (l->hold_started && !reached(now, l->hold_deadline_ms)) s.hold_remaining_ms = (uint16_t)(l->hold_deadline_ms - now);
    return s;
}

void hid_bridge_set_boot(HidBridge *b, uint32_t boot) {
    critical_section_enter_blocking(&b->lock);
    b->lease.boot = boot ? boot : 1;
    critical_section_exit(&b->lock);
}

static uint8_t lease_apply(HidBridge *b, const HidLeaseRequest *r, uint32_t now) {
    HidLease *l = &b->lease;
    if (r->op == LEASE_QUERY || r->op == LEASE_DIAGNOSTIC) return LEASE_OK;
    if (r->boot != l->boot) return LEASE_STALE;
    if (r->op == LEASE_OPEN) {
        /* An ACK can be queried/replayed without extending authorization. */
        if (l->active && r->version == l->version && r->session == l->session && r->epoch == b->epoch - 1u) return LEASE_OK;
        if (r->epoch != b->epoch) return LEASE_STALE;
        if (!lease_can_open(b)) return LEASE_UNAVAILABLE;
        invalidate(b,HID_STOP_NONE,now); /* Clears old state before lease use. */
        b->diagnostics.release_context_valid=false;
        l->active = true;
        l->version = r->version;
        memset(&b->motion,0,sizeof(b->motion));
        l->session = r->session;
        l->accepted_seq = l->applied_seq = l->target_seq = 0;
        l->expiry_ms = now + r->lease_ms;
        l->hold_ms = 0;
        l->hold_started = l->hold_expired = l->desired_pending = false;
        l->release_pending = true;
        l->last_request = *r;
        return LEASE_OK;
    }
    if (r->op == LEASE_STOP && !l->active && r->epoch == l->closed_epoch &&
        r->session == l->closed_session && r->session) return LEASE_OK;
    if (r->epoch != b->epoch) return LEASE_STALE;
    if (!l->active || r->session != l->session) return LEASE_INACTIVE;
    if (r->version != l->version) return LEASE_INVALID;
    if (r->op == LEASE_STOP) {
        invalidate(b,HID_STOP_CLIENT,now);
        return LEASE_OK;
    }
    if (!b->usb_available || !b->network_up) return LEASE_UNAVAILABLE;
    uint32_t delta = r->seq - l->accepted_seq;
    if (!delta) return hid_lease_same_request(r, &l->last_request) ? LEASE_OK : LEASE_OLD_SEQUENCE;
    if (delta >= 0x80000000u) return LEASE_OLD_SEQUENCE;
    if (r->op == LEASE_WINDOW) {
        HidMotion *m=&b->motion;
        uint32_t lifetime=r->not_after_ms-now;
        uint32_t revision_delta=r->revision-m->last_revision;
        uint32_t event_delta=r->event_id-m->last_event;
        if (!lifetime || lifetime>200u) return LEASE_STALE;
        if (!revision_delta || revision_delta>=0x80000000u) return LEASE_OLD_SEQUENCE;
        if (r->event_id && (!event_delta || event_delta>=0x80000000u)) return LEASE_OLD_SEQUENCE;
        /* The entire future replaces atomically. No release report at the seam. */
        memcpy(m->frames,r->frames,sizeof(m->frames)); m->count=(uint8_t)r->count;
        m->index=0; m->active=true; m->started=false; m->frame_pending=true;
        m->revision=m->last_revision=r->revision; m->event_id=r->event_id;
        if (r->event_id) m->last_event=r->event_id;
        m->not_after_ms=r->not_after_ms;
        l->desired_pending=l->hold_started=l->hold_expired=false;
        l->hold_ms=0; l->target_seq=r->seq;
    }
    l->accepted_seq = r->seq;
    l->expiry_ms = now + r->lease_ms;
    l->last_request = *r;
    if (r->op == LEASE_SET) {
        b->motion.active=b->motion.frame_pending=false;
        l->target_seq = r->seq;
        l->modifier = r->modifier;
        memcpy(l->keys, r->keys, 6);
        l->hold_ms = r->hold_ms;
        l->hold_started = l->hold_expired = false;
        l->desired_pending = true;
    }
    return LEASE_OK;
}

void hid_bridge_lease_request(HidBridge *b, const uint8_t *data, size_t size,
                              uint32_t now, uint8_t response[HID_LEASE_RESPONSE_SIZE]) {
    HidLeaseRequest r;
    bool valid = hid_lease_decode(data, size, &r) && r.version==3;
    uint32_t token = size >= 16 ? hid_read_u32(data + 12) : 0;
    critical_section_enter_blocking(&b->lock);
    b->clock_ms=now;
    lease_expire(b, now);
    uint8_t result = valid ? lease_apply(b, &r, now) : LEASE_INVALID;
    HidLeaseStatus status = lease_status(b, now, token, result);
    hid_lease_encode(response, &status);
    critical_section_exit(&b->lock);
}

static void motion_w32(uint8_t *p,uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(i*8));
}

void hid_bridge_motion_request(HidBridge *b,const uint8_t *data,size_t size,
                               uint32_t now,uint8_t response[HID_MOTION_RESPONSE_SIZE]) {
    HidLeaseRequest r;
    bool valid=hid_lease_decode(data,size,&r) && r.version==4 && r.op!=LEASE_DIAGNOSTIC;
    uint32_t token=size>=16?hid_read_u32(data+12):0;
    critical_section_enter_blocking(&b->lock);
    b->clock_ms=now; lease_expire(b,now);
    uint8_t result=valid?lease_apply(b,&r,now):LEASE_INVALID;
    HidLeaseStatus status=lease_status(b,now,token,result);
    if (b->motion.active && b->motion.frame_pending) status.flags|=LEASE_PENDING;
    hid_lease_encode(response,&status); response[4]=4;
    HidMotion *m=&b->motion;
    motion_w32(response+48,m->revision);
    motion_w32(response+52,m->submitted_revision);
    motion_w32(response+56,m->completed_revision);
    motion_w32(response+60,m->started?m->start_ms:0);
    motion_w32(response+64,m->active?m->not_after_ms-now:0);
    motion_w32(response+68,m->applied_event);
    response[72]=m->index; response[73]=m->count;
    response[74]=(uint8_t)m->submitted_frame; response[75]=(uint8_t)m->completed_frame;
    motion_w32(response+76,1u); /* bounded timeline capability */
    critical_section_exit(&b->lock);
}

void hid_bridge_lease_tick(HidBridge *b, uint32_t now,
                          bool (*keyboard)(void *, uint8_t, const uint8_t[6]), void *context) {
    critical_section_enter_blocking(&b->lock);
    b->clock_ms=now;
    lease_expire(b, now);
    HidLease *l = &b->lease;
    HidMotion *m = &b->motion;
    if (l->active && m->active && m->started && !m->frame_pending && reached(now,m->due_ms)) {
        if (++m->index>=m->count) {
            invalidate(b,HID_STOP_WINDOW_END,now);
        } else {
            m->frame_pending=true;
            m->ready_deadline_ms=m->due_ms+5u;
        }
    }
    /* Allow normal USB scheduling jitter, never compress a delayed state or
     * replay old states to catch up. The absolute window deadline never moves. */
    if (l->active && m->active && m->started && m->frame_pending && reached(now,m->ready_deadline_ms))
        invalidate(b,HID_STOP_TIMELINE_LATE,now);
    /* The old engine must have acknowledged the cancellation and finished all
     * of its keyboard/mouse releases before the new state can be submitted. */
    if (!b->usb_available || b->acknowledged_epoch != b->epoch || b->engine_busy || b->in_flight) {
        critical_section_exit(&b->lock);
        return;
    }
    if (l->release_pending) {
        const uint8_t empty[6] = {0};
        if (keyboard(context, 0, empty)) {
            hid_bridge_keyboard_submitted_locked(b,0,empty,now,false);
            l->actual_modifier = 0;
            memset(l->actual_keys, 0, 6);
            l->release_pending = false;
        }
    } else if (l->active && b->network_up && m->active && m->frame_pending) {
        HidKeyframe *f=&m->frames[m->index];
        if (keyboard(context,f->modifier,f->keys)) {
            hid_bridge_keyboard_submitted_locked(b,f->modifier,f->keys,now,true);
            l->actual_modifier=f->modifier; memcpy(l->actual_keys,f->keys,6);
            l->applied_seq=l->target_seq;
            m->submitted_revision=m->revision; m->submitted_frame=m->index;
            m->submitted_seq=l->target_seq; m->submit_time_ms=now;
            if (!m->index && m->event_id) m->applied_event=m->event_id;
            if (!m->started) {
                m->started=true; m->start_ms=now;
            }
            m->due_ms=now+f->duration_ms;
            m->frame_pending=false;
        }
    } else if (l->active && b->network_up && l->desired_pending) {
        if (keyboard(context, l->modifier, l->keys)) {
            hid_bridge_keyboard_submitted_locked(b,l->modifier,l->keys,now,true);
            l->actual_modifier = l->modifier;
            memcpy(l->actual_keys, l->keys, 6);
            l->applied_seq = l->target_seq;
            l->desired_pending = false;
            if (l->hold_ms && !l->hold_expired) {
                l->hold_started = true;
                l->hold_deadline_ms = now + l->hold_ms;
            }
        }
    }
    critical_section_exit(&b->lock);
}


void hid_bridge_network_at(HidBridge *b, bool up, bool force_abort, uint32_t now_ms) {
    critical_section_enter_blocking(&b->lock);
    b->clock_ms=now_ms;
    if ((!up && b->network_up) || force_abort) invalidate(b,HID_STOP_WIFI,now_ms);
    b->network_up=up;
    critical_section_exit(&b->lock);
}

void hid_bridge_cancel_reason(HidBridge *b, uint32_t reason, uint32_t now_ms) {
    critical_section_enter_blocking(&b->lock);
    b->clock_ms=now_ms;
    if (reason==HID_STOP_USB_FAILED || reason==HID_STOP_USB_STALL) {
        ++b->diagnostics.snapshot.usb_errors;
        b->diagnostics.pending=false;
        b->diagnostics.tracking_blocked=false;
    }
    invalidate(b,reason,now_ms);
    critical_section_exit(&b->lock);
}

void hid_bridge_network_heartbeat_at(HidBridge *b, uint64_t now_us) {
    uint32_t now_ms=(uint32_t)(now_us/1000u);
    uint32_t current=(uint32_t)now_us;
    critical_section_enter_blocking(&b->lock);
    b->network_heartbeat_ms=b->clock_ms=now_ms;
    HidDiagnostics *d=&b->diagnostics;
    if (d->core1_seen) {
        uint32_t gap=current-d->core1_last_us;
        if (gap>d->snapshot.core1_max_gap_us) d->snapshot.core1_max_gap_us=gap;
    }
    d->core1_last_us=current; d->core1_seen=true;
    critical_section_exit(&b->lock);
}

void hid_bridge_release_at(HidBridge *b, uint32_t now_ms) {
    critical_section_enter_blocking(&b->lock);
    b->clock_ms=now_ms;
    invalidate(b,HID_STOP_LEGACY_RELEASE,now_ms);
    ++b->accepted;
    critical_section_exit(&b->lock);
}

void hid_bridge_note_send_error(HidBridge *b) {
    critical_section_enter_blocking(&b->lock);
    ++b->diagnostics.snapshot.net_send_errors;
    critical_section_exit(&b->lock);
}

void hid_bridge_diagnostic_request(HidBridge *b, const uint8_t *data, size_t size,
                                   uint32_t now_ms, uint8_t response[HID_DIAGNOSTIC_SIZE]) {
    HidLeaseRequest r;
    bool valid=hid_lease_decode(data,size,&r) && r.op==LEASE_DIAGNOSTIC;
    uint32_t token=size>=16?hid_read_u32(data+12):0;
    critical_section_enter_blocking(&b->lock);
    /* A read does not renew or process control. Core 0 owns timed expiration. */
    HidDiagnosticSnapshot snapshot=b->diagnostics.snapshot;
    uint32_t boot=b->lease.boot;
    critical_section_exit(&b->lock);
    hid_diagnostic_encode(response,valid?LEASE_OK:LEASE_INVALID,token,boot,now_ms,&snapshot);
}

void hid_bridge_keyboard_submitted_locked(HidBridge *b, uint8_t modifier,
                                         const uint8_t keys[6], uint32_t now_ms, bool lease_state) {
    HidDiagnostics *d=&b->diagnostics;
    bool release=modifier==0;
    for (unsigned i=0;i<6;++i) release=release && !keys[i];
    if (d->pending) {
        ++d->snapshot.usb_tracking_overruns;
        /* A single TinyUSB endpoint should never accept a replacement before
         * its callback. If this invariant fails, matching bytes cannot identify
         * which report completed: suppress completions until USB reset/failure. */
        d->tracking_blocked=true;
    }
    d->pending=true; d->pending_release=release; d->pending_tracked=lease_state;
    d->pending_modifier=modifier; memcpy(d->pending_keys,keys,6);
    d->pending_event=(HidDiagnosticEvent){b->epoch,b->lease.session,b->lease.target_seq,now_ms};
    if (!lease_state && release && !b->lease.active && b->lease.release_pending &&
        d->release_context_valid && !d->release_context_submitted) {
        d->pending_tracked=true;
        d->pending_event=d->release_context; d->pending_event.time_ms=now_ms;
        d->release_context_submitted=true;
    }
    if (!d->pending_tracked) return;
    d->snapshot.submitted=d->pending_event;
    d->snapshot.validity=(d->snapshot.validity & ~HID_SUBMITTED_RELEASE) | HID_VALID_SUBMITTED |
        (release?HID_SUBMITTED_RELEASE:0);
    if (release) {
        d->snapshot.release_submitted=d->pending_event;
        d->snapshot.validity |= HID_VALID_RELEASE_SUBMITTED;
    }
}

void hid_bridge_keyboard_complete(HidBridge *b, uint8_t modifier, const uint8_t keys[6],
                                  uint32_t now_ms, bool valid_report) {
    critical_section_enter_blocking(&b->lock);
    HidDiagnostics *d=&b->diagnostics;
    if (!d->pending || !valid_report || modifier!=d->pending_modifier || memcmp(keys,d->pending_keys,6)) {
        ++d->snapshot.usb_unmatched_completions;
        d->tracking_blocked=true;
        d->pending=false;
    } else {
        if (d->pending_tracked && !d->tracking_blocked) {
            HidDiagnosticEvent event=d->pending_event; event.time_ms=now_ms;
            d->snapshot.completed=event;
            if (b->lease.version==4 && b->motion.submitted_revision &&
                event.seq==b->motion.submitted_seq && !d->pending_release) {
                b->motion.completed_revision=b->motion.submitted_revision;
                b->motion.completed_frame=b->motion.submitted_frame;
            }
            d->snapshot.validity=(d->snapshot.validity & ~HID_COMPLETED_RELEASE) | HID_VALID_COMPLETED |
                (d->pending_release?HID_COMPLETED_RELEASE:0);
            if (d->pending_release) {
                d->snapshot.release_completed=event;
                d->snapshot.validity |= HID_VALID_RELEASE_COMPLETED;
            }
        }
        d->pending=false;
    }
    critical_section_exit(&b->lock);
}
