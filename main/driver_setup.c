#include "driver_setup.h"

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "memory.h"

esp_netif_t *driver_setup_eth_netif = NULL;
esp_netif_t *driver_setup_wifi_netif = NULL;

// Semaphore that gets set by an event handler
// when the internet driver is fully running
static SemaphoreHandle_t internet_ready = NULL;
static StaticSemaphore_t internet_ready_mem;

// Name that will be used for logging
static const char *TAG = "driver_setup";

// Ethernet event handler. Sets `internet_ready` when the driver has
// been started.
static void ethernet_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data);

// Ethernet event handler. Sets `internet_ready` when the driver has
// been started.
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

// Logs information whenever acquired IP.
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);

// A FreeRTOS task that gets spawned by `driver_setup_can()`.
// It tries reconnecting to Wi-Fi every 30 seconds if
// Wi-Fi is disconnected.
static void wifi_recovery_task(void *pvParameters);
static StackType_t wifi_recovery_task_stack[4096];
static StaticTask_t wifi_recovery_task_mem;

// A FreeRTOS task that gets spawned by `driver_setup_can()`.
// It initiates CAN recovery mode whenever
// the bus is disconnected due to excessive error count.
static void can_recovery_task(void *pvParameters);
static StackType_t can_recovery_task_stack[4096];
static StaticTask_t can_recovery_task_mem;

// Prints the status of `netif` to `buf` in JSON format.
// Returns an error if `buflen` was too small.
// Increments `bytes_written` by the number of bytes written.
static esp_err_t print_netif_status(esp_netif_t *netif, char *buf_out,
                                    size_t buflen, size_t *bytes_written);

// Prints the status of the CAN bus to `buf` in JSON format.
// Returns an error if `buflen` was too small.
// Increments `bytes_written` by the number of bytes written.
static esp_err_t print_can_status(char *buf_out, size_t buflen,
                                  size_t *bytes_written);

esp_err_t driver_setup_ethernet(const esp_netif_ip_info_t *ip_info) {
  esp_err_t err;
  // Based on:
  // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_eth.html

  // Check if the ethernet driver has already been started
  if (driver_setup_eth_netif != NULL) {
    ESP_LOGE(TAG, "driver_setup_eth_netif is already initialized.");
    return ESP_FAIL;
  }

  //// Create an ethernet MAC object ////
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  // 23 nased on ESP32-EVB schematic
  esp32_emac_config.smi_mdc_gpio_num = 23;
  // 18 based on ESP32-EVB schematic
  esp32_emac_config.smi_mdio_gpio_num = 18;
  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
  if (mac == NULL) {
    ESP_LOGE(TAG, "Couldn't create Ethernet MAC object.");
    return ESP_FAIL;
  }

  //// Create an ethernet PHY object ////
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  // ESP32-EVB schematic says SMI address is 0x00
  phy_config.phy_addr = 0;
  // ESP32-EVB schematic says PHY reset is unconnected
  phy_config.reset_gpio_num = -1;
  esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
  if (phy == NULL) {
    ESP_LOGE(TAG, "Couldn't create Ethernet PHY object.");
    return ESP_FAIL;
  }

  //// Get an eth_handle by installing the driver ////
  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t eth_handle = NULL;
  err = esp_eth_driver_install(&eth_config, &eth_handle);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't install ethernet driver.");

  //// Create netif object ////
  esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t *esp_netif = esp_netif_new(&netif_config);
  if (esp_netif == NULL) {
    ESP_LOGE(TAG, "Couldn't create Ethernet ESP-NETIF object.");
    return ESP_FAIL;
  }

  //// Set static IP info if needed ////
  if (ip_info != NULL) {
    err = esp_netif_dhcpc_stop(esp_netif);
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't stop DHCPC to set up static IP.");
    err = esp_netif_set_ip_info(esp_netif, ip_info);
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set ethernet IP info.");
  }

  internet_ready = xSemaphoreCreateBinaryStatic(&internet_ready_mem);

  // Register event handlers for debugging purposes
  err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                   &ethernet_event_handler, NULL);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register ethernet event handler.");

  err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                   &ip_event_handler, NULL);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register IP event handler.");

  //// Attach the ethernet object to ESP netif ////
  esp_eth_netif_glue_handle_t eth_netif_glue =
      esp_eth_new_netif_glue(eth_handle);

  err = esp_netif_attach(esp_netif, eth_netif_glue);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't attach ethernet to ESP netif.");

  //// Start ethernet ////
  err = esp_eth_start(eth_handle);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't start ethernet.");

  /// Wait for internet to actually work ////
  if (xSemaphoreTake(internet_ready, pdMS_TO_TICKS(10000)) == pdFALSE) {
    ESP_LOGE(TAG, "Couldn't start ethernet driver in 10 seconds.");
    return ESP_FAIL;
  }
  vSemaphoreDelete(internet_ready);

  driver_setup_eth_netif = esp_netif;
  return ESP_OK;
}

