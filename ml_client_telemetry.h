#ifndef ML_CLIENT_TELEMETRY_H
#define ML_CLIENT_TELEMETRY_H

#include "g_local.h"

qboolean ML_ClientTelemetryActive(edict_t *ent);
void ML_ClientTelemetryCaptureRespawnAction(edict_t *ent);
qboolean ML_ClientTelemetryRestoreRespawnAction(edict_t *ent);
void ML_ClientTelemetryClientDisconnected(edict_t *ent);
void ML_ClientTelemetryRecordCommand(edict_t *ent, usercmd_t *ucmd);
void ML_ClientTelemetryFinalizeCommand(edict_t *ent);
void ML_ClientTelemetryApplyDeferredControls(edict_t *ent);
void ML_ClientTelemetryFrame(void);
qboolean ML_ClientTelemetryEpochDrainReady(void);
void ML_ClientTelemetryShutdown(void);

#endif
