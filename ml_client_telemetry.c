/* Privileged observation conduit for network-native training clients.
 *
 * The player still connects and acts through protocol 34 like an ordinary
 * Quake II client.  A separate authenticated UDP socket binds one opaque
 * client_id to that connected player entity and unicasts only its ml_obs_t.
 */
#include "ml_client_telemetry.h"
#include "ml_client_lifecycle.h"
#include "ml_client_respawn_settle.h"
#include "ml_client_wire.h"
#include "ml_obs.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern lvar_t *use_hook;

typedef struct {
    qboolean active;
    struct sockaddr_in endpoint;
    char client_id[ML_CLIENT_ID_SIZE];
    uint32_t sequence;
    qboolean has_last_packet;
    ml_client_telemetry_t last_packet;
} ml_client_route_t;

typedef struct {
    qboolean pending;
    char client_id[ML_CLIENT_ID_SIZE];
    uint32_t prior_life_epoch;
    ml_client_lifecycle_echo_t echo;
} ml_client_respawn_restore_t;

static int ml_client_fd = -1;
static int ml_client_bound_port = 0;
static ml_client_route_t ml_client_routes[MAX_CLIENTS];
static ml_client_respawn_restore_t ml_client_respawn_restore[MAX_CLIENTS];
static cvar_t *ml_client_telemetry;
static cvar_t *ml_client_telemetry_port;
static cvar_t *ml_client_telemetry_token;
static cvar_t *ml_client_frame_barrier;
static cvar_t *ml_client_frame_barrier_test_mode;
static cvar_t *ml_client_frame_barrier_test_fault;
static cvar_t *ml_client_frame_barrier_test_tick;
static cvar_t *ml_client_frame_barrier_epoch_drain;
static cvar_t *ml_client_frame_barrier_map_epoch;
static uint32_t ml_client_map_epoch;
static char ml_client_epoch_map[32];
static qboolean ml_client_epoch_drain_announced;

#define ML_HARNESS_IMPULSE_BASE 16
#define ML_HARNESS_ACTION_COUNT 40
#define ML_HARNESS_HIGH_GENERATION_COUNT 6
#define ML_HARNESS_IMPULSE_COUNT \
    (ML_HARNESS_ACTION_COUNT * ML_HARNESS_HIGH_GENERATION_COUNT)
#define ML_HARNESS_BUTTON_GENERATION_SHIFT 2
#define ML_HARNESS_BUTTON_GENERATION_MASK 0x7C
#define ML_HARNESS_LOW_GENERATION_COUNT 32
#define ML_HARNESS_GENERATION_COUNT \
    (ML_HARNESS_HIGH_GENERATION_COUNT * ML_HARNESS_LOW_GENERATION_COUNT)

static float ML_ClientAngleDelta(float left, float right)
{
    float delta = anglemod(left - right);
    if (delta > 180.0f)
        delta -= 360.0f;
    return delta;
}

static void ML_ClientTelemetryDeactivateRoute(ml_client_route_t *route)
{
    if (!route)
        return;
    route->active = qfalse;
    memset(&route->endpoint, 0, sizeof(route->endpoint));
}

static void ML_ClientTelemetryCvars(void)
{
    if (ml_client_telemetry)
        return;
    ml_client_telemetry = gi.cvar("ml_client_telemetry", "0", 0);
    ml_client_telemetry_port = gi.cvar("ml_client_telemetry_port", "27949", 0);
    ml_client_telemetry_token = gi.cvar("ml_client_telemetry_token", "", 0);
    ml_client_frame_barrier = gi.cvar("sv_ml_frame_barrier", "0", 0);
    ml_client_frame_barrier_test_mode = gi.cvar(
        "sv_ml_frame_barrier_test_mode", "0", 0);
    ml_client_frame_barrier_test_fault = gi.cvar(
        "sv_ml_frame_barrier_test_fault", "", 0);
    ml_client_frame_barrier_test_tick = gi.cvar(
        "sv_ml_frame_barrier_test_tick", "0", 0);
    ml_client_frame_barrier_epoch_drain = gi.cvar(
        "ml_frame_barrier_epoch_drain", "0", 0);
    ml_client_frame_barrier_map_epoch = gi.cvar(
        "ml_frame_barrier_map_epoch", "0", 0);
}

static void ML_ClientTelemetryClose(void)
{
    if (ml_client_fd >= 0)
        close(ml_client_fd);
    ml_client_fd = -1;
    ml_client_bound_port = 0;
    memset(ml_client_routes, 0, sizeof(ml_client_routes));
    memset(ml_client_respawn_restore, 0,
        sizeof(ml_client_respawn_restore));
}

