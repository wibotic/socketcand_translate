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
