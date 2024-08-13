#include "frame_io.h"

#include "esp_log.h"
#include "lwip/sockets.h"

// Name that will be used for logging
static const char *TAG = "frame_io";

esp_err_t frame_io_read_next_frame(frame_io_messenger *reader, char *buf,
                                   size_t buflen) {
  // index into `buf` where we will put the next character
  int buf_i = 0;

  while (true) {
    // if there's still data in the buffer
    if (reader->l < reader->r) {
      if (buf_i + 1 >= buflen) {
        // provided buffer is smaller than frame
        ESP_LOGE(TAG, "Buffer too small to read full socketcand frame.");
        return ESP_ERR_NO_MEM;
      }

      // Verify that the frame actually starts with "<"
      if (buf_i == 0 && reader->buf[reader->l] != '<') {
        ESP_LOGE(TAG,
                 "Excpected next socketcand frame but received character '%c'.",
                 reader->buf[reader->l]);
        return ESP_FAIL;
      }

      // copy a byte to the buffer, and consume
      buf[buf_i] = reader->buf[reader->l];
      buf_i += 1;
      reader->l += 1;

      // if reached end of frame
      if (buf[buf_i - 1] == '>') {
        buf[buf_i] = '\0';
        ESP_LOGV(TAG, "Received this frame from TCP: '%s'.", buf);
        return ESP_OK;
      }

      // the buffer is empty, so read more bytes
    } else {
      int bytes_read = read(reader->socket_fd, reader->buf, sizeof(buf));

      if (bytes_read < 0 && errno != EINTR && errno != EAGAIN &&
          errno != EWOULDBLOCK) {
        // network error
        ESP_LOGE(TAG, "TCP < > frame read failed with errno %d", errno);
        return ESP_FAIL;

      } else if (bytes_read == 0) {
        // network end-of-file
        ESP_LOGD(TAG, "TCP EOF.");
        return ESP_FAIL;
      }
      // Increment the right pointer of the buffer, to point to the end of
      // the read data.
      reader->l = 0;
      reader->r = bytes_read;
    }
  }
}

esp_err_t frame_io_write_str(int fd, char *str) {
  size_t str_len = strlen(str);
  size_t to_write = str_len;

  while (to_write > 0) {
    int written = write(fd, str + (str_len - to_write), to_write);
    if ((written < 0) && (errno != EINTR) && (errno != EAGAIN) &&
        (errno != EWOULDBLOCK)) {
      ESP_LOGD(TAG, "TCP write failed: errno %d", errno);
      return ESP_FAIL;
    }
    to_write -= written;
  }
  return ESP_OK;
}