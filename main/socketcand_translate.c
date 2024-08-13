#include "socketcand_translate.h"

#include "esp_log.h"

// Name that will be used for logging
static const char *TAG = "socketcand_server";

// Mask for 11-bit header identifier of CAN 2.0A
// See: https://en.wikipedia.org/wiki/CAN_bus#Frames
#define CAN_SHORT_ID_MASK 0x000007FFU

// TODO: Figure out how to add some simple unit tests.

esp_err_t socketcand_translate_frame_to_string(
    char *buf, size_t bufsize, const socketcand_translate_frame_t *can_frame,
    uint32_t secs, uint32_t usecs) {
  // Can't write more than 8 bytes to classic CAN payload
  if (can_frame->len > 8) {
    ESP_LOGE(TAG, "Can't write more than 8 bytes in classic CAN payload.");
    return ESP_ERR_NO_MEM;
  }

  int written = 0;

  int res = snprintf(buf + written, bufsize - written, "< frame %lX %ld.%ld ",
                     can_frame->id, secs, usecs);
  written += res;

  if (res < 0 || written >= bufsize) {
    return ESP_ERR_NO_MEM;
  }

  // Convert each byte to hex
  for (int i = 0; i < can_frame->len; i++) {
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
    const char *buf, socketcand_translate_frame_t *msg) {
  // if this frame isn't a send frame
  if (strncmp("< send ", buf, 7 != 0)) {
    ESP_LOGE(TAG, "Invalid syntax in received socketcand frame.");
    return ESP_FAIL;
  }

  int count =
      sscanf(buf, "< send %lx %hhu %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx >",
             &(msg->id), &(msg->len), &msg->data[0], &msg->data[1],
             &msg->data[2], &msg->data[3], &msg->data[4], &msg->data[5],
             &msg->data[6], &msg->data[7]);

  if ((count < 2) || (msg->len > 8) || (count != 2 + msg->len)) {
    ESP_LOGE(TAG, "Invalid syntax in received socketcand frame.");
  }
  if (msg->id > CAN_SHORT_ID_MASK) {
    msg->ext = 1;
  } else {
    msg->ext = 0;
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
