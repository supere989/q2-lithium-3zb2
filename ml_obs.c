#include <stdbool.h>
/*
 * ml_obs.c — pack ml_obs_t from current game state, apply ml_action_t.
 *
 * This is the data-bridge layer between game.so internal entity state and
 * the wire format defined in ml_bridge.h.  Called from Bot_Think() when
 * the bot's zc.ml_enabled flag is set.
 */

#include "g_local.h"
#include "bot.h"
#include "ml_bridge.h"
#include "rune_bits.h"
#include "ml_sensors.h"
#include <math.h>
#include <string.h>

/* Lithium server framenum source */
extern int sm_framenum;
extern lvar_t *use_hook;

#define ML_FIRE_YAW_DEGREES   14.0f
#define ML_FIRE_PITCH_DEGREES 16.0f

static float ML_AngleDelta(float left, float right)
{
	float delta = anglemod(left - right);
	if (delta > 180.0f)
		delta -= 360.0f;
	return delta;
}

/* Return true only for a target that can be acted on now.  LOS alone is not
   engagement: the target must be alive, hostile, damageable, unprotected, and
   aligned with the supplied full-resolution view angles.  The 14/16-degree
   window is deliberately just outside Python's 12/14-degree aim labels. */
qboolean ML_HasEngageableTarget(edict_t *ent, const vec3_t view_angles)
{
	edict_t *other;
	vec3_t toward, target_angles;
	float yaw_error, pitch_error;
	int i;

	if (!ent || !ent->client || !view_angles || !ent->inuse || ent->deadflag ||
		ent->health <= 0 || ent->solid == SOLID_NOT ||
		ent->client->pers.spectator || ent->client->resp.spectator ||
		(ent->lithium_flags & LITHIUM_OBSERVER) ||
		ent->safety_time > level.time ||
		ent->client->invincible_framenum > level.framenum)
		return qfalse;

	for (i = 1; i <= maxclients->value; i++)
	{
		other = &g_edicts[i];
		if (other == ent || !other->inuse || !other->client ||
			other->deadflag || other->health <= 0 ||
			other->solid != SOLID_BBOX || !other->takedamage)
			continue;
		if (other->client->pers.spectator || other->client->resp.spectator ||
			(other->lithium_flags & LITHIUM_OBSERVER))
			continue;
		if (ctf->value &&
			other->client->resp.ctf_team == ent->client->resp.ctf_team)
			continue;
		if (OnSameTeam(ent, other))
			continue;
		if (other->safety_time > level.time ||
			other->client->invincible_framenum > level.framenum)
			continue;
		if (ML_TargetExposure(ent, other) <= 0.0f)
			continue;

		VectorSubtract(other->s.origin, ent->s.origin, toward);
		if (VectorLength(toward) <= 0.001f)
			continue;
		vectoangles(toward, target_angles);
		yaw_error = fabsf(ML_AngleDelta(view_angles[YAW], target_angles[YAW]));
		pitch_error = fabsf(ML_AngleDelta(view_angles[PITCH], target_angles[PITCH]));
		if (yaw_error <= ML_FIRE_YAW_DEGREES &&
			pitch_error <= ML_FIRE_PITCH_DEGREES)
			return qtrue;
	}

	return qfalse;
}
void Use_Weapon(edict_t *ent, gitem_t *item);
void Weapon_Hook_Fire(edict_t *ent);
void Hook_Reset(edict_t *rhook);
void Hook_Service(edict_t *self);

#define ML_STUCK_MAX_SLOTS 32
#define ML_STUCK_NUDGE_TICKS 12
#define ML_STUCK_RESPAWN_TICKS 32

static vec3_t ml_last_origin[ML_STUCK_MAX_SLOTS];
static int ml_last_origin_valid[ML_STUCK_MAX_SLOTS];
static int ml_stuck_ticks[ML_STUCK_MAX_SLOTS];

static qboolean ML_OriginBlockedByClient(edict_t *ent, vec3_t origin)
{
	edict_t *touch[MAX_EDICTS];
	vec3_t absmin, absmax;
	int i, count;

	VectorAdd(origin, ent->mins, absmin);
	VectorAdd(origin, ent->maxs, absmax);

	count = gi.BoxEdicts(absmin, absmax, touch, MAX_EDICTS, AREA_SOLID);
	for (i = 0; i < count; i++) {
		if (!touch[i] || touch[i] == ent || !touch[i]->inuse || !touch[i]->client)
			continue;
		if (touch[i]->solid == SOLID_NOT || touch[i]->health <= 0)
			continue;
		return qtrue;
	}

	return qfalse;
}

