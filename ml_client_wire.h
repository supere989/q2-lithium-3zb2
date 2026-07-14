/* Wire protocol for a normal Quake II client paired with privileged ML
 * telemetry.  Keep this header byte-for-byte synchronized with the client
 * harness copy.  All packets are fixed-layout, little-endian POD. */
#ifndef ML_CLIENT_WIRE_H
#define ML_CLIENT_WIRE_H

#include <stdint.h>
#include "ml_bridge.h"

#define ML_CLIENT_WIRE_VERSION 2u
#define ML_CLIENT_REGISTER_MAGIC 0x52434d51u /* "QMCR" */
#define ML_CLIENT_ACK_MAGIC      0x41434d51u /* "QMCA" */
#define ML_CLIENT_TELEM_MAGIC    0x54434d51u /* "QMCT" */

#define ML_CLIENT_ID_SIZE 40
#define ML_CLIENT_TOKEN_SIZE 64

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t reserved;
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
    char client_id[ML_CLIENT_ID_SIZE];
} ml_client_ack_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t packet_size;
    uint32_t sequence;
    uint32_t client_slot;
    uint32_t server_frame;
    char client_id[ML_CLIENT_ID_SIZE];
    char map_name[32];
    ml_obs_t obs;
} ml_client_telemetry_t;

_Static_assert(sizeof(ml_obs_t) == 1032, "ml_obs_t wire size changed");
_Static_assert(sizeof(ml_client_register_t) == 120, "registration wire size changed");
_Static_assert(sizeof(ml_client_ack_t) == 64, "ack wire size changed");
_Static_assert(sizeof(ml_client_telemetry_t) == 1128, "telemetry wire size changed");

#endif
