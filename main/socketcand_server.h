#pragma once

#include "esp_err.h"

// Starts a socketcand TCP server listening on IPv4 `0.0.0.0:port` on a new
// task. Accepts up to 5 simultaneous TCP connections.
// Must only be called one time.
esp_err_t socketcand_server_start(uint16_t port);
