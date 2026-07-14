#include "ml_hook_physics.h"

#include <math.h>

q2_hook_touch_action_t
Q2_HookClassifyTouch(int target_is_owner, int owner_has_client,
	int target_is_nonblocking, int target_is_flymissile,
	int surface_is_sky, int hook_sky_enabled)
{
	if (target_is_owner)
		return Q2_HOOK_TOUCH_IGNORE_OWNER;
	if (!owner_has_client)
		return Q2_HOOK_TOUCH_IGNORE_INVALID_OWNER;
	if (target_is_nonblocking || target_is_flymissile)
		return Q2_HOOK_TOUCH_IGNORE_NONBLOCKING;
	if (surface_is_sky && !hook_sky_enabled)
		return Q2_HOOK_TOUCH_RESET_SKY;
	return Q2_HOOK_TOUCH_ATTACH;
}

void
Q2_HookLaunchVelocity(const float forward[3], float hook_speed,
	float velocity[3])
{
	velocity[0] = forward[0] * hook_speed;
	velocity[1] = forward[1] * hook_speed;
	velocity[2] = forward[2] * hook_speed;
}

float
Q2_HookPullVelocity(const float owner_origin[3], const float hook_origin[3],
	const float enemy_origin[3], int enemy_is_client, float hook_pullspeed,
	float velocity[3])
{
	const float *target = enemy_is_client ? enemy_origin : hook_origin;
	float length, inverse_length;

	velocity[0] = target[0] - owner_origin[0];
	velocity[1] = target[1] - owner_origin[1];
	velocity[2] = target[2] - owner_origin[2];
	length = velocity[0] * velocity[0] +
		velocity[1] * velocity[1] + velocity[2] * velocity[2];
	length = sqrt(length);
	if (length) {
		inverse_length = 1 / length;
		velocity[0] *= inverse_length;
		velocity[1] *= inverse_length;
		velocity[2] *= inverse_length;
	}
	velocity[0] *= hook_pullspeed;
	velocity[1] *= hook_pullspeed;
	velocity[2] *= hook_pullspeed;
	return length;
}

void
Q2_HookBackoffOrigin(float hook_origin[3], const float forward[3],
	float distance)
{
	hook_origin[0] += -distance * forward[0];
	hook_origin[1] += -distance * forward[1];
	hook_origin[2] += -distance * forward[2];
}

const char *
Q2_HookTouchActionName(q2_hook_touch_action_t action)
{
	switch (action) {
	case Q2_HOOK_TOUCH_ATTACH: return "attach";
	case Q2_HOOK_TOUCH_IGNORE_OWNER: return "ignore_owner";
	case Q2_HOOK_TOUCH_IGNORE_INVALID_OWNER: return "ignore_invalid_owner";
	case Q2_HOOK_TOUCH_IGNORE_NONBLOCKING: return "ignore_nonblocking";
	case Q2_HOOK_TOUCH_RESET_SKY: return "reset_sky";
	default: return "invalid";
	}
}
