#include "persistent_settings.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_system.h"
#include "iot_button.h"
#include "nvs_flash.h"

// Name that will be used for logging
static const char *TAG = "persistent_settings";

// A callback that gets called whenever button 1 is long-pressed.
// Resets the persistent settings back to default.
static void button_handler(void *button_handle, void *usr_data);

const persistent_settings_t persistent_settings_default = {
    .eth_use_static = true,
    .eth_ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 2, 163),
    .eth_ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    .eth_ip_info.gw.addr = ESP_IP4TOADDR(192, 168, 2, 1),
    .wifi_enabled = false,
    .wifi_ssid = "ssid_changeme",
    .wifi_pass = "password_changeme",
    .wifi_use_static = false,
    .wifi_ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 2, 163),
    .wifi_ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    .wifi_ip_info.gw.addr = ESP_IP4TOADDR(192, 168, 2, 1),
    .can_bitrate = CAN_KBITS_500};

esp_err_t persistent_settings_init(void) {
  esp_err_t res = nvs_flash_init();
  if (res != ESP_OK) {
    ESP_LOGE(TAG, "NVS error: %s", esp_err_to_name(res));
    // See if erasing NVS flash fixes the error
    nvs_flash_erase();
    res = nvs_flash_init();
  }
  return res;
}

esp_err_t persistent_settings_save(const persistent_settings_t *config) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  err = nvs_open("main_config", NVS_READWRITE, &nvs_handle);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't open NVS.");

  err =
      nvs_set_blob(nvs_handle, "config", config, sizeof(persistent_settings_t));
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't save value to NVS.");

  err = nvs_commit(nvs_handle);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't commit settings to NVS storage.");

  nvs_close(nvs_handle);
  esp_restart();
  return ESP_OK;
}

esp_err_t persistent_settings_load(persistent_settings_t *out_config) {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  err = nvs_open("main_config", NVS_READWRITE, &nvs_handle);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't open NVS.");

  size_t config_size = sizeof(persistent_settings_t);
  err = nvs_get_blob(nvs_handle, "config", out_config, &config_size);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    *out_config = persistent_settings_default;
  } else {
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't read value to NVS.");
  }

  nvs_close(nvs_handle);
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
  button_config_t button_config = {
      .type = BUTTON_TYPE_GPIO,
      .long_press_time = 1000,
      .gpio_button_config = {.gpio_num = 34, .active_level = 0}};
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
  ESP_LOGI(TAG, "Button 1 held. Resetting settings to default, and rebooting.");
  ESP_ERROR_CHECK(persistent_settings_save(&persistent_settings_default));
}

esp_err_t persistent_settings_to_json(
    const persistent_settings_t *current_config, char *buf_out, size_t buflen) {
  int bytes_written = snprintf(
      buf_out, buflen,
      "{\n"
      "\"eth_use_static\": "
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
      "null,\n"

      "\"wifi_use_static\": "
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
      "%d\n"

      "}",
      current_config->eth_use_static ? "true" : "false",
      IP2STR(&current_config->eth_ip_info.ip),
      IP2STR(&current_config->eth_ip_info.netmask),
      IP2STR(&current_config->eth_ip_info.gw),
      current_config->wifi_enabled ? "true" : "false",
      current_config->wifi_ssid,
      // current_config.wifi_pass,
      current_config->wifi_use_static ? "true" : "false",
      IP2STR(&current_config->wifi_ip_info.ip),
      IP2STR(&current_config->wifi_ip_info.netmask),
      IP2STR(&current_config->wifi_ip_info.gw), current_config->can_bitrate);

  if (bytes_written < 0 || bytes_written >= buflen) {
    return -1;
  }
  return bytes_written;
}