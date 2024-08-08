#pragma once

#include "driver/twai.h"
#include "esp_netif.h"

// A pointer to the global ethernet netif object.
// NULL if ethernet isn't started.
extern esp_netif_t* driver_setup_eth_netif;

// A pointer to the global WIFI netif object.
// NULL if WIFI isn't started.
extern esp_netif_t* driver_setup_wifi_netif;

// Boilerplate for intializing drivers.

// Starts the ESP32-EVB ethernet driver and populates `driver_setup_eth_netif`.
// `ip_info` specifies the static IP address config. Uses DHCP if `ip_info` is
// NULL. Panics on error.
// May only be called at most one time.
void driver_setup_ethernet(const esp_netif_ip_info_t* ip_info);

// Starts the ESP32-EVB wifi driver and populates `driver_setup_wifi_netif`.
// `ip_info` specifies the static IP address config. Uses DHCP if `ip_info` is
// NULL. Panics on error.
// May only be called at most one time.
void driver_setup_wifi(const esp_netif_ip_info_t* ip_info, const char ssid[32],
                       const char password[64]);

// Starts the ESP32-EVB CAN driver
// with the given `timing_config`.
// Panics on error.
// May only be called at most one time.
void driver_setup_can(const twai_timing_config_t* timing_config);

// Print the network status as a JSON null-terminated C-string to `buf_out`.
// `buflen` is the length of `buf_out`.
// Returns the number of characters written.
// Returns -1 if `buflen` was too small to fit the string.
int driver_setup_get_status_json(char* buf_out, size_t buflen);
