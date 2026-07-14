#include "ml_fall_physics.h"

#include <limits.h>
#include <string.h>

#define Q2_FALL_DF_NO_FALLING 8

static int
project_health(int health, int damage)
{
	long long projected = (long long)health - (long long)damage;
	if (projected < INT_MIN)
		return INT_MIN;
	if (projected > INT_MAX)
		return INT_MAX;
	return (int)projected;
}

void
Q2_FallEvaluate(const q2_fall_input_t *input, q2_fall_result_t *result)
{
	float delta;
	int damage;

	memset(result, 0, sizeof(*result));
	result->unmitigated_health_after = input->health;
	if (input->modelindex != Q2_FALL_PLAYER_MODEL_INDEX) {
		result->suppression = Q2_FALL_SUPPRESS_NOT_PLAYER_MODEL;
		return;
	}
	if (input->movetype == Q2_FALL_MOVETYPE_NOCLIP) {
		result->suppression = Q2_FALL_SUPPRESS_NOCLIP;
		return;
	}
	if (input->hook_out && !input->grounded) {
		result->suppression = Q2_FALL_SUPPRESS_HOOK_OUT;
		return;
	}
	if ((input->old_velocity_z < 0.0f) &&
		(input->velocity_z > input->old_velocity_z) && !input->grounded) {
		delta = input->old_velocity_z;
	} else {
		if (!input->grounded) {
			result->suppression = Q2_FALL_SUPPRESS_AIRBORNE;
			return;
		}
		delta = input->velocity_z - input->old_velocity_z;
	}
	delta = delta * delta * 0.0001;
	result->delta = delta;

	if (input->grapple_release_elapsed <=
		Q2_FALL_GRAPPLE_RELEASE_GRACE_SECONDS ||
		(input->grapple_present &&
		 input->grapple_state > Q2_FALL_GRAPPLE_STATE_FLY)) {
		result->suppression = Q2_FALL_SUPPRESS_GRAPPLE;
		return;
	}
	if (input->waterlevel == 3) {
		result->suppression = Q2_FALL_SUPPRESS_UNDERWATER;
		return;
	}
	if (input->waterlevel == 2)
		delta *= 0.25;
	if (input->waterlevel == 1)
		delta *= 0.5;
	result->delta = delta;

	if (delta < 1.0f) {
		result->suppression = Q2_FALL_SUPPRESS_BELOW_THRESHOLD;
		return;
	}
	if (delta < 15.0f) {
		result->severity = Q2_FALL_SEVERITY_FOOTSTEP;
		result->emit_event = 1;
		return;
	}

	result->set_fall_state = 1;
	result->fall_value = delta * 0.5;
	if (result->fall_value > 40.0f)
		result->fall_value = 40.0f;
	result->fall_time_offset = Q2_FALL_TIME_SECONDS;

	if (delta > 30.0f) {
		result->severity = delta >= 55.0f ?
			Q2_FALL_SEVERITY_FAR : Q2_FALL_SEVERITY_FALL;
		result->emit_event = input->health > 0;
		result->set_pain_debounce = 1;
		damage = (delta - 30.0f) / 2.0f;
		if (damage < 1)
			damage = 1;
		damage = damage * input->damage_mod;
		result->damage = damage;
		result->apply_damage = !input->deathmatch ||
			!(input->dmflags & Q2_FALL_DF_NO_FALLING);
		result->unmitigated_health_after = result->apply_damage ?
			project_health(input->health, damage) : input->health;
		result->unmitigated_lethal = result->apply_damage && damage > 0 &&
			result->unmitigated_health_after <= 0;
		return;
	}

	result->severity = Q2_FALL_SEVERITY_SHORT;
	result->emit_event = 1;
}

const char *
Q2_FallSuppressionName(q2_fall_suppression_t suppression)
{
	switch (suppression) {
	case Q2_FALL_SUPPRESS_NONE: return "none";
	case Q2_FALL_SUPPRESS_NOT_PLAYER_MODEL: return "not_player_model";
	case Q2_FALL_SUPPRESS_NOCLIP: return "noclip";
	case Q2_FALL_SUPPRESS_HOOK_OUT: return "hook_out";
	case Q2_FALL_SUPPRESS_AIRBORNE: return "airborne";
	case Q2_FALL_SUPPRESS_GRAPPLE: return "grapple";
	case Q2_FALL_SUPPRESS_UNDERWATER: return "underwater";
	case Q2_FALL_SUPPRESS_BELOW_THRESHOLD: return "below_threshold";
	default: return "invalid";
	}
}

const char *
Q2_FallSeverityName(q2_fall_severity_t severity)
{
	switch (severity) {
	case Q2_FALL_SEVERITY_NONE: return "none";
	case Q2_FALL_SEVERITY_FOOTSTEP: return "footstep";
	case Q2_FALL_SEVERITY_SHORT: return "short";
	case Q2_FALL_SEVERITY_FALL: return "fall";
	case Q2_FALL_SEVERITY_FAR: return "far";
	default: return "invalid";
	}
}
