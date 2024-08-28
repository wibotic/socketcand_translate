#pragma once

#include "esp_err.h"

// Starts a task that broadcasts a socketcand CANBeacon
// over UDP to port 42000 every 2 seconds.
// Must only be called one time.
esp_err_t discovery_beacon_start();
