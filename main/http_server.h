#pragma once

#include "esp_http_server.h"

// Starts an HTTP server on port 80.
// Servers an info screen with config options.
// Panics on error.
extern httpd_handle_t start_http_server(void);