static qboolean ML_TryLateralUnstick(edict_t *ent)
{
	static const float radii[] = {48.0f, 96.0f, 144.0f};
	vec3_t start, candidate, drop;
	trace_t tr;
	int r, i;

	VectorCopy(ent->s.origin, start);
	start[2] += 4.0f;

	for (r = 0; r < (int)(sizeof(radii) / sizeof(radii[0])); r++) {
		for (i = 0; i < 12; i++) {
			float angle = ((float)i / 12.0f) * 2.0f * M_PI;
			candidate[0] = start[0] + cos(angle) * radii[r];
			candidate[1] = start[1] + sin(angle) * radii[r];
			candidate[2] = start[2];

			tr = gi.trace(start, ent->mins, ent->maxs, candidate, ent, MASK_PLAYERSOLID);
			if (tr.startsolid || tr.allsolid || tr.fraction < 0.95f)
				continue;

			VectorCopy(candidate, drop);
			drop[2] -= 96.0f;
			tr = gi.trace(candidate, ent->mins, ent->maxs, drop, ent, MASK_SOLID);
			if (tr.startsolid || tr.allsolid || tr.fraction == 1.0f || tr.plane.normal[2] < 0.7f)
				continue;
			if (ML_OriginBlockedByClient(ent, tr.endpos))
				continue;

			VectorCopy(tr.endpos, ent->s.origin);
			VectorCopy(ent->s.origin, ent->s.old_origin);
			VectorClear(ent->velocity);
			ent->groundentity = tr.ent;
			ent->groundentity_linkcount = tr.ent ? tr.ent->linkcount : 0;
			if (ent->client)
				ent->client->ps.pmove.pm_flags |= PMF_ON_GROUND;
			gi.linkentity(ent);
			return qtrue;
		}
	}

	return qfalse;
}

static gitem_t *ML_WeaponForAction(uint8_t weapon)
{
	switch (weapon)
	{
	case 1: return FindItem("Blaster");
	case 2: return FindItem("Shotgun");
	case 3: return FindItem("Super Shotgun");
	case 4: return FindItem("Machinegun");
	case 5: return FindItem("Chaingun");
	case 6: return FindItem("Grenade Launcher");
	case 7: return FindItem("Rocket Launcher");
	case 8: return FindItem("HyperBlaster");
	case 9: return FindItem("Railgun");
	default: return NULL;
	}
}

static void ML_SelectWeapon(edict_t *ent, uint8_t weapon)
{
	gitem_t *item;
	int index;

	if (!ent || !ent->client)
		return;

	item = ML_WeaponForAction(weapon);
	if (!item)
		return;

	index = ITEM_INDEX(item);
	if (index <= 0 || !ent->client->pers.inventory[index])
		return;

	Use_Weapon(ent, item);
}

static void ML_UpdateStuckGuard(edict_t *ent, const ml_action_t *act)
{
	int slot;
	vec3_t delta;
	float moved;
	qboolean wants_move;

	if (!ent || !ent->client || !act)
		return;

	slot = (int)(ent - g_edicts - 1);
	if (slot < 0 || slot >= ML_STUCK_MAX_SLOTS)
		return;

	wants_move = (fabs(act->move_forward) > 0.15f ||
		fabs(act->move_right) > 0.15f || act->jump || act->hook);

	if (!ml_last_origin_valid[slot]) {
		VectorCopy(ent->s.origin, ml_last_origin[slot]);
		ml_last_origin_valid[slot] = 1;
		ml_stuck_ticks[slot] = 0;
		return;
	}

	VectorSubtract(ent->s.origin, ml_last_origin[slot], delta);
	moved = VectorLength(delta);
	VectorCopy(ent->s.origin, ml_last_origin[slot]);

	if (!wants_move || moved > 2.0f || ent->deadflag) {
		ml_stuck_ticks[slot] = 0;
		return;
	}

	ml_stuck_ticks[slot]++;
	if (ml_stuck_ticks[slot] == ML_STUCK_NUDGE_TICKS) {
		if (ML_TryLateralUnstick(ent)) {
			ml_stuck_ticks[slot] = 0;
			VectorCopy(ent->s.origin, ml_last_origin[slot]);
			return;
		}
		ent->velocity[0] += 360.0f * crandom();
		ent->velocity[1] += 360.0f * crandom();
	}
	else if (ml_stuck_ticks[slot] >= ML_STUCK_RESPAWN_TICKS) {
		int protection_until = ent->client->invincible_framenum;
		gi.dprintf("ML: slot %d stuck at %.1f %.1f %.1f; respawning\n",
			slot, ent->s.origin[0], ent->s.origin[1], ent->s.origin[2]);
		ml_stuck_ticks[slot] = 0;
		PutBotInServer(ent);
		/* This is a relocation, not a new life.  Do not let repeated stuck
		   recovery renew spawn protection indefinitely. */
		ent->client->invincible_framenum = protection_until;
		VectorCopy(ent->s.origin, ml_last_origin[slot]);
	}
}

