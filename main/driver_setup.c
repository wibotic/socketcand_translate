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
// It initiates CAN recovery mode whenever
// the bus is disconnected due to excessive error count.
static void can_recovery_task(void *pvParameters);
static StackType_t can_recovery_task_stack[4096];
static StaticTask_t can_recovery_task_mem;

// Prints the status of `netif` to `buf` in JSON format.
// Returns the number of bytes written, or -1 if ran out of space.
static int print_netif_status(esp_netif_t *netif, char *buf_out, size_t buflen);

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
  wifi_config.sta.failure_retry_cnt = 2;

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

  // Connect to wifi
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
                    sizeof(can_recovery_task_stack), NULL, 15,
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

static void can_recovery_task(void *pvParameters) {
  esp_err_t err;
  // Constantly initiate recovery if needed.
  while (true) {
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

static char status_json[1024];
static SemaphoreHandle_t status_json_mutex = NULL;
static StaticSemaphore_t status_json_mutex_mem;

const char *driver_setup_get_status_json(void) {
  if (status_json_mutex == NULL) {
    status_json_mutex = xSemaphoreCreateMutexStatic(&status_json_mutex_mem);
  }
  xSemaphoreTake(status_json_mutex, portMAX_DELAY);

  int res;
  int written = 0;

  res = snprintf(status_json + written, sizeof(status_json) - written,
                 "{\n"
                 "\"ethernet\": ");
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return NULL;
  }

  res = print_netif_status(driver_setup_eth_netif, status_json + written,
                           sizeof(status_json) - written);
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return NULL;
  }

  res = snprintf(status_json + written, sizeof(status_json) - written,
                 ",\n"
                 "\"wifi\": ");
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return NULL;
  }

  res = print_netif_status(driver_setup_wifi_netif, status_json + written,
                           sizeof(status_json) - written);
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return NULL;
  }

  res = snprintf(status_json + written, sizeof(status_json) - written,
                 "\n"
                 "}\n");
  written += res;

  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return NULL;
  }

  return status_json;
}

esp_err_t driver_setup_release_json_status() {
  if (xSemaphoreGive(status_json_mutex) != pdTRUE) {
    ESP_LOGE(TAG, "Error realising driver setup JSON status mutex.");
    return ESP_FAIL;
  } else {
    return ESP_OK;
  }
}

static int print_netif_status(esp_netif_t *netif, char *buf_out,
                              size_t buflen) {
  int written = 0;
  esp_err_t err;

  // null if netif if is null
  if (netif == NULL) {
    written = snprintf(buf_out + written, buflen - written, "null\n");
  } else {
    bool is_up = esp_netif_is_netif_up(netif);

    uint8_t mac_address[6];
    err = esp_netif_get_mac(netif, mac_address);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't get MAC address: %s", esp_err_to_name(err));
      return -1;
    }

    esp_netif_dhcp_status_t dhcp_status_code;
    err = esp_netif_dhcpc_get_status(netif, &dhcp_status_code);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't get DHCP status: %s", esp_err_to_name(err));
      return -1;
    }
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
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't get IP info: %s", esp_err_to_name(err));
      return -1;
    }

    const char *description = esp_netif_get_desc(netif);

    written =
        snprintf(buf_out, buflen,
                 "{\n"
                 "\"is_up\": "
                 "%s,\n"

                 "\"mac_address\": "
                 "\"%02x:%02x:%02x:%02x:%02x:%02x\",\n"

                 "\"dhcp_status\": "
                 "\"%s\",\n"

                 "\"ip\": "
                 "\"" IPSTR
                 "\",\n"

                 "\"netmask\": "
                 "\"" IPSTR
                 "\",\n"

                 "\"gw\": "
                 "\"" IPSTR
                 "\",\n"

                 "\"type\": "
                 "\"%s\"\n"

                 "}",
                 is_up ? "true" : "false", mac_address[0], mac_address[1],
                 mac_address[2], mac_address[3], mac_address[4], mac_address[5],
                 dhcp_status, IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask),
                 IP2STR(&ip_info.gw), description);
  }

  if (written < 0 || written >= buflen) {
    ESP_LOGE(TAG, "print_netif_status buflen too short.");
    return -1;  // buf is too small
  }
  return written;
}
