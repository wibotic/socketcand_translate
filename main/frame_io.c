#include "frame_io.h"

#include "esp_log.h"
#include "lwip/sockets.h"

// Name that will be used for logging
static const char *TAG = "frame_io";

int frame_io_read_next_frame(frame_io_messenger *reader, char *buf) {
  int buf_i = 0;

  while (true) {
    // there's still data in the buffer
    if (reader->l < reader->r) {
      // copy a byte to the buffer, and consume
      buf[buf_i] = reader->buf[reader->l];
      buf_i += 1;
      reader->l += 1;

      if (buf_i >= SOCKETCAND_BUF_LEN) {
        // provided buffer is smaller than frame
        return -1;
      }

      // if reached end of frame
      if (buf[buf_i - 1] == '>') {
        buf[buf_i] = '\0';
        ESP_LOGV(TAG, "Received this frame from TCP: '%s'.", buf);
        return buf_i;
      }

      // the buffer is empty, so read more bytes
    } else {
      int len = read(reader->socket_fd, reader->buf, sizeof(buf));
      if (len < 0 && errno != EINTR && errno != EAGAIN &&
          errno != EWOULDBLOCK) {
        ESP_LOGD(TAG, "Socket read failed with errno %d", errno);
        return -1;
      } else if (len == 0) {
        ESP_LOGD(TAG, "TCP EOF.");
        return -1;
      }

      // Increment the right pointer of the buffer, to point to the end of
      // the read data.
      reader->l = 0;
      reader->r = len;
    }
  }
}

int frame_io_write_str(int fd, char *str) {
  int str_len = strlen(str);
  int to_write = str_len;

  while (to_write > 0) {
    int written = write(fd, str + (str_len - to_write), to_write);
    if ((written < 0) && (errno != EINTR) && (errno != EAGAIN) &&
        (errno != EWOULDBLOCK)) {
      ESP_LOGD(TAG, "TCP write failed: errno %d", errno);
      return written;
    }
    to_write -= written;
  }
  ESP_LOGV(TAG, "Wrote this to TCP: '%s'", str);

  return str_len;
}