static qboolean ML_ClientTelemetryOpen(void)
{
    struct sockaddr_in local;
    int flags;
    int port;

    ML_ClientTelemetryCvars();
    if (!ml_client_telemetry->value)
    {
        if (ml_client_fd >= 0)
            ML_ClientTelemetryClose();
        return qfalse;
    }

    port = (int)ml_client_telemetry_port->value;
    if (port < 1 || port > 65535)
        return qfalse;
    if (ml_client_fd >= 0 && ml_client_bound_port == port)
        return qtrue;

    ML_ClientTelemetryClose();
    ml_client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ml_client_fd < 0)
        return qfalse;

    flags = fcntl(ml_client_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(ml_client_fd, F_SETFL, flags | O_NONBLOCK);

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((uint16_t)port);
    if (bind(ml_client_fd, (struct sockaddr *)&local, sizeof(local)) < 0)
    {
        gi.dprintf("ML client telemetry: bind port %d failed: %s\n",
            port, strerror(errno));
        ML_ClientTelemetryClose();
        return qfalse;
    }

    ml_client_bound_port = port;
    gi.dprintf("ML client telemetry: listening on UDP %d\n", port);
    return qtrue;
}

static qboolean ML_ClientIdEqual(const char *left, const char *right)
{
    size_t left_len, right_len;
    if (!left || !right)
        return qfalse;
    left_len = strnlen(left, ML_CLIENT_ID_SIZE);
    right_len = strnlen(right, ML_CLIENT_ID_SIZE);
    return left_len > 0 && left_len < ML_CLIENT_ID_SIZE &&
        left_len == right_len && memcmp(left, right, left_len) == 0;
}

static qboolean ML_ClientTelemetryIdentified(edict_t *ent)
{
    char *client_id;
    size_t length;

    if (!ent || !ent->client)
        return qfalse;
    client_id = Info_ValueForKey(ent->client->pers.userinfo, "ml_client_id");
    if (!client_id)
        return qfalse;
    length = strnlen(client_id, ML_CLIENT_ID_SIZE);
    return length > 0 && length < ML_CLIENT_ID_SIZE ? qtrue : qfalse;
}

static qboolean ML_ClientTokenEqual(const char *provided)
{
    const char *expected;
    size_t provided_len, expected_len, i;
    unsigned char different = 0;

    expected = ml_client_telemetry_token ? ml_client_telemetry_token->string : "";
    provided_len = strnlen(provided, ML_CLIENT_TOKEN_SIZE);
    expected_len = strnlen(expected, ML_CLIENT_TOKEN_SIZE);
    if (!expected_len || expected_len >= ML_CLIENT_TOKEN_SIZE ||
        provided_len != expected_len)
        return qfalse;
    for (i = 0; i < expected_len; i++)
        different |= (unsigned char)(provided[i] ^ expected[i]);
    return different == 0 ? qtrue : qfalse;
}

static qboolean ML_ClientAddressMatches(edict_t *ent,
    const struct sockaddr_in *source)
{
    char expected[64];
    char *userinfo_ip;
    char *colon;
    char *port_text;
    char *port_end;
    long port;
    struct in_addr parsed;

    userinfo_ip = Info_ValueForKey(ent->client->pers.userinfo, "ip");
    if (!userinfo_ip || !userinfo_ip[0])
        return qfalse;
    strncpy(expected, userinfo_ip, sizeof(expected) - 1);
    expected[sizeof(expected) - 1] = '\0';
    if (expected[0] == '[')
    {
        char *closing = strchr(expected, ']');
        if (!closing || closing[1] != ':' || !closing[2])
            return qfalse;
        port_text = closing + 2;
        *closing = '\0';
        memmove(expected, expected + 1, strlen(expected));
    }
    else
    {
        colon = strrchr(expected, ':');
        if (!colon || !colon[1])
            return qfalse;
        port_text = colon + 1;
        *colon = '\0';
    }
    port = strtol(port_text, &port_end, 10);
    if (*port_end || port < 1 || port > 65535)
        return qfalse;
    if (inet_pton(AF_INET, expected, &parsed) != 1)
        return qfalse;
    return parsed.s_addr == source->sin_addr.s_addr &&
        htons((uint16_t)port) == source->sin_port ? qtrue : qfalse;
}

