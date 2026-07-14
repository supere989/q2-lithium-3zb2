#include "../ml_fall_physics.h"
#include "fall_oracle_identity.h"
#include "oracle_sha256.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_BYTES (1024 * 1024)
#define VELOCITY_LIMIT 32768.0
#define ELAPSED_LIMIT 86400.0
#define DAMAGE_MOD_LIMIT 1000.0
#define HEALTH_LIMIT 1000000

typedef enum {
	FIELD_ID = 0,
	FIELD_OP,
	FIELD_OLD_VELOCITY_Z,
	FIELD_VELOCITY_Z,
	FIELD_GRAPPLE_RELEASE_ELAPSED,
	FIELD_FALL_DAMAGEMOD,
	FIELD_MODELINDEX,
	FIELD_MOVETYPE,
	FIELD_GROUNDED,
	FIELD_HOOK_OUT,
	FIELD_GRAPPLE_PRESENT,
	FIELD_GRAPPLE_STATE,
	FIELD_WATERLEVEL,
	FIELD_DEATHMATCH,
	FIELD_DMFLAGS,
	FIELD_HEALTH,
	FIELD_COUNT
} field_t;

typedef struct {
	const char *begin;
	const char *end;
} span_t;

typedef struct {
	span_t values[FIELD_COUNT];
	uint32_t present;
} object_t;

typedef struct {
	float damage_mod;
	int deathmatch;
	int dmflags;
} parameters_t;

static const char *const field_names[FIELD_COUNT] = {
	"id", "op", "old_velocity_z", "velocity_z",
	"grapple_release_elapsed", "fall_damagemod", "modelindex", "movetype",
	"grounded", "hook_out", "grapple_present", "grapple_state",
	"waterlevel", "deathmatch", "dmflags", "health"
};

static const char *
skip_space(const char *p)
{
	while (p && *p && isspace((unsigned char)*p))
		++p;
	return p;
}

static int
field_for_name(const char *name)
{
	int field;
	for (field = 0; field < FIELD_COUNT; ++field) {
		if (!strcmp(name, field_names[field]))
			return field;
	}
	return -1;
}

static const char *
skip_string(const char *p)
{
	if (*p++ != '"')
		return NULL;
	while (*p) {
		if ((unsigned char)*p < 0x20)
			return NULL;
		if (*p == '"')
			return p + 1;
		if (*p == '\\') {
			++p;
			if (!*p)
				return NULL;
			if (*p == 'u') {
				int i;
				for (i = 0; i < 4; ++i) {
					++p;
					if (!isxdigit((unsigned char)*p))
						return NULL;
				}
			} else if (!strchr("\"\\/bfnrt", *p)) {
				return NULL;
			}
		}
		++p;
	}
	return NULL;
}

static const char *
skip_value(const char *p)
{
	const char *start;
	int array_depth = 0, object_depth = 0;

	p = skip_space(p);
	if (*p == '"')
		return skip_string(p);
	if (*p == '[' || *p == '{') {
		do {
			if (*p == '"') {
				p = skip_string(p);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '[') ++array_depth;
			else if (*p == ']') --array_depth;
			else if (*p == '{') ++object_depth;
			else if (*p == '}') --object_depth;
			if (array_depth < 0 || object_depth < 0)
				return NULL;
			++p;
		} while (*p && (array_depth || object_depth));
		return (array_depth || object_depth) ? NULL : p;
	}
	start = p;
	while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p))
		++p;
	return p == start ? NULL : p;
}