static uint32_t ML_ControlSource(edict_t *ent)
{
	if (!ent || !ent->client)
		return ML_CONTROL_UNKNOWN;
	if (ent->client->zc.ml_enabled)
		return ML_CONTROL_ML_BOT;
	if (ent->svflags & SVF_MONSTER)
		return ML_CONTROL_LEGACY_BOT;
	return ML_CONTROL_HUMAN;
}

static uint32_t ML_DebugFlags(edict_t *ent, float visible)
{
	uint32_t flags = 0;

	if (!ent)
		return flags;
	if (ent->client)
		flags |= ML_ENTITY_CLIENT;
	if (ent->svflags & SVF_MONSTER)
		flags |= ML_ENTITY_BOT;
	if (ent->client && ent->client->zc.ml_enabled)
		flags |= ML_ENTITY_ML;
	if (visible > 0.0f)
		flags |= ML_ENTITY_VISIBLE;
	if (ent->deadflag)
		flags |= ML_ENTITY_DEAD;
	if (ent->lithium_flags & LITHIUM_OBSERVER)
		flags |= ML_ENTITY_OBSERVER;
	if (ent->solid == SOLID_NOT)
		flags |= ML_ENTITY_SOLID_NOT;
	if (ent->movetype == MOVETYPE_NOCLIP)
		flags |= ML_ENTITY_NOCLIP;
	if (ent->svflags & SVF_NOCLIENT)
		flags |= ML_ENTITY_NOCLIENT;
	if (ent->client && (ent->client->pers.spectator || ent->client->resp.spectator))
		flags |= ML_ENTITY_SPECTATOR;
	if (ent->safety_time > level.time ||
		(ent->client && ent->client->invincible_framenum > level.framenum))
		flags |= ML_ENTITY_PROTECTED;
	if (ent->flags & FL_FLY)
		flags |= ML_ENTITY_FLY;
	if (ent->flags & FL_SWIM)
		flags |= ML_ENTITY_SWIM;
	if (ent->client && ent->client->ps.pmove.pm_type == PM_SPECTATOR)
		flags |= ML_ENTITY_PM_SPECTATOR;
	if (ent->client && ent->client->ps.pmove.pm_type == PM_FREEZE)
		flags |= ML_ENTITY_PM_FREEZE;
	if (ent->groundentity)
		flags |= ML_ENTITY_GROUNDED;
	if (ent->client && (ent->client->ps.pmove.pm_flags & PMF_ON_GROUND))
		flags |= ML_ENTITY_PM_ON_GROUND;
	return flags;
}

static void ML_FillDebugIdentity(ml_entity_debug_t *dst, edict_t *ent, float visible)
{
	int edict_index = ent ? (int)(ent - g_edicts) : 0;

	memset(dst, 0, sizeof(*dst));
	dst->edict_index    = (uint32_t)edict_index;
	dst->client_slot    = (edict_index > 0) ? (uint32_t)(edict_index - 1) : 0;
	dst->control_source = ML_ControlSource(ent);
	dst->flags          = ML_DebugFlags(ent, visible);
}

static void ML_FillActionDebug(ml_action_debug_t *dst, zgcl_t *zc)
{
	memset(dst, 0, sizeof(*dst));
	dst->tick          = (uint32_t)zc->ml_last_action_tick;
	dst->accepted      = (uint32_t)zc->ml_last_action_ok;
	dst->timeout_count = (uint32_t)zc->ml_timeout_count;
	dst->weapon        = (uint32_t)zc->ml_weapon;
	dst->move_forward  = zc->ml_move_forward;
	dst->move_right    = zc->ml_move_right;
	dst->look_yaw      = zc->ml_look_yaw;
	dst->look_pitch    = zc->ml_look_pitch;
	dst->jump          = (uint32_t)zc->ml_jump;
	dst->fire          = (uint32_t)zc->ml_fire;
	dst->hook          = (uint32_t)zc->ml_hook;
	if (zc->ml_fire_suppressed)
		dst->_pad |= ML_FIRE_GATE_SUPPRESSED;
}

