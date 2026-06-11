/*
 * ml_sensors.c - game-state sensors for ML observations.
 *
 * Keep target acquisition, exposure, sound, lighting, and future spatial
 * sensing here so observation packing stays a serialization layer.
 */

#include "g_local.h"
#include "ml_sensors.h"

static qboolean ML_TargetRayClear(edict_t *ent, edict_t *other, vec3_t start, vec3_t end)
{
	trace_t tr;

	tr = gi.trace(start, NULL, NULL, end, ent,
	              CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_LAVA | CONTENTS_SLIME);
	if (tr.fraction == 1.0f)
		return qtrue;
	if (tr.ent == other)
		return qtrue;
	return qfalse;
}

float ML_TargetExposure(edict_t *ent, edict_t *other)
{
	vec3_t start;
	vec3_t samples[6];
	vec3_t rel, lateral;
	float len;
	int clear = 0;
	int count = 0;

	if (!ent || !ent->client || !other || !other->client)
		return 0.0f;

	VectorCopy(ent->s.origin, start);
	start[2] += ent->viewheight - 8;

	VectorSubtract(other->s.origin, ent->s.origin, rel);
	VectorSet(lateral, -rel[1], rel[0], 0.0f);
	len = VectorLength(lateral);
	if (len > 0.001f)
		VectorScale(lateral, 1.0f / len, lateral);
	else
		VectorClear(lateral);

	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += other->viewheight - 8;
	count++;

	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += (other->mins[2] + other->maxs[2]) * 0.5f;
	count++;

	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += other->maxs[2] - 4.0f;
	count++;

	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += other->mins[2] + 16.0f;
	count++;

	VectorCopy(samples[1], samples[count]);
	VectorMA(samples[count], 12.0f, lateral, samples[count]);
	count++;

	VectorCopy(samples[1], samples[count]);
	VectorMA(samples[count], -12.0f, lateral, samples[count]);
	count++;

	for (int i = 0; i < count; i++)
	{
		if (ML_TargetRayClear(ent, other, start, samples[i]))
			clear++;
	}

	if (clear <= 0)
		return 0.0f;

	return 0.55f + 0.45f * ((float)clear / (float)count);
}