esp_err_t driver_setup_wifi(const esp_netif_ip_info_t *ip_info,
                            const char ssid[32], const char password[64]) {
  esp_err_t err;
  // Check if the ethernet driver has already been started
  if (driver_setup_wifi_netif != NULL) {
    ESP_LOGE(TAG, "Can only call driver_setup_wifi() one time.");
    return ESP_FAIL;
  }

  //// Create netif object ////
  esp_netif_t *esp_netif = esp_netif_create_default_wifi_sta();
  if (esp_netif == NULL) {
    ESP_LOGE(TAG, "Couldn't create WIFI ESP-NETIF object.");
    return ESP_FAIL;
  }

  //// Set static IP info if needed ////
  if (ip_info != NULL) {
    err = esp_netif_dhcpc_stop(esp_netif);
    ESP_RETURN_ON_ERROR(err, TAG,
                        "Couldn't stop WIFI dhcp to set up static IP.");
    err = esp_netif_set_ip_info(esp_netif, ip_info);
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set IP info for WIFI.");
  }

  //// Initialize the wifi driver ////
  wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&wifi_init_config);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't initialize WIFI");

  /// Configure the wifi driver ////
  wifi_config_t wifi_config = {0};
  memcpy(wifi_config.sta.ssid, ssid, 32);
  memcpy(wifi_config.sta.password, password, 64);
  wifi_config.sta.failure_retry_cnt = 3;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't set WIFI to station mode.");
  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't configure WIFI.");

  internet_ready = xSemaphoreCreateBinaryStatic(&internet_ready_mem);

  // Register event handlers for debugging purposes
  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, NULL);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register WIFI handler.");
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &ip_event_handler, NULL);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't register IP handler.");

  // Tell the driver to try connecting to WIFI.
  // Note: This will NOT return an error if WIFI can't connect.
  // All reconnection logic is instead handled by
  // `wifi_event_handler()`. TODO
  err = esp_wifi_start();
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't start WIFI.");
  err = esp_wifi_connect();
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't connect to WIFI.");

  //// Wait for internet to actually work ////
  if (xSemaphoreTake(internet_ready, pdMS_TO_TICKS(10000)) == pdFALSE) {
    ESP_LOGE(TAG, "Couldn't start WIFI driver in 10 seconds.");
    return ESP_FAIL;
  }
  vSemaphoreDelete(internet_ready);

  // Spawn a task that will reconnect to Wi-Fi if it disconnects.
  xTaskCreateStatic(wifi_recovery_task, "wifi_recovery",
                    sizeof(wifi_recovery_task_stack), NULL, 7,
                    wifi_recovery_task_stack, &wifi_recovery_task_mem);

  driver_setup_wifi_netif = esp_netif;
  return ESP_OK;
}

esp_err_t driver_setup_can(const twai_timing_config_t *timing_config) {
  esp_err_t err;

  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_5, GPIO_NUM_35, TWAI_MODE_NORMAL);
  // Change this line to enable/disable logging:
  g_config.alerts_enabled = TWAI_ALERT_AND_LOG | TWAI_ALERT_ABOVE_ERR_WARN |
                            TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;
  g_config.tx_queue_len = 32;
  g_config.rx_queue_len = 32;
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  // Install TWAI driver
  err = twai_driver_install(&g_config, timing_config, &f_config);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't install CAN driver.");

  // Start TWAI driver
  err = twai_start();
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't start CAN driver.");

  // Spawn a task that will put CAN in recovery mode
  // whenever it enters BUS_OFF state.
  xTaskCreateStatic(can_recovery_task, "can_recovery",
                    sizeof(can_recovery_task_stack), NULL, 7,
                    can_recovery_task_stack, &can_recovery_task_mem);

  return ESP_OK;
}

