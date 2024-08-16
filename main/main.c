#include "discovery_beacon.h"
#include "driver_setup.h"
#include "esp_log.h"
#include "http_server.h"
#include "persistent_settings.h"
#include "socketcand_server.h"

// Name that will be used for logging
static const char* TAG = "main";

void app_main(void) {
  // Create an event loop.
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Initialize the networking stack.
  ESP_ERROR_CHECK(esp_netif_init());

  // Initialize nonvolatile memory storage
  ESP_ERROR_CHECK(persistent_settings_load());

  // Set up the long-press settings reset button
  ESP_ERROR_CHECK(persistent_settings_setup_reset_button());

  // Print the settings over uart
  ESP_LOGI(TAG, "Current settings:\n");
  esp_log_write(ESP_LOG_INFO, TAG, persistent_settings_json);

  // Start the CAN bus driver.
  twai_timing_config_t timing_config;
  esp_err_t err = persistent_settings_get_timing_config(
      persistent_settings->can_bitrate, &timing_config);
  if (err == ESP_ERR_INVALID_ARG) {
    ESP_ERROR_CHECK(persistent_settings_save(&persistent_settings_default));
  }

  // Ethernet driver setup will fail unless the ethernet hardware
  // accquired a clock signal, which takes a few milliseconds.
  vTaskDelay(pdMS_TO_TICKS(200));

  err = driver_setup_can(&timing_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start CAN driver: %s",
             esp_err_to_name(err));
  }

  // start wifi driver
  if (persistent_settings->wifi_enabled) {
    if (persistent_settings->wifi_use_dhcp) {
      err = driver_setup_wifi(NULL, persistent_settings->wifi_ssid,
                              persistent_settings->wifi_pass);

    } else {
      err = driver_setup_wifi(&persistent_settings->wifi_ip_info,
                              persistent_settings->wifi_ssid,
                              persistent_settings->wifi_pass);
    }

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "CRITICAL: Couldn't start WIFI driver: %s",
               esp_err_to_name(err));
    }
  }

  // start ethernet driver
  if (persistent_settings->eth_use_dhcp) {
    err = driver_setup_ethernet(NULL);
  } else {
    err = driver_setup_ethernet(&persistent_settings->eth_ip_info);
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start ethernet driver: %s",
             esp_err_to_name(err));
  }

  // start HTTP server used for configuring stuff
  ESP_ERROR_CHECK(start_http_server());

  // Start the socketcand translation server
  ESP_ERROR_CHECK(socketcand_server_start(29536));

  // start the UDP beacon
  if (discovery_beacon_start() != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't start UDP beacon: %s",
             esp_err_to_name(err));
  }

  // Log network status:
  vTaskDelay(pdMS_TO_TICKS(10000));
  const char* json_status;
  err = driver_setup_get_status_json(&json_status);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Network status after startup:");
    esp_log_write(ESP_LOG_INFO, TAG, json_status);
  } else {
    ESP_LOGE(TAG, "Couldn't get driver status: %s", esp_err_to_name(err));
  }
  err = driver_setup_release_json_status();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Couldn't release JSON status.");
  }
}
