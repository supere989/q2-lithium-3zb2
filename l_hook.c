#include <stdbool.h>
/*============================================================================

    This file is part of Lithium II Mod for Quake II
    Copyright (C) 1997, 1998, 1999, 2010 Matthew A. Ayres

    Lithium II Mod is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Lithium II Mod is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Lithium II Mod.  If not, see <http://www.gnu.org/licenses/>.

    Quake II is a trademark of Id Software, Inc.

    Code by Matt "WhiteFang" Ayres, matt@lithium.com

============================================================================*/

// Offhand laser grappling hook
//
// Code originally from Orange 2 Mod

#include "g_local.h"
#include "ml_bridge.h"
#include "ml_hook_physics.h"

lvar_t *hook_speed;
lvar_t *hook_pullspeed;
lvar_t *hook_sky;
lvar_t *hook_maxtime;
lvar_t *hook_damage;
lvar_t *hook_initdamage;
lvar_t *hook_maxdamage;
lvar_t *hook_delay;

void Hook_InitGame(void) {
	hook_speed = lvar("hook_speed", "900", "####", VAR_HOOK);
	hook_pullspeed = lvar("hook_pullspeed", "700", "####", VAR_HOOK);
	hook_sky = lvar("hook_sky", "0", "^", VAR_HOOK);
	hook_maxtime = lvar("hook_maxtime", "5", "##", VAR_HOOK);
	hook_damage = lvar("hook_damage", "1", "##", VAR_HOOK);
	hook_initdamage = lvar("hook_initdamage", "10", "###", VAR_HOOK);
	hook_maxdamage = lvar("hook_maxdamage", "20", "###", VAR_HOOK);
	hook_delay = lvar("hook_delay", "0.2", "#.#", VAR_HOOK);
}

void Hook_PlayerDie(edict_t *attacker, edict_t *self) {
	Hook_Reset(self->client->hook);
}

// reset the hook.  pull all entities out of the world and reset
// the clients weapon state
void Hook_Reset(edict_t *rhook) {
   if(!rhook) return;

   // start with NULL pointer checks
   if(rhook->owner && rhook->owner->client) {
	   // client's hook is no longer out
	   rhook->owner->client->hook_out = qfalse;
	   rhook->owner->client->hook_on = qfalse;
	   rhook->owner->client->hook = NULL;
//	   rhook->owner->client->ps.pmove.pm_flags &= ~PMF_NO_PREDICTION;
	}

	// this should always be true and free the laser beam
	if(rhook->laser)
		G_FreeEdict(rhook->laser);

	// delete ourself
	G_FreeEdict(rhook);
};

// resets the hook if it needs to be
qboolean Hook_Check(edict_t *self) {
	if(!self->enemy || !self->owner) {
		Hook_Reset(self);
		return qtrue;
	}

	// drop the hook if either party dies/leaves the game/etc.
	if((!self->enemy->inuse) || (!self->owner->inuse) ||
		(self->enemy->client && self->enemy->health <= 0) || 
		(self->owner->health <= 0)) {
		Hook_Reset(self);
		return qtrue;
	}

	// drop the hook if player lets go of button
	// and has the hook as current weapon
	if(!((self->owner->client->latched_buttons|self->owner->client->buttons) & BUTTON_ATTACK)
		&& (strcmp(self->owner->client->pers.weapon->pickup_name, "Hook") == 0)) {
		Hook_Reset(self);
		return qtrue;
	}

	return qfalse;
}

void Hook_Service(edict_t *self) {
	const vec_t *enemy_origin;

	// if hook should be dropped, just return
	if(Hook_Check(self)) return;

	// give the client some velocity ...
	enemy_origin = self->enemy->s.origin;
	Q2_HookPullVelocity(self->owner->s.origin, self->s.origin,
		enemy_origin, self->enemy->client != NULL, hook_pullspeed->value,
		self->owner->velocity);

//	SV_AddGravity(self->owner);
}

