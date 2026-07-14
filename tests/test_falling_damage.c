#include "../g_local.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

level_locals_t level;
edict_t game_edicts[2];
edict_t *g_edicts = game_edicts;
vec3_t vec3_origin = {0, 0, 0};
cvar_t deathmatch_value;
cvar_t dmflags_value;
cvar_t *deathmatch = &deathmatch_value;
cvar_t *dmflags = &dmflags_value;
lvar_t fall_damagemod_value;
lvar_t *fall_damagemod = &fall_damagemod_value;

static int damage_calls;
static int last_damage;

void
T_Damage(edict_t *target, edict_t *inflictor, edict_t *attacker,
	vec3_t dir, vec3_t point, vec3_t normal, int damage, int knockback,
	int dflags, int mod)
{
	(void)inflictor; (void)attacker; (void)dir; (void)point; (void)normal;
	(void)knockback; (void)dflags;
	assert(mod == MOD_FALLING);
	damage_calls++;
	last_damage = damage;
	target->health -= damage;
}

void P_FallingDamage(edict_t *ent);

static void
prepare(edict_t *player, gclient_t *client, float old_velocity_z)
{
	memset(player, 0, sizeof(*player));
	memset(client, 0, sizeof(*client));
	memset(&deathmatch_value, 0, sizeof(deathmatch_value));
	memset(&dmflags_value, 0, sizeof(dmflags_value));
	memset(&fall_damagemod_value, 0, sizeof(fall_damagemod_value));
	player->client = client;
	player->s.modelindex = 255;
	player->movetype = MOVETYPE_WALK;
	player->groundentity = &game_edicts[0];
	player->health = 100;
	client->oldvelocity[2] = old_velocity_z;
	client->ctf_grapplereleasetime = 0;
	level.time = 1;
	deathmatch_value.value = 1;
	fall_damagemod_value.value = 1;
	damage_calls = 0;
	last_damage = 0;
}

int
main(void)
{
	edict_t player;
	gclient_t client;

	prepare(&player, &client, -300);
	P_FallingDamage(&player);
	assert(player.s.event == EV_FOOTSTEP && damage_calls == 0);

	prepare(&player, &client, -400);
	P_FallingDamage(&player);
	assert(player.s.event == EV_FALLSHORT && client.fall_value == 8);
	assert(client.fall_time == 1.3f && damage_calls == 0);

	prepare(&player, &client, -600);
	P_FallingDamage(&player);
	assert(player.s.event == EV_FALL && last_damage == 3 && player.health == 97);
	assert(player.pain_debounce_time == level.time);

	prepare(&player, &client, -1000);
	fall_damagemod_value.value = 1.5f;
	P_FallingDamage(&player);
	assert(player.s.event == EV_FALLFAR && last_damage == 52 && player.health == 48);

	prepare(&player, &client, -1600);
	P_FallingDamage(&player);
	assert(player.s.event == EV_FALLFAR && last_damage == 113 && player.health == -13);

	prepare(&player, &client, -1000);
	dmflags_value.value = DF_NO_FALLING;
	P_FallingDamage(&player);
	assert(player.s.event == EV_FALLFAR && damage_calls == 0 && player.health == 100);

	prepare(&player, &client, -1000);
	client.ctf_grapple = &player;
	client.ctf_grapplestate = CTF_GRAPPLE_STATE_PULL;
	P_FallingDamage(&player);
	assert(player.s.event == EV_NONE && damage_calls == 0);

	puts("runtime P_FallingDamage shared-law side effects: ok");
	return 0;
}
