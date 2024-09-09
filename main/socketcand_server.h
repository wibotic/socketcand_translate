#pragma once

#include "esp_err.h"

// The status of the socketcand server.
// Get the current status using `socketcand_server_status()`.
typedef struct {
  uint64_t socketcand_frames_received;
  uint64_t socketcand_frames_sent;
  uint64_t invalid_socketcand_frames_received;
  uint64_t can_bus_frames_sent;
  uint64_t can_bus_frames_send_timeouts;
} socketcand_server_status_t;

// Starts a socketcand TCP server listening on IPv4 `0.0.0.0:29536` on a new
// task. Accepts up to 5 simultaneous TCP connections.
// Must only be called one time.
// Must be called after `can_listener` has been started.
esp_err_t socketcand_server_start(void);

// Fills `status_out` with the current `socketcand_server_status_t`.
// Returns an error if the server isn't running.
esp_err_t socketcand_server_status(socketcand_server_status_t* status_out);