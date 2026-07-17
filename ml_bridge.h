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
#define ML_PROTOCOL_GENERATION 2u
#define ML_OBS_MAGIC    0x514D324F  /* "QM2O": multires/Atlas observation */
#define ML_ACT_MAGIC    0x514D3241  /* "QM2A": multires/Atlas action */
#define ML_TEACHER_MAGIC 0x5154345A /* "QT4Z" */
#define ML_TEACHER_VERSION 4

/* Private causal/debug facts.  This block is never embedded in ml_obs_t and
   therefore can never become policy input by taking a public observation
   slice.  Network-client and teacher packets carry this exact same contract. */
#define ML_CAUSAL_MAGIC   0x514D3343u /* "QM3C" */
#define ML_CAUSAL_VERSION 2u

#define ML_CAUSAL_TARGET_VALID          (1u << 0)
#define ML_CAUSAL_ENV_SOURCE_ACTIVE      (1u << 1)
#define ML_CAUSAL_ENV_SOURCE_EVIDENCE    (1u << 2)
#define ML_CAUSAL_ENV_DAMAGE            (1u << 3)
#define ML_CAUSAL_ENV_DEATH             (1u << 4)
#define ML_CAUSAL_ENV_SOURCE_CLEARED     (1u << 5)
#define ML_CAUSAL_CROUCH_EDGE_ACTIVE     (1u << 6)
#define ML_CAUSAL_CROUCH_EDGE_ENTERED    (1u << 7)
#define ML_CAUSAL_CROUCH_EDGE_COMPLETED  (1u << 8)
#define ML_CAUSAL_HOOK_ATTEMPTED         (1u << 9)
#define ML_CAUSAL_HOOK_ATTACHED          (1u << 10)
#define ML_CAUSAL_HOOK_VALID             (1u << 11)
#define ML_CAUSAL_HOOK_NECESSITY_KNOWN   (1u << 12)
#define ML_CAUSAL_HOOK_WAS_NECESSARY     (1u << 13)
#define ML_CAUSAL_ECHO_VALID             (1u << 14)
#define ML_CAUSAL_FACTS_COMPLETE         (1u << 15)
#define ML_CAUSAL_TRANSITION_TRAINABLE   (1u << 16)
#define ML_CAUSAL_TARGET_HIT              (1u << 17)
#define ML_CAUSAL_TARGET_KILLED           (1u << 18)
#define ML_CAUSAL_HOOK_INVALID            (1u << 19)
#define ML_CAUSAL_ROLE_PLAYING             (1u << 20)
#define ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL    (1u << 21)
#define ML_CAUSAL_FLAGS_MASK              ((1u << 22) - 1u)

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

#define ML_VERTICAL_DOWN_OR_CROUCH 0u
#define ML_VERTICAL_NEUTRAL        1u
#define ML_VERTICAL_UP_OR_JUMP     2u
#define ML_VERTICAL_COUNT          3u

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
#define ML_ENTITY_PROTECTED    0x10000
#define ML_ENTITY_DUCKED       0x20000
#define ML_ENTITY_EPOCH_SHIFT  18
#define ML_ENTITY_EPOCH_MASK   0xFFFC0000u

#define ML_FIRE_GATE_PROTECTED 0x01
#define ML_FIRE_GATE_TARGET    0x02
#define ML_FIRE_GATE_SUPPRESSED 0x04
#define ML_HIT_STREAK_SHIFT 8
#define ML_HIT_STREAK_MASK  0x0000FF00u
#define ML_ACTION_GENERATION_SHIFT 16
#define ML_ACTION_GENERATION_MASK  0x00FF0000u
#define ML_ACTION_GENERATION_COUNT 192u

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
    /* Eye-to-best-damageable-point in full client->v_angle
       forward/Quake-right/up coordinates, world units. */
    float rel_pos[3];
    /* target velocity minus shooter velocity in the same local basis */
    float vel[3];
    float health;
    float is_enemy;         /* 1=enemy 0=teammate */
    /* Exact clear-probe fraction. Positive is fire-actionable; negative is
       sensed but shooter-protected (aim/thermal may use the magnitude). */
    float visible;          /* -1..1 */
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
    uint32_t vertical_intent;
    int32_t  applied_upmove;
    uint32_t actual_ducked;
    uint32_t water_vertical_mode;
    uint32_t fire;
    uint32_t hook;
    uint32_t flags;
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

    /* q2-ml-bot extended reward channels — always sent, consumed by both
       runs' reward shaping (never part of the policy input vector). */
    float           reward_damage_taken_prox; /* hit hardness: take × proximity */
    float           reward_offense;           /* offense-rune + same-target focus payoff */
    float           reward_survival;          /* recovery payoff w/ regen|vampire */

    /* Factual observation extension. This block is mandatory in protocol
       generation 2 and is always part of the frozen 198-float factual input. */
    float           rune_flags[5];      /* resist, strength, haste, regen, vampire (0/1) */
    float           inbound_dmg_dir[3]; /* unit vector toward most recent attacker */
    float           inbound_dmg_dist;   /* units to that attacker, -1 if none */
    float           inbound_dmg_recency;/* 1.0 fresh → 0 by ~1s, decays per frame */
    float           actual_ducked;      /* resulting PMF_DUCKED state */
    float           standing_blocked;   /* standing hull cannot fit at origin */
    float           water_vertical_mode;/* waterlevel >= 2 */

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

    /* categorical vertical intent plus buttons */
    uint8_t     vertical_intent; /* ML_VERTICAL_* */
    uint8_t     fire;
    uint8_t     hook;           /* 0=idle 1=fire 2=hold 3=release */
    uint8_t     weapon;         /* 0=no-change, 1-9=select weapon */
} ml_action_t;