static int ML_FindRegisteredSlot(const ml_client_register_t *registration,
    const struct sockaddr_in *source)
{
    int i;
    edict_t *ent;
    char client_id[ML_CLIENT_ID_SIZE];

    for (i = 0; i < (int)maxclients->value && i < MAX_CLIENTS; i++)
    {
        ent = &g_edicts[i + 1];
        if (!ent->inuse || !ent->client || (ent->svflags & SVF_MONSTER))
            continue;
        strncpy(client_id,
            Info_ValueForKey(ent->client->pers.userinfo, "ml_client_id"),
            sizeof(client_id) - 1);
        client_id[sizeof(client_id) - 1] = '\0';
        if (ML_ClientIdEqual(client_id, registration->client_id) &&
            ML_ClientAddressMatches(ent, source))
            return i;
    }
    return -1;
}

static void ML_SendRegistrationAck(const struct sockaddr_in *destination,
    const ml_client_register_t *registration, int slot)
{
    ml_client_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic = ML_CLIENT_ACK_MAGIC;
    ack.version = ML_CLIENT_WIRE_VERSION;
    ack.packet_size = (uint32_t)sizeof(ack);
    ack.accepted = slot >= 0 ? 1u : 0u;
    ack.client_slot = slot >= 0 ? (uint32_t)slot : UINT32_MAX;
    ack.server_frame = (uint32_t)level.framenum;
    ack.barrier_version = ML_CLIENT_FRAME_BARRIER_VERSION;
    ack.barrier_capabilities = ML_CLIENT_FRAME_BARRIER_CAPABILITY;
    ack.obs_magic = ML_OBS_MAGIC;
    ack.action_magic = ML_ACT_MAGIC;
    ack.obs_size = (uint32_t)sizeof(ml_obs_t);
    ack.action_size = (uint32_t)sizeof(ml_action_t);
    ack.causal_magic = ML_CAUSAL_MAGIC;
    ack.causal_version = ML_CAUSAL_VERSION;
    ack.causal_size = (uint32_t)sizeof(ml_causal_telemetry_t);
    strncpy(ack.client_id, registration->client_id, sizeof(ack.client_id) - 1);
    sendto(ml_client_fd, &ack, sizeof(ack), MSG_DONTWAIT,
        (const struct sockaddr *)destination, sizeof(*destination));
    if (slot >= 0 && ml_client_frame_barrier &&
        ml_client_frame_barrier->value)
        gi.dprintf("ML_FRAME_BARRIER_EVENT event=route_ack slot=%d "
            "client_id=%s server_frame=%u wire=%u barrier=%u capability=%u\n",
            slot, registration->client_id, ack.server_frame, ack.version,
            ack.barrier_version, ack.barrier_capabilities);
}

static void ML_ClientTelemetryPoll(void)
{
    ml_client_register_t registration;
    struct sockaddr_in source;
    socklen_t source_len;
    ssize_t received;
    int slot;

    while (1)
    {
        source_len = sizeof(source);
        received = recvfrom(ml_client_fd, &registration, sizeof(registration),
            MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr *)&source, &source_len);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (received != sizeof(registration))
            continue;
        if (registration.magic != ML_CLIENT_REGISTER_MAGIC ||
            registration.version != ML_CLIENT_WIRE_VERSION ||
            registration.packet_size != sizeof(registration) ||
            (registration.barrier_capabilities &
                ML_CLIENT_FRAME_BARRIER_CAPABILITY) == 0 ||
            registration.obs_magic != ML_OBS_MAGIC ||
            registration.action_magic != ML_ACT_MAGIC ||
            registration.obs_size != sizeof(ml_obs_t) ||
            registration.action_size != sizeof(ml_action_t) ||
            registration.causal_magic != ML_CAUSAL_MAGIC ||
            registration.causal_version != ML_CAUSAL_VERSION ||
            registration.causal_size != sizeof(ml_causal_telemetry_t) ||
            !ML_ClientTokenEqual(registration.token))
        {
            ML_SendRegistrationAck(&source, &registration, -1);
            continue;
        }

        slot = ML_FindRegisteredSlot(&registration, &source);
        ML_SendRegistrationAck(&source, &registration, slot);
        if (slot < 0)
        {
            gi.dprintf("ML client telemetry: no connected slot for client_id=%s\n",
                registration.client_id);
            continue;
        }

        /* The client refreshes its NAT binding periodically. Preserve the
           route sequence for the same identity so Python never observes a
           false rollback; a genuinely different identity starts at zero. */
        if (!ML_ClientIdEqual(ml_client_routes[slot].client_id,
                registration.client_id))
        {
            memset(&ml_client_routes[slot], 0, sizeof(ml_client_routes[slot]));
            strncpy(ml_client_routes[slot].client_id, registration.client_id,
                sizeof(ml_client_routes[slot].client_id) - 1);
        }
        ml_client_routes[slot].active = qtrue;
        ml_client_routes[slot].endpoint = source;
        gi.dprintf("ML client telemetry: %s bound to client slot %d\n",
            ml_client_routes[slot].client_id, slot);
    }
}

