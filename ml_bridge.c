/*
 * ml_bridge.c — UDP bridge between game.so Bot_Think and Python harness.
 *
 * Each bot slot owns one UDP socket (non-blocking send, blocking recv with
 * timeout).  The Python process binds the server side of each socket.
 *
 * Thread safety: not needed — all calls happen on the single game thread.
 */

#include "ml_bridge.h"
#include "ml_client_role.h"
#include "ml_client_respawn_settle.h"
#include "g_local.h"
#include "ml_obs.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

#define MAX_BOTS_ML     32
#define HARNESS_ADDR    "127.0.0.1"

typedef struct {
    int             fd;             /* UDP socket, -1 if unused */
    int             bot_slot;
    uint16_t        port;           /* ml_port_base + slot */
    struct sockaddr_in harness_addr;
    ml_action_t     last_action;    /* cached for timeout fallback */
    uint32_t        last_action_tick; /* newest tick ever applied (input cache) */
} ml_bot_sock_t;

static ml_bot_sock_t g_socks[MAX_BOTS_ML];
static int           g_initialized = 0;
static int           g_teacher_fd = -1;
static uint32_t      g_teacher_sequence = 0;
static struct sockaddr_in g_teacher_addr;

static int ml_action_valid(const ml_action_t *action) {
    return action && action->magic == ML_ACT_MAGIC &&
        action->vertical_intent < ML_VERTICAL_COUNT &&
        action->fire <= 1 && action->hook <= 3 && action->weapon <= 9 &&
        isfinite(action->move_forward) && isfinite(action->move_right) &&
        isfinite(action->look_yaw) && isfinite(action->look_pitch) &&
        fabsf(action->move_forward) <= 1.0001f &&
        fabsf(action->move_right) <= 1.0001f &&
        fabsf(action->look_yaw) <= 45.0001f &&
        fabsf(action->look_pitch) <= 30.0001f;
}

static float ml_teacher_clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

static float ml_teacher_angle_delta(float after, float before) {
    float delta = after - before;
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    return delta;
}

static uint8_t ml_teacher_weapon(edict_t *ent) {
    const char *name;
    if (!ent || !ent->client || !ent->client->pers.weapon)
        return 0;
    name = ent->client->pers.weapon->pickup_name;
    if (!name) return 0;
    if (!strcmp(name, "Blaster")) return 1;
    if (!strcmp(name, "Shotgun")) return 2;
    if (!strcmp(name, "Super Shotgun")) return 3;
    if (!strcmp(name, "Machinegun")) return 4;
    if (!strcmp(name, "Chaingun")) return 5;
    if (!strcmp(name, "Grenade Launcher")) return 6;
    if (!strcmp(name, "Rocket Launcher")) return 7;
    if (!strcmp(name, "HyperBlaster")) return 8;
    if (!strcmp(name, "Railgun")) return 9;
    return 0;
}

static int ml_teacher_init(void) {
    if (g_teacher_fd >= 0) return 0;
    g_teacher_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_teacher_fd < 0) return -1;
    memset(&g_teacher_addr, 0, sizeof(g_teacher_addr));
    g_teacher_addr.sin_family = AF_INET;
    g_teacher_addr.sin_port = htons(ml_teacher_port
        ? (uint16_t)ml_teacher_port->value : 32511);
    if (!ml_teacher_addr ||
        inet_pton(AF_INET, ml_teacher_addr->string, &g_teacher_addr.sin_addr) != 1) {
        close(g_teacher_fd);
        g_teacher_fd = -1;
        return -1;
    }
    return 0;
}

