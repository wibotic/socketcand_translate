#pragma once

#include <stddef.h>
#include <strings.h>

// A buffer of this size is large
// enough to hold all socketcand < >
// strings.
#define SOCKETCAND_BUF_LEN 256

// Used to read < > frames one at a time.
typedef struct {
  // Buffer for reading socketcand
  char buf[SOCKETCAND_BUF_LEN];

  // Points to the first character in the buffer.
  // MUST always be initialized to zero.
  size_t l;

  // Points to the first uninitialized char in the buffer.
  // MUST be initialized to zero.
  size_t r;

  // Socket file descriptor from which to read
  int socket_fd;
} frame_io_messenger;

// Fills `buf` with a C string containing the next received < > frame.
// `buf` must be a byte array of length `SOCKETCAND_BUF_LEN`.
// Returns the length (excluding '\0') of the string written to `buf` if
// successful. Returns -1 if the message won't fit in `SOCKETCAND_BUF_LEN`.
// Returns -1 if there is a connection error.
int frame_io_read_next_frame(frame_io_messenger *reader, char *buf);

// Writes the whole string (excluding '\0').
// If succeeded writing the whole string, returns the number of bytes written.
// Otherwise returns -1 and sets errno.
int frame_io_write_str(int fd, char *str);
