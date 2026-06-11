#ifndef ML_SENSORS_H
#define ML_SENSORS_H

#include "g_local.h"

/* Returns 0 when no target ray is clear, otherwise a compatibility-preserving
 * exposure score above 0.5. Existing Python checks still work as binary
 * visibility, while future policies can use the fractional score. */
float ML_TargetExposure(edict_t *ent, edict_t *other);

#endif /* ML_SENSORS_H */
