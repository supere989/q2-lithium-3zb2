/* Privileged observation conduit for network-native training clients.
 *
 * The player still connects and acts through protocol 34 like an ordinary
 * Quake II client.  A separate authenticated UDP socket binds one opaque
 * client_id to that connected player entity and unicasts only its ml_obs_t.
 */
#include "ml_client_telemetry.h"
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

typedef struct {
    qboolean active;
    struct sockaddr_in endpoint;
    char client_id[ML_CLIENT_ID_SIZE];
    uint32_t sequence;
} ml_client_route_t;

static int ml_client_fd = -1;
static int ml_client_bound_port = 0;
static ml_client_route_t ml_client_routes[MAX_CLIENTS];
static cvar_t *ml_client_telemetry;
static cvar_t *ml_client_telemetry_port;
static cvar_t *ml_client_telemetry_token;

#define ML_HARNESS_IMPULSE_BASE 160
#define ML_HARNESS_IMPULSE_COUNT 40

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
}

static void ML_ClientTelemetryClose(void)
{
    if (ml_client_fd >= 0)
        close(ml_client_fd);
    ml_client_fd = -1;
    ml_client_bound_port = 0;
    memset(ml_client_routes, 0, sizeof(ml_client_routes));
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
    strncpy(ack.client_id, registration->client_id, sizeof(ack.client_id) - 1);
    sendto(ml_client_fd, &ack, sizeof(ack), MSG_DONTWAIT,
        (const struct sockaddr *)destination, sizeof(*destination));
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
            MSG_DONTWAIT, (struct sockaddr *)&source, &source_len);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (received != sizeof(registration))
            continue;
        if (registration.magic != ML_CLIENT_REGISTER_MAGIC ||
            registration.version != ML_CLIENT_WIRE_VERSION ||
            registration.packet_size != sizeof(registration) ||
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
}

void ML_ClientTelemetryRecordCommand(edict_t *ent, usercmd_t *ucmd)
{
    zgcl_t *zc;
    vec3_t intended_angles;
    int i;
    qboolean requested_fire;
    int requested_reliable;
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
    zc->ml_fire_suppressed = requested_fire &&
        !ML_HasEngageableTarget(ent, intended_angles);
    if (zc->ml_fire_suppressed)
        ucmd->buttons &= ~BUTTON_ATTACK;

    zc->ml_last_action_tick = level.framenum;
    zc->ml_last_action_ok = 1;
    zc->ml_move_forward = (float)ucmd->forwardmove / 320.0f;
    zc->ml_move_right = (float)ucmd->sidemove / 320.0f;
    zc->ml_look_yaw = ML_ClientAngleDelta(
        intended_angles[YAW], ent->client->v_angle[YAW]);
    zc->ml_look_pitch = intended_angles[PITCH] - ent->client->v_angle[PITCH];
    zc->ml_jump = ucmd->upmove > 0;
    zc->ml_fire = (ucmd->buttons & BUTTON_ATTACK) != 0;
    requested_reliable = (int)ucmd->impulse - ML_HARNESS_IMPULSE_BASE;
    if (requested_reliable >= 0 &&
        requested_reliable < ML_HARNESS_IMPULSE_COUNT)
    {
        zc->ml_hook = requested_reliable / 10;
        zc->ml_weapon = requested_reliable % 10;
    }
    else
    {
        /* Missing attribution is a no-op, never a stale prior decision. */
        zc->ml_hook = 0;
        zc->ml_weapon = 0;
    }

    /* A protocol client normally presses attack to leave the death screen.
       The policy's attack bit is target-gated, though, and a dead player can
       never have an engageable target.  Inject the lifecycle button only
       after recording the authoritative policy echo so PPO admission remains
       about the requested combat action, not this transport-side respawn. */
    if (ent->deadflag && level.time > ent->client->respawn_time)
        ucmd->buttons |= BUTTON_ATTACK;
}

void ML_ClientTelemetryFrame(void)
{
    int slot;
    edict_t *ent;
    ml_client_route_t *route;
    ml_client_telemetry_t packet;
    char current_id[ML_CLIENT_ID_SIZE];
    ssize_t sent;

    if (!ML_ClientTelemetryOpen())
        return;
    ML_ClientTelemetryPoll();

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

        memset(&packet, 0, sizeof(packet));
        packet.magic = ML_CLIENT_TELEM_MAGIC;
        packet.version = ML_CLIENT_WIRE_VERSION;
        packet.packet_size = (uint32_t)sizeof(packet);
        packet.sequence = ++route->sequence;
        packet.client_slot = (uint32_t)slot;
        packet.server_frame = (uint32_t)level.framenum;
        strncpy(packet.client_id, route->client_id,
            sizeof(packet.client_id) - 1);
        strncpy(packet.map_name, level.mapname, sizeof(packet.map_name) - 1);
        ML_PackObs(ent, &packet.obs);
        sent = sendto(ml_client_fd, &packet, sizeof(packet), MSG_DONTWAIT,
            (struct sockaddr *)&route->endpoint, sizeof(route->endpoint));
        if (sent == sizeof(packet))
        {
            if (packet.obs.terminal_reason == ML_TERMINAL_DEATH)
                ent->client->zc.ml_death_obs_sent = 1;
            else if (packet.obs.terminal_reason == ML_TERMINAL_INTERMISSION)
                ent->client->zc.ml_intermission_obs_sent = 1;
        }
    }
}

void ML_ClientTelemetryShutdown(void)
{
    ML_ClientTelemetryClose();
}