static int
parse_object(const char *json, object_t *object)
{
	const char *p = skip_space(json);
	memset(object, 0, sizeof(*object));
	if (*p++ != '{')
		return 0;
	p = skip_space(p);
	if (*p == '}')
		return skip_space(p + 1)[0] == '\0';
	for (;;) {
		char key[64];
		const char *key_end, *value_end;
		size_t length;
		int field;

		if (*p != '"')
			return 0;
		key_end = strchr(p + 1, '"');
		if (!key_end || memchr(p + 1, '\\', (size_t)(key_end - p - 1)))
			return 0;
		length = (size_t)(key_end - p - 1);
		if (!length || length >= sizeof(key))
			return 0;
		memcpy(key, p + 1, length);
		key[length] = '\0';
		field = field_for_name(key);
		if (field < 0 || (object->present & (1u << field)))
			return 0;
		p = skip_space(key_end + 1);
		if (*p++ != ':')
			return 0;
		p = skip_space(p);
		value_end = skip_value(p);
		if (!value_end)
			return 0;
		object->values[field].begin = p;
		object->values[field].end = value_end;
		object->present |= 1u << field;
		p = skip_space(value_end);
		if (*p == '}')
			return skip_space(p + 1)[0] == '\0';
		if (*p++ != ',')
			return 0;
		p = skip_space(p);
	}
}

static int
parse_string(span_t span, char *output, size_t size)
{
	const char *p = span.begin;
	size_t used = 0;
	if (!p || p >= span.end || *p++ != '"' || !size)
		return 0;
	while (p < span.end && *p != '"') {
		unsigned char character = (unsigned char)*p++;
		if (character == '\\') {
			if (p >= span.end)
				return 0;
			character = (unsigned char)*p++;
			switch (character) {
			case '"': case '\\': case '/': break;
			case 'b': character = '\b'; break;
			case 'f': character = '\f'; break;
			case 'n': character = '\n'; break;
			case 'r': character = '\r'; break;
			case 't': character = '\t'; break;
			default: return 0;
			}
		}
		if (character < 0x20 || used + 1 >= size)
			return 0;
		output[used++] = (char)character;
	}
	if (p >= span.end || *p++ != '"' || p != span.end)
		return 0;
	output[used] = '\0';
	return 1;
}

static int
parse_number(span_t span, double *output)
{
	const char *syntax;
	char *end;
	if (!span.begin || !output)
		return 0;
	syntax = span.begin;
	if (syntax < span.end && *syntax == '-')
		++syntax;
	if (syntax >= span.end)
		return 0;
	if (*syntax == '0') {
		++syntax;
		if (syntax < span.end && isdigit((unsigned char)*syntax))
			return 0;
	} else {
		if (*syntax < '1' || *syntax > '9')
			return 0;
		while (syntax < span.end && isdigit((unsigned char)*syntax))
			++syntax;
	}
	if (syntax < span.end && *syntax == '.') {
		++syntax;
		if (syntax >= span.end || !isdigit((unsigned char)*syntax))
			return 0;
		while (syntax < span.end && isdigit((unsigned char)*syntax))
			++syntax;
	}
	if (syntax < span.end && (*syntax == 'e' || *syntax == 'E')) {
		++syntax;
		if (syntax < span.end && (*syntax == '+' || *syntax == '-'))
			++syntax;
		if (syntax >= span.end || !isdigit((unsigned char)*syntax))
			return 0;
		while (syntax < span.end && isdigit((unsigned char)*syntax))
			++syntax;
	}
	if (syntax != span.end)
		return 0;
	errno = 0;
	*output = strtod(span.begin, &end);
	return end != span.begin && end == span.end && errno != ERANGE &&
		isfinite(*output);
}

static int
parse_int(span_t span, int *output)
{
	double number;
	if (!parse_number(span, &number) || number < INT_MIN || number > INT_MAX ||
		number != (double)(int)number)
		return 0;
	*output = (int)number;
	return 1;
}

static int
parse_bool(span_t span, int *output)
{
	size_t length;
	if (!span.begin || !output)
		return 0;
	length = (size_t)(span.end - span.begin);
	if (length == 4 && !memcmp(span.begin, "true", 4)) {
		*output = 1;
		return 1;
	}
	if (length == 5 && !memcmp(span.begin, "false", 5)) {
		*output = 0;
		return 1;
	}
	return 0;
}

