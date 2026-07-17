/* Wire protocol for a normal Quake II client paired with privileged ML
 * telemetry.  Keep this header byte-for-byte synchronized with the client
 * harness copy.  All packets are fixed-layout, little-endian POD. */
#ifndef ML_CLIENT_WIRE_H
#define ML_CLIENT_WIRE_H

#include <stdint.h>
#include "ml_bridge.h"

#define ML_CLIENT_WIRE_VERSION 8u
#define ML_CLIENT_FRAME_BARRIER_VERSION 1u
#define ML_CLIENT_FRAME_BARRIER_CAPABILITY 0x00000001u
#define ML_CLIENT_REGISTER_MAGIC 0x52434d51u /* "QMCR" */
#define ML_CLIENT_ACK_MAGIC      0x41434d51u /* "QMCA" */
#define ML_CLIENT_TELEM_MAGIC    0x54434d51u /* "QMCT" */

#define ML_CLIENT_ID_SIZE 40
#define ML_CLIENT_TOKEN_SIZE 64

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t barrier_capabilities;
    uint32_t obs_magic;
    uint32_t action_magic;
    uint32_t obs_size;
    uint32_t action_size;
    uint32_t causal_magic;
    uint32_t causal_version;
    uint32_t causal_size;
    char client_id[ML_CLIENT_ID_SIZE];
    char token[ML_CLIENT_TOKEN_SIZE];
} ml_client_register_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t accepted;
    uint32_t client_slot;
    uint32_t server_frame;
    uint32_t barrier_version;
    uint32_t barrier_capabilities;
    uint32_t obs_magic;
    uint32_t action_magic;
    uint32_t obs_size;
    uint32_t action_size;
    uint32_t causal_magic;
    uint32_t causal_version;
    uint32_t causal_size;
    char client_id[ML_CLIENT_ID_SIZE];
} ml_client_ack_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t sequence;
    uint32_t client_slot;
    uint32_t server_frame;
    uint32_t barrier_version;
    uint32_t barrier_capabilities;
    uint32_t map_epoch;
    uint32_t applied_action_tick;
    char client_id[ML_CLIENT_ID_SIZE];
    char map_name[32];
    ml_obs_t obs;
    ml_causal_telemetry_t causal;
} ml_client_telemetry_t;

_Static_assert(sizeof(ml_obs_t) == 1056, "ml_obs_t wire size changed");
_Static_assert(sizeof(ml_action_t) == 28, "ml_action_t wire size changed");
_Static_assert(sizeof(ml_causal_telemetry_t) == 80, "causal wire size changed");
_Static_assert(sizeof(ml_client_register_t) == 148, "registration wire size changed");
_Static_assert(sizeof(ml_client_ack_t) == 100, "ack wire size changed");
_Static_assert(sizeof(ml_client_telemetry_t) == 1248, "telemetry wire size changed");

#endif
