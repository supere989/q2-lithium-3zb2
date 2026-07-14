#ifndef ML_OBS_H
#define ML_OBS_H

#include "ml_bridge.h"

void ML_PackObs(edict_t *ent, ml_obs_t *obs);
void ML_ApplyAction(edict_t *ent, const ml_action_t *act);
qboolean ML_HasEngageableTarget(edict_t *ent, const vec3_t view_angles);

#endif