void ML_TeacherSend(edict_t *ent, const ml_obs_t *before,
                    float yaw_before, float pitch_before,
                    float velocity_z_before, int grounded_before,
                    int hook_before) {
    ml_teacher_sample_t sample;
    ml_action_t *action;
    vec3_t forward, right;
    vec3_t displacement, teacher_velocity;
    int hook_after;
    int stride;

    if (!ent || !ent->client || !before || !ml_teacher_enabled ||
        !ml_teacher_enabled->value || ent->client->zc.ml_enabled)
        return;
    stride = ml_teacher_stride ? (int)ml_teacher_stride->value : 1;
    if (stride < 1) stride = 1;
    if ((int)level.framenum % stride != 0 || ml_teacher_init() != 0)
        return;

    memset(&sample, 0, sizeof(sample));
    sample.magic = ML_TEACHER_MAGIC;
    sample.version = ML_TEACHER_VERSION;
    sample.packet_size = (uint32_t)sizeof(sample);
    sample.sequence = ++g_teacher_sequence;
    sample.tick = (uint32_t)level.framenum;
    sample.bot_slot = (uint32_t)(ent - g_edicts - 1);
    sample.flags = grounded_before ? 1u : 0u;
    strncpy(sample.map_name, level.mapname, sizeof(sample.map_name) - 1);
    sample.obs = *before;
    action = &sample.action;
    action->magic = ML_ACT_MAGIC;
    action->tick = sample.tick;
    action->look_yaw = ml_teacher_clamp(
        ml_teacher_angle_delta(ent->s.angles[YAW], yaw_before), -45.0f, 45.0f);
    action->look_pitch = ml_teacher_clamp(
        ml_teacher_angle_delta(ent->s.angles[PITCH], pitch_before), -30.0f, 30.0f);

    AngleVectors(ent->s.angles, forward, right, NULL);
    forward[2] = right[2] = 0.0f;
    VectorNormalize(forward);
    VectorNormalize(right);
    /* 3ZB2 locomotion uses direct walkmove-style origin changes rather than
       the player-command velocity that ML_ApplyAction consumes. Convert the
       observed per-frame displacement back to world units/second first. */
    VectorSubtract(ent->s.origin, before->self.pos, displacement);
    VectorScale(displacement, 1.0f / FRAMETIME, teacher_velocity);
    action->move_forward = ml_teacher_clamp(
        DotProduct(teacher_velocity, forward) / 320.0f, -1.0f, 1.0f);
    action->move_right = ml_teacher_clamp(
        DotProduct(teacher_velocity, right) / 320.0f, -1.0f, 1.0f);
    if (before->water_vertical_mode > 0.5f) {
        if (ent->velocity[2] > velocity_z_before + 50.0f)
            action->vertical_intent = ML_VERTICAL_UP_OR_JUMP;
        else if (ent->velocity[2] < velocity_z_before - 50.0f)
            action->vertical_intent = ML_VERTICAL_DOWN_OR_CROUCH;
        else
            action->vertical_intent = ML_VERTICAL_NEUTRAL;
    } else if (ent->client->ps.pmove.pm_flags & PMF_DUCKED) {
        action->vertical_intent = ML_VERTICAL_DOWN_OR_CROUCH;
    } else if ((grounded_before && !ent->groundentity) ||
        ent->velocity[2] > velocity_z_before + 50.0f) {
        action->vertical_intent = ML_VERTICAL_UP_OR_JUMP;
    } else {
        action->vertical_intent = ML_VERTICAL_NEUTRAL;
    }
    action->fire = (uint8_t)((ent->client->buttons & BUTTON_ATTACK) != 0);
    hook_after = ent->client->hook_on || ent->client->ctf_grapple != NULL;
    action->hook = (uint8_t)(
        !hook_before && hook_after ? 1 :
        hook_before && !hook_after ? 3 :
        hook_after ? 2 : 0);
    action->weapon = ml_teacher_weapon(ent);

    ML_PackCausalTelemetry(ent, &sample.causal, 1);

    sendto(g_teacher_fd, &sample, sizeof(sample), MSG_DONTWAIT,
           (struct sockaddr *)&g_teacher_addr, sizeof(g_teacher_addr));
}

static void ml_global_init(void) {
    if (g_initialized) return;
    memset(g_socks, 0, sizeof(g_socks));
    for (int i = 0; i < MAX_BOTS_ML; i++) g_socks[i].fd = -1;
    g_initialized = 1;
}

int ML_BotInit(int bot_slot) {
    ml_global_init();
    if (bot_slot < 0 || bot_slot >= MAX_BOTS_ML) return -1;

    ml_bot_sock_t *s = &g_socks[bot_slot];
    if (s->fd >= 0) close(s->fd);

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) {
        gi.dprintf("ML_BotInit: socket() failed for slot %d: %s\n",
                   bot_slot, strerror(errno));
        return -1;
    }

    int base_port = ML_BASE_PORT;
    if (ml_port_base && ml_port_base->value > 0) {
        base_port = (int)ml_port_base->value;
    }
    s->bot_slot = bot_slot;
    s->port     = (uint16_t)(base_port + bot_slot);

    memset(&s->harness_addr, 0, sizeof(s->harness_addr));
    s->harness_addr.sin_family      = AF_INET;
    s->harness_addr.sin_port        = htons(s->port);
    inet_pton(AF_INET, HARNESS_ADDR, &s->harness_addr.sin_addr);

    /* default action: stand still */
    memset(&s->last_action, 0, sizeof(s->last_action));
    s->last_action.magic = ML_ACT_MAGIC;
    s->last_action.vertical_intent = ML_VERTICAL_NEUTRAL;
    s->last_action_tick = 0;

    gi.dprintf("ML: bot slot %d → UDP port %d\n", bot_slot, s->port);
    return 0;
}

