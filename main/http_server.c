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
static esp_err_t serve_get(httpd_req_t *req);

static const httpd_uri_t get_handler = {
    .uri = "/", .handler = serve_get, .method = HTTP_GET, .user_ctx = NULL};

// Handles GET /alpine.js
static esp_err_t serve_get_alpine_js(httpd_req_t *req);

static const httpd_uri_t get_alpine_js_handler = {
    .uri = "/alpine.js",
    .handler = serve_get_alpine_js,
    .method = HTTP_GET,
    .user_ctx = NULL};

// Handles GET /api/config
static esp_err_t serve_get_config_api(httpd_req_t *req);

static const httpd_uri_t get_config_api_handler = {
    .uri = "/api/config",
    .handler = serve_get_config_api,
    .method = HTTP_GET,
    .user_ctx = NULL};

// Handles GET /api/status
static esp_err_t handle_get_status_api(httpd_req_t *req);

static const httpd_uri_t get_status_api_handler = {
    .uri = "/api/status",
    .handler = handle_get_status_api,
    .method = HTTP_GET,
    .user_ctx = NULL};

// Handles POST /api/config
static esp_err_t handle_post_config_api(httpd_req_t *req);

static const httpd_uri_t post_config_api_handler = {
    .uri = "/api/config",
    .handler = handle_post_config_api,
    .method = HTTP_POST,
    .user_ctx = NULL};

// Updates `persistent_settings` from `json`.
// `persistent_settings` must be the current settings,
// and they are changed to updated settings based on `json`.
// On success, restarts the microcontroller.
// On failure, returns an error.
static esp_err_t update_persistent_settings_from_json(
    const char *json, persistent_settings_t *persistent_settings);

esp_err_t start_http_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;
  esp_err_t err;

  err = httpd_start(&server, &config);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't start HTTP server.");

  err = httpd_register_uri_handler(server, &get_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_alpine_js_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_config_api_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &post_config_api_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  err = httpd_register_uri_handler(server, &get_status_api_handler);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register HTTP URI handler.");

  return ESP_OK;
}

static esp_err_t serve_get(httpd_req_t *req) {
  return httpd_resp_send(req, (char *)index_html, index_html_end - index_html);
}

static esp_err_t serve_get_alpine_js(httpd_req_t *req) {
  esp_err_t err = httpd_resp_set_type(req, "text/javascript");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, (char *)alpine_js, alpine_js_end - alpine_js);
}

static esp_err_t serve_get_config_api(httpd_req_t *req) {
  esp_err_t err = httpd_resp_set_type(req, "application/json");
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set response type.");
  return httpd_resp_send(req, persistent_settings_json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_get_status_api(httpd_req_t *req) {
  esp_err_t err;

  const char *status_json = driver_setup_get_status_json();
  if (status_json == NULL) {
    ESP_LOGE(TAG, "Couldn't get current driver status.");
    httpd_resp_send_err(req, 500, "Couldn't get current driver status.");
    err = driver_setup_release_json_status();
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't release JSON status.");
    return ESP_FAIL;
  }

  esp_err_t http_err = httpd_resp_send(req, status_json, HTTPD_RESP_USE_STRLEN);

  err = driver_setup_release_json_status();
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't release JSON status.");

  return http_err;
}

// Stores the body of the request in `handle_post_config_api()`.
static char post_buf[1024];
SemaphoreHandle_t post_buf_mutex = NULL;
StaticSemaphore_t post_buf_mutex_mem;

static esp_err_t handle_post_config_api(httpd_req_t *req) {
  // If POST request is too large
  if (req->content_len >= sizeof(post_buf)) {
    httpd_resp_send_err(req, 500, "POST post_buf too long.");
    ESP_LOGE(TAG, "POST post_buf too long.");
    return ESP_FAIL;
  }

  // Ensure we're the only ones using `post_buf`.
  if (post_buf_mutex == NULL) {
    post_buf_mutex = xSemaphoreCreateMutexStatic(&post_buf_mutex_mem);
  }
  xSemaphoreTake(post_buf_mutex, portMAX_DELAY);

  // Read the post request post_buf
  int ret = httpd_req_recv(req, post_buf, req->content_len);

  // If unexpected post_buf length
  if (ret != req->content_len) {
    httpd_resp_send_err(req, 500, "Couldn't read POST post_buf");
    ESP_LOGE(TAG, "Couldn't read POST post_buf.");
    return ret;
  }

  post_buf[req->content_len] = '\0';

  // Get the updated persistent_settings
  persistent_settings_t new_persistent_settings = *persistent_settings;
  esp_err_t err =
      update_persistent_settings_from_json(post_buf, &new_persistent_settings);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Error parsing POSTed persistent settings JSON: %s",
             esp_err_to_name(err));
    httpd_resp_send_err(req, 400,
                        "Couldn't parse the given settings. Make sure they're "
                        "formatted correctly!");
    xSemaphoreGive(post_buf_mutex);
    return ESP_FAIL;
  }

  //// Save the new configuration

  httpd_resp_send(req, "Updating settings...", HTTPD_RESP_USE_STRLEN);

  err = persistent_settings_save(&new_persistent_settings);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't save persistent settings.");
    xSemaphoreGive(post_buf_mutex);
    return ESP_FAIL;
  }

  xSemaphoreGive(post_buf_mutex);
  return ESP_OK;
}