static void
print_string(const char *value)
{
	fputc('"', stdout);
	for (; *value; ++value) {
		switch (*value) {
		case '"': case '\\': fputc('\\', stdout); fputc(*value, stdout); break;
		case '\b': fputs("\\b", stdout); break;
		case '\f': fputs("\\f", stdout); break;
		case '\n': fputs("\\n", stdout); break;
		case '\r': fputs("\\r", stdout); break;
		case '\t': fputs("\\t", stdout); break;
		default: fputc(*value, stdout); break;
		}
	}
	fputc('"', stdout);
}

static void
print_error(const char *id, const char *error, const char *detail)
{
	fputs("{\"ok\":false,\"id\":", stdout); print_string(id);
	fputs(",\"error\":", stdout); print_string(error);
	fputs(",\"detail\":", stdout); print_string(detail); fputs("}\n", stdout);
}

static int
parse_parameters(const object_t *object, parameters_t *parameters, int required)
{
	double number;
	parameters->damage_mod = 1.0f;
	parameters->deathmatch = 1;
	parameters->dmflags = 0;
	if (object->present & (1u << FIELD_FALL_DAMAGEMOD)) {
		if (!parse_number(object->values[FIELD_FALL_DAMAGEMOD], &number) ||
			number < 0 || number > DAMAGE_MOD_LIMIT)
			return 0;
		parameters->damage_mod = (float)number;
	} else if (required) return 0;
	if (object->present & (1u << FIELD_DEATHMATCH)) {
		if (!parse_bool(object->values[FIELD_DEATHMATCH], &parameters->deathmatch))
			return 0;
	} else if (required) return 0;
	if (object->present & (1u << FIELD_DMFLAGS)) {
		if (!parse_int(object->values[FIELD_DMFLAGS], &parameters->dmflags) ||
			parameters->dmflags < 0)
			return 0;
	} else if (required) return 0;
	return 1;
}

static void
physics_identity(const parameters_t *parameters, char output[65])
{
	char canonical[1536];
	uint8_t hash[32];
	hook_sha256_ctx_t context;
	snprintf(canonical, sizeof(canonical),
		"schema=q2-fall-oracle-v1;tool=%s;shared_c=%s;shared_h=%s;integration=%s;"
		"game_header=%s;constants_sha256=%s;constants=%s;build=%s;"
		"fall_damagemod=%.9g;deathmatch=%d;dmflags=%d",
		Q2_FALL_TOOL_CLOSURE_SHA256, Q2_FALL_SHARED_C_SHA256,
		Q2_FALL_SHARED_H_SHA256, Q2_FALL_INTEGRATION_SHA256,
		Q2_FALL_GAME_HEADER_SHA256, Q2_FALL_CONSTANTS_SHA256,
		Q2_FALL_CONSTANTS_CONTRACT, Q2_FALL_BUILD_CONTRACT,
		parameters->damage_mod, parameters->deathmatch, parameters->dmflags);
	hook_sha256_init(&context);
	hook_sha256_update(&context, canonical, strlen(canonical));
	hook_sha256_final(&context, hash);
	hook_sha256_hex(hash, output);
}

static void
print_prefix(const char *id, const char *op, const parameters_t *parameters)
{
	char identity[65];
	physics_identity(parameters, identity);
	fputs("{\"ok\":true,\"id\":", stdout); print_string(id);
	fputs(",\"op\":", stdout); print_string(op);
	fputs(",\"schema\":\"q2-fall-oracle-v1\",\"physics_identity\":", stdout);
	print_string(identity);
	fputs(",\"tool_identity\":", stdout); print_string(Q2_FALL_TOOL_CLOSURE_SHA256);
}

