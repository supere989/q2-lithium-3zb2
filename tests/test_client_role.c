#include "../ml_client_role.h"

#include <assert.h>

static uint32_t role(int flags, int movetype, int pers_spectator,
    int resp_spectator, int pm_type)
{
    return ML_ClientRoleCausalFlagsFromFacts(qtrue, qfalse, qfalse,
        flags, movetype, SOLID_BBOX, qfalse, pers_spectator,
        resp_spectator, pm_type);
}

int main(void)
{
    assert(role(LITHIUM_PLAYING, MOVETYPE_WALK, 0, 0, PM_NORMAL) ==
        (ML_CAUSAL_ROLE_PLAYING | ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL));
    assert(role(LITHIUM_PLAYING, MOVETYPE_WALK, 0, 0, PM_DEAD) ==
        ML_CAUSAL_ROLE_PLAYING);
    assert(role(LITHIUM_PLAYING, MOVETYPE_WALK, 0, 0, PM_GIB) ==
        ML_CAUSAL_ROLE_PLAYING);
    assert(role(LITHIUM_PLAYING, MOVETYPE_WALK, 0, 0, PM_FREEZE) ==
        ML_CAUSAL_ROLE_PLAYING);

    assert(!role(0, MOVETYPE_WALK, 0, 0, PM_NORMAL));
    assert(!role(LITHIUM_PLAYING | LITHIUM_OBSERVER,
        MOVETYPE_WALK, 0, 0, PM_NORMAL));
    assert(!role(LITHIUM_PLAYING, MOVETYPE_NOCLIP, 0, 0, PM_SPECTATOR));
    assert(!role(LITHIUM_PLAYING, MOVETYPE_WALK, 1, 0, PM_NORMAL));
    assert(!role(LITHIUM_PLAYING, MOVETYPE_WALK, 0, 1, PM_NORMAL));
    assert(ML_ClientRoleCausalFlagsFromFacts(qtrue, qfalse, qfalse,
        LITHIUM_PLAYING, MOVETYPE_WALK, SOLID_NOT, qfalse, qfalse,
        qfalse, PM_NORMAL) == ML_CAUSAL_ROLE_PLAYING);
    assert(ML_ClientRoleCausalFlagsFromFacts(qtrue, qfalse, qfalse,
        LITHIUM_PLAYING, MOVETYPE_WALK, SOLID_NOT, qfalse, qfalse,
        qfalse, PM_FREEZE) == ML_CAUSAL_ROLE_PLAYING);
    assert(!ML_ClientRoleCausalFlagsFromFacts(qtrue, qfalse, qfalse,
        LITHIUM_PLAYING, MOVETYPE_WALK, SOLID_BBOX, qtrue, qfalse,
        qfalse, PM_NORMAL));
    assert(!ML_ClientRoleCausalFlagsFromFacts(qfalse, qfalse, qfalse,
        LITHIUM_PLAYING, MOVETYPE_WALK, SOLID_BBOX, qfalse, qfalse,
        qfalse, PM_NORMAL));
    assert(!ML_ClientRoleCausalFlagsFromFacts(qtrue, qtrue, qfalse,
        LITHIUM_PLAYING, MOVETYPE_WALK, SOLID_BBOX, qfalse, qfalse,
        qfalse, PM_NORMAL));
    assert(!ML_ClientRoleCausalFlagsFromFacts(qtrue, qfalse, qtrue,
        LITHIUM_PLAYING, MOVETYPE_WALK, SOLID_BBOX, qfalse, qfalse,
        qfalse, PM_NORMAL));
    return 0;
}
