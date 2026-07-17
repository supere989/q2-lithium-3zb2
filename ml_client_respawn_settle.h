/* Stock respawn-settling law shared by ordinary and routed ML clients. */
#ifndef ML_CLIENT_RESPAWN_SETTLE_H
#define ML_CLIENT_RESPAWN_SETTLE_H

#include "g_local.h"

#define ML_CLIENT_RESPAWN_TELEPORT_TIME 14

static inline void ML_ClientApplyStockRespawnHold(
    pmove_state_t *state, qboolean routed_ml)
{
    (void)routed_ml;
    if (!state)
        return;
    state->pm_flags = PMF_TIME_TELEPORT;
    state->pm_time = ML_CLIENT_RESPAWN_TELEPORT_TIME;
}

/* Once any duplicate ClientThink for a decision starts under the stock hold,
 * the entire action-bound transition remains settling even if Pmove consumes
 * the last pm_time unit before telemetry is packed. */
static inline int ML_ClientRespawnSettlingAtEntry(
    int prior_settling, qboolean same_decision, int pm_flags)
{
    int settling;
    settling = (pm_flags & PMF_TIME_TELEPORT) != 0;
    return same_decision ? (prior_settling || settling) : settling;
}

static inline qboolean ML_ClientCausalTransitionTrainable(
    qboolean echo_valid, qboolean facts_complete, qboolean settling)
{
    return echo_valid && facts_complete && !settling;
}

static inline float ML_ClientActualPitchDelta(float actual, float base)
{
    return actual - base;
}

#endif