static int
parse_evaluate_input(const object_t *object, const parameters_t *parameters,
	q2_fall_input_t *input)
{
	double number;
	memset(input, 0, sizeof(*input));
#define PARSE_BOUNDED_FLOAT(field, member, limit) \
	do { \
		if (!parse_number(object->values[field], &number) || \
			fabs(number) > (limit)) return 0; \
		input->member = (float)number; \
	} while (0)
	PARSE_BOUNDED_FLOAT(FIELD_OLD_VELOCITY_Z, old_velocity_z, VELOCITY_LIMIT);
	PARSE_BOUNDED_FLOAT(FIELD_VELOCITY_Z, velocity_z, VELOCITY_LIMIT);
	PARSE_BOUNDED_FLOAT(FIELD_GRAPPLE_RELEASE_ELAPSED,
		grapple_release_elapsed, ELAPSED_LIMIT);
#undef PARSE_BOUNDED_FLOAT
	input->damage_mod = parameters->damage_mod;
	input->deathmatch = parameters->deathmatch;
	input->dmflags = parameters->dmflags;
#define PARSE_INT(field, member) \
	do { if (!parse_int(object->values[field], &input->member)) return 0; } while (0)
	PARSE_INT(FIELD_MODELINDEX, modelindex);
	PARSE_INT(FIELD_MOVETYPE, movetype);
	PARSE_INT(FIELD_GRAPPLE_STATE, grapple_state);
	PARSE_INT(FIELD_WATERLEVEL, waterlevel);
	PARSE_INT(FIELD_HEALTH, health);
#undef PARSE_INT
#define PARSE_BOOL(field, member) \
	do { if (!parse_bool(object->values[field], &input->member)) return 0; } while (0)
	PARSE_BOOL(FIELD_GROUNDED, grounded);
	PARSE_BOOL(FIELD_HOOK_OUT, hook_out);
	PARSE_BOOL(FIELD_GRAPPLE_PRESENT, grapple_present);
#undef PARSE_BOOL
	return input->modelindex >= 0 && input->modelindex <= 255 &&
		input->movetype >= 0 && input->movetype <= 9 &&
		input->grapple_state >= 0 && input->grapple_state <= 2 &&
		input->waterlevel >= 0 && input->waterlevel <= 3 &&
		input->health >= -HEALTH_LIMIT && input->health <= HEALTH_LIMIT;
}

static void
print_parameters(const parameters_t *parameters)
{
	fprintf(stdout, "{\"fall_damagemod\":%.9g,\"deathmatch\":%s,\"dmflags\":%d}",
		parameters->damage_mod, parameters->deathmatch ? "true" : "false",
		parameters->dmflags);
}

static void
print_input(const q2_fall_input_t *input)
{
	fprintf(stdout, "{\"old_velocity_z\":%.9g,\"velocity_z\":%.9g,"
		"\"grapple_release_elapsed\":%.9g,\"fall_damagemod\":%.9g,"
		"\"modelindex\":%d,\"movetype\":%d,\"grounded\":%s,\"hook_out\":%s,"
		"\"grapple_present\":%s,\"grapple_state\":%d,\"waterlevel\":%d,"
		"\"deathmatch\":%s,\"dmflags\":%d,\"health\":%d}",
		input->old_velocity_z, input->velocity_z,
		input->grapple_release_elapsed, input->damage_mod,
		input->modelindex, input->movetype, input->grounded ? "true" : "false",
		input->hook_out ? "true" : "false",
		input->grapple_present ? "true" : "false", input->grapple_state,
		input->waterlevel, input->deathmatch ? "true" : "false",
		input->dmflags, input->health);
}