qboolean ML_ClientTelemetryActive(edict_t *ent)
{
    int slot;
    if (!ent || !ent->client)
        return qfalse;
    slot = (int)(ent - g_edicts - 1);
    if (slot < 0 || slot >= MAX_CLIENTS)
        return qfalse;
    return ml_client_routes[slot].active;
}

void ML_ClientTelemetryCaptureRespawnAction(edict_t *ent)
{
    int slot;
    char current_id[ML_CLIENT_ID_SIZE];
    ml_client_route_t *route;
    ml_client_respawn_restore_t *restore;

    if (!ent || !ent->client)
        return;
    slot = (int)(ent - g_edicts - 1);
    if (slot < 0 || slot >= MAX_CLIENTS)
        return;

    route = &ml_client_routes[slot];
    restore = &ml_client_respawn_restore[slot];
    memset(restore, 0, sizeof(*restore));
    if (!route->active)
        return;

    strncpy(current_id,
        Info_ValueForKey(ent->client->pers.userinfo, "ml_client_id"),
        sizeof(current_id) - 1);
    current_id[sizeof(current_id) - 1] = '\0';
    if (!ML_ClientIdEqual(current_id, route->client_id) ||
        !ML_ClientLifecycleCaptureEcho(&restore->echo, &ent->client->zc,
            qtrue, ent->deadflag != DEAD_NO))
        return;

    strncpy(restore->client_id, route->client_id,
        sizeof(restore->client_id) - 1);
    restore->prior_life_epoch =
        (uint32_t)ent->client->resp.ml_life_epoch;
    restore->pending = qtrue;
}

qboolean ML_ClientTelemetryRestoreRespawnAction(edict_t *ent)
{
    int slot;
    char current_id[ML_CLIENT_ID_SIZE];
    ml_client_route_t *route;
    ml_client_respawn_restore_t saved;

    if (!ent || !ent->client)
        return qfalse;
    slot = (int)(ent - g_edicts - 1);
    if (slot < 0 || slot >= MAX_CLIENTS)
        return qfalse;

    saved = ml_client_respawn_restore[slot];
    memset(&ml_client_respawn_restore[slot], 0,
        sizeof(ml_client_respawn_restore[slot]));
    if (!saved.pending)
        return qfalse;

    route = &ml_client_routes[slot];
    strncpy(current_id,
        Info_ValueForKey(ent->client->pers.userinfo, "ml_client_id"),
        sizeof(current_id) - 1);
    current_id[sizeof(current_id) - 1] = '\0';
    if (!route->active ||
        !ML_ClientIdEqual(saved.client_id, route->client_id) ||
        !ML_ClientIdEqual(current_id, route->client_id))
    {
        ML_ClientTelemetryDeactivateRoute(route);
        return qfalse;
    }

    ML_ClientLifecycleRestoreEcho(&ent->client->zc, &saved.echo);
    ML_ClientTelemetryCvars();
    if (ml_client_frame_barrier_test_mode->value)
        gi.dprintf("ML_FRAME_BARRIER_EVENT event=respawn_action_restore "
            "slot=%d client_id=%s prior_life_epoch=%u life_epoch=%u "
            "action_tick=%d action_generation=%u route_preserved=1 "
            "attribution=exact alive=1\n",
            slot, route->client_id, saved.prior_life_epoch,
            (uint32_t)ent->client->resp.ml_life_epoch,
            saved.echo.action_tick,
            (uint32_t)saved.echo.action_generation + 1u);
    return qtrue;
}

void ML_ClientTelemetryClientDisconnected(edict_t *ent)
{
    int slot;
    if (!ent)
        return;
    slot = (int)(ent - g_edicts - 1);
    if (slot < 0 || slot >= MAX_CLIENTS)
        return;
    /* Keep the identity and sequence as a tombstone.  A reconnect by the
       same routed client must not roll Python's monotonic packet filter back
       to zero; a different client_id still resets both on registration. */
    ML_ClientTelemetryDeactivateRoute(&ml_client_routes[slot]);
    memset(&ml_client_respawn_restore[slot], 0,
        sizeof(ml_client_respawn_restore[slot]));
}

