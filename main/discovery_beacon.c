#include "discovery_beacon.h"

#include "driver_setup.h"
#include "esp_log.h"
#include "lwip/sockets.h"
// #include "esp_netif.h"

// TODO: Finish this up!!

// Name that will be used for logging
static const char* TAG = "discovery_beacon";

// Static memory for the discovery_beacon_task stack.
static StackType_t task_stack[4096];

// Static memory for the discovery_beacon_task.
static StaticTask_t task_mem;

static void discovery_beacon_task(void* pvParameters);

int discovery_beacon_start() {
  // create a UDP socket
  int server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (server_sock < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    return -1;
  }

  // Enable broadcast functionality
  int broadcastEnable = 1;
  int err = setsockopt(server_sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable,
                       sizeof(broadcastEnable));
  if (err != 0) {
    ESP_LOGE(TAG, "Unable to enable UDP broadcast on socket: %d", errno);
    close(server_sock);
    return -1;
  }

  xTaskCreateStatic(discovery_beacon_task, "discovery_beacon",
                    sizeof(task_stack), (void*)server_sock, 0, task_stack,
                    &task_mem);
  return 0;
}

static void discovery_beacon_task(void* pvParameters) {
  int server_sock = (int)pvParameters;

  // Set up the broadcast address
  struct sockaddr_in broadcast_addr = {0};
  broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(42000);

  char msg_buf[1024];

  // Run the server
  while (true) {
    int bytes_printed = 0;
    int res = snprintf(msg_buf + bytes_printed, sizeof(msg_buf) - bytes_printed,
                       "<CANBeacon name='ESP32-socketcand' type='adapter' "
                       "description='ESP32-EVB socketcand adapter'>\n");
    bytes_printed += res;
    if (res < 0 || bytes_printed >= sizeof(msg_buf)) {
      ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message. Aborting.");
      abort();
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
        ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message. Aborting.");
        abort();
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
        ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message. Aborting.");
        abort();
      }
    }

    res = snprintf(msg_buf + bytes_printed, sizeof(msg_buf) - bytes_printed,
                   "<Bus name='can0'/>\n"
                   "</CANBeacon>\n");
    bytes_printed += res;
    if (res < 0 || bytes_printed >= sizeof(msg_buf)) {
      ESP_LOGE(TAG, "Couldn't snprintf CAN beacon message. Aborting.");
      abort();
    }

    msg_buf[bytes_printed] = '\0';

    res = sendto(server_sock, msg_buf, bytes_printed, 0,
                 (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));

    if (res < 0) {
      ESP_LOGE(TAG, "Couldn't send UDP broadcast packet: errno %d", errno);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}