/* Pack the bot's current state into an ml_obs_t structure. */
void ML_PackObs(edict_t *ent, ml_obs_t *obs)
{
	int       i, n;
	edict_t   *other;
	vec3_t    rel;
	vec3_t    rel_local;
	vec3_t    forward, right, up;
	zgcl_t    *zc = &ent->client->zc;

	memset(obs, 0, sizeof(*obs));
	obs->magic    = ML_OBS_MAGIC;
	obs->tick     = (uint32_t)level.framenum;
	obs->bot_slot = (uint32_t)(ent - g_edicts - 1);
	obs->yaw      = ent->s.angles[YAW];
	obs->pitch    = ent->s.angles[PITCH];

	/* ── self ──────────────────────────────────────────────── */
	VectorCopy(ent->s.origin,   obs->self.pos);
	VectorCopy(ent->velocity,   obs->self.vel);
	obs->self.health    = (float)ent->health;
	obs->self.armor     = (float)ent->client->pers.inventory[
		ITEM_INDEX(FindItem("Body Armor"))];
	obs->self.weapon_id = ent->client->pers.weapon
		? (float)ITEM_INDEX(ent->client->pers.weapon) : 0.0f;
	obs->self.ammo      = (float)(ent->client->pers.weapon && ent->client->pers.weapon->ammo
		? ent->client->pers.inventory[ITEM_INDEX(FindItem(ent->client->pers.weapon->ammo))]
		: 0);
	ML_FillDebugIdentity(&obs->self_debug, ent, 1.0f);

	/* ── visible entities (other clients only for now) ─────── */
	AngleVectors(ent->s.angles, forward, right, up);
	n = 0;
	for (i = 1; i <= maxclients->value && n < ML_MAX_ENTITIES; i++)
	{
		other = &g_edicts[i];
		if (other == ent || !other->inuse || other->deadflag)
			continue;
		if (!other->client)
			continue;
		if (other->solid == SOLID_NOT ||
		    other->client->pers.spectator ||
		    other->client->resp.spectator ||
		    (other->lithium_flags & LITHIUM_OBSERVER))
			continue;

		VectorSubtract(other->s.origin, ent->s.origin, rel);
		rel_local[0] = DotProduct(rel, forward);
		rel_local[1] = DotProduct(rel, right);
		rel_local[2] = DotProduct(rel, up);
		VectorCopy(rel_local,       obs->entities[n].rel_pos);
		VectorCopy(other->velocity, obs->entities[n].vel);
		obs->entities[n].health    = (float)other->health;
		obs->entities[n].is_enemy  = ctf->value
			? (other->client->resp.ctf_team != ent->client->resp.ctf_team ? 1.0f : 0.0f)
			: 1.0f;
		obs->entities[n].visible   = ML_TargetExposure(ent, other);
		ML_FillDebugIdentity(&obs->entity_debug[n], other, obs->entities[n].visible);
		n++;
	}
	obs->entity_count = (uint32_t)n;

	/* ── rays / hook zones ────────────────────────────────── */
	ML_FillRays(ent, obs);
	ML_FillHookZones(ent, obs);
	ML_FillActionDebug(&obs->action_debug, zc);
	if (ent->safety_time > level.time ||
		ent->client->invincible_framenum > level.framenum)
		obs->action_debug._pad |= ML_FIRE_GATE_PROTECTED;
	if (ML_HasEngageableTarget(ent, ent->client->v_angle))
		obs->action_debug._pad |= ML_FIRE_GATE_TARGET;

	/* ── audio ────────────────────────────────────────────── */
	if (level.sound_entity && level.sound_entity_framenum >= level.framenum - 30)
	{
		VectorSubtract(level.sound_entity->s.origin, ent->s.origin, rel);
		float len = VectorLength(rel);
		if (len > 0.001f)
		{
			VectorScale(rel, 1.0f / len, obs->audio.sound_dir);
			obs->audio.sound_age = (float)(level.framenum - level.sound_entity_framenum);
			obs->audio.alert_level = 1.0f - (obs->audio.sound_age / 30.0f);
		}
	}

	/* ── survival-recovery payoff (regen|vampire heal you back up) ── */
	if ((ent->rune & (RUNE_REGEN | RUNE_VAMPIRE)) && !ent->deadflag)
	{
		float gain = (float)ent->health - zc->ml_last_health;
		if (gain > 0.0f) zc->ml_reward_survival += gain;
	}
	zc->ml_last_health = (float)ent->health;

	/* ── rune awareness (extended obs; in policy input only when EXT_OBS) ── */
	obs->rune_flags[0] = (ent->rune & RUNE_RESIST)   ? 1.0f : 0.0f;
	obs->rune_flags[1] = (ent->rune & RUNE_STRENGTH) ? 1.0f : 0.0f;
	obs->rune_flags[2] = (ent->rune & RUNE_HASTE)    ? 1.0f : 0.0f;
	obs->rune_flags[3] = (ent->rune & RUNE_REGEN)    ? 1.0f : 0.0f;
	obs->rune_flags[4] = (ent->rune & RUNE_VAMPIRE)  ? 1.0f : 0.0f;

	/* ── inbound damage vector, decaying over ~1s (10 frames) ── */
	{
		int age = (int)level.framenum - zc->ml_inbound_dmg_frame;
		if (zc->ml_inbound_dmg_frame > 0 && age >= 0 && age <= 10)
		{
			VectorCopy(zc->ml_inbound_dmg_dir, obs->inbound_dmg_dir);
			obs->inbound_dmg_dist    = zc->ml_inbound_dmg_dist;
			obs->inbound_dmg_recency = 1.0f - (float)age / 10.0f;
		}
		else
		{
			obs->inbound_dmg_dist    = -1.0f;
			obs->inbound_dmg_recency = 0.0f;
		}
	}

	/* ── reward components (cleared after send) ───────────── */
	obs->reward_damage_dealt        = zc->ml_reward_damage_dealt;
	obs->reward_damage_taken        = zc->ml_reward_damage_taken;
	obs->reward_kill                = zc->ml_reward_kill;
	obs->reward_death               = zc->ml_reward_death;
	obs->reward_item_pickup         = zc->ml_reward_item;
	obs->reward_hook_traversal      = zc->ml_reward_hook;
	obs->reward_damage_taken_prox   = zc->ml_reward_damage_taken_prox;
	obs->reward_offense             = zc->ml_reward_offense;
	obs->reward_survival            = zc->ml_reward_survival;

	zc->ml_reward_damage_dealt      = 0;
	zc->ml_reward_damage_taken      = 0;
	zc->ml_reward_kill              = 0;
	zc->ml_reward_death             = 0;
	zc->ml_reward_item              = 0;
	zc->ml_reward_hook              = 0;
	zc->ml_reward_damage_taken_prox = 0;
	zc->ml_reward_offense           = 0;
	zc->ml_reward_survival          = 0;

	/* PutBotInServer preserves the intermission one-shot across respawns,
	   because dead bots can respawn while the scoreboard is up. Re-arm it
	   only after the intermission has actually ended (normally on the next
	   map), rather than on every respawn. */
	if (level.intermissiontime <= 0)
		zc->ml_intermission_obs_sent = 0;

	if (level.intermissiontime > 0)
	{
		/* Intermission takes priority over death, and the explicit else is
		   important: a dead bot must not fall through and emit a death
		   terminal on every later intermission frame. The sender records
		   success in ml_intermission_obs_sent after the UDP send succeeds. */
		if (!zc->ml_intermission_obs_sent)
		{
			obs->is_terminal = 1;
			obs->terminal_reason = ML_TERMINAL_INTERMISSION;
		}
		else
		{
			obs->is_terminal = 0;
			obs->terminal_reason = ML_TERMINAL_NONE;
		}
	}
	else if (ent->deadflag && !zc->ml_death_obs_sent)
	{
		/* Terminal exactly once per death. Dead bots keep streaming
		   regular (non-terminal) corpse obs from the G_RunFrame pre-pass
		   so lockstep never starves while a bot awaits respawn; without
		   this gate every corpse frame would end a one-step episode and
		   collect a fresh death penalty. */
		obs->is_terminal = 1;
		obs->terminal_reason = ML_TERMINAL_DEATH;
	}
	else
	{
		obs->is_terminal = 0;
		obs->terminal_reason = ML_TERMINAL_NONE;
	}
}