void ML_ClientTelemetryRecordCommand(edict_t *ent, usercmd_t *ucmd)
{
    zgcl_t *zc;
    vec3_t intended_angles;
    int i;
    qboolean requested_fire;
    qboolean same_frame;
    qboolean same_decision;
    qboolean requested_reliable_valid;
    int requested_reliable;
    int requested_action;
    int requested_generation_high;
    int requested_generation_low;
    int requested_generation;
    if (!ucmd || (!ML_ClientTelemetryActive(ent) &&
        !ML_ClientTelemetryIdentified(ent)))
        return;

    for (i = 0; i < 3; i++)
    {
        int16_t packed = (int16_t)(ucmd->angles[i] +
            ent->client->ps.pmove.delta_angles[i]);
        intended_angles[i] = SHORT2ANGLE(packed);
    }
    if (intended_angles[PITCH] > 89.0f)
        intended_angles[PITCH] = 89.0f;
    if (intended_angles[PITCH] < -89.0f)
        intended_angles[PITCH] = -89.0f;
    intended_angles[ROLL] = 0.0f;

    requested_fire = (ucmd->buttons & BUTTON_ATTACK) != 0;
    zc = &ent->client->zc;
    same_frame = zc->ml_last_action_ok &&
        zc->ml_last_action_tick == level.framenum;
    requested_reliable = (int)ucmd->impulse - ML_HARNESS_IMPULSE_BASE;
    requested_reliable_valid = requested_reliable >= 0 &&
        requested_reliable < ML_HARNESS_IMPULSE_COUNT;
    requested_generation_high = requested_reliable_valid
        ? requested_reliable / ML_HARNESS_ACTION_COUNT : 0;
    requested_generation_low =
        ((int)ucmd->buttons & ML_HARNESS_BUTTON_GENERATION_MASK) >>
        ML_HARNESS_BUTTON_GENERATION_SHIFT;
    requested_generation =
        requested_generation_high * ML_HARNESS_LOW_GENERATION_COUNT +
        requested_generation_low;
    /* Private attestation bits never reach ordinary game button handling. */
    ucmd->buttons &= ~ML_HARNESS_BUTTON_GENERATION_MASK;
    same_decision = same_frame && requested_reliable_valid &&
        zc->ml_action_generation_valid &&
        requested_generation == zc->ml_action_generation;
    zc->ml_respawn_settling_action =
        ML_ClientRespawnSettlingAtEntry(
            zc->ml_respawn_settling_action, same_decision,
            ent->client->ps.pmove.pm_flags);
    if (!same_decision)
    {
        zc->ml_look_base_yaw = ent->client->v_angle[YAW];
        zc->ml_look_base_pitch = ent->client->v_angle[PITCH];
    }
    zc->ml_fire_suppressed = requested_fire &&
        !ML_HasEngageableTarget(ent, intended_angles);
    if (zc->ml_fire_suppressed)
        ucmd->buttons &= ~BUTTON_ATTACK;

    zc->ml_last_action_tick = level.framenum;
    zc->ml_last_action_ok = 1;
    zc->ml_move_forward = (float)ucmd->forwardmove / 320.0f;
    zc->ml_move_right = (float)ucmd->sidemove / 320.0f;
    /* ClientThink may run many times while level.framenum is unchanged.
       Record latest intended angle minus the frame's initial view. Summing
       per-call deltas multiplies duplicate held usercmds; overwriting against
       the changing current view lets a later duplicate erase the decision. */
    zc->ml_look_yaw = ML_ClientAngleDelta(
        intended_angles[YAW], zc->ml_look_base_yaw);
    zc->ml_look_pitch = intended_angles[PITCH] - zc->ml_look_base_pitch;
    zc->ml_vertical_intent = ucmd->upmove > 0
        ? ML_VERTICAL_UP_OR_JUMP
        : ucmd->upmove < 0
            ? ML_VERTICAL_DOWN_OR_CROUCH
            : ML_VERTICAL_NEUTRAL;
    zc->ml_applied_upmove = (int)ucmd->upmove;
    zc->ml_fire = (ucmd->buttons & BUTTON_ATTACK) != 0;
    if (requested_reliable_valid)
    {
        zc->ml_action_generation = requested_generation;
        requested_action = requested_reliable % ML_HARNESS_ACTION_COUNT;
        zc->ml_hook = requested_action / 10;
        zc->ml_weapon = requested_action % 10;
        zc->ml_action_generation_valid = 1;
    }
    else
    {
        /* Missing attribution is a no-op, never a stale prior decision. */
        zc->ml_hook = 0;
        zc->ml_weapon = 0;
        zc->ml_action_generation = 0;
        zc->ml_action_generation_valid = 0;
    }
    ML_CausalHookBindAction(ent);
    /* A protocol client normally presses attack to leave the death screen.
       The policy's attack bit is target-gated, though, and a dead player can
       never have an engageable target.  Inject the lifecycle button only
       after recording the authoritative policy echo so PPO admission remains
       about the requested combat action, not this transport-side respawn. */
    if (ent->deadflag && level.time > ent->client->respawn_time)
        ucmd->buttons |= BUTTON_ATTACK;
}

