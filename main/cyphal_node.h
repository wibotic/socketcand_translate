#pragma once

#include "esp_err.h"
#include "stdint.h"

typedef struct {
    int64_t heartbeats_received;
    int64_t heartbeats_sent;
} cyphal_node_status_t;


// Starts an OpenCyphal node with `node_id` that sends
// a heartbeat every second.
// This function must be called only after
// `can_listener` has been started.
esp_err_t cyphal_node_start(uint8_t node_id);

// Fills `status_out` with the current `cyphal_node_status_t`.
// Returns an error if the OpenCyphal node hasn't been started yet.
esp_err_t cyphal_node_get_status(cyphal_node_status_t *status_out);