// keeps the invisible hook entity on hook->enemy (can be world or an entity)
void Hook_Track(edict_t *self) {
	vec3_t normal;

	// if hook should be dropped, just return
	if(Hook_Check(self)) return;

	// bring the pAiN!
	if(self->enemy->client) {
		// move the hook along with the player.  It's invisible, but
		// we need this to make the sound come from the right spot

		if(self->owner->client->hook_damage >= hook_maxdamage->value) {
			Hook_Reset(self);
			return;
		}

		gi.unlinkentity(self);
		VectorCopy(self->enemy->s.origin, self->s.origin);
		gi.linkentity(self);
			
		_VectorSubtract(self->owner->s.origin, self->enemy->s.origin, normal);
		
		T_Damage(self->enemy, self, self->owner, vec3_origin, self->enemy->s.origin, normal, hook_damage->value, 0, DAMAGE_NO_KNOCKBACK, MOD_HOOK);

		self->owner->client->hook_damage += hook_damage->value;
	}
	else {
		// If the hook is not attached to the player, constantly copy
		// copy the target's velocity. Velocity copying DOES NOT work properly
		// for a hooked client. 
		VectorCopy(self->enemy->velocity, self->velocity);

		if(hook_maxtime->value && level.time - self->owner->hook_time > hook_maxtime->value) {
			Hook_Reset(self);
			return;
		}
	}

	self->nextthink = level.time + 0.1;
}

// the hook has hit something.  what could it be?
void Hook_Touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf) {
	vec3_t dir, normal;
	q2_hook_touch_action_t touch_action;

	touch_action = Q2_HookClassifyTouch(
		other == self->owner,
		self->owner && self->owner->client,
		other->solid == SOLID_NOT || other->solid == SOLID_TRIGGER,
		other->movetype == MOVETYPE_FLYMISSILE,
		surf && (surf->flags & SURF_SKY),
		hook_sky->value != 0);
	if(touch_action == Q2_HOOK_TOUCH_RESET_SKY) {
		Hook_Reset(self);
		return;
	}
	if(touch_action != Q2_HOOK_TOUCH_ATTACH)
		return;
	
	if(other->client) {		// we hit a player
		// ignore hitting a teammate
		if(OnSameTeam(other, self->owner))
			return;

		// we hit an enemy, so do a bit of damage
		_VectorSubtract(other->s.origin, self->owner->s.origin, dir);
		_VectorSubtract(self->owner->s.origin, other->s.origin, normal);

		if(self->owner->client->hook_damage >= hook_maxdamage->value) {
			Hook_Reset(self);
			return;
		}

		if(hook_maxdamage->value >= hook_initdamage->value)
			T_Damage(other, self, self->owner, dir, self->s.origin, normal, hook_initdamage->value, hook_initdamage->value, 0, MOD_HOOK);

		self->owner->client->hook_damage += hook_initdamage->value;
	} 
	else {					// we hit something thats not a player
		// if we can hurt it, then do a bit of damage
		if (other->takedamage) {
			_VectorSubtract(other->s.origin, self->owner->s.origin, dir);
			_VectorSubtract(self->owner->s.origin, other->s.origin, normal);
			T_Damage(other, self, self->owner, dir, self->s.origin, normal, hook_damage->value, hook_damage->value, 0, 0);

			self->owner->client->hook_damage += hook_initdamage->value;
		}
		// stop moving
		VectorClear(self->velocity);
		
		// gi.sound() doesnt work because the origin of an entity with no model is not 
		// transmitted to clients or something.  hoped this would be fixed in Q2 ...
		gi.positioned_sound(self->s.origin, self, CHAN_WEAPON, gi.soundindex("flyer/Flyatck2.wav"), 1, ATTN_NORM, 0);
	}
	
	// remember who/what we hit, must be set before Hook_Check() is called
	self->enemy = other;

	// if hook should be dropped, just return
	if(Hook_Check(self))
		return;

	// we are now anchored
	self->owner->client->hook_on = qtrue;
	ML_CausalHookAttached(self->owner, self->s.origin);
//	self->owner->client->ps.pmove.pm_flags |= PMF_NO_PREDICTION;

	// keep up with that thing
	self->think = Hook_Track;
	self->nextthink = level.time + 0.1;
	
	self->solid = SOLID_NOT;

	self->owner->hook_time = level.time;
}