/* Authoritative event attribution and local admission facts.  Environmental
   source identity is damage-source provenance, never Atlas reward hazard
   component identity. All identities use zero for unavailable.
   action_generation stores generation+1 so zero cannot be confused with
   modulo-generation zero. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t flags;
    uint32_t tick;
    uint32_t client_life_epoch;
    uint32_t target_id;
    uint32_t target_epoch;
    uint32_t environmental_source_id;
    uint32_t environmental_source_epoch;
    uint32_t environmental_mod;
    uint32_t environmental_damage;
    uint32_t crouch_edge_id;
    uint32_t crouch_edge_epoch;
    uint32_t echo_tick;
    uint32_t action_generation;
    uint32_t hook_zone_id;
    uint32_t hook_attempt_tick;
    uint32_t hook_action_generation;
    uint32_t reserved;
} ml_causal_telemetry_t;

/* Passive 3ZB2 demonstration packet. Kept below the Tailscale MTU so one
   legacy-bot tick is always one UDP datagram. */
typedef struct {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    packet_size;
    uint32_t    sequence;
    uint32_t    tick;
    uint32_t    bot_slot;
    uint32_t    flags;       /* bit 0: grounded before action */
    char        map_name[32];
    ml_obs_t    obs;         /* state immediately before 3ZB2 acts */
    ml_causal_telemetry_t causal; /* physically separate; never policy-visible */
    ml_action_t action;      /* 3ZB2 result projected into ML action space */
} ml_teacher_sample_t;

_Static_assert(sizeof(ml_action_debug_t) == 60,
    "ml_action_debug_t wire size changed");
_Static_assert(sizeof(ml_obs_t) == 1056, "ml_obs_t wire size changed");
_Static_assert(sizeof(ml_action_t) == 28, "ml_action_t wire size changed");
_Static_assert(sizeof(ml_causal_telemetry_t) == 80,
    "causal telemetry wire size changed");
_Static_assert(sizeof(ml_teacher_sample_t) == 1224,
    "teacher sample wire size changed");


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

/* Fire-and-forget legacy-bot teacher sample. Never waits or retries. */
void ML_TeacherSend(struct edict_s *ent, const ml_obs_t *before,
                    float yaw_before, float pitch_before,
                    float velocity_z_before, int grounded_before,
                    int hook_before);

/* Pack and consume private per-transition causal events.  teacher_actual is
   true only for a post-action 3ZB2 demonstration, whose action is observed
   directly instead of generation-echoed over protocol 34. */
void ML_PackCausalTelemetry(struct edict_s *ent, ml_causal_telemetry_t *causal,
                            int teacher_actual);

/* Runtime hook callbacks supply actual attach validity to the causal lane. */
void ML_CausalHookAttempt(struct edict_s *ent);
void ML_CausalHookBindAction(struct edict_s *ent);
void ML_CausalHookAttached(struct edict_s *ent, const float *anchor);
int ML_CausalHookFireAccepted(int hook_out, float current_time,
                              float last_hook_time, float delay_seconds);
int ML_CausalHookOriginValid(uint32_t current_tick, uint32_t attempt_tick,
                             uint32_t attempt_generation,
                             int require_generation);
uint32_t ML_CausalEnvironmentalSourceEpoch(uint32_t current_epoch,
                                           uint32_t current_source_id,
                                           uint32_t event_source_id,
                                           int source_active,
                                           int clear_ticks);
int ML_HookNecessityBudgetProven(float walk_distance_lower_bound,
                                 float hook_travel_seconds,
                                 int zone_required);

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
