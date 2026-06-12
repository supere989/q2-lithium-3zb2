/*
 * q2-ml-bot: UDP bridge between game.so and Python training harness.
 *
 * Every server tick (10Hz) game.so packs an ml_obs_t and sends it to the
 * Python harness over UDP loopback.  The harness replies with ml_action_t.
 * Both structs are plain C POD — no pointers, fixed width, little-endian.
 *
 * One UDP socket per bot; port = ml_port_base + bot_slot.
 */

#ifndef ML_BRIDGE_H
#define ML_BRIDGE_H

#include <stdint.h>

#define ML_BASE_PORT    27950   /* bot 0 = 27950, bot 1 = 27951, ... */
#define ML_OBS_MAGIC    0x514D4C4F  /* "QMLO" */
#define ML_ACT_MAGIC    0x514D4C41  /* "QMLA" */

#define ML_MAX_ENTITIES 8       /* visible enemies/teammates in obs */
#define ML_RAY_COUNT    16      /* directional depth-trace rays */
#define ML_HOOK_ZONES   4       /* nearest annotated hook zones */

#define ML_CONTROL_UNKNOWN    0
#define ML_CONTROL_HUMAN      1
#define ML_CONTROL_ML_BOT     2
#define ML_CONTROL_LEGACY_BOT 3

#define ML_TERMINAL_NONE          0
#define ML_TERMINAL_DEATH         1
#define ML_TERMINAL_INTERMISSION  2

#define ML_ENTITY_CLIENT  0x01
#define ML_ENTITY_BOT     0x02
#define ML_ENTITY_ML      0x04
#define ML_ENTITY_VISIBLE 0x08
#define ML_ENTITY_DEAD    0x10
#define ML_ENTITY_OBSERVER  0x20
#define ML_ENTITY_SOLID_NOT 0x40
#define ML_ENTITY_NOCLIP    0x80
#define ML_ENTITY_NOCLIENT  0x100
#define ML_ENTITY_SPECTATOR 0x200
#define ML_ENTITY_FLY       0x400
#define ML_ENTITY_SWIM      0x800
#define ML_ENTITY_PM_SPECTATOR 0x1000
#define ML_ENTITY_PM_FREEZE    0x2000
#define ML_ENTITY_GROUNDED     0x4000
#define ML_ENTITY_PM_ON_GROUND 0x8000

/* ── Observation sent game.so → Python ───────────────────────────────── */

typedef struct {
    float pos[3];
    float vel[3];
    float health;           /* 0-200 */
    float armor;
    float weapon_id;        /* enum index */
    float ammo;
} ml_self_t;

typedef struct {
    float rel_pos[3];       /* relative to bot, world units */
    float vel[3];
    float health;
    float is_enemy;         /* 1=enemy 0=teammate */
    float visible;          /* 1=LOS clear */
} ml_entity_t;

typedef struct {
    uint32_t edict_index;    /* g_edicts index, 1..maxclients for players */
    uint32_t client_slot;    /* edict_index - 1 */
    uint32_t control_source; /* ML_CONTROL_* */
    uint32_t flags;          /* ML_ENTITY_* */
} ml_entity_debug_t;

typedef struct {
    uint32_t tick;           /* last action tick applied by the engine */
    uint32_t accepted;       /* 1 if last ML_BotStep matched the obs tick */
    uint32_t timeout_count;  /* cumulative rejected/timeout steps */
    uint32_t weapon;
    float    move_forward;
    float    move_right;
    float    look_yaw;
    float    look_pitch;
    uint32_t jump;
    uint32_t fire;
    uint32_t hook;
    uint32_t _pad;
} ml_action_debug_t;

typedef struct {
    float direction[3];     /* unit vector */
    float distance;         /* units to first solid, -1 if open */
} ml_ray_t;

typedef struct {
    float anchor[3];        /* hook attachment point, world coords */
    float landing[3];       /* expected landing zone */
    float distance;         /* from bot to anchor */
    float flags;            /* HOOK_CEILING=1 HOOK_WALL=2 HOOK_REQUIRED=4 */
} ml_hook_zone_t;

