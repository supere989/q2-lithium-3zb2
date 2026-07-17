#include "../ml_client_respawn_settle.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    pmove_state_t ordinary;
    pmove_state_t routed;
    int settling;

    memset(&ordinary, 0, sizeof(ordinary));
    memset(&routed, 0, sizeof(routed));
    ML_ClientApplyStockRespawnHold(&ordinary, qfalse);
    ML_ClientApplyStockRespawnHold(&routed, qtrue);
    assert(ordinary.pm_flags == PMF_TIME_TELEPORT);
    assert(ordinary.pm_time == ML_CLIENT_RESPAWN_TELEPORT_TIME);
    assert(routed.pm_flags == ordinary.pm_flags);
    assert(routed.pm_time == ordinary.pm_time);

    settling = ML_ClientRespawnSettlingAtEntry(
        0, qfalse, PMF_TIME_TELEPORT);
    assert(settling);
    /* Pmove may clear the live flag, but a duplicate for the same decision
       cannot erase the entry fact. */
    settling = ML_ClientRespawnSettlingAtEntry(
        settling, qtrue, 0);
    assert(settling);
    assert(!ML_ClientCausalTransitionTrainable(qtrue, qtrue, settling));

    /* Teleport PMove forces pitch to zero.  Truth echo is the actual delta,
       not the requested +2 degree policy delta. */
    assert(ML_ClientActualPitchDelta(0.0f, 0.0f) == 0.0f);

    settling = ML_ClientRespawnSettlingAtEntry(0, qfalse, 0);
    assert(!settling);
    assert(ML_ClientCausalTransitionTrainable(qtrue, qtrue, settling));
    /* pm_time is shared by landing/waterjump timers; only the teleport flag
       owns pitch suppression and settle admission. */
    settling = ML_ClientRespawnSettlingAtEntry(
        0, qfalse, PMF_TIME_LAND);
    assert(!settling);
    assert(ML_ClientCausalTransitionTrainable(qtrue, qtrue, settling));
    return 0;
}