// move the two ends of the laser beam to the proper positions
void Hook_Think(edict_t *self) {
	vec3_t forward, right, offset, start;

	// stupid check for NULL pointers ...
 	if(!(self && self->owner && self->owner->owner && self->owner->owner->client)) {
		gi.dprintf("Hook_Think: error\n");
		G_FreeEdict(self);
		return;	
	}

	// put start position into start
	AngleVectors (self->owner->owner->client->v_angle, forward, right, NULL);
	VectorSet(offset, 24, 8, self->owner->owner->viewheight-8);
	P_ProjectSource(self->owner->owner->client, self->owner->owner->s.origin, offset, forward, right, start);

	// move the two ends
	VectorCopy(start, self->s.origin);
	VectorCopy(self->owner->s.origin, self->s.old_origin);

	gi.linkentity(self);

	// set up to go again
	self->nextthink = level.time + FRAMETIME;
}

// create a laser and return a pointer to it
edict_t *Hook_Start(edict_t *ent) {
	edict_t *self;

	self = G_Spawn();
	self->movetype = MOVETYPE_NONE;
	self->solid = SOLID_NOT;
	self->s.renderfx |= RF_BEAM | RF_TRANSLUCENT;
	self->s.modelindex = 1;			// must be non-zero
	self->owner = ent;

	// set the beam diameter
	self->s.frame = 4;

	// set the color
	self->s.skinnum = 0xf0f0f0f0;  // red

	if(ctf->value && ctf_coloredhook->value && ent->owner->client->resp.ctf_team == 2)
		self->s.skinnum = 0xf1f1f1f1;  // blue

	self->think = Hook_Think;

	VectorSet(self->mins, -8, -8, -8);
	VectorSet(self->maxs, 8, 8, 8);
	gi.linkentity(self);

	self->spawnflags |= 0x80000001;
	self->svflags &= ~SVF_NOCLIENT;
	Hook_Think(self);

	return self;
}

// creates the invisible hook entity and sends it on its way
// attaches a laser to it
void Hook_Fire(edict_t *owner, vec3_t start, vec3_t forward) {
	edict_t	*hook;
	trace_t tr;

	hook = G_Spawn();
	hook->movetype = MOVETYPE_FLYMISSILE;
	hook->solid = SOLID_BBOX;
	hook->clipmask = MASK_SHOT;
	hook->owner = owner;			// this hook belongs to me
	owner->client->hook = hook;		// this is my hook
	hook->classname = "hook";		// this is a hook

	vectoangles (forward, hook->s.angles);
	Q2_HookLaunchVelocity(forward, hook_speed->value, hook->velocity);

	hook->touch = Hook_Touch;
	hook->think = G_FreeEdict;
	hook->nextthink = level.time + 5;

	gi.setmodel(hook, "");
	
	VectorCopy(start, hook->s.origin);
	VectorCopy(hook->s.origin, hook->s.old_origin);

	VectorClear(hook->mins);
	VectorClear(hook->maxs);

	// start up the laser
	hook->laser = Hook_Start(hook);

	// put it in the world
	gi.linkentity(hook);

	// from id's code.
	tr = gi.trace(owner->s.origin, NULL, NULL, hook->s.origin, hook, MASK_SHOT);
	if(tr.fraction < 1.0) {
		Q2_HookBackoffOrigin(hook->s.origin, forward, 10);
		hook->touch(hook, tr.ent, NULL, NULL);
	}
}

