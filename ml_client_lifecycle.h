/* Pure action-echo preservation for a routed client's in-frame respawn.
 *
 * PutClientInServer deliberately clears gclient_t.  A network ML usercmd may
 * have been applied immediately before that clear, so preserve only its
 * immutable attribution fields.  Prior-life rewards, causal facts, target
 * state, thermal state, and gameplay state are intentionally not copied. */
#ifndef ML_CLIENT_LIFECYCLE_H
#define ML_CLIENT_LIFECYCLE_H

#include "g_local.h"
#include "ml_bridge.h"

typedef struct {
    float move_forward;
    float move_right;
    float look_yaw;
    float look_pitch;
    int vertical_intent;
    int applied_upmove;
    int fire;
    int fire_suppressed;
    int hook;
    int weapon;
    int action_generation;
    int action_tick;
} ml_client_lifecycle_echo_t;

static inline qboolean ML_ClientLifecycleCaptureEcho(
    ml_client_lifecycle_echo_t *dst, const zgcl_t *zc,
    qboolean routed, qboolean dead)
{
    if (!dst || !zc || !routed || !dead || !zc->ml_last_action_ok ||
        zc->ml_last_action_tick <= 0 || !zc->ml_action_generation_valid ||
        zc->ml_action_generation < 0 ||
        (unsigned int)zc->ml_action_generation >=
            ML_ACTION_GENERATION_COUNT ||
        zc->ml_vertical_intent < 0 ||
        (unsigned int)zc->ml_vertical_intent >= ML_VERTICAL_COUNT ||
        zc->ml_hook < 0 || zc->ml_hook > 3 ||
        zc->ml_weapon < 0 || zc->ml_weapon > 9)
        return qfalse;

    dst->move_forward = zc->ml_move_forward;
    dst->move_right = zc->ml_move_right;
    dst->look_yaw = zc->ml_look_yaw;
    dst->look_pitch = zc->ml_look_pitch;
    dst->vertical_intent = zc->ml_vertical_intent;
    dst->applied_upmove = zc->ml_applied_upmove;
    dst->fire = zc->ml_fire;
    dst->fire_suppressed = zc->ml_fire_suppressed;
    dst->hook = zc->ml_hook;
    dst->weapon = zc->ml_weapon;
    dst->action_generation = zc->ml_action_generation;
    dst->action_tick = zc->ml_last_action_tick;
    return qtrue;
}

static inline void ML_ClientLifecycleRestoreEcho(
    zgcl_t *zc, const ml_client_lifecycle_echo_t *src)
{
    if (!zc || !src)
        return;
    zc->ml_move_forward = src->move_forward;
    zc->ml_move_right = src->move_right;
    zc->ml_look_yaw = src->look_yaw;
    zc->ml_look_pitch = src->look_pitch;
    zc->ml_vertical_intent = src->vertical_intent;
    zc->ml_applied_upmove = src->applied_upmove;
    zc->ml_fire = src->fire;
    zc->ml_fire_suppressed = src->fire_suppressed;
    zc->ml_hook = src->hook;
    zc->ml_weapon = src->weapon;
    zc->ml_action_generation = src->action_generation;
    zc->ml_action_generation_valid = 1;
    zc->ml_last_action_tick = src->action_tick;
    zc->ml_last_action_ok = 1;
}

#endif
