/* Authoritative ordinary-player role facts for routed network ML clients. */
#ifndef ML_CLIENT_ROLE_H
#define ML_CLIENT_ROLE_H

#include "g_local.h"
#include "ml_bridge.h"

static inline qboolean ML_ClientRolePlayingFromFacts(
    qboolean valid_client_slot, qboolean inprocess_ml_bot,
    qboolean monster_slot, int lithium_flags, int movetype,
    qboolean has_chase_target, qboolean pers_spectator,
    qboolean resp_spectator)
{
    return valid_client_slot && !inprocess_ml_bot && !monster_slot &&
        (lithium_flags & LITHIUM_PLAYING) &&
        !(lithium_flags & LITHIUM_OBSERVER) &&
        movetype != MOVETYPE_NOCLIP &&
        !has_chase_target &&
        !pers_spectator && !resp_spectator;
}

static inline uint32_t ML_ClientRoleCausalFlagsFromFacts(
    qboolean valid_client_slot, qboolean inprocess_ml_bot,
    qboolean monster_slot, int lithium_flags, int movetype,
    int solid, qboolean has_chase_target, qboolean pers_spectator,
    qboolean resp_spectator, int pm_type)
{
    uint32_t flags = 0;
    if (!ML_ClientRolePlayingFromFacts(valid_client_slot, inprocess_ml_bot,
        monster_slot, lithium_flags, movetype, has_chase_target,
        pers_spectator, resp_spectator))
        return 0;
    flags |= ML_CAUSAL_ROLE_PLAYING;
    /* MoveClientToIntermission intentionally makes an ordinary playing slot
       SOLID_NOT while it emits the one terminal PM_FREEZE observation.  Solid
       therefore proves an actionable public body, not the persistent routed
       player role.  A mid-run PM_NORMAL/SOLID_NOT anomaly omits this bit and
       is fatal at the client's exact public-playerstate comparison. */
    if (pm_type == PM_NORMAL && solid != SOLID_NOT)
        flags |= ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL;
    return flags;
}

static inline uint32_t ML_ClientRoleCausalFlags(const edict_t *ent)
{
    int slot;
    qboolean valid_slot;
    if (!ent || !ent->client || !ent->inuse)
        return 0;
    slot = (int)(ent - g_edicts - 1);
    valid_slot = slot >= 0 && slot < MAX_CLIENTS &&
        slot < (int)maxclients->value;
    return ML_ClientRoleCausalFlagsFromFacts(valid_slot,
        ent->client->zc.ml_enabled != 0,
        (ent->svflags & SVF_MONSTER) != 0, ent->lithium_flags,
        ent->movetype, ent->solid, ent->client->chase_target != NULL,
        ent->client->pers.spectator, ent->client->resp.spectator,
        ent->client->ps.pmove.pm_type);
}

#endif
