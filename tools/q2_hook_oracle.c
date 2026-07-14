#include "../ml_hook_physics.h"
#include "hook_oracle_identity.h"
#include "oracle_sha256.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_BYTES (1024 * 1024)

typedef struct {
	float speed;
	float pullspeed;
	float maxtime;
	int sky;
} parameters_t;

static const char *skip_space(const char *p)
{
	while (p && *p && isspace((unsigned char)*p)) ++p;
	return p;
}

static const char *value_for(const char *json, const char *key)
{
	char token[96];
	const char *p;
	if (strlen(key) + 3 > sizeof(token)) return NULL;
	snprintf(token, sizeof(token), "\"%s\"", key);
	for (p = json; (p = strstr(p, token)) != NULL; p += strlen(token)) {
		const char *after = skip_space(p + strlen(token));
		if (*after == ':') return skip_space(after + 1);
	}
	return NULL;
}

static int json_string(const char *json, const char *key, char *out, size_t size)
{
	const char *p = value_for(json, key);
	size_t n = 0;
	if (!p || *p++ != '"' || !size) return 0;
	while (*p && *p != '"') {
		if (*p == '\\') return 0;
		if ((unsigned char)*p < 0x20 || n + 1 >= size) return 0;
		out[n++] = *p++;
	}
	if (*p != '"') return 0;
	out[n] = '\0';
	return 1;
}

static int json_number(const char *json, const char *key, double *out)
{
	const char *p = value_for(json, key);
	char *end;
	if (!p) return 0;
	errno = 0;
	*out = strtod(p, &end);
	return end != p && errno != ERANGE && isfinite(*out);
}

static int json_bool(const char *json, const char *key, int *out)
{
	const char *p = value_for(json, key);
	if (!p) return 0;
	if (!strncmp(p, "true", 4)) { *out = 1; return 1; }
	if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
	return 0;
}

static int json_vec3(const char *json, const char *key, float out[3])
{
	const char *p = value_for(json, key);
	char *end;
	if (!p || *p != '[') return 0;
	p = skip_space(p + 1);
	for (int i = 0; i < 3; ++i) {
		double value;
		errno = 0; value = strtod(p, &end);
		if (end == p || errno == ERANGE || !isfinite(value) || value < -FLT_MAX || value > FLT_MAX) return 0;
		out[i] = (float)value; p = skip_space(end);
		if (i < 2) { if (*p != ',') return 0; p = skip_space(p + 1); }
	}
	return *p == ']';
}

static void print_string(const char *value)
{
	fputc('"', stdout);
	for (; *value; ++value) {
		if (*value == '"' || *value == '\\') fputc('\\', stdout);
		fputc(*value, stdout);
	}
	fputc('"', stdout);
}

static void print_vec3(const float value[3])
{
	fprintf(stdout, "[%.9g,%.9g,%.9g]", value[0], value[1], value[2]);
}

static void print_error(const char *id, const char *error, const char *detail)
{
	fputs("{\"ok\":false,\"id\":", stdout); print_string(id);
	fputs(",\"error\":", stdout); print_string(error);
	fputs(",\"detail\":", stdout); print_string(detail); fputs("}\n", stdout);
}

static int parameters(const char *line, parameters_t *out)
{
	double number;
	out->speed = 900; out->pullspeed = 700; out->maxtime = 5; out->sky = 0;
	if (json_number(line, "hook_speed", &number)) out->speed = (float)number;
	if (json_number(line, "hook_pullspeed", &number)) out->pullspeed = (float)number;
	if (json_number(line, "hook_maxtime", &number)) out->maxtime = (float)number;
	json_bool(line, "hook_sky", &out->sky);
	return isfinite(out->speed) && out->speed >= 0 && isfinite(out->pullspeed) &&
		out->pullspeed >= 0 && isfinite(out->maxtime) && out->maxtime >= 0;
}

static void physics_identity(const parameters_t *p, char output[65])
{
	char canonical[1024];
	uint8_t hash[32];
	hook_sha256_ctx_t ctx;
	snprintf(canonical, sizeof(canonical),
		"schema=q2-hook-oracle-v1;shared_c=%s;shared_h=%s;integration=%s;math=%s;build=%s;tool_closure=%s;"
		"hook_speed=%.9g;hook_pullspeed=%.9g;hook_sky=%d;hook_maxtime=%.9g;full_velocity_overwrite=1",
		Q2_HOOK_SHARED_C_SHA256, Q2_HOOK_SHARED_H_SHA256,
		Q2_HOOK_INTEGRATION_SHA256, Q2_HOOK_MATH_SHA256,
		Q2_HOOK_BUILD_CONTRACT, Q2_HOOK_TOOL_CLOSURE_SHA256,
		p->speed, p->pullspeed, p->sky, p->maxtime);
	hook_sha256_init(&ctx); hook_sha256_update(&ctx, canonical, strlen(canonical));
	hook_sha256_final(&ctx, hash); hook_sha256_hex(hash, output);
}

