/*
 * ml_bridge.c — UDP bridge between game.so Bot_Think and Python harness.
 *
 * Each bot slot owns one UDP socket (non-blocking send, blocking recv with
 * timeout).  The Python process binds the server side of each socket.
 *
 * Thread safety: not needed — all calls happen on the single game thread.
 */

#include "ml_bridge.h"
#include "g_local.h"

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
    action->jump = (uint8_t)(
        (grounded_before && !ent->groundentity) ||
        ent->velocity[2] > velocity_z_before + 50.0f);
    action->fire = (uint8_t)((ent->client->buttons & BUTTON_ATTACK) != 0);
    hook_after = ent->client->hook_on || ent->client->ctf_grapple != NULL;
    action->hook = (uint8_t)(
        !hook_before && hook_after ? 1 :
        hook_before && !hook_after ? 3 :
        hook_after ? 2 : 0);
    action->weapon = ml_teacher_weapon(ent);

    sendto(g_teacher_fd, &sample, sizeof(sample), MSG_DONTWAIT,
           (struct sockaddr *)&g_teacher_addr, sizeof(g_teacher_addr));
}

/* Human teacher sample. The bot path infers the action from before/after
   state because 3ZB2 moves via walkmove-style origin changes; a human's
   genuine input IS the usercmd, so take it directly. Same wire struct,
   same fire-and-forget semantics; never waits or retries. */
void ML_TeacherSendHuman(edict_t *ent, const ml_obs_t *before,
                         const usercmd_t *ucmd, float yaw_before,
                         float pitch_before, int hook_on) {
    ml_teacher_sample_t sample;
    ml_action_t *action;
    int stride;

    if (!ent || !ent->client || !before || !ucmd || !ml_teacher_enabled ||
        !ml_teacher_enabled->value || !ml_teacher_humans ||
        !ml_teacher_humans->value || ent->client->zc.ml_enabled)
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
    sample.flags = ent->groundentity ? 1u : 0u;
    strncpy(sample.map_name, level.mapname, sizeof(sample.map_name) - 1);
    sample.obs = *before;

    action = &sample.action;
    action->magic = ML_ACT_MAGIC;
    action->tick = sample.tick;
    /* ucmd->angles is the view the player commanded THIS frame; the delta
       against the still-unapplied v_angle is the look action. */
    action->look_yaw = ml_teacher_clamp(
        ml_teacher_angle_delta(SHORT2ANGLE(ucmd->angles[YAW]), yaw_before),
        -45.0f, 45.0f);
    action->look_pitch = ml_teacher_clamp(
        ml_teacher_angle_delta(SHORT2ANGLE(ucmd->angles[PITCH]), pitch_before),
        -30.0f, 30.0f);
    action->move_forward = ml_teacher_clamp(ucmd->forwardmove / 400.0f,
                                            -1.0f, 1.0f);
    action->move_right = ml_teacher_clamp(ucmd->sidemove / 400.0f,
                                          -1.0f, 1.0f);
    action->jump = (uint8_t)(ucmd->upmove > 0);
    action->fire = (uint8_t)((ucmd->buttons & BUTTON_ATTACK) != 0);
    action->hook = (uint8_t)(hook_on ? 2 : 0);
    action->weapon = ml_teacher_weapon(ent);

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
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming), MSG_DONTWAIT);
        if (n == sizeof(incoming)) {
            if (incoming.magic == ML_ACT_MAGIC &&
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
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming), 0);
        if (n < 0)
            break;
        if (n == sizeof(incoming) && incoming.magic == ML_ACT_MAGIC) {
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
            n = recv(s->fd, &incoming, sizeof(incoming), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;                       /* timeout — not ready yet */
            }
            if (n == sizeof(incoming) && incoming.magic == ML_ACT_MAGIC) {
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
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;                           /* timeout */
        }
        if (n == sizeof(incoming) && incoming.magic == ML_ACT_MAGIC &&
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
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming), MSG_DONTWAIT);
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