int ML_BotStep(int bot_slot, const ml_obs_t *obs, ml_action_t *act,
               int timeout_ms) {
    if (bot_slot < 0 || bot_slot >= MAX_BOTS_ML) return -1;
    ml_bot_sock_t *s = &g_socks[bot_slot];
    if (s->fd < 0) { *act = s->last_action; return -1; }
    ml_action_t queued_action;
    int got_queued = 0;

    /* Drain queued actions, keeping the newest decision we have not yet
       applied (newest-action-wins input cache). Exact tick match is not
       required: in pipelined multi-bot operation a fresh decision for the
       previous frame is far better than discarding it and coasting. */
    while (1) {
        ml_action_t incoming;
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming),
                         MSG_DONTWAIT | MSG_TRUNC);
        if (n == sizeof(incoming)) {
            if (ml_action_valid(&incoming) &&
                incoming.tick > s->last_action_tick &&
                (!got_queued || incoming.tick >= queued_action.tick)) {
                queued_action = incoming;
                got_queued = 1;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        break;
    }

    /* send observation */
    ssize_t sent = sendto(s->fd, obs, sizeof(*obs), 0,
                          (struct sockaddr *)&s->harness_addr,
                          sizeof(s->harness_addr));
    if (sent != sizeof(*obs)) {
        *act = s->last_action;
        return -1;
    }

    /* Apply the newest drained decision immediately */
    if (got_queued) {
        s->last_action = queued_action;
        s->last_action_tick = queued_action.tick;
        *act = queued_action;
        return 0;
    }

    /* Pipelined (async) mode: never block the frame — coast on the cached
       action; the decision for this obs will be drained next frame. */
    if (timeout_ms <= 0) {
        *act = s->last_action;
        return 0;
    }

    /* Lockstep mode: wait for this frame's action with timeout */
    struct timeval tv = { 0, timeout_ms * 1000 };
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        ml_action_t incoming;
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming), MSG_TRUNC);
        if (n < 0)
            break;
        if (n == sizeof(incoming) && ml_action_valid(&incoming)) {
            if (incoming.tick == obs->tick) {
                s->last_action = incoming;
                s->last_action_tick = incoming.tick;
                *act = incoming;
                return 0;
            }
        }
    }

    /* timeout or bad packet — reuse last action (fallback) */
    *act = s->last_action;
    return -1;
}


/* Two-phase lockstep, phase 2: block for this frame's action. The obs was
   already sent for every ML bot in the G_RunFrame pre-pass (phase 1), so
   the harness sees the whole frame's observations before any bot blocks —
   sequential per-bot send-then-block deadlocked multi-bot lockstep. */
int ML_RecvAction(int bot_slot, uint32_t tick, ml_action_t *act,
                  int timeout_ms) {
    if (bot_slot < 0 || bot_slot >= MAX_BOTS_ML) return -1;
    ml_bot_sock_t *s = &g_socks[bot_slot];
    if (s->fd < 0) { *act = s->last_action; return -1; }

    /* Self-arming lockstep: never block LONG before the harness has
       answered this socket at least once. ML bots spawn staggered at
       boot, and a bot blocking its FULL timeout every frame before the
       harness is even listening delays every later spawn — with 4 ML
       slots the last bot took ~60s to appear and the harness's 25s
       reset window starved.

       BUGFIX (2026-07-10): the original MSG_DONTWAIT (zero-wait) check
       here meant this bootstrap path could only ever succeed if
       Python's reply for THIS SAME frame's just-sent obs was already
       sitting in the socket buffer at the exact instant this runs —
       i.e. a real network+inference round-trip completing in zero
       elapsed C instructions. In a many-bot training run there's
       enough inter-bot processing time within one frame for that
       race to occasionally resolve favorably; in a single/solo-bot
       live deployment there is no such slack, so last_action_tick
       NEVER left 0 and every action silently applied as the
       zero-initialized fallback forever (bot "stuck at spawn").
       Give this bootstrap check a short real timeout (bounded, so
       staggered multi-bot startup still can't compound into the old
       60s-delay problem) instead of a zero-wait check. */
    if (s->last_action_tick == 0) {
        ml_action_t incoming;
        ssize_t n;
        struct timeval btv;
        btv.tv_sec  = 0;
        btv.tv_usec = 50000; /* 50ms — enough for a loopback round-trip,
                                 bounded so N bots cold-starting can't
                                 stack into a multi-second stall */
        setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &btv, sizeof(btv));
        while (1) {
            n = recv(s->fd, &incoming, sizeof(incoming), MSG_TRUNC);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;                       /* timeout — not ready yet */
            }
            if (n == sizeof(incoming) && ml_action_valid(&incoming)) {
                s->last_action = incoming;
                s->last_action_tick = incoming.tick;
                *act = incoming;
                return 0;
            }
        }
        *act = s->last_action;
        return -1;
    }

    /* The harness answers a frame with one burst of actions for every
       bot, so only the FIRST waiter of a frame needs the long timeout
       (covering the trainer's full step). Later waiters' actions are
       either already queued or never coming — without this cap a frame
       the harness skipped cost n_bots stacked timeouts, and the harness
       could never re-synchronize inside its own step deadline. */
    static uint32_t long_wait_frame = 0;
    if (tick == long_wait_frame && timeout_ms > 100)
        timeout_ms = 100;
    else
        long_wait_frame = tick;

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        ml_action_t incoming;
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming), MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;                           /* timeout */
        }
        if (n == sizeof(incoming) && ml_action_valid(&incoming) &&
            incoming.tick == tick) {
            s->last_action = incoming;
            s->last_action_tick = incoming.tick;
            *act = incoming;
            return 0;
        }
    }

    /* timeout or bad packet — reuse last action (fallback) */
    *act = s->last_action;
    return -1;
}

int ML_SendObsOnly(int bot_slot, const ml_obs_t *obs) {
    if (bot_slot < 0 || bot_slot >= MAX_BOTS_ML) return -1;
    ml_bot_sock_t *s = &g_socks[bot_slot];
    if (s->fd < 0) return -1;

    /* drain any stale queued actions so the socket buffer stays clean */
    while (1) {
        ml_action_t incoming;
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming),
                         MSG_DONTWAIT | MSG_TRUNC);
        if (n >= 0) continue;               /* discard stale packet, keep draining */
        if (errno == EINTR) continue;
        break;                              /* EAGAIN/EWOULDBLOCK or real error */
    }

    ssize_t sent = sendto(s->fd, obs, sizeof(*obs), 0,
                          (struct sockaddr *)&s->harness_addr,
                          sizeof(s->harness_addr));
    return (sent == sizeof(*obs)) ? 0 : -1;
}

