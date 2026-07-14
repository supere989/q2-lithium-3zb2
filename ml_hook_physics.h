#ifndef ML_HOOK_PHYSICS_H
#define ML_HOOK_PHYSICS_H

/* Shared, side-effect-free authority for Lithium's Orange 2 hook law.
 * Keep this header independent of game ABI types so the offline oracle and
 * game module compile the exact same implementation. */

typedef enum {
	Q2_HOOK_TOUCH_ATTACH = 0,
	Q2_HOOK_TOUCH_IGNORE_OWNER = 1,
	Q2_HOOK_TOUCH_IGNORE_INVALID_OWNER = 2,
	Q2_HOOK_TOUCH_IGNORE_NONBLOCKING = 3,
	Q2_HOOK_TOUCH_RESET_SKY = 4
} q2_hook_touch_action_t;

q2_hook_touch_action_t Q2_HookClassifyTouch(
	int target_is_owner,
	int owner_has_client,
	int target_is_nonblocking,
	int target_is_flymissile,
	int surface_is_sky,
	int hook_sky_enabled);

void Q2_HookLaunchVelocity(const float forward[3], float hook_speed,
	float velocity[3]);

/* Mirrors Hook_Service exactly: choose an attached player's origin when the
 * enemy is a client, normalize target-owner, then overwrite all velocity. */
float Q2_HookPullVelocity(const float owner_origin[3],
	const float hook_origin[3], const float enemy_origin[3],
	int enemy_is_client, float hook_pullspeed, float velocity[3]);

void Q2_HookBackoffOrigin(float hook_origin[3], const float forward[3],
	float distance);

const char *Q2_HookTouchActionName(q2_hook_touch_action_t action);

#endif