// a call has been made to fire the hook
void Weapon_Hook_Fire(edict_t *ent) {
	vec3_t forward, right;
	vec3_t start;
	vec3_t offset;

	/* This predicate is also the causal qualification seam: denied requests
	   cannot open an authoritative hook-attempt episode. */
	if(!ML_CausalHookFireAccepted(ent->client->hook_out, level.time,
		ent->client->last_hook_time, hook_delay->value))
		return;
	ent->client->last_hook_time = level.time;
	ML_CausalHookAttempt(ent);

    ent->client->hook_out = qtrue;
	ent->client->hook_damage = 0;

	// calculate start position and forward direction
	AngleVectors(ent->client->v_angle, forward, right, NULL);
	VectorSet(offset, 24, 8, ent->viewheight-8);
	P_ProjectSource(ent->client, ent->s.origin, offset, forward, right, start);

	// kick back??
	VectorScale(forward, -2, ent->client->kick_origin);
	ent->client->kick_angles[0] = -1;

	// actually launch the hook off
	Hook_Fire(ent, start, forward);

	gi.sound(ent, CHAN_WEAPON, gi.soundindex("flyer/Flyatck3.wav"), 1, ATTN_NORM, 0);

	PlayerNoise(ent, start, PNOISE_WEAPON);
}

// boring service routine
void Weapon_Hook (edict_t *ent) {
	static int pause_frames[]	= {19, 32, 0};
	static int fire_frames[]	= {5, 0};

	Weapon_Generic(ent, 4, 8, 52, 55, pause_frames, fire_frames, Weapon_Hook_Fire);
}

#ifdef Q2_HOOK_PARITY_PROBE
/* Isolated-q2ded-only probe. This symbol and command do not exist in normal
 * game-module builds; the parity harness compiles it explicitly. */
static trace_t Hook_ProbePMTrace(vec3_t start, vec3_t mins, vec3_t maxs,
	vec3_t end)
{
	return gi.trace(start, mins, maxs, end, NULL, MASK_PLAYERSOLID);
}