typedef struct {
    float sound_dir[3];     /* direction of last heard sound, or (0,0,0) */
    float sound_age;        /* frames since heard, 0=this frame */
    float alert_level;      /* 0-1 accumulated awareness */
} ml_audio_t;

typedef struct {
    uint32_t        magic;          /* ML_OBS_MAGIC */
    uint32_t        tick;           /* server framenum */
    uint32_t        bot_slot;
    float           yaw;            /* current facing yaw degrees */
    float           pitch;

    ml_self_t       self;
    ml_entity_t     entities[ML_MAX_ENTITIES];
    uint32_t        entity_count;

    ml_ray_t        rays[ML_RAY_COUNT];
    ml_hook_zone_t  hook_zones[ML_HOOK_ZONES];
    uint32_t        hook_zone_count;

    ml_audio_t      audio;

    /* reward shaping components (computed server-side) */
    float           reward_damage_dealt;
    float           reward_damage_taken;
    float           reward_kill;
    float           reward_death;
    float           reward_item_pickup;
    float           reward_hook_traversal;

    uint8_t         is_terminal;    /* 1 on death/level-change */
    uint8_t         terminal_reason;/* ML_TERMINAL_* */
    uint8_t         _pad[2];

    /* Debug-only identity metadata. This is intentionally kept out of the
       policy vector so existing checkpoints retain the same observation size. */
    ml_entity_debug_t self_debug;
    ml_entity_debug_t entity_debug[ML_MAX_ENTITIES];
    ml_action_debug_t action_debug;
} ml_obs_t;


/* ── Action sent Python → game.so ────────────────────────────────────── */

typedef struct {
    uint32_t    magic;          /* ML_ACT_MAGIC */
    uint32_t    tick;           /* must match obs tick */

    /* movement [-1,1] */
    float       move_forward;
    float       move_right;

    /* look delta degrees this tick */
    float       look_yaw;
    float       look_pitch;

    /* buttons */
    uint8_t     jump;
    uint8_t     fire;
    uint8_t     hook;           /* 0=idle 1=fire 2=hold 3=release */
    uint8_t     weapon;         /* 0=no-change, 1-9=select weapon */
} ml_action_t;


/* ── C API (called from bot.c / Bot_Think) ───────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declare so the prototypes don't create a function-scope struct. */
struct edict_s;

/* Call once per bot at spawn. Opens a UDP socket on ml_port_base+slot. */
int  ML_BotInit(int bot_slot);

/* Call every Bot_Think tick. Sends obs, waits up to timeout_ms for action.
   Returns 0 on success, -1 on timeout (use previous action). */
int  ML_BotStep(int bot_slot, const ml_obs_t *obs, ml_action_t *act,
                int timeout_ms);

/* Fire-and-forget obs send with no action wait. Used for terminal packets
   (death) where the harness will not reply; never blocks the server frame.
   Also phase 1 of two-phase lockstep (G_RunFrame pre-pass sends every ML
   bot's obs before any bot blocks). Returns 0 on send, -1 on error. */
int  ML_SendObsOnly(int bot_slot, const ml_obs_t *obs);

/* Two-phase lockstep, phase 2: block up to timeout_ms for the action
   answering the obs with this tick. Returns 0 on match, -1 on timeout
   (act holds the previous action as fallback). */
int  ML_RecvAction(int bot_slot, uint32_t tick, ml_action_t *act,
                   int timeout_ms);

/* Call on bot removal. Closes socket. */
void ML_BotShutdown(int bot_slot);

/* Fill obs rays from server-side traces. */
void ML_FillRays(struct edict_s *ent, ml_obs_t *obs);

/* Fill hook zones from nav annotation sidecar for current map. */
void ML_FillHookZones(struct edict_s *ent, ml_obs_t *obs);

/* Load hook zone sidecar when a new map loads (call from SpawnEntities). */
void ML_LoadHookZones(const char *mapname);

#ifdef __cplusplus
}
#endif

#endif /* ML_BRIDGE_H */
