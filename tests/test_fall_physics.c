#include "../ml_fall_physics.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static q2_fall_input_t
base_input(float old_velocity_z)
{
	q2_fall_input_t input;
	memset(&input, 0, sizeof(input));
	input.old_velocity_z = old_velocity_z;
	input.velocity_z = 0;
	input.grapple_release_elapsed = 1;
	input.damage_mod = 1;
	input.modelindex = Q2_FALL_PLAYER_MODEL_INDEX;
	input.movetype = 4;
	input.grounded = 1;
	input.waterlevel = 0;
	input.deathmatch = 1;
	input.health = 100;
	return input;
}

static void
evaluate(float velocity, q2_fall_result_t *result)
{
	q2_fall_input_t input = base_input(velocity);
	Q2_FallEvaluate(&input, result);
}

int
main(void)
{
	const float velocities[] = {-300, -400, -600, -1000, -1600};
	const q2_fall_severity_t severities[] = {
		Q2_FALL_SEVERITY_FOOTSTEP, Q2_FALL_SEVERITY_SHORT,
		Q2_FALL_SEVERITY_FALL, Q2_FALL_SEVERITY_FAR,
		Q2_FALL_SEVERITY_FAR
	};
	const int damages[] = {0, 0, 3, 35, 113};
	q2_fall_input_t input;
	q2_fall_result_t result;
	int i;

	for (i = 0; i < 5; ++i) {
		evaluate(velocities[i], &result);
		assert(result.suppression == Q2_FALL_SUPPRESS_NONE);
		assert(result.severity == severities[i]);
		assert(result.damage == damages[i]);
	}

	input = base_input(-1000);
	input.waterlevel = 1;
	Q2_FallEvaluate(&input, &result);
	assert(result.delta == 50 && result.damage == 10);
	input.waterlevel = 2;
	Q2_FallEvaluate(&input, &result);
	assert(result.delta == 25 && result.severity == Q2_FALL_SEVERITY_SHORT);
	input.waterlevel = 3;
	Q2_FallEvaluate(&input, &result);
	assert(result.suppression == Q2_FALL_SUPPRESS_UNDERWATER);

	input = base_input(-1000);
	input.damage_mod = 1.5f;
	input.health = 52;
	Q2_FallEvaluate(&input, &result);
	assert(result.damage == 52);
	assert(result.unmitigated_health_after == 0);
	assert(result.unmitigated_lethal);
	input.health = 53;
	Q2_FallEvaluate(&input, &result);
	assert(result.unmitigated_health_after == 1);
	assert(!result.unmitigated_lethal);

	input.dmflags = 8;
	Q2_FallEvaluate(&input, &result);
	assert(result.damage == 52);
	assert(!result.apply_damage);
	assert(result.unmitigated_health_after == 53);

	input = base_input(-600);
	input.grounded = 0;
	input.velocity_z = -500;
	Q2_FallEvaluate(&input, &result);
	assert(result.damage == 3);
	input.velocity_z = -700;
	Q2_FallEvaluate(&input, &result);
	assert(result.suppression == Q2_FALL_SUPPRESS_AIRBORNE);

	input = base_input(-1000);
	input.grapple_release_elapsed = 0.1f;
	Q2_FallEvaluate(&input, &result);
	assert(result.suppression == Q2_FALL_SUPPRESS_GRAPPLE);
	input.grapple_release_elapsed = 1;
	input.grapple_present = 1;
	input.grapple_state = 1;
	Q2_FallEvaluate(&input, &result);
	assert(result.suppression == Q2_FALL_SUPPRESS_GRAPPLE);

	assert(!strcmp(Q2_FallSeverityName(Q2_FALL_SEVERITY_FAR), "far"));
	assert(!strcmp(Q2_FallSuppressionName(Q2_FALL_SUPPRESS_HOOK_OUT), "hook_out"));
	puts("shared fall physics vectors, water, grapple, flags, and lethality: ok");
	return 0;
}
