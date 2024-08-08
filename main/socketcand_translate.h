#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A CAN frame
typedef struct {
  uint32_t id;
  uint8_t len;
  uint8_t data[8];
  bool ext;
} socketcand_translate_frame;

// Translates a `socketcand_translate_frame` to a socketcand string of form `<
// frame can_id seconds.useconds [data]* >`. Returns the length of the string
// written to `buf` or -1 if `bufsize` was too small to hold the string. For
// more info, see:
// https://github.com/linux-can/socketcand/blob/master/doc/protocol.md
extern int socketcand_translate_frame_to_string(
    char *buf, size_t bufsize, const socketcand_translate_frame *can_frame,
    uint32_t secs, uint32_t usecs);

// Translates a string of form `< send can_id can_dlc [data]* >` to a
// `socketcand_translate_frame`. Returns 0 on success, and -1 on error. into a
// seperated format contained in the socketcand_translate_frame struct
extern int socketcand_translate_string_to_frame(
    const char *buf, socketcand_translate_frame *msg);

// This function is used to mimic the socketcand protocol for opening a rawmode.
// The buf buffer must have size of at least 12 bytes.
//
// RETURN ARGUMENTS:
//     buf is filled with a socketcand respone of form "< CONTENT >". Send
//     it to the client.
//
// USAGE:
//     When a client connects, call this function with `buf = ""`.
//
//     After each call to this function, send the contents of `buf` to the
//     client, and call this function again with the response.
//
// RETURNS:
//     -1: received unexpected message in buf. Filled buf with an error message.
//     1: Completed step 1 of negotiating a rawmode connection
//     2: Completed step 2 of negotiating a rawmode connection
//     3: Completed final step of negotiating a rawmode connection
extern int socketcand_translate_open_raw(char *buf);