void ML_BotShutdown(int bot_slot) {
    if (bot_slot < 0 || bot_slot >= MAX_BOTS_ML) return;
    ml_bot_sock_t *s = &g_socks[bot_slot];
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
}

/* ── Ray casting ────────────────────────────────────────────────────── */

void ML_FillRays(edict_t *ent, ml_obs_t *obs) {
    static const float angles[ML_RAY_COUNT] = {
        0, 22.5f, 45, 67.5f, 90, 112.5f, 135, 157.5f,
        180, 202.5f, 225, 247.5f, 270, 292.5f, 315, 337.5f
    };

    vec3_t origin;
    VectorCopy(ent->s.origin, origin);
    origin[2] += ent->viewheight;

    float yaw_rad = ent->s.angles[YAW] * (M_PI / 180.0f);

    for (int i = 0; i < ML_RAY_COUNT; i++) {
        float a = (angles[i] + ent->s.angles[YAW]) * (M_PI / 180.0f);
        vec3_t end;
        end[0] = origin[0] + cosf(a) * 2048.0f;
        end[1] = origin[1] + sinf(a) * 2048.0f;
        end[2] = origin[2];

        trace_t tr = gi.trace(origin, NULL, NULL, end, ent,
                              CONTENTS_SOLID | CONTENTS_WINDOW);

        obs->rays[i].direction[0] = cosf(a);
        obs->rays[i].direction[1] = sinf(a);
        obs->rays[i].direction[2] = 0;
        obs->rays[i].distance = (tr.fraction < 1.0f)
                                ? tr.fraction * 2048.0f
                                : -1.0f;
    }
    (void)yaw_rad;
}

/* ── Hook zone lookup ───────────────────────────────────────────────── */
/*
 * Hook zone sidecar format (maps/<mapname>.json, actually a simple text file):
 *   # comment lines
 *   anchor_x anchor_y anchor_z  landing_x landing_y landing_z  distance  flags
 *
 * Loaded once at map start via ML_LoadHookZones().
 * ML_FillHookZones() fills the nearest ML_HOOK_ZONES zones per tick.
 */

#define MAX_HOOK_ZONES 256

typedef struct {
    float anchor[3];
    float landing[3];
    float distance;
    int   flags;
} hook_zone_t;

static hook_zone_t g_hook_zones[MAX_HOOK_ZONES];
static int         g_hook_zone_count = 0;

#define ML_HOOK_REQUIRED_FLAG 4
#define ML_HOOK_ZONE_MATCH_DISTANCE 96.0f
#define ML_HOOK_RECOVERY_WALK_BUDGET_TICKS 15
#define ML_CAUSAL_ENV_SOURCE_CLEAR_TICKS 3

extern lvar_t *hook_speed;
extern lvar_t *hook_pullspeed;

void ML_LoadHookZones(const char *mapname)
{
    char path[256];
    FILE *f;
    char line[256];
    int n = 0;

    /* Try both the mod dir and baseq2 since Q2 uses VFS but we use fopen */
    snprintf(path, sizeof(path), "baseq2/maps/%s.json", mapname);
    f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "lithium/maps/%s.json", mapname);
        f = fopen(path, "r");
    }
    if (!f) {
        gi.dprintf("ML: no hook zones sidecar for %s\n", mapname);
        g_hook_zone_count = 0;
        return;
    }

    while (fgets(line, sizeof(line), f) && n < MAX_HOOK_ZONES) {
        if (line[0] == '#' || line[0] == '\n') continue;
        hook_zone_t *z = &g_hook_zones[n];
        if (sscanf(line, "%f %f %f %f %f %f %f %d",
                   &z->anchor[0],  &z->anchor[1],  &z->anchor[2],
                   &z->landing[0], &z->landing[1], &z->landing[2],
                   &z->distance,   &z->flags) == 8)
            n++;
    }
    fclose(f);

    g_hook_zone_count = n;
    gi.dprintf("ML: loaded %d hook zones for %s\n", n, mapname);
}

