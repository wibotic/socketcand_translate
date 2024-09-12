#include "http_server.h"

#include "driver_setup.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "persistent_settings.h"
#include "status_report.h"

// Name that will be used for logging
#define TAG "http_server"

// GET /
extern const uint8_t index_html[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
static esp_err_t serve_get(httpd_req_t *req) {
  return httpd_resp_send(req, (char *)index_html, index_html_end - index_html);
}
static const httpd_uri_t get_handler = {
    .uri = "/", .handler = serve_get, .method = HTTP_GET, .user_ctx = NULL};

// GET /favicon.svg
extern const uint8_t favicon_svg[] asm("_binary_favicon_svg_start");
extern const uint8_t favicon_svg_end[] asm("_binary_favicon_svg_end");
static esp_err_t serve_get_favicon_svg(httpd_req_t *req) {
  esp_err_t err = httpd_resp_set_type(req, "image/svg+xml");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, (char *)favicon_svg,
                         favicon_svg_end - favicon_svg);
}
static const httpd_uri_t get_favicon_svg_handler = {
    .uri = "/favicon.svg",
    .handler = serve_get_favicon_svg,
    .method = HTTP_GET,
    .user_ctx = NULL};

// GET /script.js
extern const uint8_t script_js[] asm("_binary_script_js_start");
extern const uint8_t script_js_end[] asm("_binary_script_js_end");
static esp_err_t serve_get_script_js(httpd_req_t *req) {
  esp_err_t err = httpd_resp_set_type(req, "text/javascript");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, (char *)script_js, script_js_end - script_js);
}
static const httpd_uri_t get_script_js_handler = {
    .uri = "/script.js",
    .handler = serve_get_script_js,
    .method = HTTP_GET,
    .user_ctx = NULL};

// GET /alpine.js
extern const uint8_t alpine_js[] asm("_binary_alpine_js_start");
extern const uint8_t alpine_js_end[] asm("_binary_alpine_js_end");
static esp_err_t serve_get_alpine_js(httpd_req_t *req) {
  esp_err_t err = httpd_resp_set_type(req, "text/javascript");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, (char *)alpine_js, alpine_js_end - alpine_js);
}
static const httpd_uri_t get_alpine_js_handler = {
    .uri = "/alpine.js",
    .handler = serve_get_alpine_js,
    .method = HTTP_GET,
    .user_ctx = NULL};

// GET /api/config
static esp_err_t serve_get_api_config(httpd_req_t *req) {
  esp_err_t err = httpd_resp_set_type(req, "application/json");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, persistent_settings_json, HTTPD_RESP_USE_STRLEN);
}
static const httpd_uri_t get_api_config_handler = {
    .uri = "/api/config",
    .handler = serve_get_api_config,
    .method = HTTP_GET,
    .user_ctx = NULL};

// GET /api/status
static esp_err_t serve_get_api_status(httpd_req_t *req);
static const httpd_uri_t get_api_status_handler = {
    .uri = "/api/status",
    .handler = serve_get_api_status,
    .method = HTTP_GET,
    .user_ctx = NULL};

// POST /api/config
static esp_err_t serve_post_api_config(httpd_req_t *req);
static const httpd_uri_t post_api_config_handler = {
    .uri = "/api/config",
    .handler = serve_post_api_config,
    .method = HTTP_POST,
    .user_ctx = NULL};

// Updates `settings_to_update` with any updated values from `json`.
// On success, `settings_to_update` will hold the updated settings.
// On failure, returns an error.
static esp_err_t update_persistent_settings_from_json(
    const char *json, persistent_settings_t *settings_to_update);

esp_err_t start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;
  esp_err_t err;

  err = httpd_start(&server, &config);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't start HTTP server.");

  err = httpd_register_uri_handler(server, &get_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_favicon_svg_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_script_js_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_alpine_js_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_api_config_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_api_status_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &post_api_config_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  return ESP_OK;
}

static esp_err_t serve_get_api_status(httpd_req_t *req) {
  esp_err_t err;

  const char *status_json;

  err = status_report_get(&status_json, driver_setup_eth_netif,
                          driver_setup_wifi_netif);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't get current driver status: %s",
             esp_err_to_name(err));
    httpd_resp_send_err(req, 500, "Couldn't get current driver status.");
    err = status_report_release();
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't release JSON status.");
    return err;
  }

  esp_err_t http_err = httpd_resp_send(req, status_json, HTTPD_RESP_USE_STRLEN);

  err = status_report_release();
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't release JSON status.");

  return http_err;
}

// Temporarily stores the body of
// the request in `serve_post_api_config()`.
static char shared_post_buf[2048];
SemaphoreHandle_t post_buf_mutex = NULL;
StaticSemaphore_t post_buf_mutex_mem;