static void ethernet_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data) {
  const esp_eth_handle_t *eth_handle = (esp_eth_handle_t *)event_data;
  switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
      uint8_t mac_addr[6] = {0};
      esp_eth_ioctl(*eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
      ESP_LOGD(TAG, "Ethernet Connected");
      ESP_LOGD(TAG, "Ethernet HW Addr %2x:%2x:%2x:%2x:%2x:%2x", mac_addr[0],
               mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      ESP_LOGE(TAG, "Ethernet disconnected");
      break;
    case ETHERNET_EVENT_START:
      ESP_LOGD(TAG, "Ethernet Started");
      xSemaphoreGive(internet_ready);
      break;
    case ETHERNET_EVENT_STOP:
      ESP_LOGE(TAG, "Ethernet Stopped");
      break;
    default:
      ESP_LOGE(TAG, "Unrecognized ethernet event.");
      break;
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  switch (event_id) {
    case WIFI_EVENT_STA_START:
      ESP_LOGD(TAG, "WIFI station started.");
      xSemaphoreGive(internet_ready);
      break;
    case WIFI_EVENT_STA_CONNECTED:
      ESP_LOGD(TAG, "WIFI station connected.");
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      ESP_LOGE(TAG, "WIFI station disconnected.");
      break;
    default:
      break;
  }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
  const ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  const esp_netif_ip_info_t *ip_info = &event->ip_info;
  ESP_LOGI(TAG, "----- Got IP Address -----");

  char hostname[8];
  esp_err_t err = esp_netif_get_netif_impl_name(event->esp_netif, hostname);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Name: %s", hostname);
  } else {
    ESP_LOGE(TAG, "Couldn't get ESP impl name: %s", esp_err_to_name(err));
  }

  ESP_LOGI(TAG, "IP:       " IPSTR, IP2STR(&ip_info->ip));
  ESP_LOGI(TAG, "Net mask: " IPSTR, IP2STR(&ip_info->netmask));
  ESP_LOGI(TAG, "Gateway:  " IPSTR, IP2STR(&ip_info->gw));
  ESP_LOGI(TAG, "--------------------------");
}

static void wifi_recovery_task(void *pvParameters) {
  // Constantly re-connect to Wi-Fi if needed.
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!esp_netif_is_netif_up(driver_setup_wifi_netif)) {
      ESP_LOGE(TAG, "Retrying connecting to Wi-Fi.");
      esp_err_t err = esp_wifi_connect();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Couldn't start connection attempt: %s",
                 esp_err_to_name(err));
      }
      vTaskDelay(pdMS_TO_TICKS(28000));
    }
  }
}

static void can_recovery_task(void *pvParameters) {
  esp_err_t err;
  // Constantly initiate recovery if needed.
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    // Read TWAI alerts
    uint32_t alerts;
    while (true) {
      err = twai_read_alerts(&alerts, portMAX_DELAY);
      if (err == ESP_OK) {
        break;
      } else {
        ESP_LOGE(TAG, "Couldn't read CAN alerts: %s", esp_err_to_name(err));
      }
    }
    // If the bus is off, enter recovery mode.
    if ((alerts & TWAI_ALERT_BUS_OFF) != 0) {
      err = twai_initiate_recovery();
      if (err == ESP_OK) {
        ESP_LOGE(TAG, "Initiated CAN recovery.");
      } else {
        ESP_LOGE(TAG, "Couldn't initiate CAN recovery: %s",
                 esp_err_to_name(err));
      }
    }
  }
}

static char status_json[2048];
static SemaphoreHandle_t status_json_mutex = NULL;
static StaticSemaphore_t status_json_mutex_mem;

esp_err_t driver_setup_get_status_json(const char **json_out) {
  if (status_json_mutex == NULL) {
    status_json_mutex = xSemaphoreCreateMutexStatic(&status_json_mutex_mem);
  }
  xSemaphoreTake(status_json_mutex, portMAX_DELAY);

  esp_err_t err;
  int res;
  size_t written = 0;

  res = snprintf(status_json + written, sizeof(status_json) - written,
                 "{\n"
                 "\"Ethernet status\": ");
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return ESP_ERR_NO_MEM;
  }

  err = print_netif_status(driver_setup_eth_netif, status_json + written,
                           sizeof(status_json) - written, &written);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't print ethernet status.");

  res = snprintf(status_json + written, sizeof(status_json) - written,
                 ",\n"
                 "\"Wi-Fi status\": ");
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return ESP_ERR_NO_MEM;
  }

  res = print_netif_status(driver_setup_wifi_netif, status_json + written,
                           sizeof(status_json) - written, &written);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't print Wi-Fi netif status.");

  res = snprintf(status_json + written, sizeof(status_json) - written,
                 ",\n"
                 "\"CAN Bus status\": ");
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return ESP_ERR_NO_MEM;
  }

  res = print_can_status(status_json + written, sizeof(status_json) - written,
                         &written);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't print CAN bus status.");

  res = snprintf(status_json + written, sizeof(status_json) - written,
                 "\n"
                 "}\n");
  written += res;

  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return ESP_ERR_NO_MEM;
  }

  *json_out = status_json;
  return ESP_OK;
}

esp_err_t driver_setup_release_json_status() {
  if (xSemaphoreGive(status_json_mutex) != pdTRUE) {
    ESP_LOGE(TAG, "Error releasing driver setup JSON status mutex.");
    return ESP_FAIL;
  } else {
    return ESP_OK;
  }
}

