#include "socketcand_translate.h"

#include "esp_log.h"

// Name that will be used for logging
static const char *TAG = "socketcand_server";

// Mask for 11-bit header identifier of CAN 2.0A
// See: https://en.wikipedia.org/wiki/CAN_bus#Frames
#define CAN_SHORT_ID_MASK 0x000007FFU

esp_err_t socketcand_translate_frame_to_string(
    char *buf, size_t bufsize, const twai_message_t *can_frame,
    uint32_t secs, uint32_t usecs) {
  // Can't write more than 8 bytes to classic CAN payload
  if (can_frame->data_length_code > 8) {
    ESP_LOGE(TAG, "Can't write more than 8 bytes in classic CAN payload.");
    return ESP_ERR_NO_MEM;
  }

  int written = 0;

  int res = snprintf(buf + written, bufsize - written, "< frame %lX %ld.%ld ",
                     can_frame->identifier, secs, usecs);
  written += res;

  if (res < 0 || written >= bufsize) {
    return ESP_ERR_NO_MEM;
  }

  // Convert each byte to hex
  for (int i = 0; i < can_frame->data_length_code; i++) {
    res =
        snprintf(buf + written, bufsize - written, "%02X", can_frame->data[i]);
    written += res;
    if (res < 0 || written >= bufsize) {
      return ESP_ERR_NO_MEM;
    }
  }

  // add a closing '>'
  res = snprintf(buf + written, bufsize - written, " >");
  written += res;
  if (res < 0 || written >= bufsize) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t socketcand_translate_string_to_frame(
    const char *buf, twai_message_t *msg) {
  
  // Set unused fields to zero
  msg->rtr = 0;
  msg->ss = 0;
  msg->self = 0;
  msg->dlc_non_comp = 0;
  msg->reserved = 0;

  // if this frame isn't a send frame
  if (strncmp("< send ", buf, 7 != 0)) {
    ESP_LOGE(TAG, "Invalid syntax in received socketcand frame.");
    return ESP_FAIL;
  }

  int count =
      sscanf(buf, "< send %lx %hhu %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx >",
             &(msg->identifier), &(msg->data_length_code), &msg->data[0], &msg->data[1],
             &msg->data[2], &msg->data[3], &msg->data[4], &msg->data[5],
             &msg->data[6], &msg->data[7]);

  // Validate the frame syntax.
  if ((count < 2) || (msg->data_length_code > 8) || (count != 2 + msg->data_length_code)) {
    ESP_LOGE(TAG, "Invalid syntax in received socketcand frame.");
    return ESP_FAIL;
  }

  if (msg->identifier > CAN_SHORT_ID_MASK) {
    msg->extd = 1;
  } else {
    msg->extd = 0;
  }

  return ESP_OK;
}

int32_t socketcand_translate_open_raw(char *buf, size_t bufsize) {
  if (bufsize < 12) {
    // buf is too small
    return -1;

  } else if (buf[0] == '\0') {
    // "" buf indicates a new connection
    // let's send hi
    snprintf(buf, 12, "< hi >");
    return 1;

  } else if (strncmp("< open ", buf, 7) == 0) {
    snprintf(buf, 12, "< ok >");
    return 2;

  } else if (strncmp("< rawmode >", buf, 11) == 0) {
    snprintf(buf, 12, "< ok >");
    return 3;
  }

  // buf didn't match any of the above patterns.
  // return an error
  snprintf(buf, 12, "< error >");
  return 0;
}
