#ifndef ML_SENSORS_H
#define ML_SENSORS_H

#include "g_local.h"

/* Eye origin shared by exposure, observation packing, and fire gating. */
void ML_TargetEyePoint(edict_t *ent, vec3_t out_eye);

/* Select the first damageable point in torso/head/hips/legs/lateral order.
 * A point must be clear from both the eye and the common weapon muzzle. Returns
 * qtrue for at least one clear pair and writes the exact clear-probe fraction. */
qboolean ML_TargetAcquire(edict_t *ent, edict_t *other,
                          vec3_t out_point, float *out_exposure);

/* Compatibility wrapper for callers that only need the exact exposure. */
float ML_TargetExposure(edict_t *ent, edict_t *other);

#endif /* ML_SENSORS_H */