static void print_prefix(const char *id, const char *op, const parameters_t *p)
{
	char identity[65];
	physics_identity(p, identity);
	fputs("{\"ok\":true,\"id\":", stdout); print_string(id);
	fputs(",\"op\":", stdout); print_string(op);
	fputs(",\"schema\":\"q2-hook-oracle-v1\",\"physics_identity\":", stdout); print_string(identity);
	fputs(",\"tool_identity\":", stdout); print_string(Q2_HOOK_TOOL_CLOSURE_SHA256);
}

static void handle_line(const char *line)
{
	char id[128] = "", op[48] = "";
	parameters_t p;
	float a[3], b[3], c[3], velocity[3], prior[3] = {0,0,0};
	int flag, target_owner, owner_client, nonblocking, flymissile, sky;
	double distance;
	q2_hook_touch_action_t action;
	if (!json_string(line, "op", op, sizeof(op))) { print_error(id, "invalid_request", "op string is required"); return; }
	json_string(line, "id", id, sizeof(id));
	if (!parameters(line, &p)) { print_error(id, "invalid_parameters", "hook parameters must be finite and nonnegative"); return; }
	if (!strcmp(op, "identity")) {
		print_prefix(id, op, &p);
		fprintf(stdout, ",\"parameters\":{\"hook_speed\":%.9g,\"hook_pullspeed\":%.9g,"
			"\"hook_sky\":%s,\"hook_maxtime\":%.9g,\"full_velocity_overwrite\":true},\"source\":{",
			p.speed, p.pullspeed, p.sky ? "true" : "false", p.maxtime);
		fputs("\"shared_c_sha256\":", stdout); print_string(Q2_HOOK_SHARED_C_SHA256);
		fputs(",\"shared_h_sha256\":", stdout); print_string(Q2_HOOK_SHARED_H_SHA256);
		fputs(",\"integration_sha256\":", stdout); print_string(Q2_HOOK_INTEGRATION_SHA256);
		fputs(",\"math_sha256\":", stdout); print_string(Q2_HOOK_MATH_SHA256);
		fputs(",\"build_contract\":", stdout); print_string(Q2_HOOK_BUILD_CONTRACT);
		fputs(",\"tool_closure_sha256\":", stdout); print_string(Q2_HOOK_TOOL_CLOSURE_SHA256);
		fputs("}}\n", stdout);
		return;
	}
	if (!strcmp(op, "launch")) {
		if (!json_vec3(line, "forward", a)) { print_error(id, "invalid_forward", "forward vec3 is required"); return; }
		Q2_HookLaunchVelocity(a, p.speed, velocity); print_prefix(id, op, &p);
		fputs(",\"velocity\":", stdout); print_vec3(velocity); fputs("}\n", stdout); return;
	}
	if (!strcmp(op, "pull")) {
		if (!json_vec3(line, "owner_origin", a) || !json_vec3(line, "hook_origin", b)) {
			print_error(id, "invalid_origin", "owner_origin and hook_origin vec3 are required"); return;
		}
		flag = 0; json_bool(line, "enemy_is_client", &flag);
		if (flag) { if (!json_vec3(line, "enemy_origin", c)) { print_error(id, "invalid_enemy", "enemy_origin is required for a client target"); return; } }
		else { c[0]=c[1]=c[2]=0; }
		json_vec3(line, "prior_velocity", prior);
		distance = Q2_HookPullVelocity(a, b, c, flag, p.pullspeed, velocity);
		print_prefix(id, op, &p); fputs(",\"target_source\":", stdout); print_string(flag ? "enemy_origin" : "hook_origin");
		fprintf(stdout, ",\"distance\":%.9g,\"prior_velocity\":", distance); print_vec3(prior);
		fputs(",\"velocity\":", stdout); print_vec3(velocity); fputs(",\"full_velocity_overwrite\":true}\n", stdout); return;
	}
	if (!strcmp(op, "touch")) {
		target_owner=owner_client=nonblocking=flymissile=sky=0;
		json_bool(line, "target_is_owner", &target_owner); json_bool(line, "owner_has_client", &owner_client);
		json_bool(line, "target_is_nonblocking", &nonblocking); json_bool(line, "target_is_flymissile", &flymissile);
		json_bool(line, "surface_is_sky", &sky);
		action = Q2_HookClassifyTouch(target_owner, owner_client, nonblocking, flymissile, sky, p.sky);
		print_prefix(id, op, &p); fputs(",\"action\":", stdout); print_string(Q2_HookTouchActionName(action));
		fprintf(stdout, ",\"attached\":%s}\n", action == Q2_HOOK_TOUCH_ATTACH ? "true" : "false"); return;
	}
	if (!strcmp(op, "backoff")) {
		distance = 10;
		if (!json_vec3(line, "hook_origin", a) || !json_vec3(line, "forward", b)) { print_error(id, "invalid_backoff", "hook_origin and forward vec3 are required"); return; }
		json_number(line, "distance", &distance); if (distance < 0) { print_error(id, "invalid_backoff", "distance must be nonnegative"); return; }
		Q2_HookBackoffOrigin(a, b, (float)distance); print_prefix(id, op, &p);
		fputs(",\"hook_origin\":", stdout); print_vec3(a); fputs("}\n", stdout); return;
	}
	print_error(id, "unknown_operation", op);
}

int main(void)
{
	char *line = malloc(LINE_MAX_BYTES);
	if (!line) return 70;
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
