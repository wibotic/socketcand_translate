#include "discovery_beacon.h"
#include "driver_setup.h"
#include "esp_log.h"
#include "http_server.h"
#include "persistent_settings.h"
#include "socketcand_server.h"

// Name that will be used for logging
static const char *TAG = "main";

void app_main(void) {
  // Create an event loop.
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Initialize the networking stack.
  ESP_ERROR_CHECK(esp_netif_init());

  // Wait for hardware to start
  vTaskDelay(pdMS_TO_TICKS(200));

  // Initialize nonvolatile memory storage
  ESP_ERROR_CHECK(persistent_settings_init());

  // Set up the long-press settings reset button
  ESP_ERROR_CHECK(persistent_settings_setup_reset_button());

  // Read saved settings
  persistent_settings_t settings;
  ESP_ERROR_CHECK(persistent_settings_load(&settings));

  // Print the settings over uart
  char settings_json[512];
  int bytes_written = persistent_settings_to_json(&settings, settings_json,
                                                  sizeof(settings_json));
  if (bytes_written == -1) {
    ESP_LOGE(TAG, "Couldn't get JSON of persistent settings. Aborting.");
    abort();
  }
  ESP_LOGI(TAG, "Current settings:\n%s\n", settings_json);

  // Start the CAN bus driver.
  twai_timing_config_t timing_config;
  esp_err_t err = persistent_settings_get_timing_config(settings.can_bitrate,
                                                        &timing_config);
  if (err == ESP_ERR_INVALID_ARG) {
    ESP_ERROR_CHECK(persistent_settings_save(&persistent_settings_default));
  }
  driver_setup_can(&timing_config);

  // start wifi driver
  if (settings.wifi_enabled) {
    if (settings.wifi_use_static) {
      driver_setup_wifi(&settings.wifi_ip_info, settings.wifi_ssid,
                        settings.wifi_pass);
    } else {
      driver_setup_wifi(NULL, settings.wifi_ssid, settings.wifi_pass);
    }
  }

  // start ethernet driver
  if (settings.eth_use_static) {
    driver_setup_ethernet(&settings.eth_ip_info);
  } else {
    driver_setup_ethernet(NULL);
  }

  // start HTTP server used for configuring stuff
  start_http_server();

  // Start the socketcand translation server
  ESP_ERROR_CHECK(socketcand_server_start(9999));

  // start the UDP beacon
  if (discovery_beacon_start() == -1) {
    ESP_LOGE(TAG, "Couldn't start UDP beacon. Aborting.");
    abort();
  }
}
