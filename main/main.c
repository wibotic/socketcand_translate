#include "discovery_beacon.h"
#include "driver_setup.h"
#include "esp_log.h"
#include "http_server.h"
#include "persistent_settings.h"
#include "socketcand_server.h"
#include "status_report.h"

// Name that will be used for logging
static const char* TAG = "main";

void app_main(void) {
  // Create an event loop.
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Initialize the networking stack.
  ESP_ERROR_CHECK(esp_netif_init());

  // Initialze the NVS partition.
  ESP_ERROR_CHECK(persistent_settings_init_nvs());

  // Load persistent settings, or get defaults.
  ESP_ERROR_CHECK(persistent_settings_load());

  // Set up the long-press settings reset button
  ESP_ERROR_CHECK(persistent_settings_setup_reset_button());

  // Print the persistent settings over uart
  ESP_LOGI(TAG, "Current settings:");
  esp_log_write(ESP_LOG_INFO, TAG, persistent_settings_json);
  ESP_LOGI(
      TAG,
      "Hold button BUT1 for one second to reset these settings to default.");

  esp_err_t err;
  const esp_netif_ip_info_t* ip_info_setting;

  // start wifi driver if enabled
  if (persistent_settings->wifi_enabled) {
    // Enable or disable DHCP
    if (persistent_settings->wifi_use_dhcp) {
      ip_info_setting = NULL;
    } else {
      ip_info_setting = &persistent_settings->wifi_ip_info;
    }

    err = driver_setup_wifi(ip_info_setting, persistent_settings->hostname,
                            persistent_settings->wifi_ssid,
                            persistent_settings->wifi_pass);

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "CRITICAL: Couldn't start WIFI driver: %s",
               esp_err_to_name(err));
    }
  }

  // Ethernet driver setup will fail unless the ethernet hardware
  // accquires a clock signal, which takes a few milliseconds.
  vTaskDelay(pdMS_TO_TICKS(200));

  // Enable or disable DHCP
  if (persistent_settings->eth_use_dhcp) {
    ip_info_setting = NULL;
  } else {
    ip_info_setting = &persistent_settings->eth_ip_info;
  }

  // Start ethernet driver
  err = driver_setup_ethernet(ip_info_setting, persistent_settings->hostname);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start ethernet driver: %s",
             esp_err_to_name(err));
  }

  // Get the CAN bus timing configuration
  twai_timing_config_t timing_config;
  err = persistent_settings_get_timing_config(persistent_settings->can_bitrate,
                                              &timing_config);

  if (err != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Invalid CAN bitrate in settings. Resetting settings to defaults.");
    ESP_ERROR_CHECK(persistent_settings_save(&persistent_settings_default));
  }

  // Set up the CAN bus driver.
  err = driver_setup_can(&timing_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start CAN driver: %s",
             esp_err_to_name(err));
  }

  // start HTTP server used for configuring stuff
  err = start_http_server();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start HTTP server: %s",
             esp_err_to_name(err));
  }

  // Start the socketcand translation server
  err = socketcand_server_start(29536);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start socketcand server: %s",
             esp_err_to_name(err));
  }

  // start the UDP beacon
  err = discovery_beacon_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start UDP beacon: %s",
             esp_err_to_name(err));
  }

  // Log network status after giving some time
  // for connections to establish.
  vTaskDelay(pdMS_TO_TICKS(10000));
  const char* json_status;
  err = status_report_get(&json_status, driver_setup_eth_netif,
                          driver_setup_wifi_netif);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Network status after startup:");
    esp_log_write(ESP_LOG_INFO, TAG, json_status);
  } else {
    ESP_LOGE(TAG, "CRITICAL: Couldn't get driver status: %s",
             esp_err_to_name(err));
  }
  err = status_report_release();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't release JSON status.");
  }
}
