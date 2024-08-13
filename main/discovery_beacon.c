#include "discovery_beacon.h"

#include "driver_setup.h"
#include "esp_log.h"
#include "lwip/sockets.h"

// Name that will be used for logging
static const char* TAG = "discovery_beacon";

// Task that broadcasts the beacon.
static void discovery_beacon_task(void* pvParameters);
static StackType_t task_stack[4096];
static StaticTask_t task_mem;

// Buffer that `discovery_beacon_task` uses to form
// a CANBeacon message.
static char msg_buf[1024];

esp_err_t discovery_beacon_start() {
  // create a UDP socket
  int server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (server_sock < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    return ESP_FAIL;
  }

  // Enable broadcast functionality
  int broadcastEnable = 1;
  int err = setsockopt(server_sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable,
                       sizeof(broadcastEnable));
  if (err != 0) {
    ESP_LOGE(TAG, "Unable to enable UDP broadcast on socket: %d", errno);
    close(server_sock);
    return ESP_FAIL;
  }

  xTaskCreateStatic(discovery_beacon_task, "discovery_beacon",
                    sizeof(task_stack), (void*)server_sock, 2, task_stack,
                    &task_mem);
  return ESP_OK;
}

static void discovery_beacon_task(void* pvParameters) {
  int server_sock = (int)pvParameters;

  // Set up the broadcast address
  struct sockaddr_in broadcast_addr = {0};
  broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(42000);

  // Run the server
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    int bytes_printed = 0;
    int res = snprintf(msg_buf + bytes_printed, sizeof(msg_buf) - bytes_printed,
                       "<CANBeacon name='ESP32-socketcand' type='adapter' "
                       "description='ESP32-EVB socketcand adapter'>\n");
    bytes_printed += res;
    if (res < 0 || bytes_printed >= sizeof(msg_buf)) {
      ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message.");
      continue;
    }

    if (driver_setup_eth_netif != NULL &&
        esp_netif_is_netif_up(driver_setup_eth_netif)) {
      esp_netif_ip_info_t ip_info;
      ESP_ERROR_CHECK(esp_netif_get_ip_info(driver_setup_eth_netif, &ip_info));

      res =
          snprintf(msg_buf + bytes_printed, sizeof(msg_buf) - bytes_printed,
                   "<URL>can://%d.%d.%d.%d:9999</URL>\n", IP2STR(&ip_info.ip));
      bytes_printed += res;
      if (res < 0 || bytes_printed >= sizeof(msg_buf)) {
        ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message.");
        continue;
      }
    }

    if (driver_setup_wifi_netif != NULL &&
        esp_netif_is_netif_up(driver_setup_wifi_netif)) {
      esp_netif_ip_info_t ip_info;
      ESP_ERROR_CHECK(esp_netif_get_ip_info(driver_setup_wifi_netif, &ip_info));

      res =
          snprintf(msg_buf + bytes_printed, sizeof(msg_buf) - bytes_printed,
                   "<URL>can://%d.%d.%d.%d:9999</URL>\n", IP2STR(&ip_info.ip));
      bytes_printed += res;
      if (res < 0 || bytes_printed >= sizeof(msg_buf)) {
        ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message.");
        continue;
      }
    }

    res = snprintf(msg_buf + bytes_printed, sizeof(msg_buf) - bytes_printed,
                   "<Bus name='can0'/>\n"
                   "</CANBeacon>\n");
    bytes_printed += res;
    if (res < 0 || bytes_printed >= sizeof(msg_buf)) {
      ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message.");
      continue;
    }

    res = sendto(server_sock, msg_buf, bytes_printed, 0,
                 (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));

    if (res < 0) {
      ESP_LOGE(TAG, "Couldn't send UDP broadcast packet: errno %d", errno);
    }
  }
}