void ML_ClientTelemetryFinalizeCommand(edict_t *ent)
{
    zgcl_t *zc;

    if (!ent || !ent->client ||
        (!ML_ClientTelemetryActive(ent) &&
        !ML_ClientTelemetryIdentified(ent)))
        return;
    zc = &ent->client->zc;
    if (!zc->ml_last_action_ok || zc->ml_last_action_tick != level.framenum)
        return;
    /* Pmove is the action authority.  In particular, stock teleport settling
       accepts yaw but forces pitch to zero; echo the resulting view delta,
       never the pre-pmove request. */
    zc->ml_look_yaw = ML_ClientAngleDelta(
        ent->client->v_angle[YAW], zc->ml_look_base_yaw);
    zc->ml_look_pitch = ML_ClientActualPitchDelta(
        ent->client->v_angle[PITCH], zc->ml_look_base_pitch);
}

void ML_ClientTelemetryApplyDeferredControls(edict_t *ent)
{
    static const char *weapon_names[] = {
        NULL, "Blaster", "Shotgun", "Super Shotgun", "Machinegun",
        "Chaingun", "Grenade Launcher", "Rocket Launcher", "HyperBlaster",
        "Railgun"
    };
    zgcl_t *zc;
    gitem_t *item;
    ML_ClientTelemetryCvars();
    if (!ml_client_frame_barrier->value || !ent || !ent->client ||
        (!ML_ClientTelemetryActive(ent) &&
        !ML_ClientTelemetryIdentified(ent)))
        return;
    zc = &ent->client->zc;
    if (!zc->ml_action_generation_valid)
        return;

    /* Preserve the historical reliable-command order, but execute both only
       inside the deferred ClientThink transaction after pmove established
       this action's authoritative view angle. */
    if (zc->ml_hook == 1 && use_hook->value &&
        !(ent->lithium_flags & LITHIUM_OBSERVER) && ent->deadflag == DEAD_NO)
    {
        Weapon_Hook_Fire(ent);
        ent->safety_time = 0;
    }
    else if (zc->ml_hook == 3 && use_hook->value &&
        !(ent->lithium_flags & LITHIUM_OBSERVER))
    {
        Hook_Reset(ent->client->hook);
    }
    if (zc->ml_weapon > 0 && (size_t)zc->ml_weapon <
        sizeof(weapon_names) / sizeof(weapon_names[0]))
    {
        item = FindItem((char *)weapon_names[zc->ml_weapon]);
        if (item && item->use)
            item->use(ent, item);
    }
    if (ml_client_frame_barrier_test_mode->value &&
        (zc->ml_hook || zc->ml_weapon))
        gi.dprintf("ML_FRAME_BARRIER_EVENT event=deferred_control slot=%d "
            "action_tick=%d hook=%d weapon=%d order=hook_then_weapon\n",
            (int)(ent - g_edicts - 1), zc->ml_last_action_tick,
            zc->ml_hook, zc->ml_weapon);
}

