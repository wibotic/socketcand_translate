#pragma once

#include <hal/twai_types.h>

#include "esp_netif.h"

// The different CAN bitrates that the ESP32 supports
enum can_bitrate_setting {
  CAN_KBITS_25 = 25,
  CAN_KBITS_50 = 50,
  CAN_KBITS_100 = 100,
  CAN_KBITS_125 = 125,
  CAN_KBITS_250 = 250,
  CAN_KBITS_500 = 500,
  CAN_KBITS_800 = 800,
  CAN_KBITS_1000 = 1000,
};

// Persistent settings for this socketcand adapter.
typedef struct {
  // Ethernet hostname
  char eth_hostname[256];

  // Should ethernet use DHCP instead of static IP?
  bool eth_use_dhcp;

  // If not `eth_use_dhcp`, static IP info to use
  esp_netif_ip_info_t eth_ip_info;

  // Is WIFI enabled?
  bool wifi_enabled;

  // WIFI name
  char wifi_ssid[32];

  // WIFI password
  char wifi_pass[64];

  // WIFI hostname
  char wifi_hostname[256];

  // Should WIFI use DHCP instead of static IP?
  bool wifi_use_dhcp;

  // If not `wifi_use_dhcp`, static IP info to use
  esp_netif_ip_info_t wifi_ip_info;

  // Bitrate of CAN interface
  enum can_bitrate_setting can_bitrate;

} persistent_settings_t;

// Default `persistent_settings_t`.
extern const persistent_settings_t persistent_settings_default;

// Pointer to the current persistent settings.
// Starts out NULL. Read current persistent settings using
// `persistent_settings_load()`.
extern const persistent_settings_t* persistent_settings;

// Pointer to the current persistent settings in JSON form.
// Starts out NULL. Read current persistent settings using
// `persistent_settings_load()`.
extern const char* persistent_settings_json;

// Initializes NVS flash memory and reads the stored persistent settings.
// Fills out `persistent_settings` and `persistent_settings_json`
// with the loaded value.
// Initializes them to the default if no saved settings were found.
// Must not be called more than once.
esp_err_t persistent_settings_load(void);

// Saves the given settings to NVS flash memory.
// On success, immediately restarts the ESP32 and doesn't return.
// On failure, returns an error code.
// NVS flash must be initialized before calling this.
esp_err_t persistent_settings_save(const persistent_settings_t* settings);

// Sets `timing_config_out` to a config that
// corresponds to `can_bitrate`.
// If the `can_bitrate` enum was invalid, returns
// `ESP_ERR_INVALID_ARG`.
esp_err_t persistent_settings_get_timing_config(
    enum can_bitrate_setting can_bitrate,
    twai_timing_config_t* timing_config_out);

// Sets up button 1 (GPIO 34) on the ESP-EVB to
// reset the persistent settings to default
// when held for 1 second.
// Must only be called at most one time.
esp_err_t persistent_settings_setup_reset_button(void);