#include "http_server.h"

#include "driver_setup.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "persistent_settings.h"

// Name that will be used for logging
#define TAG "http_server"

extern const uint8_t index_html[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t alpine_js[] asm("_binary_alpine_js_start");
extern const uint8_t alpine_js_end[] asm("_binary_alpine_js_end");

// Handles GET /
static esp_err_t handle_get(httpd_req_t *req);

static const httpd_uri_t get_handler = {
    .uri = "/", .handler = handle_get, .method = HTTP_GET, .user_ctx = NULL};

// Handles GET /alpine.js
static esp_err_t handle_get_alpine(httpd_req_t *req);

static const httpd_uri_t get_alpine_handler = {.uri = "/alpine.js",
                                               .handler = handle_get_alpine,
                                               .method = HTTP_GET,
                                               .user_ctx = NULL};

// Handles GET /config
static esp_err_t handle_get_config(httpd_req_t *req);

static const httpd_uri_t get_config_handler = {.uri = "/config",
                                               .handler = handle_get_config,
                                               .method = HTTP_GET,
                                               .user_ctx = NULL};

// Handles POST /config
static esp_err_t handle_post_config(httpd_req_t *req);

static const httpd_uri_t post_config_handler = {.uri = "/config",
                                                .handler = handle_post_config,
                                                .method = HTTP_POST,
                                                .user_ctx = NULL};

// Handles GET /status
static esp_err_t handle_get_status(httpd_req_t *req);

static const httpd_uri_t get_status_handler = {.uri = "/status",
                                               .handler = handle_get_status,
                                               .method = HTTP_GET,
                                               .user_ctx = NULL};

httpd_handle_t start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;

  httpd_handle_t server = NULL;

  ESP_ERROR_CHECK(httpd_start(&server, &config));
  assert(server);

  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_handler));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_alpine_handler));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_config_handler));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post_config_handler));
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_status_handler));

  return server;
}

static esp_err_t handle_get(httpd_req_t *req) {
  return httpd_resp_send(req, (char *)index_html, index_html_end - index_html);
}

static esp_err_t handle_get_alpine(httpd_req_t *req) {
  esp_err_t err = httpd_resp_set_type(req, "text/javascript");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, (char *)alpine_js, alpine_js_end - alpine_js);
}

static esp_err_t handle_get_config(httpd_req_t *req) {
  // Get the current settings
  persistent_settings_t settings;
  esp_err_t err = persistent_settings_load(&settings);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, 500, "Couldn't load config.");
    ESP_LOGE(TAG, "Couldn't load config: %d", err);
    return err;
  }

  char settings_json[512];

  int bytes_written = persistent_settings_to_json(&settings, settings_json,
                                                  sizeof(settings_json));

  if (bytes_written < 0 || bytes_written >= sizeof(settings_json)) {
    httpd_resp_send_err(req, 500, "Couldn't generate JSON of settings.");
    ESP_LOGE(TAG, "Couldn't generate JSON of settings.");
    return ESP_FAIL;
  }

  err = httpd_resp_set_type(req, "application/json");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, settings_json, bytes_written);
}