void ML_ClientTelemetryFrame(void)
{
    int slot;
    qboolean epoch_drain;
    qboolean all_drain_terminals_sent;
    edict_t *ent;
    ml_client_route_t *route;
    ml_client_telemetry_t packet;
    char current_id[ML_CLIENT_ID_SIZE];
    ssize_t sent;

    ML_ClientTelemetryCvars();
    if (ml_client_frame_barrier->value)
    {
        if (ml_client_frame_barrier_map_epoch->value < 1)
            return;
        ml_client_map_epoch =
            (uint32_t)ml_client_frame_barrier_map_epoch->value;
        strncpy(ml_client_epoch_map, level.mapname,
            sizeof(ml_client_epoch_map) - 1);
        ml_client_epoch_map[sizeof(ml_client_epoch_map) - 1] = '\0';
    }
    else if (strncmp(ml_client_epoch_map, level.mapname,
        sizeof(ml_client_epoch_map) - 1))
    {
        strncpy(ml_client_epoch_map, level.mapname,
            sizeof(ml_client_epoch_map) - 1);
        ml_client_epoch_map[sizeof(ml_client_epoch_map) - 1] = '\0';
        ml_client_map_epoch++;
    }
    if (!ML_ClientTelemetryOpen())
        return;
    ML_ClientTelemetryPoll();
    epoch_drain = ml_client_frame_barrier->value &&
        ml_client_frame_barrier_epoch_drain->value;
    if (!epoch_drain)
        ml_client_epoch_drain_announced = qfalse;

    for (slot = 0; slot < (int)maxclients->value &&
        slot < MAX_CLIENTS; slot++)
    {
        route = &ml_client_routes[slot];
        if (!route->active)
            continue;
        ent = &g_edicts[slot + 1];
        if (!ent->client || (ent->svflags & SVF_MONSTER))
        {
            ML_ClientTelemetryDeactivateRoute(route);
            continue;
        }
        if (!ent->inuse)
        {
            /* SpawnEntities() clears every edict before the engine calls
               ClientBegin() for connected clients on the new map.  Keep the
               route (and, critically, its monotonic sequence) through that
               short gap.  A real disconnect clears pers.connected, so stale
               routes are still discarded before a slot can be reused. */
            if (!ent->client->pers.connected)
                ML_ClientTelemetryDeactivateRoute(route);
            continue;
        }
        strncpy(current_id,
            Info_ValueForKey(ent->client->pers.userinfo, "ml_client_id"),
            sizeof(current_id) - 1);
        current_id[sizeof(current_id) - 1] = '\0';
        if (!ML_ClientIdEqual(current_id, route->client_id))
        {
            ML_ClientTelemetryDeactivateRoute(route);
            continue;
        }

        /* One terminal observation closes the old trajectory before the
           drain-start witness.  From that witness through map_reset, no
           action-bound telemetry is produced at all. */
        if (epoch_drain && (ml_client_epoch_drain_announced ||
            level.intermissiontime <= 0 ||
            ent->client->zc.ml_intermission_obs_sent))
        {
            if (ml_client_frame_barrier_test_mode->value)
                gi.dprintf("ML_FRAME_BARRIER_EVENT "
                    "event=telemetry_suppressed_epoch_drain slot=%d "
                    "server_frame=%d\n", slot, level.framenum);
            continue;
        }

        memset(&packet, 0, sizeof(packet));
        packet.magic = ML_CLIENT_TELEM_MAGIC;
        packet.version = ML_CLIENT_WIRE_VERSION;
        packet.packet_size = (uint32_t)sizeof(packet);
        packet.sequence = ++route->sequence;
        packet.client_slot = (uint32_t)slot;
        packet.server_frame = (uint32_t)level.framenum;
        packet.barrier_version = ML_CLIENT_FRAME_BARRIER_VERSION;
        packet.barrier_capabilities = ML_CLIENT_FRAME_BARRIER_CAPABILITY;
        packet.map_epoch = ml_client_map_epoch;
        packet.applied_action_tick =
            (uint32_t)ent->client->zc.ml_last_action_tick;
        strncpy(packet.client_id, route->client_id,
            sizeof(packet.client_id) - 1);
        strncpy(packet.map_name, level.mapname, sizeof(packet.map_name) - 1);
        ML_PackObs(ent, &packet.obs);
        ML_PackCausalTelemetry(ent, &packet.causal, 0);
        if (ml_client_frame_barrier_test_mode->value &&
            ent->client->zc.ml_respawn_settling_action &&
            !(ent->client->ps.pmove.pm_flags & PMF_TIME_TELEPORT))
            gi.dprintf("ML_FRAME_BARRIER_EVENT "
                "event=ml_respawn_settling_action slot=%d client_id=%s "
                "client_life_epoch=%u server_frame=%u action_tick=%u "
                "entry_latched=1 live_pmf_time_teleport=0 active=1 "
                "post_pmove_active=0 echo_valid=%d facts_complete=%d "
                "transition_trainable=%d actual_look_yaw=%.6f "
                "actual_look_pitch=%.6f\n",
                slot, packet.client_id, packet.causal.client_life_epoch,
                packet.server_frame,
                packet.applied_action_tick,
                (packet.causal.flags & ML_CAUSAL_ECHO_VALID) != 0,
                (packet.causal.flags & ML_CAUSAL_FACTS_COMPLETE) != 0,
                (packet.causal.flags & ML_CAUSAL_TRANSITION_TRAINABLE) != 0,
                ent->client->zc.ml_look_yaw,
                ent->client->zc.ml_look_pitch);
        if (ml_client_frame_barrier_test_mode->value)
            gi.dprintf("ML_FRAME_BARRIER_EVENT event=telemetry slot=%d "
                "server_frame=%u applied_action_tick=%u map_epoch=%u "
                "sequence=%u causal_echo_tick=%u causal_generation=%u "
                "client_id=%s client_life_epoch=%u terminal_reason=%u "
                "alive=%d action_tick=%u action_generation=%u "
                "role_playing=%d role_public_pm_normal=%d pm_type=%d\n",
                slot, packet.server_frame, packet.applied_action_tick,
                packet.map_epoch, packet.sequence, packet.causal.echo_tick,
                packet.causal.action_generation, packet.client_id,
                packet.causal.client_life_epoch, packet.obs.terminal_reason,
                ent->deadflag == DEAD_NO ? 1 : 0,
                packet.applied_action_tick,
                packet.causal.action_generation,
                (packet.causal.flags & ML_CAUSAL_ROLE_PLAYING) != 0,
                (packet.causal.flags &
                    ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL) != 0,
                ent->client->ps.pmove.pm_type);
        sent = sendto(ml_client_fd, &packet, sizeof(packet), MSG_DONTWAIT,
            (struct sockaddr *)&route->endpoint, sizeof(route->endpoint));
        if (sent == sizeof(packet))
        {
            if (packet.obs.terminal_reason == ML_TERMINAL_DEATH)
                ent->client->zc.ml_death_obs_sent = 1;
            else if (packet.obs.terminal_reason == ML_TERMINAL_INTERMISSION)
                ent->client->zc.ml_intermission_obs_sent = 1;

            if (!epoch_drain && ml_client_frame_barrier_test_mode->value &&
                !Q_stricmp(ml_client_frame_barrier_test_fault->string,
                    "old-telemetry") &&
                (uint32_t)ml_client_frame_barrier_test_tick->value ==
                    packet.applied_action_tick && route->has_last_packet)
            {
                sendto(ml_client_fd, &route->last_packet,
                    sizeof(route->last_packet), MSG_DONTWAIT,
                    (struct sockaddr *)&route->endpoint,
                    sizeof(route->endpoint));
                gi.dprintf("ML_FRAME_BARRIER_EVENT event=telemetry_replay "
                    "slot=%d replay_frame=%u current_frame=%u\n", slot,
                    route->last_packet.server_frame, packet.server_frame);
            }
            route->last_packet = packet;
            route->has_last_packet = qtrue;
        }
    }

    if (epoch_drain && !ml_client_epoch_drain_announced)
    {
        all_drain_terminals_sent = qtrue;
        for (slot = 0; slot < (int)maxclients->value &&
            slot < MAX_CLIENTS; slot++)
        {
            route = &ml_client_routes[slot];
            ent = &g_edicts[slot + 1];
            if (route->active && ent->inuse && ent->client &&
                !ent->client->zc.ml_intermission_obs_sent)
            {
                all_drain_terminals_sent = qfalse;
                break;
            }
        }
        if (all_drain_terminals_sent)
        {
            ml_client_epoch_drain_announced = qtrue;
            gi.dprintf("ML_FRAME_BARRIER_EVENT event=epoch_drain_enter "
                "server_frame=%d source=intermission\n", level.framenum);
        }
        else
        {
            /* Do not permit an immediate intermission timeout to outrun a
               transient nonblocking UDP send failure. */
            level.exitintermission = qfalse;
        }
    }
}

qboolean ML_ClientTelemetryEpochDrainReady(void)
{
    ML_ClientTelemetryCvars();
    if (!ml_client_frame_barrier->value ||
        !ml_client_frame_barrier_epoch_drain->value)
        return qtrue;
    return ml_client_epoch_drain_announced;
}

void ML_ClientTelemetryShutdown(void)
{
    ML_ClientTelemetryClose();
    ml_client_map_epoch = 0;
    ml_client_epoch_drain_announced = qfalse;
    memset(ml_client_epoch_map, 0, sizeof(ml_client_epoch_map));
}