static esp_err_t print_netif_status(esp_netif_t *netif, char *buf_out,
                                    size_t buflen, size_t *bytes_written) {
  int written;
  esp_err_t err;

  // null if netif is null
  if (netif == NULL) {
    written = snprintf(buf_out, buflen, "\"Disabled\"");
  } else {
    bool is_up = esp_netif_is_netif_up(netif);

    uint8_t mac_address[6];
    err = esp_netif_get_mac(netif, mac_address);
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't get netif MAC address.");

    esp_netif_dhcp_status_t dhcp_status_code;
    err = esp_netif_dhcpc_get_status(netif, &dhcp_status_code);
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't get netif DHCP status.");
    char dhcp_status[32];
    switch (dhcp_status_code) {
      case ESP_NETIF_DHCP_INIT:
        strcpy(dhcp_status, "not yet started");
        break;
      case ESP_NETIF_DHCP_STARTED:
        strcpy(dhcp_status, "started");
        break;
      case ESP_NETIF_DHCP_STOPPED:
        strcpy(dhcp_status, "stopped");
        break;
      case ESP_NETIF_DHCP_STATUS_MAX:
        strcpy(dhcp_status, "max");
        break;
      default:
        strcpy(dhcp_status, "UNDEFINED");
        break;
    }

    esp_netif_ip_info_t ip_info;
    err = esp_netif_get_ip_info(netif, &ip_info);
    ESP_RETURN_ON_ERROR(err, TAG, "Couldn't get netif IP info.");

    const char *description = esp_netif_get_desc(netif);

    written =
        snprintf(buf_out, buflen,
                 "{\n"
                 "\"Is up?\": "
                 "%s,\n"

                 "\"MAC Address\": "
                 "\"%02x:%02x:%02x:%02x:%02x:%02x\",\n"

                 "\"DHCP Status\": "
                 "\"%s\",\n"

                 "\"IP\": "
                 "\"" IPSTR
                 "\",\n"

                 "\"Network Mask\": "
                 "\"" IPSTR
                 "\",\n"

                 "\"Gateway\": "
                 "\"" IPSTR
                 "\",\n"

                 "\"Type\": "
                 "\"%s\"\n"

                 "}",
                 is_up ? "true" : "false", mac_address[0], mac_address[1],
                 mac_address[2], mac_address[3], mac_address[4], mac_address[5],
                 dhcp_status, IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask),
                 IP2STR(&ip_info.gw), description);
  }

  if (written < 0 || written >= buflen) {
    ESP_LOGE(TAG, "print_netif_status buflen too short.");
    return ESP_ERR_NO_MEM;
  }

  *bytes_written += written;
  return ESP_OK;
}

static esp_err_t print_can_status(char *buf_out, size_t buflen,
                                  size_t *bytes_written) {
  twai_status_info_t can_status;
  esp_err_t err = twai_get_status_info(&can_status);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't get CAN bus info.");

  char can_state[64];
  switch (can_status.state) {
    case TWAI_STATE_STOPPED:
      strcpy(can_state, "stopped");
      break;
    case TWAI_STATE_RUNNING:
      strcpy(can_state, "running");
      break;
    case TWAI_STATE_BUS_OFF:
      strcpy(can_state, "bus off due to exceeded error count");
      break;
    case TWAI_STATE_RECOVERING:
      strcpy(can_state, "recovering");
      break;
    default:
      strcpy(can_state, "UNDEFINED");
      break;
  }

  int written = snprintf(
      buf_out, buflen,
      "{\n"
      "\"State\": "
      "\"%s\",\n"

      "\"Total number of messages queued for transmission\": "
      "%ld,\n"

      "\"Total number of messages waiting in receive queue\": "
      "%ld,\n"

      "\"Transmit error counter\": "
      "%ld,\n"

      "\"Receive error counter\": "
      "%ld,\n"

      "\"Total number of failed message transmissions\": "
      "%ld,\n"

      "\"Total number of failed message receptions\": "
      "%ld,\n"

      "\"Total number of incoming messages lost due to FIFO overrun\": "
      "%ld,\n"

      "\"Total number of lost arbitrations\": "
      "%ld,\n"

      "\"Total number of bus errors\": "
      "%ld\n"

      "}",
      can_state, can_status.msgs_to_tx, can_status.msgs_to_rx,
      can_status.tx_error_counter, can_status.rx_error_counter,
      can_status.tx_failed_count, can_status.rx_missed_count,
      can_status.rx_overrun_count, can_status.arb_lost_count,
      can_status.bus_error_count);

  if (written < 0 || written >= buflen) {
    ESP_LOGE(TAG, "print_netif_status buflen too short.");
    return ESP_ERR_NO_MEM;
  }

  *bytes_written += written;
  return ESP_OK;
}