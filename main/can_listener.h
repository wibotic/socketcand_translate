#pragma once

#include "driver/twai.h"
#include "esp_err.h"

// The maximum number of CAN receive queues
// that may be loaned with `can_listener_get()`
// at any time.
// The socketcand_server will use up to 4 of these,
// and the OpenCyphal node may use 1.
#define CAN_LISTENERS_MAX 5

// The status of the CAN listener.
// Get the current status using `can_listener_get_status()`.
typedef struct {
  uint64_t can_bus_frames_received;
  uint64_t can_bus_incoming_frames_dropped;
} can_listener_status_t;

// Starts a task that listens to CAN packets.
// Call this function before calling the other ones.
// Must only be called once.
// Must be called after the CAN driver is initialized.
esp_err_t can_listener_start(void);

// Fills `status_out` with the current `can_listener_status_t`.
// Returns an error if the CAN listener hasn't been started yet.
esp_err_t can_listener_get_status(can_listener_status_t* status_out);

// Fills `can_rx_queue` with a `QueueHandle_t`.
// The CAN listener task will send CAN frames to the queue
// as they are received.
// Up to `CAN_LISTENERS_MAX` queues can be active at any time.
// Returns `ESP_ERR_NO_MEM` if there are already `CAN_LISTENERS_MAX` loaned.
// Call `can_listener_free()` to return your queue.
esp_err_t can_listener_get(QueueHandle_t* can_rx_queue);

// Frees the `QueueHandle_t` loaned by `can_listener_get()`.
// It can't be used after this.
esp_err_t can_listener_free(const QueueHandle_t can_rx_queue);

// Pushes `message` to all the receiving queues except for `skip_queue`.
// This function is used to simulate receiving a CAN message.
// Set `skip_queue` to NULL to not skip any queues.
void can_listener_enqueue_msg(const twai_message_t* message,
                              const QueueHandle_t skip_queue);