void ML_FillHookZones(edict_t *ent, ml_obs_t *obs)
{
    int i, out = 0;
    float best_dist[ML_HOOK_ZONES];
    int   best_idx[ML_HOOK_ZONES];

    if (g_hook_zone_count == 0) {
        obs->hook_zone_count = 0;
        memset(obs->hook_zones, 0, sizeof(obs->hook_zones));
        return;
    }

    /* initialise nearest-N tracker */
    for (i = 0; i < ML_HOOK_ZONES; i++) { best_dist[i] = 1e18f; best_idx[i] = -1; }

    for (i = 0; i < g_hook_zone_count; i++) {
        float dx = g_hook_zones[i].anchor[0] - ent->s.origin[0];
        float dy = g_hook_zones[i].anchor[1] - ent->s.origin[1];
        float dz = g_hook_zones[i].anchor[2] - ent->s.origin[2];
        float d  = dx*dx + dy*dy + dz*dz;

        /* insertion into sorted top-N */
        for (int k = 0; k < ML_HOOK_ZONES; k++) {
            if (d < best_dist[k]) {
                /* shift down */
                for (int m = ML_HOOK_ZONES - 1; m > k; m--) {
                    best_dist[m] = best_dist[m-1];
                    best_idx[m]  = best_idx[m-1];
                }
                best_dist[k] = d;
                best_idx[k]  = i;
                break;
            }
        }
    }

    out = 0;
    for (i = 0; i < ML_HOOK_ZONES; i++) {
        if (best_idx[i] < 0) break;
        hook_zone_t *z = &g_hook_zones[best_idx[i]];
        obs->hook_zones[out].anchor[0]  = z->anchor[0];
        obs->hook_zones[out].anchor[1]  = z->anchor[1];
        obs->hook_zones[out].anchor[2]  = z->anchor[2];
        obs->hook_zones[out].landing[0] = z->landing[0];
        obs->hook_zones[out].landing[1] = z->landing[1];
        obs->hook_zones[out].landing[2] = z->landing[2];
        obs->hook_zones[out].distance   = z->distance;
        obs->hook_zones[out].flags      = (float)z->flags;
        out++;
    }
    obs->hook_zone_count = (uint32_t)out;
}