static esp_err_t serve_post_api_config(httpd_req_t *req) {
  // If POST request is too large
  if (req->content_len >= sizeof(shared_post_buf)) {
    httpd_resp_send_err(req, 500, "POST request payload is too long.");
    ESP_LOGE(TAG, "POST request payload was too long.");
    return ESP_FAIL;
  }

  // Ensure we're the only ones using `post_buf`.
  if (post_buf_mutex == NULL) {
    post_buf_mutex = xSemaphoreCreateMutexStatic(&post_buf_mutex_mem);
  }
  assert(xSemaphoreTake(post_buf_mutex, portMAX_DELAY) == pdTRUE);

  // Read the post request post_buf
  int ret = httpd_req_recv(req, shared_post_buf, req->content_len);

  // If unexpected post_buf length
  if (ret != req->content_len) {
    assert(xSemaphoreGive(post_buf_mutex) == pdTRUE);
    httpd_resp_send_err(req, 500, "Couldn't read POST post_buf");
    ESP_LOGE(TAG, "Couldn't read POST post_buf.");
    return ret;
  }

  // null-terminate as a precaution
  shared_post_buf[req->content_len] = '\0';

  // Parse the posted JSON
  persistent_settings_t new_persistent_settings = *persistent_settings;
  esp_err_t err = update_persistent_settings_from_json(
      shared_post_buf, &new_persistent_settings);

  assert(xSemaphoreGive(post_buf_mutex) == pdTRUE);

  // If couldn't parse the posted JSON
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error parsing POSTed persistent settings JSON: %s",
             esp_err_to_name(err));
    httpd_resp_send_err(req, 400,
                        "Received invalid settings. Make sure they're "
                        "formatted correctly!");
    return ESP_FAIL;
  }

  // Save the new configuration

  err = persistent_settings_save(&new_persistent_settings);

  if (err == ESP_OK) {
    err = httpd_resp_send(req, "Updating settings and restarting adapter...",
                          HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "Restarting ESP32 to enact updated settings.");
    esp_restart();
    return err;

  } else {
    ESP_LOGE(TAG, "Couldn't save persistent settings.");
    err = httpd_resp_send_err(
        req, 500, "Internal error: Couldn't save persistent settings.");
    return err;
  }
}

static esp_err_t update_persistent_settings_from_json(
    const char *json, persistent_settings_t *settings_to_update) {
  // rebind for brevity
  persistent_settings_t *cnf = settings_to_update;

  // Temporary buffer for reading the http query values.
  // `wifi_pass` is the largest field in `persistent_settings_t`,
  // and its length is 64.
  char arg_buf[64];

  esp_err_t err;

  // read hostname field
  memset(arg_buf, 0, sizeof(cnf->hostname));
  err = httpd_query_key_value(json, "hostname", arg_buf, sizeof(cnf->hostname));
  if (err == ESP_OK) {
    memcpy(cnf->hostname, arg_buf, sizeof(cnf->hostname));
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read eth_use_dhcp field
  err = httpd_query_key_value(json, "eth_use_dhcp", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    if (strncasecmp(arg_buf, "true", 4) == 0)
      cnf->eth_use_dhcp = true;
    else
      cnf->eth_use_dhcp = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read eth_ip field
  err = httpd_query_key_value(json, "eth_ip", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->eth_ip_info.ip);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read eth_netmask field
  err = httpd_query_key_value(json, "eth_netmask", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->eth_ip_info.netmask);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read eth_gw field
  err = httpd_query_key_value(json, "eth_gw", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->eth_ip_info.gw);
    if (err != ESP_OK) {
      return err;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_enabled field
  err = httpd_query_key_value(json, "wifi_enabled", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    if (strncasecmp(arg_buf, "true", 4) == 0)
      cnf->wifi_enabled = true;
    else
      cnf->wifi_enabled = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_ssid field
  memset(arg_buf, 0, sizeof(cnf->wifi_ssid));
  err =
      httpd_query_key_value(json, "wifi_ssid", arg_buf, sizeof(cnf->wifi_ssid));
  if (err == ESP_OK) {
    memcpy(cnf->wifi_ssid, arg_buf, sizeof(cnf->wifi_ssid));
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_pass field
  memset(arg_buf, 0, sizeof(cnf->wifi_pass));
  err =
      httpd_query_key_value(json, "wifi_pass", arg_buf, sizeof(cnf->wifi_pass));
  if (err == ESP_OK) {
    memcpy(cnf->wifi_pass, arg_buf, sizeof(cnf->wifi_pass));
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_use_dhcp field
  err = httpd_query_key_value(json, "wifi_use_dhcp", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    if (strncasecmp(arg_buf, "true", 4) == 0)
      cnf->wifi_use_dhcp = true;
    else
      cnf->wifi_use_dhcp = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_ip field
  err = httpd_query_key_value(json, "wifi_ip", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->wifi_ip_info.ip);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_netmask field
  err = httpd_query_key_value(json, "wifi_netmask", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->wifi_ip_info.netmask);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_gw field
  err = httpd_query_key_value(json, "wifi_gw", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->wifi_ip_info.gw);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read can_bitrate field
  err = httpd_query_key_value(json, "can_bitrate", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    uint32_t num = strtol(arg_buf, NULL, 10);

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
        return ESP_FAIL;
    }
    cnf->can_bitrate = bitrate;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read enable_cyphal field
  err = httpd_query_key_value(shared_post_buf, "enable_cyphal", arg_buf,
                              sizeof(arg_buf));
  if (err == ESP_OK) {
    if (strncasecmp(arg_buf, "true", 4) == 0)
      cnf->enable_cyphal = true;
    else
      cnf->enable_cyphal = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read cyphal_node_id field
  err = httpd_query_key_value(shared_post_buf, "cyphal_node_id", arg_buf,
                              sizeof(arg_buf));
  if (err == ESP_OK) {
    uint32_t num = strtol(arg_buf, NULL, 10);
    if (num > 127) {
      return ESP_FAIL;
    }
    cnf->cyphal_node_id = num;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  return ESP_OK;
}