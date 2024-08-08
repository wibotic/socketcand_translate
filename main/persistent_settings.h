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
  // Should ethernet use static IP instead of DHCP?
  bool eth_use_static;

  // If `eth_use_static`, static IP info to use
  esp_netif_ip_info_t eth_ip_info;

  // Is WIFI enabled?
  bool wifi_enabled;

  // WIFI name
  char wifi_ssid[32];

  // WIFI password
  char wifi_pass[64];

  // Should WIFI use static IP instead of DHCP?
  bool wifi_use_static;

  // If `wifi_use_static`, static IP info to use
  esp_netif_ip_info_t wifi_ip_info;

  // Bitrate of CAN interface
  enum can_bitrate_setting can_bitrate;

} persistent_settings_t;

// Default `persistent_settings_t`.
extern const persistent_settings_t persistent_settings_default;

// Initializes NVS flash memory. Must be called
// before saving and loading settings.
esp_err_t persistent_settings_init(void);

// Saves the given settings to NVS flash memory.
// On success, immediately restarts the ESP32 and doesn't return.
// On failure, returns an error code.
// NVS flash must be initialized before calling this.
esp_err_t persistent_settings_save(const persistent_settings_t* settings);

// Loads the settings saved in NVS flash memory.
// If config wasn't saved, returns the default config.
// NVS flash must be initialized before calling this.
esp_err_t persistent_settings_load(persistent_settings_t* settings_out);

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
// May only be called at most one time.
esp_err_t persistent_settings_setup_reset_button(void);

// Print `settings` as a JSON null-terminated C-string to `buf_out`.
// `buflen` is the length of `buf_out`.
// Returns the number of characters written.
// Returns -1 if `buflen` was too small to fit the string.
esp_err_t persistent_settings_to_json(const persistent_settings_t* settings,
                                      char* buf_out, size_t buflen);