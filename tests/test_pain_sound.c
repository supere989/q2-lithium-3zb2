#include "../g_local.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

game_import_t gi;
level_locals_t level;

void P_DamageFeedback(edict_t *player);

static int sound_calls;
static char indexed_sound[MAX_QPATH];

static int
TestSoundIndex(char *name)
{
	strncpy(indexed_sound, name, sizeof(indexed_sound) - 1);
	indexed_sound[sizeof(indexed_sound) - 1] = '\0';
	return 1;
}

static void
TestSound(edict_t *ent, int channel, int soundindex, float volume,
	float attenuation, float timeofs)
{
	(void)ent;
	(void)channel;
	(void)soundindex;
	(void)volume;
	(void)attenuation;
	(void)timeofs;
	sound_calls++;
}

char *
va(char *format, ...)
{
	static char buffer[1024];
	va_list args;

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

vec_t
VectorNormalize(vec3_t vector)
{
	(void)vector;
	return 0;
}

void
VectorMA(vec3_t a, float scale, vec3_t b, vec3_t result)
{
	result[0] = a[0] + scale * b[0];
	result[1] = a[1] + scale * b[1];
	result[2] = a[2] + scale * b[2];
}

char *
Info_ValueForKey(char *info, char *key)
{
	static char value[MAX_INFO_VALUE];
	char needle[MAX_INFO_KEY + 2];
	char *start;
	char *end;
	size_t length;

	if (snprintf(needle, sizeof(needle), "\\%s\\", key) >=
		(int)sizeof(needle))
		return "";
	start = strstr(info, needle);
	if (!start)
		return "";
	start += strlen(needle);
	end = strchr(start, '\\');
	length = end ? (size_t)(end - start) : strlen(start);
	if (length >= sizeof(value))
		length = sizeof(value) - 1;
	memcpy(value, start, length);
	value[length] = '\0';
	return value;
}

static void
PrepareDamage(edict_t *player, gclient_t *client, qboolean connected,
	const char *client_id)
{
	memset(player, 0, sizeof(*player));
	memset(client, 0, sizeof(*client));

	player->client = client;
	player->health = 60;
	player->s.modelindex = 255;
	client->pers.connected = connected;
	client->damage_blood = 20;
	if (client_id)
		snprintf(client->pers.userinfo, sizeof(client->pers.userinfo),
			"\\ml_client_id\\%s", client_id);

	level.time = 10;
	level.framenum = 100;
}

int
main(void)
{
	edict_t player;
	gclient_t client;

	gi.soundindex = TestSoundIndex;
	gi.sound = TestSound;

	PrepareDamage(&player, &client, qfalse, NULL);
	P_DamageFeedback(&player);
	assert(sound_calls == 0);

	PrepareDamage(&player, &client, qtrue, NULL);
	P_DamageFeedback(&player);
	assert(sound_calls == 1);
	assert(strncmp(indexed_sound, "*pain75_", 8) == 0);
	assert(strcmp(indexed_sound + strlen(indexed_sound) - 4, ".wav") == 0);

	PrepareDamage(&player, &client, qtrue, "lattice-00");
	P_DamageFeedback(&player);
	assert(sound_calls == 2);
	assert(strncmp(indexed_sound, "#players/male/pain75_", 21) == 0);
	assert(strcmp(indexed_sound + strlen(indexed_sound) - 4, ".wav") == 0);

	puts("pain sound routing: bot suppressed, human sexed, ML explicit: ok");
	return 0;
}
