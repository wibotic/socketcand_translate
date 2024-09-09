#pragma once

#include "esp_http_server.h"

// Starts an HTTP server on port 80.
// Serves an info screen with config options.
// May only be called once.
esp_err_t start_http_server(void);
