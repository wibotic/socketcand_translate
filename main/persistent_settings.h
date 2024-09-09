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
  // Device hostname.
  // Length 32 based on `esp_netif_set_hostname()` documentation.
  char hostname[32];

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

  // Should WIFI use DHCP instead of static IP?
  bool wifi_use_dhcp;

  // If not `wifi_use_dhcp`, static IP info to use
  esp_netif_ip_info_t wifi_ip_info;

  // Bitrate of CAN interface
  enum can_bitrate_setting can_bitrate;

  // OpenCyphal node enabled?
  bool enable_cyphal;

  // ID of Cyphal Node if enabled.
  uint8_t cyphal_node_id;

} persistent_settings_t;

// Default `persistent_settings_t`.
static const persistent_settings_t persistent_settings_default = {
    .hostname = "socketcand-adapter",
    .eth_use_dhcp = false,
    .eth_ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 2, 163),
    .eth_ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    .eth_ip_info.gw.addr = ESP_IP4TOADDR(192, 168, 2, 1),
    .wifi_enabled = false,
    .wifi_ssid = "ssid_changeme",
    .wifi_pass = "password_changeme",
    .wifi_use_dhcp = true,
    .wifi_ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 2, 163),
    .wifi_ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    .wifi_ip_info.gw.addr = ESP_IP4TOADDR(192, 168, 2, 1),
    .can_bitrate = CAN_KBITS_500,
    .enable_cyphal = false,
    .cyphal_node_id = 98,
};

// Pointer to the current persistent settings.
// Starts out NULL. Call `persistent_settings_load()`
// to fill this with the loaded settings.
extern const persistent_settings_t* persistent_settings;

// Pointer to the current persistent settings in JSON form.
// Starts out NULL. Call `persistent_settings_load()`
// to fill this with the loaded settings.
extern const char* persistent_settings_json;

// Initialzies NVS flash memory with `nvs_flash_init()`.
// Must be called before other `persistent_settings` functions.
// On failure, erases all NVS memory, and retries.
// On second failure, returns the error.
esp_err_t persistent_settings_init_nvs(void);

// Fills out `persistent_settings` and `persistent_settings_json`
// with the current persistent settings.
// Initializes them to `persistent_settings_default` if no saved settings were
// found. NVS flash must be initialized before calling this.
esp_err_t persistent_settings_load(void);

// Saves the given settings to NVS flash memory.
// To enact the saved settings, call `esp_restart()`.
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