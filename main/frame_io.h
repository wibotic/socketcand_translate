#pragma once

#include <stddef.h>

#include "esp_err.h"

// Used to read < > frames one at a time.
typedef struct {
  // Buffer for reading socketcand
  char buf[1024];

  // Points to the first character in the buffer.
  // MUST be initialized to zero.
  size_t l;

  // Points to the first uninitialized char in the buffer.
  // MUST be initialized to zero.
  size_t r;

  // Socket file descriptor from which to read.
  int socket_fd;
} frame_io_messenger;

// Fills `buf` with a C string containing the next received < > frame.
// Returns `ESP_ERR_NO_MEM` if the incoming frame is longer than `buflen`.
// Logs an error and returns `ESP_FAIL` on network error.
// The `reader` is no longer valid after an error was returned.
esp_err_t frame_io_read_next_frame(frame_io_messenger *reader, char *buf,
                                   size_t buflen);

// Writes the whole C-string `str` (excluding '\0') over TCP to `fd`.
// On network error, returns `ESP_FAIL`.
esp_err_t frame_io_write_str(int fd, char *str);