static esp_err_t handle_post_config(httpd_req_t *req) {
  char content[512];

  // If POST request is too large
  if (req->content_len >= sizeof(content)) {
    httpd_resp_send_err(req, 500, "POST content too long.");
    ESP_LOGE(TAG, "POST content too long.");
    return ESP_FAIL;
  }

  // Read the post request content
  int ret = httpd_req_recv(req, content, req->content_len);

  // If unexpected content length
  if (ret != req->content_len) {
    httpd_resp_send_err(req, 500, "Couldn't read POST content");
    ESP_LOGE(TAG, "Couldn't read POST content.");
    return ESP_FAIL;
  }

  content[req->content_len] = '\0';

  // Get the current config
  persistent_settings_t conf;
  esp_err_t err = persistent_settings_load(&conf);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, 500, "Couldn't load config.");
    ESP_LOGE(TAG, "Couldn't load config: %d", err);
    return err;
  }

  char val_buf[128];

  // read eth_use_static field
  err = httpd_query_key_value(content, "eth_use_static", val_buf,
                              sizeof(val_buf));
  if (err == ESP_OK) {
    if (strncasecmp(val_buf, "true", 4) == 0)
      conf.eth_use_static = true;
    else
      conf.eth_use_static = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read eth_ip field
  err = httpd_query_key_value(content, "eth_ip", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(val_buf, &conf.eth_ip_info.ip);
    if (err != ESP_OK) {
      httpd_resp_send_err(req, 500, "IP address must be of form '1.2.3.4'.");
      ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read eth_netmask field
  err = httpd_query_key_value(content, "eth_netmask", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(val_buf, &conf.eth_ip_info.netmask);
    if (err != ESP_OK) {
      httpd_resp_send_err(req, 500, "IP address must be of form '1.2.3.4'.");
      ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read eth_gw field
  err = httpd_query_key_value(content, "eth_gw", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(val_buf, &conf.eth_ip_info.gw);
    if (err != ESP_OK) {
      httpd_resp_send_err(req, 500, "IP address must be of form '1.2.3.4'.");
      ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read wifi_enabled field
  err =
      httpd_query_key_value(content, "wifi_enabled", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    if (strncasecmp(val_buf, "true", 4) == 0)
      conf.wifi_enabled = true;
    else
      conf.wifi_enabled = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read wifi_ssid field
  err = httpd_query_key_value(content, "wifi_ssid", val_buf,
                              sizeof(conf.wifi_ssid));
  if (err == ESP_OK) {
    memcpy(conf.wifi_ssid, val_buf, sizeof(conf.wifi_ssid));
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read wifi_pass field
  err = httpd_query_key_value(content, "wifi_pass", val_buf,
                              sizeof(conf.wifi_pass));
  if (err == ESP_OK) {
    memcpy(conf.wifi_pass, val_buf, sizeof(conf.wifi_pass));
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read wifi_use_static field
  err = httpd_query_key_value(content, "wifi_use_static", val_buf,
                              sizeof(val_buf));
  if (err == ESP_OK) {
    if (strncasecmp(val_buf, "true", 4) == 0)
      conf.wifi_use_static = true;
    else
      conf.wifi_use_static = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read wifi_ip field
  err = httpd_query_key_value(content, "wifi_ip", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(val_buf, &conf.wifi_ip_info.ip);
    if (err != ESP_OK) {
      httpd_resp_send_err(req, 500, "IP address must be of form '1.2.3.4'.");
      ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read wifi_netmask field
  err =
      httpd_query_key_value(content, "wifi_netmask", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(val_buf, &conf.wifi_ip_info.netmask);
    if (err != ESP_OK) {
      httpd_resp_send_err(req, 500, "IP address must be of form '1.2.3.4'.");
      ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read wifi_gw field
  err = httpd_query_key_value(content, "wifi_gw", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(val_buf, &conf.wifi_ip_info.gw);
    if (err != ESP_OK) {
      httpd_resp_send_err(req, 500, "IP address must be of form '1.2.3.4'.");
      ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  // read can_bitrate field
  err = httpd_query_key_value(content, "can_bitrate", val_buf, sizeof(val_buf));
  if (err == ESP_OK) {
    long num = strtol(val_buf, NULL, 10);

    enum can_bitrate_setting bitrate = (enum can_bitrate_setting)num;

    // check if the bitrate is one of the possible options
    switch (bitrate) {
      case CAN_KBITS_25:
      case CAN_KBITS_50:
      case CAN_KBITS_100:
      case CAN_KBITS_125:
      case CAN_KBITS_250:
      case CAN_KBITS_500:
      case CAN_KBITS_800:
      case CAN_KBITS_1000:
        break;
      default:
        httpd_resp_send_err(req, 500, "Invalid CAN bitrate value was given.");
        ESP_LOGE(TAG, "Invalid CAN bitrate value was given.");
        return ESP_FAIL;
    }
    conf.can_bitrate = bitrate;
  } else if (err != ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, 500, "Error reading query parameters.");
    ESP_LOGE(TAG, "Error reading query parameters: %s", esp_err_to_name(err));
    return err;
  }

  //// Save the new configuration

  err = httpd_resp_send(req, "Updating settings...", HTTPD_RESP_USE_STRLEN);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't send response: %s", esp_err_to_name(err));
    return err;
  }

  err = persistent_settings_save(&conf);

  if (err != ESP_OK) {
    httpd_resp_send_err(req, 500, "Couldn't save the given config.");
    ESP_LOGE(TAG, "Couldn't save the given config: %s", esp_err_to_name(err));
    return ESP_FAIL;
  }

  return ESP_OK;
}

static esp_err_t handle_get_status(httpd_req_t *req) {
  char http[2048];

  int written = driver_setup_get_status_json(http, sizeof(http));
  if (written < 0 || written >= sizeof(http)) {
    httpd_resp_send_err(req, 500, "Internal buffer to small to send response.");
    ESP_LOGE(TAG, "Internal buffer too small to send response.");
    return ESP_FAIL;
  }

  return httpd_resp_send(req, http, written);
}