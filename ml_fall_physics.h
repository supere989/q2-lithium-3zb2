#ifndef ML_FALL_PHYSICS_H
#define ML_FALL_PHYSICS_H

/* Side-effect-free authority for Lithium's P_FallingDamage decision and raw
 * damage law. Keep this header independent of game ABI types so the runtime
 * and offline oracle compile the exact same implementation. */

#define Q2_FALL_PLAYER_MODEL_INDEX 255
#define Q2_FALL_MOVETYPE_NOCLIP 1
#define Q2_FALL_GRAPPLE_STATE_FLY 0
#define Q2_FALL_GRAPPLE_RELEASE_GRACE_SECONDS 0.2
#define Q2_FALL_TIME_SECONDS 0.3
#define Q2_FALL_CONSTANTS_CONTRACT "player_model=255,noclip=1,grapple_fly=0,release_grace=0.2,delta_scale=0.0001,water1=0.5,water2=0.25,water3=suppress,footstep=1,short=15,damage=30,far=55,fall_value_scale=0.5,fall_value_max=40,fall_time=0.3,damage_divisor=2,df_no_falling=8"

typedef enum {
	Q2_FALL_SUPPRESS_NONE = 0,
	Q2_FALL_SUPPRESS_NOT_PLAYER_MODEL,
	Q2_FALL_SUPPRESS_NOCLIP,
	Q2_FALL_SUPPRESS_HOOK_OUT,
	Q2_FALL_SUPPRESS_AIRBORNE,
	Q2_FALL_SUPPRESS_GRAPPLE,
	Q2_FALL_SUPPRESS_UNDERWATER,
	Q2_FALL_SUPPRESS_BELOW_THRESHOLD
} q2_fall_suppression_t;

typedef enum {
	Q2_FALL_SEVERITY_NONE = 0,
	Q2_FALL_SEVERITY_FOOTSTEP,
	Q2_FALL_SEVERITY_SHORT,
	Q2_FALL_SEVERITY_FALL,
	Q2_FALL_SEVERITY_FAR
} q2_fall_severity_t;

typedef struct {
	float old_velocity_z;
	float velocity_z;
	float grapple_release_elapsed;
	float damage_mod;
	int modelindex;
	int movetype;
	int grounded;
	int hook_out;
	int grapple_present;
	int grapple_state;
	int waterlevel;
	int deathmatch;
	int dmflags;
	int health;
} q2_fall_input_t;

typedef struct {
	q2_fall_suppression_t suppression;
	q2_fall_severity_t severity;
	float delta;
	float fall_value;
	float fall_time_offset;
	int emit_event;
	int set_fall_state;
	int set_pain_debounce;
	int damage;
	int apply_damage;
	int unmitigated_health_after;
	int unmitigated_lethal;
} q2_fall_result_t;

void Q2_FallEvaluate(const q2_fall_input_t *input, q2_fall_result_t *result);
const char *Q2_FallSuppressionName(q2_fall_suppression_t suppression);
const char *Q2_FallSeverityName(q2_fall_severity_t severity);

#endif