static void
handle_line(const char *line)
{
	const uint32_t identity_allowed = (1u << FIELD_ID) | (1u << FIELD_OP) |
		(1u << FIELD_FALL_DAMAGEMOD) | (1u << FIELD_DEATHMATCH) |
		(1u << FIELD_DMFLAGS);
	const uint32_t evaluate_required = ((1u << FIELD_COUNT) - 1u) &
		~(1u << FIELD_ID);
	object_t object;
	parameters_t parameters;
	q2_fall_input_t input;
	q2_fall_result_t result;
	char id[128] = "", op[48] = "";

	if (!parse_object(line, &object)) {
		print_error("", "invalid_request", "request must be one strict JSON object with unique known fields");
		return;
	}
	if ((object.present & (1u << FIELD_ID)) &&
		!parse_string(object.values[FIELD_ID], id, sizeof(id))) {
		print_error("", "invalid_request", "id must be a string of at most 127 bytes");
		return;
	}
	if (!(object.present & (1u << FIELD_OP)) ||
		!parse_string(object.values[FIELD_OP], op, sizeof(op))) {
		print_error(id, "invalid_request", "op string is required");
		return;
	}
	if (!strcmp(op, "identity")) {
		if (object.present & ~identity_allowed) {
			print_error(id, "invalid_request", "identity contains operation-inapplicable fields");
			return;
		}
		if (!parse_parameters(&object, &parameters, 0)) {
			print_error(id, "invalid_parameters", "fall parameters are malformed or out of range");
			return;
		}
		print_prefix(id, op, &parameters);
		fputs(",\"parameters\":", stdout); print_parameters(&parameters);
		fputs(",\"constants\":", stdout); print_string(Q2_FALL_CONSTANTS_CONTRACT);
		fputs(",\"source\":{\"shared_c_sha256\":", stdout); print_string(Q2_FALL_SHARED_C_SHA256);
		fputs(",\"shared_h_sha256\":", stdout); print_string(Q2_FALL_SHARED_H_SHA256);
		fputs(",\"integration_sha256\":", stdout); print_string(Q2_FALL_INTEGRATION_SHA256);
		fputs(",\"game_header_sha256\":", stdout); print_string(Q2_FALL_GAME_HEADER_SHA256);
		fputs(",\"constants_sha256\":", stdout); print_string(Q2_FALL_CONSTANTS_SHA256);
		fputs(",\"build_contract\":", stdout); print_string(Q2_FALL_BUILD_CONTRACT);
		fputs(",\"tool_closure_sha256\":", stdout); print_string(Q2_FALL_TOOL_CLOSURE_SHA256);
		fputs("}}\n", stdout);
		return;
	}
	if (strcmp(op, "evaluate")) {
		print_error(id, "unknown_operation", op);
		return;
	}
	if ((object.present & evaluate_required) != evaluate_required) {
		print_error(id, "invalid_request", "evaluate requires every state and parameter field");
		return;
	}
	if (!parse_parameters(&object, &parameters, 1) ||
		!parse_evaluate_input(&object, &parameters, &input)) {
		print_error(id, "invalid_input", "evaluate fields are malformed, nonfinite, or out of range");
		return;
	}
	Q2_FallEvaluate(&input, &result);
	print_prefix(id, op, &parameters);
	fputs(",\"input\":", stdout); print_input(&input);
	fputs(",\"suppression\":", stdout); print_string(Q2_FallSuppressionName(result.suppression));
	fputs(",\"severity\":", stdout); print_string(Q2_FallSeverityName(result.severity));
	fprintf(stdout, ",\"delta\":%.9g,\"fall_value\":%.9g,\"fall_time_offset\":%.9g,"
		"\"emit_event\":%s,\"set_fall_state\":%s,\"set_pain_debounce\":%s,"
		"\"damage\":%d,\"apply_damage\":%s,\"unmitigated_health_after\":%d,"
		"\"unmitigated_lethal\":%s}\n",
		result.delta, result.fall_value, result.fall_time_offset,
		result.emit_event ? "true" : "false",
		result.set_fall_state ? "true" : "false",
		result.set_pain_debounce ? "true" : "false", result.damage,
		result.apply_damage ? "true" : "false",
		result.unmitigated_health_after,
		result.unmitigated_lethal ? "true" : "false");
}

int
main(void)
{
	char *line = malloc(LINE_MAX_BYTES);
	if (!line)
		return 70;
	while (fgets(line, LINE_MAX_BYTES, stdin)) {
		if (!strchr(line, '\n') && !feof(stdin)) {
			int character;
			print_error("", "request_too_large", "NDJSON line exceeds one MiB");
			while ((character = fgetc(stdin)) != '\n' && character != EOF) { }
			continue;
		}
		handle_line(line);
		fflush(stdout);
	}
	free(line);
	return ferror(stdin) ? 74 : 0;
}