void Hook_OracleParityProbe(void)
{
	vec3_t owner = {10, -20, 30};
	vec3_t hook = {110, 30, 80};
	vec3_t enemy = {-40, 80, 55};
	vec3_t zero = {1, 2, 3};
	vec3_t velocity;
	vec3_t forward = {0.6f, 0, 0.8f};
	vec3_t backoff = {24, 8, 14};
	float distance;
	q2_hook_touch_action_t action;
	pmove_t pm;
	trace_t direct_trace;
	vec3_t trace_end = {0, 0, -64};
	vec3_t stand_mins = {-16, -16, -24};
	vec3_t stand_maxs = {16, 16, 32};
	float probe_speed = hook_speed->value;
	float probe_pullspeed = hook_pullspeed->value;
	float probe_maxtime = hook_maxtime->value;
	qboolean probe_sky = hook_sky->value != 0;

	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"parameters\",\"op\":\"parameters\","
		"\"hook_speed\":%.9g,\"hook_pullspeed\":%.9g,\"hook_sky\":%s,"
		"\"hook_maxtime\":%.9g,\"full_velocity_overwrite\":true}\n",
		probe_speed, probe_pullspeed, probe_sky ? "true" : "false", probe_maxtime);
	distance = Q2_HookPullVelocity(owner, hook, enemy, 0,
		probe_pullspeed, velocity);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"world-pull\",\"op\":\"pull\","
		"\"owner_origin\":[10,-20,30],\"hook_origin\":[110,30,80],"
		"\"hook_pullspeed\":%.9g,\"server_distance\":%.9g,"
		"\"server_velocity\":[%.9g,%.9g,%.9g]}\n",
		probe_pullspeed, distance, velocity[0], velocity[1], velocity[2]);
	distance = Q2_HookPullVelocity(owner, hook, enemy, 1,
		probe_pullspeed, velocity);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"client-pull\",\"op\":\"pull\","
		"\"owner_origin\":[10,-20,30],\"hook_origin\":[110,30,80],"
		"\"enemy_origin\":[-40,80,55],\"enemy_is_client\":true,"
		"\"hook_pullspeed\":%.9g,\"server_distance\":%.9g,"
		"\"server_velocity\":[%.9g,%.9g,%.9g]}\n",
		probe_pullspeed, distance, velocity[0], velocity[1], velocity[2]);
	distance = Q2_HookPullVelocity(zero, zero, zero, 0,
		probe_pullspeed, velocity);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"zero-pull\",\"op\":\"pull\","
		"\"owner_origin\":[1,2,3],\"hook_origin\":[1,2,3],"
		"\"hook_pullspeed\":%.9g,"
		"\"server_distance\":%.9g,\"server_velocity\":[%.9g,%.9g,%.9g]}\n",
		probe_pullspeed, distance, velocity[0], velocity[1], velocity[2]);
	Q2_HookLaunchVelocity(forward, probe_speed, velocity);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"launch\",\"op\":\"launch\","
		"\"hook_speed\":%.9g,"
		"\"forward\":[0.600000024,0,0.800000012],"
		"\"server_velocity\":[%.9g,%.9g,%.9g]}\n",
		probe_speed, velocity[0], velocity[1], velocity[2]);
	Q2_HookBackoffOrigin(backoff, forward, 10);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"backoff\",\"op\":\"backoff\","
		"\"hook_origin\":[24,8,14],\"forward\":[0.600000024,0,0.800000012],"
		"\"server_hook_origin\":[%.9g,%.9g,%.9g]}\n",
		backoff[0], backoff[1], backoff[2]);
	action = Q2_HookClassifyTouch(0, 1, 0, 0, 1, probe_sky);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"sky\",\"op\":\"touch\","
		"\"owner_has_client\":true,\"surface_is_sky\":true,"
		"\"hook_sky\":%s,"
		"\"server_action\":\"%s\"}\n",
		probe_sky ? "true" : "false", Q2_HookTouchActionName(action));
	action = Q2_HookClassifyTouch(0, 1, 0, 0, 0, 0);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"attach\",\"op\":\"touch\","
		"\"owner_has_client\":true,\"server_action\":\"%s\"}\n",
		Q2_HookTouchActionName(action));

	/* Verify hook-driven collision and landing through q2ded's Pmove too. */
	VectorSet(owner, 0, 0, 80);
	VectorSet(hook, 0, 0, 0);
	direct_trace = gi.trace(owner, stand_mins, stand_maxs, trace_end,
		NULL, MASK_PLAYERSOLID);
	Q2_HookPullVelocity(owner, hook, hook, 0, probe_pullspeed, velocity);
	memset(&pm, 0, sizeof(pm));
	pm.s.pm_type = PM_NORMAL;
	pm.s.origin[0] = owner[0] * 8;
	pm.s.origin[1] = owner[1] * 8;
	pm.s.origin[2] = owner[2] * 8;
	pm.s.velocity[0] = velocity[0] * 8;
	pm.s.velocity[1] = velocity[1] * 8;
	pm.s.velocity[2] = velocity[2] * 8;
	pm.s.gravity = 800;
	pm.cmd.msec = 100;
	pm.trace = Hook_ProbePMTrace;
	pm.pointcontents = gi.pointcontents;
	gi.Pmove(&pm);
	gi.dprintf("Q2_HOOK_PARITY {\"id\":\"hook-landing\",\"op\":\"hook_landing\","
		"\"owner_origin\":[0,0,80],\"hook_origin\":[0,0,0],"
		"\"hook_pullspeed\":%.9g,\"gravity\":800,\"commands\":[{\"msec\":100}],"
		"\"server_origin_fixed\":[%d,%d,%d],"
		"\"server_velocity_fixed\":[%d,%d,%d],\"server_grounded\":%s,"
		"\"server_mins\":[%.9g,%.9g,%.9g],\"server_maxs\":[%.9g,%.9g,%.9g],"
		"\"server_direct_endpos\":[%.9g,%.9g,%.9g],\"server_direct_fraction\":%.9g,"
		"\"server_direct_entnum\":%d}\n",
		probe_pullspeed, pm.s.origin[0], pm.s.origin[1], pm.s.origin[2],
		pm.s.velocity[0], pm.s.velocity[1], pm.s.velocity[2],
		pm.groundentity ? "true" : "false",
		pm.mins[0], pm.mins[1], pm.mins[2], pm.maxs[0], pm.maxs[1], pm.maxs[2],
		direct_trace.endpos[0], direct_trace.endpos[1], direct_trace.endpos[2],
		direct_trace.fraction, direct_trace.ent ? (int)(direct_trace.ent - g_edicts) : -1);
}
#endif