/* Apply an action to the bot's edict — translates wire format into Q2
 * physics inputs and weapon triggers. */
void ML_ApplyAction(edict_t *ent, const ml_action_t *act)
{
	zgcl_t *zc = &ent->client->zc;
	float forward_speed = 320.0f;
	vec3_t forward, right;
	vec3_t intended_angles;
	qboolean fire_allowed;

	if (ent->safety_time && ent->safety_time <= level.time)
		ent->safety_time = 0;
	VectorCopy(ent->client->v_angle, intended_angles);
	intended_angles[YAW] += act->look_yaw;
	intended_angles[PITCH] += act->look_pitch;
	if (intended_angles[PITCH] > 89.0f) intended_angles[PITCH] = 89.0f;
	if (intended_angles[PITCH] < -89.0f) intended_angles[PITCH] = -89.0f;
	fire_allowed = !ent->safety_time &&
		ent->client->invincible_framenum <= level.framenum &&
		ML_HasEngageableTarget(ent, intended_angles);

	/* cache for any sub-tick logic */
	zc->ml_move_forward = act->move_forward;
	zc->ml_move_right   = act->move_right;
	zc->ml_look_yaw     = act->look_yaw;
	zc->ml_look_pitch   = act->look_pitch;
	zc->ml_jump         = act->jump;
	zc->ml_fire         = act->fire && fire_allowed;
	zc->ml_fire_suppressed = act->fire && !fire_allowed;
	zc->ml_hook         = act->hook;
	zc->ml_weapon       = act->weapon;

	/* ── look angles ──────────────────────────────────────── */
	ent->s.angles[YAW]   += act->look_yaw;
	ent->s.angles[PITCH] += act->look_pitch;
	if (ent->s.angles[PITCH] >  89.0f) ent->s.angles[PITCH] =  89.0f;
	if (ent->s.angles[PITCH] < -89.0f) ent->s.angles[PITCH] = -89.0f;
	ent->client->v_angle[YAW]   = ent->s.angles[YAW];
	ent->client->v_angle[PITCH] = ent->s.angles[PITCH];

	/* ── movement ─────────────────────────────────────────── */
	AngleVectors(ent->s.angles, forward, right, NULL);
	forward[2] = 0;
	right[2]   = 0;
	VectorNormalize(forward);
	VectorNormalize(right);

	if (ent->groundentity)
	{
		VectorScale(forward, act->move_forward * forward_speed, ent->velocity);
		VectorMA(ent->velocity, act->move_right * forward_speed, right, ent->velocity);

		/* preserve gravity */
		if (act->jump)
			ent->velocity[2] = VEL_BOT_JUMP;
	}
	else
	{
		/* air control: small adjustments only */
		ent->velocity[0] += forward[0] * act->move_forward * 30.0f;
		ent->velocity[1] += forward[1] * act->move_forward * 30.0f;
		ent->velocity[0] += right[0]   * act->move_right   * 30.0f;
		ent->velocity[1] += right[1]   * act->move_right   * 30.0f;
	}

	/* ── weapon select ────────────────────────────────────── */
	if (act->weapon > 0 && act->weapon < 10)
	{
		ML_SelectWeapon(ent, act->weapon);
	}

	/* ── fire ─────────────────────────────────────────────── */
	if (act->fire && fire_allowed && ent->client->pers.weapon &&
		ent->client->weaponstate == WEAPON_READY)
	{
		ent->client->buttons     |= BUTTON_ATTACK;
		ent->client->latched_buttons |= BUTTON_ATTACK;
	}
	else
	{
		ent->client->buttons     &= ~BUTTON_ATTACK;
		ent->client->latched_buttons &= ~BUTTON_ATTACK;
	}

	/* ── hook (Lithium grapple) ───────────────────────────── */
	if (act->hook == 1)
	{
		if (use_hook && use_hook->value &&
		    !(ent->lithium_flags & LITHIUM_OBSERVER) &&
		    ent->deadflag == DEAD_NO)
		{
			Weapon_Hook_Fire(ent);
			ent->safety_time = 0;
		}
	}
	else if (act->hook == 3)
	{
		Hook_Reset(ent->client->hook);
	}

	if (ent->client->hook_on && ent->client->hook)
	{
		Hook_Service(ent->client->hook);
	}

	ML_UpdateStuckGuard(ent, act);
}