static esp_err_t update_persistent_settings_from_json(
    const char *json, persistent_settings_t *persistent_settings) {
  // rebind for brevity
  persistent_settings_t *cnf = persistent_settings;

  char arg_buf[64];

  // read eth_use_static field
  esp_err_t err = httpd_query_key_value(post_buf, "eth_use_static", arg_buf,
                                        sizeof(arg_buf));
  if (err == ESP_OK) {
    if (strncasecmp(arg_buf, "true", 4) == 0)
      cnf->eth_use_static = true;
    else
      cnf->eth_use_static = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read eth_ip field
  err = httpd_query_key_value(post_buf, "eth_ip", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->eth_ip_info.ip);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read eth_netmask field
  err =
      httpd_query_key_value(post_buf, "eth_netmask", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->eth_ip_info.netmask);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read eth_gw field
  err = httpd_query_key_value(post_buf, "eth_gw", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->eth_ip_info.gw);
    if (err != ESP_OK) {
      return err;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_enabled field
  err =
      httpd_query_key_value(post_buf, "wifi_enabled", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    if (strncasecmp(arg_buf, "true", 4) == 0)
      cnf->wifi_enabled = true;
    else
      cnf->wifi_enabled = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_ssid field
  err = httpd_query_key_value(post_buf, "wifi_ssid", arg_buf,
                              sizeof(cnf->wifi_ssid));
  if (err == ESP_OK) {
    memcpy(cnf->wifi_ssid, arg_buf, sizeof(cnf->wifi_ssid));
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_pass field
  err = httpd_query_key_value(post_buf, "wifi_pass", arg_buf,
                              sizeof(cnf->wifi_pass));
  if (err == ESP_OK) {
    memcpy(cnf->wifi_pass, arg_buf, sizeof(cnf->wifi_pass));
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_use_static field
  err = httpd_query_key_value(post_buf, "wifi_use_static", arg_buf,
                              sizeof(arg_buf));
  if (err == ESP_OK) {
    if (strncasecmp(arg_buf, "true", 4) == 0)
      cnf->wifi_use_static = true;
    else
      cnf->wifi_use_static = false;
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_ip field
  err = httpd_query_key_value(post_buf, "wifi_ip", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->wifi_ip_info.ip);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_netmask field
  err =
      httpd_query_key_value(post_buf, "wifi_netmask", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->wifi_ip_info.netmask);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read wifi_gw field
  err = httpd_query_key_value(post_buf, "wifi_gw", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    err = esp_netif_str_to_ip4(arg_buf, &cnf->wifi_ip_info.gw);
    if (err != ESP_OK) {
      return ESP_FAIL;
    }
  } else if (err != ESP_ERR_NOT_FOUND) {
    return err;
  }

  // read can_bitrate field
  err =
      httpd_query_key_value(post_buf, "can_bitrate", arg_buf, sizeof(arg_buf));
  if (err == ESP_OK) {
    long num = strtol(arg_buf, NULL, 10);

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

  return ESP_OK;
}