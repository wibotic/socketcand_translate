#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/twai.h"

#include "esp_err.h"

// Longest socketcand frames that get used during rawmode:
// '< send XXXXXXXX l xx xx xx xx xx xx xx xx >'
// '< frame XXXXXXXX 1000000.1000000 XXXXXXXXXXXXXXXX >'
// The one above is 51 bytes long. Just in case I made
// a calculation error, let's round up to 64.

// A buffer of this size should be large enough
// enough to hold all socketcand `send` and `frame` message strings.
#define SOCKETCAND_RAW_MAX_LEN 64

// Translates a `socketcand_translate_frame_t` to a socketcand string of form `<
// frame can_id seconds.useconds [data]* >`.
// Returns `ESP_ERR_NO_MEM` if `bufsize` is too small too fit the string.
// https://github.com/linux-can/socketcand/blob/master/doc/protocol.md
esp_err_t socketcand_translate_frame_to_string(
    char *buf, size_t bufsize, const twai_message_t *can_frame,
    uint32_t secs, uint32_t usecs);

// Translates a null-terminated
// string of form `< send can_id can_dlc [data]* >` to a
// `socketcand_translate_frame_t`.
// Returns `ESP_FAIL` if `buf` has an invalid socketcand syntax.
esp_err_t socketcand_translate_string_to_frame(
    const char *buf, twai_message_t *msg);

// This function is used to mimic the socketcand protocol
// for opening a rawmode connection.
//
// When a client connects, call this function with `buf = ""`.
// After each call to this function, send the string written to `buf` to the
// client, and call this function again with `buf` containing the client's
// response.
//
// PARAMETERS:
// - `buf`: the string of form < > that the client just sent over TCP.
//   Pass `buf = ""` if the client has just connected.
//   This function fills `buf` with the response string that
//   should be sent to the client.
// - `bufsize`: length of `buf` in byte. Must be at least 12.
//
// RETURNS:
// - -1: `bufsize` was less than 12. `bufsize` must be at least 12.
// -  0: received unexpected message in buf. Filled buf with an error message.
// -  1: Completed step 1 of negotiating a rawmode connection
// -  2: Completed step 2 of negotiating a rawmode connection
// -  3: Completed the final step of negotiating a rawmode connection.
//       After you send `buf` to the client, the connection should be open.
int32_t socketcand_translate_open_raw(char *buf, size_t bufsize);
