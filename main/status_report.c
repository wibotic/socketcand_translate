#include "status_report.h"

#include "driver/twai.h"
#include "esp_check.h"
#include "esp_netif_types.h"
#include "freertos/semphr.h"
#include "socketcand_server.h"
#include "string.h"

// Name that will be used for logging
static const char *TAG = "socketcand_server";

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

// Prints the status of socketcand to `buf` in JSON format.
// Returns an error if `buflen` was too small.
// Increments `bytes_written` by the number of bytes written.
static esp_err_t print_socketcand_status(char *buf_out, size_t buflen,
                                         size_t *bytes_written);

static char status_json[2048];
static SemaphoreHandle_t status_json_mutex = NULL;
static StaticSemaphore_t status_json_mutex_mem;

esp_err_t status_report_get(const char **json_out, esp_netif_t *eth_netif,
                            esp_netif_t *wifi_netif) {
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

  err = print_netif_status(eth_netif, status_json + written,
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

  res = print_netif_status(wifi_netif, status_json + written,
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
                 ",\n"
                 "\"Socketcand adapter status\": ");
  written += res;
  if (res < 0 || written >= sizeof(status_json)) {
    ESP_LOGE(TAG, "driver_setup_get_status_json() buflen too small.");
    return ESP_ERR_NO_MEM;
  }

  res = print_socketcand_status(status_json + written,
                                sizeof(status_json) - written, &written);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't print socketcand status.");

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

esp_err_t status_report_release() {
  if (status_json_mutex == NULL) {
    return ESP_FAIL;
  }
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

static esp_err_t print_socketcand_status(char *buf_out, size_t buflen,
                                         size_t *bytes_written) {
  socketcand_server_status_t status;
  esp_err_t err = socketcand_server_status(&status);
  ESP_RETURN_ON_ERROR(err, TAG, "Couldn't get socketcand server status.");

  int written =
      snprintf(buf_out, buflen,
               "{\n"

               "\"Total socketcand frames received from internet\": "
               "\"%lld\",\n"

               "\"Total frames sent to CAN bus\": "
               "\"%lld\",\n"

               "\"Total frames received from CAN bus\": "
               "\"%lld\",\n"

               "\"Total socketcand frames sent to internet\": "
               "\"%lld\",\n"

               "\"Total invalid socketcand frames received from internet\": "
               "\"%lld\"\n"

               "}",
               status.socketcand_frames_received, status.can_bus_frames_sent,
               status.can_bus_frames_received, status.socketcand_frames_sent,
               status.invalid_socketcand_frames_received);

  if (written < 0 || written >= buflen) {
    ESP_LOGE(TAG, "print_netif_status buflen too short.");
    return ESP_ERR_NO_MEM;
  }

  *bytes_written += written;
  return ESP_OK;
}