static uint32_t ml_causal_hash(const void *data, size_t length, uint32_t hash)
{
    const unsigned char *bytes = data;
    size_t i;
    for (i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t ml_crouch_edge_id(edict_t *ent)
{
    int32_t cell[3];
    uint32_t hash = 2166136261u;
    int i;

    for (i = 0; i < 3; i++)
        cell[i] = (int32_t)floorf(ent->s.origin[i] / 64.0f);
    hash = ml_causal_hash(level.mapname,
        strnlen(level.mapname, MAX_QPATH), hash);
    hash = ml_causal_hash(cell, sizeof(cell), hash);
    return hash ? hash : 1u;
}

static qboolean ml_standing_blocked(edict_t *ent)
{
    vec3_t mins = {-16.0f, -16.0f, -24.0f};
    vec3_t maxs = {16.0f, 16.0f, 32.0f};
    /* Crouch-edge authority is structural.  MASK_PLAYERSOLID includes live
       clients, which would turn a temporary body overlap into a rewardable
       low-clearance edge. */
    trace_t trace = gi.trace(ent->s.origin, mins, maxs, ent->s.origin,
        ent, MASK_DEADSOLID);
    return trace.startsolid || trace.allsolid;
}

static qboolean ml_current_environmental_source(edict_t *ent, int mod)
{
    if (!ent)
        return qfalse;
    if (mod == MOD_LAVA)
        return (ent->watertype & CONTENTS_LAVA) != 0;
    if (mod == MOD_SLIME)
        return (ent->watertype & CONTENTS_SLIME) != 0;
    if (mod == MOD_WATER)
        return ent->waterlevel == 3;
    /* Falling/crusher/hurt source attribution is cleared only after a
       supported, non-solid pose is observed; this is causal engine state,
       not Atlas safe-arrival authority or a generic no-damage timer. */
    return !ent->groundentity || ml_standing_blocked(ent);
}

static int ml_find_hook_zone(const float *anchor)
{
    float best = ML_HOOK_ZONE_MATCH_DISTANCE * ML_HOOK_ZONE_MATCH_DISTANCE;
    int best_index = -1;
    int i;

    if (!anchor)
        return -1;
    for (i = 0; i < g_hook_zone_count; i++) {
        float dx = g_hook_zones[i].anchor[0] - anchor[0];
        float dy = g_hook_zones[i].anchor[1] - anchor[1];
        float dz = g_hook_zones[i].anchor[2] - anchor[2];
        float distance = dx * dx + dy * dy + dz * dz;
        if (distance <= best) {
            best = distance;
            best_index = i;
        }
    }
    return best_index;
}

int ML_HookNecessityBudgetProven(float walk_distance_lower_bound,
    float hook_travel_seconds, int zone_required)
{
    const float budget_seconds = FRAMETIME *
        ML_HOOK_RECOVERY_WALK_BUDGET_TICKS;
    return zone_required && isfinite(walk_distance_lower_bound) &&
        isfinite(hook_travel_seconds) && walk_distance_lower_bound >
            320.0f * budget_seconds && hook_travel_seconds >= 0.0f &&
        hook_travel_seconds <= budget_seconds;
}

int ML_CausalHookFireAccepted(int hook_out, float current_time,
    float last_hook_time, float delay_seconds)
{
    return !hook_out && isfinite(current_time) && isfinite(last_hook_time) &&
        isfinite(delay_seconds) &&
        current_time >= last_hook_time + delay_seconds;
}

int ML_CausalHookOriginValid(uint32_t current_tick, uint32_t attempt_tick,
    uint32_t attempt_generation, int require_generation)
{
    if (!attempt_tick || attempt_tick > current_tick)
        return 0;
    if (require_generation)
        return attempt_generation > 0 &&
            attempt_generation <= ML_ACTION_GENERATION_COUNT;
    return attempt_generation == 0;
}

uint32_t ML_CausalEnvironmentalSourceEpoch(uint32_t current_epoch,
    uint32_t current_source_id, uint32_t event_source_id,
    int source_active, int clear_ticks)
{
    uint32_t next;
    if (current_epoch && current_source_id == event_source_id &&
        (source_active || clear_ticks < 30))
        return current_epoch;
    next = current_epoch + 1u;
    return next ? next : 1u;
}

/* Conservative runtime proof for the frozen 15-tick necessity label.
   Euclidean distance supplies a lower bound on every walking path: beyond
   320 u/s * 1.5 s, no walk path can arrive inside the budget.  A matching
   required sidecar zone, a currently clear anchor trace, a legal supported
   landing hull, and a straight-line hook travel upper bound prove the hook
   candidate.  Anything not proven stays unknown and is not trainable. */
static qboolean ml_hook_necessity(edict_t *ent, const float *anchor,
    int zone_index, qboolean *known)
{
    hook_zone_t *zone;
    vec3_t eye, landing_down, walk_delta, hook_delta, pull_delta;
    vec3_t mins = {-16.0f, -16.0f, -24.0f};
    vec3_t maxs = {16.0f, 16.0f, 32.0f};
    trace_t anchor_trace, landing_trace, support_trace;
    float walk_lower, hook_seconds;

    *known = qfalse;
    if (!ent || !ent->client || zone_index < 0 ||
        zone_index >= g_hook_zone_count || !anchor || !hook_speed ||
        !hook_pullspeed || hook_speed->value <= 0.0f ||
        hook_pullspeed->value <= 0.0f)
        return qfalse;
    zone = &g_hook_zones[zone_index];
    VectorCopy(ent->s.origin, eye);
    eye[2] += ent->viewheight;
    anchor_trace = gi.trace(eye, NULL, NULL, zone->anchor, ent, MASK_SHOT);
    if (anchor_trace.startsolid || anchor_trace.allsolid)
        return qfalse;
    if (anchor_trace.fraction < 0.999f) {
        vec3_t trace_delta;
        VectorSubtract(anchor_trace.endpos, zone->anchor, trace_delta);
        if (VectorLength(trace_delta) > 8.0f &&
            (!ent->client->hook || anchor_trace.ent != ent->client->hook->enemy))
            return qfalse;
    }
    landing_trace = gi.trace(zone->landing, mins, maxs, zone->landing,
        ent, MASK_PLAYERSOLID);
    VectorCopy(zone->landing, landing_down);
    landing_down[2] -= 48.0f;
    support_trace = gi.trace(zone->landing, mins, maxs, landing_down,
        ent, MASK_PLAYERSOLID);
    if (landing_trace.startsolid || landing_trace.allsolid ||
        support_trace.startsolid || support_trace.allsolid ||
        support_trace.fraction >= 1.0f || support_trace.plane.normal[2] < 0.7f)
        return qfalse;

    VectorSubtract(zone->landing, ent->s.origin, walk_delta);
    walk_delta[2] = 0.0f;
    walk_lower = VectorLength(walk_delta);
    VectorSubtract(anchor, ent->s.origin, hook_delta);
    VectorSubtract(zone->landing, anchor, pull_delta);
    hook_seconds = VectorLength(hook_delta) / hook_speed->value +
        VectorLength(pull_delta) / hook_pullspeed->value;
    *known = qtrue;
    return ML_HookNecessityBudgetProven(walk_lower, hook_seconds,
        (zone->flags & ML_HOOK_REQUIRED_FLAG) != 0) ? qtrue : qfalse;
}

void ML_CausalHookAttempt(edict_t *ent)
{
    zgcl_t *zc;
    if (!ent || !ent->client)
        return;
    zc = &ent->client->zc;
    zc->ml_hook_attempt_frame = level.framenum;
    zc->ml_hook_attempt_tick = (uint32_t)level.framenum;
    zc->ml_hook_action_generation = 0;
    zc->ml_hook_attempted = 1;
    zc->ml_hook_attached = 0;
    zc->ml_hook_valid = 0;
    zc->ml_hook_invalid = 0;
    zc->ml_hook_necessity_known = 0;
    zc->ml_hook_was_necessary = 0;
    zc->ml_hook_zone_id = 0;
    ML_CausalHookBindAction(ent);
}

void ML_CausalHookBindAction(edict_t *ent)
{
    zgcl_t *zc;
    if (!ent || !ent->client)
        return;
    zc = &ent->client->zc;
    if (!zc->ml_hook_attempted ||
        zc->ml_hook_attempt_frame != level.framenum ||
        !zc->ml_last_action_ok ||
        zc->ml_last_action_tick != level.framenum || zc->ml_hook != 1 ||
        !zc->ml_action_generation_valid)
        return;
    zc->ml_hook_attempt_tick = (uint32_t)zc->ml_last_action_tick;
    zc->ml_hook_action_generation =
        (uint32_t)zc->ml_action_generation + 1u;
}

void ML_CausalHookAttached(edict_t *ent, const float *anchor)
{
    zgcl_t *zc;
    qboolean known;
    int zone_index;

    if (!ent || !ent->client)
        return;
    zc = &ent->client->zc;
    zone_index = ml_find_hook_zone(anchor);
    zc->ml_hook_attached = 1;
    zc->ml_hook_valid = 1; /* Hook_Touch is the engine attach authority. */
    zc->ml_hook_invalid = 0;
    zc->ml_hook_zone_id = zone_index >= 0 ? (uint32_t)zone_index + 1u : 0u;
    zc->ml_hook_was_necessary = ml_hook_necessity(ent, anchor, zone_index,
        &known) ? 1 : 0;
    zc->ml_hook_necessity_known = known ? 1 : 0;
}

void ML_PackCausalTelemetry(edict_t *ent, ml_causal_telemetry_t *causal,
    int teacher_actual)
{
    zgcl_t *zc;
    qboolean ducked, blocked, echo_valid, facts_complete, settling;
    uint32_t role_flags;

    memset(causal, 0, sizeof(*causal));
    causal->magic = ML_CAUSAL_MAGIC;
    causal->version = ML_CAUSAL_VERSION;
    causal->packet_size = (uint32_t)sizeof(*causal);
    causal->tick = (uint32_t)level.framenum;
    if (!ent || !ent->client)
        return;
    zc = &ent->client->zc;
    causal->client_life_epoch = ML_ClientLifeEpoch(ent);

    ducked = (ent->client->ps.pmove.pm_flags & PMF_DUCKED) != 0;
    blocked = ml_standing_blocked(ent);
    if (ducked && blocked && !zc->ml_crouch_edge_active) {
        zc->ml_crouch_edge_id = ml_crouch_edge_id(ent);
        zc->ml_crouch_edge_epoch++;
        if (!zc->ml_crouch_edge_epoch)
            zc->ml_crouch_edge_epoch = 1;
        zc->ml_crouch_edge_active = 1;
        zc->ml_crouch_edge_entered = 1;
    } else if (zc->ml_crouch_edge_active && !ducked && !blocked) {
        zc->ml_crouch_edge_active = 0;
        zc->ml_crouch_edge_completed = 1;
    }

    if (zc->ml_hook_attempt_frame > 0 && !zc->ml_hook_attached &&
        !zc->ml_hook_invalid &&
        level.framenum - zc->ml_hook_attempt_frame >=
            ML_HOOK_RECOVERY_WALK_BUDGET_TICKS) {
        zc->ml_hook_invalid = 1;
        zc->ml_hook_necessity_known = 1;
        zc->ml_hook_was_necessary = 0;
    }

    if (zc->ml_environmental_source_active && !ent->deadflag &&
        zc->ml_environmental_damage == 0 &&
        !ml_current_environmental_source(ent, zc->ml_environmental_mod) &&
        (ent->groundentity || (ent->waterlevel >= 2 &&
            !(ent->watertype & (CONTENTS_LAVA | CONTENTS_SLIME)))) &&
        !blocked) {
        zc->ml_environmental_source_clear_ticks++;
        if (zc->ml_environmental_source_clear_ticks >=
            ML_CAUSAL_ENV_SOURCE_CLEAR_TICKS) {
            zc->ml_environmental_source_active = 0;
            zc->ml_environmental_source_cleared = 1;
        }
    } else if (!zc->ml_environmental_source_active && !ent->deadflag &&
        zc->ml_environmental_source_id &&
        zc->ml_environmental_source_clear_ticks < 30) {
        zc->ml_environmental_source_clear_ticks++;
    }

    causal->target_id = zc->ml_causal_target_edict > 0
        ? (uint32_t)zc->ml_causal_target_edict : 0u;
    causal->target_epoch = zc->ml_causal_target_epoch;
    causal->environmental_source_id = zc->ml_environmental_source_id;
    causal->environmental_source_epoch =
        zc->ml_environmental_source_epoch;
    causal->environmental_mod = zc->ml_environmental_mod > 0
        ? (uint32_t)zc->ml_environmental_mod : 0u;
    causal->environmental_damage = zc->ml_environmental_damage;
    causal->crouch_edge_id = zc->ml_crouch_edge_id;
    causal->crouch_edge_epoch = zc->ml_crouch_edge_epoch;
    causal->echo_tick = teacher_actual ? causal->tick :
        (zc->ml_last_action_tick > 0 ? (uint32_t)zc->ml_last_action_tick : 0u);
    causal->action_generation = !teacher_actual &&
        zc->ml_action_generation_valid
        ? (uint32_t)zc->ml_action_generation + 1u : 0u;
    causal->hook_zone_id = zc->ml_hook_zone_id;
    causal->hook_attempt_tick = zc->ml_hook_attempt_tick;
    causal->hook_action_generation = teacher_actual ? 0u :
        zc->ml_hook_action_generation;

    if (causal->target_id && causal->target_epoch)
        causal->flags |= ML_CAUSAL_TARGET_VALID;
    if (zc->ml_environmental_source_active)
        causal->flags |= ML_CAUSAL_ENV_SOURCE_ACTIVE;
    if (zc->ml_environmental_damage || zc->ml_environmental_death ||
        zc->ml_environmental_source_active ||
        zc->ml_environmental_source_cleared)
        causal->flags |= ML_CAUSAL_ENV_SOURCE_EVIDENCE;
    if (zc->ml_environmental_damage)
        causal->flags |= ML_CAUSAL_ENV_DAMAGE;
    if (zc->ml_environmental_death)
        causal->flags |= ML_CAUSAL_ENV_DEATH;
    if (zc->ml_environmental_source_cleared)
        causal->flags |= ML_CAUSAL_ENV_SOURCE_CLEARED;
    if (zc->ml_crouch_edge_active)
        causal->flags |= ML_CAUSAL_CROUCH_EDGE_ACTIVE;
    if (zc->ml_crouch_edge_entered)
        causal->flags |= ML_CAUSAL_CROUCH_EDGE_ENTERED;
    if (zc->ml_crouch_edge_completed)
        causal->flags |= ML_CAUSAL_CROUCH_EDGE_COMPLETED;
    if (zc->ml_hook_attempted)
        causal->flags |= ML_CAUSAL_HOOK_ATTEMPTED;
    if (zc->ml_hook_attached)
        causal->flags |= ML_CAUSAL_HOOK_ATTACHED;
    if (zc->ml_hook_valid)
        causal->flags |= ML_CAUSAL_HOOK_VALID;
    if (zc->ml_hook_invalid)
        causal->flags |= ML_CAUSAL_HOOK_INVALID;
    if (zc->ml_hook_necessity_known)
        causal->flags |= ML_CAUSAL_HOOK_NECESSITY_KNOWN;
    if (zc->ml_hook_was_necessary)
        causal->flags |= ML_CAUSAL_HOOK_WAS_NECESSARY;
    if (zc->ml_causal_target_hit)
        causal->flags |= ML_CAUSAL_TARGET_HIT;
    if (zc->ml_causal_target_killed)
        causal->flags |= ML_CAUSAL_TARGET_KILLED;

    /* This is private causal admission state, never policy observation.  A
       routed packet earns the positive role fact only from the authoritative
       ordinary-player state.  Teacher packets retain their separate contract. */
    role_flags = teacher_actual ? 0u : ML_ClientRoleCausalFlags(ent);
    causal->flags |= role_flags;

    echo_valid = teacher_actual || (zc->ml_last_action_ok &&
        zc->ml_last_action_tick > 0 && zc->ml_action_generation_valid);
    facts_complete = causal->client_life_epoch != 0;
    if (!teacher_actual && !(role_flags & ML_CAUSAL_ROLE_PLAYING))
        facts_complete = qfalse;
    settling = !teacher_actual &&
        (zc->ml_respawn_settling_action ||
        (ent->client->ps.pmove.pm_flags & PMF_TIME_TELEPORT));
    if ((zc->ml_causal_target_hit || zc->ml_causal_target_killed) &&
        !(causal->flags & ML_CAUSAL_TARGET_VALID))
        facts_complete = qfalse;
    if ((causal->flags & (ML_CAUSAL_ENV_SOURCE_EVIDENCE |
            ML_CAUSAL_ENV_DAMAGE | ML_CAUSAL_ENV_DEATH |
            ML_CAUSAL_ENV_SOURCE_CLEARED)) &&
        (!causal->environmental_source_id ||
         !causal->environmental_source_epoch ||
         !causal->environmental_mod))
        facts_complete = qfalse;
    if ((causal->flags & (ML_CAUSAL_CROUCH_EDGE_ACTIVE |
            ML_CAUSAL_CROUCH_EDGE_ENTERED |
            ML_CAUSAL_CROUCH_EDGE_COMPLETED)) &&
        (!causal->crouch_edge_id || !causal->crouch_edge_epoch))
        facts_complete = qfalse;
    if ((causal->flags & (ML_CAUSAL_HOOK_ATTEMPTED |
            ML_CAUSAL_HOOK_ATTACHED | ML_CAUSAL_HOOK_VALID |
            ML_CAUSAL_HOOK_INVALID | ML_CAUSAL_HOOK_NECESSITY_KNOWN |
            ML_CAUSAL_HOOK_WAS_NECESSARY)) &&
        !ML_CausalHookOriginValid(causal->tick,
            causal->hook_attempt_tick, causal->hook_action_generation,
            teacher_actual ? 0 : 1))
        facts_complete = qfalse;
    if (echo_valid)
        causal->flags |= ML_CAUSAL_ECHO_VALID;
    if (facts_complete)
        causal->flags |= ML_CAUSAL_FACTS_COMPLETE;
    if (ML_ClientCausalTransitionTrainable(
        echo_valid, facts_complete, settling) &&
        (teacher_actual ||
        (role_flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL)))
        causal->flags |= ML_CAUSAL_TRANSITION_TRAINABLE;

    /* Consume one-frame event facts only after the packet owns them. */
    zc->ml_causal_target_hit = 0;
    zc->ml_causal_target_killed = 0;
    zc->ml_environmental_damage = 0;
    zc->ml_environmental_death = 0;
    zc->ml_environmental_source_cleared = 0;
    zc->ml_crouch_edge_entered = 0;
    zc->ml_crouch_edge_completed = 0;
    if (zc->ml_hook_attached || zc->ml_hook_invalid) {
        zc->ml_hook_attempted = 0;
        zc->ml_hook_attempt_frame = 0;
        zc->ml_hook_attached = 0;
        zc->ml_hook_valid = 0;
        zc->ml_hook_invalid = 0;
        zc->ml_hook_necessity_known = 0;
        zc->ml_hook_was_necessary = 0;
        zc->ml_hook_zone_id = 0;
        zc->ml_hook_attempt_tick = 0;
        zc->ml_hook_action_generation = 0;
    }
}
