#pragma once

#include "driver/twai.h"
#include "esp_netif.h"

// Boilerplate for intializing drivers.

// A pointer to the global ethernet netif object.
// NULL if ethernet isn't started.
extern esp_netif_t* driver_setup_eth_netif;

// A pointer to the global WIFI netif object.
// NULL if WIFI isn't started.
extern esp_netif_t* driver_setup_wifi_netif;

// Starts the ESP32-EVB ethernet driver and populates `driver_setup_eth_netif`.
// `ip_info` specifies the static IP address config.
// Uses DHCP if `ip_info` is NULL.
esp_err_t driver_setup_ethernet(const esp_netif_ip_info_t* ip_info);

// Starts the ESP32-EVB wifi driver and populates `driver_setup_wifi_netif`.
// `ip_info` specifies the static IP address config.
// Uses DHCP if `ip_info` is NULL.
esp_err_t driver_setup_wifi(const esp_netif_ip_info_t* ip_info,
                            const char ssid[32], const char password[64]);

// Starts the ESP32-EVB CAN driver with the given `timing_config`.
esp_err_t driver_setup_can(const twai_timing_config_t* timing_config);

// Returns a pointer to a C-string containing a
// JSON of the current network status.
// Since the string is in a shared buffer,
// this function will block if another task is also
// currently reading the status.
// Once the caller is done using the returned
// C-string, they must call
// `driver_setup_release_json_status()` to
// allow other callers to access the status.
esp_err_t driver_setup_get_status_json(const char** json_out);

// Must be called once finished using the buffer
// returned by `driver_setup_get_status_json()`.
esp_err_t driver_setup_release_json_status(void);
