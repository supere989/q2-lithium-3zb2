/*
 * ml_sensors.c - game-state sensors for ML observations.
 *
 * Keep target acquisition, exposure, sound, lighting, and future spatial
 * sensing here so observation packing stays a serialization layer.
 */

#include "g_local.h"
#include "ml_sensors.h"

static qboolean ML_TargetRayClear(edict_t *ent, edict_t *other,
							  vec3_t start, vec3_t end)
{
	trace_t tr;

	tr = gi.trace(start, NULL, NULL, end, ent, MASK_SHOT);
	if (tr.fraction == 1.0f)
		return qtrue;
	if (tr.ent == other)
		return qtrue;
	return qfalse;
}

void ML_TargetEyePoint(edict_t *ent, vec3_t out_eye)
{
	if (!out_eye)
		return;
	VectorClear(out_eye);
	if (!ent)
		return;
	VectorCopy(ent->s.origin, out_eye);
	out_eye[2] += ent->viewheight;
}

qboolean ML_TargetAcquire(edict_t *ent, edict_t *other,
						  vec3_t out_point, float *out_exposure)
{
	vec3_t eye, muzzle, muzzle_offset;
	vec3_t samples[6];
	vec3_t rel, lateral;
	vec3_t forward, right;
	float len;
	int clear = 0;
	int count = 0;
	int best = -1;
	int i;

	if (out_point)
		VectorClear(out_point);
	if (out_exposure)
		*out_exposure = 0.0f;

	if (!ent || !ent->client || !other || !other->client)
		return qfalse;

	ML_TargetEyePoint(ent, eye);
	/* Quake weapons use a small family of muzzle offsets.  The common
	   zero-forward/eight-right offset is the conservative obstruction probe:
	   a sample is damageable only when both the view ray and weapon-side ray
	   can reach it.  Weapon-specific projectile intercept remains a later
	   refinement; the authoritative aim vector stays eye-to-damage-point. */
	AngleVectors(ent->client->v_angle, forward, right, NULL);
	VectorSet(muzzle_offset, 0.0f, 8.0f, ent->viewheight - 8.0f);
	P_ProjectSource(ent->client, ent->s.origin, muzzle_offset,
					forward, right, muzzle);

	VectorSubtract(other->s.origin, ent->s.origin, rel);
	VectorSet(lateral, -rel[1], rel[0], 0.0f);
	len = VectorLength(lateral);
	if (len > 0.001f)
		VectorScale(lateral, 1.0f / len, lateral);
	else
		VectorClear(lateral);

	/* The first clear sample is the authoritative aim point.  Keep torso
	   first for a stable, high-damage target, then fall back to exposed body
	   regions rather than aiming at an occluded origin. */
	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += other->viewheight - 12;
	count++;

	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += other->maxs[2] - 4.0f;
	count++;

	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += (other->mins[2] + other->maxs[2]) * 0.5f;
	count++;

	VectorCopy(other->s.origin, samples[count]);
	samples[count][2] += other->mins[2] + 16.0f;
	count++;

	VectorCopy(samples[0], samples[count]);
	VectorMA(samples[count], 12.0f, lateral, samples[count]);
	count++;

	VectorCopy(samples[0], samples[count]);
	VectorMA(samples[count], -12.0f, lateral, samples[count]);
	count++;

	for (i = 0; i < count; i++)
	{
		if (ML_TargetRayClear(ent, other, eye, samples[i]) &&
			ML_TargetRayClear(ent, other, muzzle, samples[i]))
		{
			if (best < 0)
				best = i;
			clear++;
		}
	}

	if (clear <= 0)
		return qfalse;

	if (out_point)
		VectorCopy(samples[best], out_point);
	if (out_exposure)
		*out_exposure = (float)clear / (float)count;
	return qtrue;
}

float ML_TargetExposure(edict_t *ent, edict_t *other)
{
	float exposure = 0.0f;

	ML_TargetAcquire(ent, other, NULL, &exposure);
	return exposure;
}
