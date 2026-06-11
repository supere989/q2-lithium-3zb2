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
} ml_bot_sock_t;

static ml_bot_sock_t g_socks[MAX_BOTS_ML];
static int           g_initialized = 0;

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

    /* Drain queued actions from socket buffer, caching the latest valid one */
    while (1) {
        ml_action_t incoming;
        ssize_t n = recv(s->fd, &incoming, sizeof(incoming), MSG_DONTWAIT);
        if (n == sizeof(incoming)) {
            if (incoming.magic == ML_ACT_MAGIC && incoming.tick == obs->tick) {
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

    /* If the drained queued action happens to match the tick we just sent, use it immediately */
    if (got_queued) {
        s->last_action = queued_action;
        *act = queued_action;
        return 0;
    }

    /* wait for action with timeout, demanding tick validation */
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
                *act = incoming;
                return 0;
            }
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
