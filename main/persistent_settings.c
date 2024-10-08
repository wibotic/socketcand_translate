#include "persistent_settings.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "iot_button.h"
#include "nvs_flash.h"

// Name that will be used for logging
static const char *TAG = "persistent_settings";

const persistent_settings_t *persistent_settings = NULL;
static persistent_settings_t persistent_settings_data;

const char *persistent_settings_json = NULL;
static char persistent_settings_json_data[1024];

// A callback that gets called whenever button 1 is long-pressed.
// Resets the persistent settings back to default.
static void button_handler(void *button_handle, void *usr_data);

esp_err_t persistent_settings_save(const persistent_settings_t *config) {
  nvs_handle_t nvs;
  esp_err_t err;

  err = nvs_open("main_config", NVS_READWRITE, &nvs);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't open NVS.");

  err = nvs_set_blob(nvs, "config", config, sizeof(persistent_settings_t));
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't save value to NVS.");

  err = nvs_commit(nvs);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't commit settings to NVS storage.");

  nvs_close(nvs);

  return ESP_OK;
}

esp_err_t persistent_settings_init_nvs(void) {
  // Initialize the flash
  esp_err_t err = nvs_flash_init();

  // Erase NVS and retry on failure
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS error: %s", esp_err_to_name(err));
    ESP_LOGE(TAG, "Erasing NVS flash in attempt to fix error.");
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  return err;
}

esp_err_t persistent_settings_load(void) {
  esp_err_t err;

  nvs_handle_t nvs;

  // Open with read and write, because this allows
  // nvs_open to create the NVS namespace if it wasn't found.
  err = nvs_open("main_config", NVS_READWRITE, &nvs);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't open NVS.");

  size_t config_size = sizeof(persistent_settings_t);
  err = nvs_get_blob(nvs, "config", &persistent_settings_data, &config_size);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Couldn't load persistent settings: %s",
             esp_err_to_name(err));
    ESP_LOGW(TAG, "Using default settings instead.");
    persistent_settings_data = persistent_settings_default;
  }

  nvs_close(nvs);

  persistent_settings = &persistent_settings_data;

  // Also fill out `persistent_settings_json`.
  int bytes_written = snprintf(
      persistent_settings_json_data, sizeof(persistent_settings_json_data),
      "{\n"

      "\"hostname\": "
      "\"%s\",\n"

      "\"eth_use_dhcp\": "
      "%s,\n"

      "\"eth_ip\": "
      "\"" IPSTR
      "\",\n"

      "\"eth_netmask\": "
      "\"" IPSTR
      "\",\n"

      "\"eth_gw\": "
      "\"" IPSTR
      "\",\n"

      "\"wifi_enabled\": "
      "%s,\n"

      "\"wifi_ssid\": "
      "\"%s\",\n"

      "\"wifi_pass\": "
      "\"******\",\n"

      "\"wifi_use_dhcp\": "
      "%s,\n"

      "\"wifi_ip\": "
      "\"" IPSTR
      "\",\n"

      "\"wifi_netmask\": "
      "\"" IPSTR
      "\",\n"

      "\"wifi_gw\": "
      "\"" IPSTR
      "\",\n"

      "\"can_bitrate\": "
      "%d,\n"

      "\"enable_cyphal\": "
      "%s,\n"

      "\"cyphal_node_id\": "
      "%d\n"

      "}\n",
      persistent_settings->hostname,
      persistent_settings->eth_use_dhcp ? "true" : "false",
      IP2STR(&persistent_settings->eth_ip_info.ip),
      IP2STR(&persistent_settings->eth_ip_info.netmask),
      IP2STR(&persistent_settings->eth_ip_info.gw),
      persistent_settings->wifi_enabled ? "true" : "false",
      persistent_settings->wifi_ssid,
      // current_config.wifi_pass,
      persistent_settings->wifi_use_dhcp ? "true" : "false",
      IP2STR(&persistent_settings->wifi_ip_info.ip),
      IP2STR(&persistent_settings->wifi_ip_info.netmask),
      IP2STR(&persistent_settings->wifi_ip_info.gw),
      persistent_settings->can_bitrate,
      persistent_settings->enable_cyphal ? "true" : "false",
      persistent_settings->cyphal_node_id);

  if (bytes_written < 0 ||
      bytes_written >= sizeof(persistent_settings_json_data)) {
    ESP_LOGE(TAG, "persistent_settings static json buffer too small.");
    return ESP_FAIL;
  }
  persistent_settings_json = persistent_settings_json_data;

  return ESP_OK;
}

esp_err_t persistent_settings_get_timing_config(
    enum can_bitrate_setting can_bitrate,
    twai_timing_config_t *timing_config_out) {
  switch (can_bitrate) {
    case CAN_KBITS_25:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_25KBITS();
      return ESP_OK;
    case CAN_KBITS_50:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_50KBITS();
      return ESP_OK;
    case CAN_KBITS_100:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_100KBITS();
      return ESP_OK;
    case CAN_KBITS_125:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
      return ESP_OK;
    case CAN_KBITS_250:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
      return ESP_OK;
    case CAN_KBITS_500:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
      return ESP_OK;
    case CAN_KBITS_800:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_800KBITS();
      return ESP_OK;
    case CAN_KBITS_1000:
      *timing_config_out = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
      return ESP_OK;
    default:
      return ESP_ERR_INVALID_ARG;
  }
}

esp_err_t persistent_settings_setup_reset_button() {
  button_config_t button_config = {.type = BUTTON_TYPE_GPIO,
                                   .long_press_time = 1000,
                                   .gpio_button_config.gpio_num = 34,
                                   .gpio_button_config.active_level = 0};

  button_handle_t button_handle = iot_button_create(&button_config);
  if (button_handle == NULL) {
    ESP_LOGE(TAG, "Couldn't create button.");
    return ESP_FAIL;
  }

  esp_err_t err = iot_button_register_cb(button_handle, BUTTON_LONG_PRESS_START,
                                         button_handler, NULL);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register button callback.");

  return ESP_OK;
}

static void button_handler(void *button_handle, void *usr_data) {
  ESP_LOGI(TAG, "Button 1 held. Resetting settings to default and rebooting.");
  esp_err_t err = persistent_settings_save(&persistent_settings_default);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't save persistent settings. Erasing all NVS memory.");
    nvs_flash_erase();
  }
  esp_restart();
}
