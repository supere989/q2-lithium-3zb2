#ifndef ML_CLIENT_TELEMETRY_H
#define ML_CLIENT_TELEMETRY_H

#include "g_local.h"

qboolean ML_ClientTelemetryActive(edict_t *ent);
void ML_ClientTelemetryClientDisconnected(edict_t *ent);
void ML_ClientTelemetryRecordCommand(edict_t *ent, usercmd_t *ucmd);
void ML_ClientTelemetryFrame(void);
void ML_ClientTelemetryShutdown(void);

#endif
