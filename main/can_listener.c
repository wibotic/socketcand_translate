#include "can_listener.h"

#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "stdatomic.h"

// The capacity of each CAN receive queue
#define CAN_RX_QUEUE_LEN 32

// The stack size of the can listener task.
#define STACK_SIZE 4096

// Name that will be used for logging
static const char *TAG = "can_listener";

// A struct that holds a CAN receiver queue that can be loaned
// with `can_listener_get()`.
typedef struct {
  // The `can_listener_task` pushes `twai_message_t`s
  // from the CAN bus onto all `rx_queue`s that are `in_use`.
  QueueHandle_t rx_queue;

  // The storage that `rx_queue` uses.
  uint8_t q_storage[CAN_RX_QUEUE_LEN * sizeof(twai_message_t)];

  // The data structure that `rx_queue` uses.
  StaticQueue_t q_buf;

  // True if this `rx_queue` has been loaned out with
  // `can_listener_get()`. The `can_listener_task` only pushes
  // messages to `can_receiver_t`s that are `in_use`.
  atomic_bool in_use;

} can_receiver_t;

// All the `can_receiver_t` that can be loaned out with `can_listener_get()`.
static can_receiver_t can_receivers[CAN_LISTENERS_MAX];

// This queue stores pointers to all unused `can_receiver_t`
// in the array `can_receivers`.
// It's initialized to point to all the `can_receivers`.
//
// An item is popped when `can_listener_get()` is called
// and pushed when `can_listener_free()` is called.
static QueueHandle_t unused_can_receivers_queue = NULL;
static uint8_t unused_can_receiver_queue_storage[CAN_LISTENERS_MAX *
                                                 sizeof(can_receiver_t *)];
static StaticQueue_t unused_can_receiver_queue_buf;

// Task that continuously pushes messages from the CAN bus
// onto all the active `rx_queue`s in `can_receivers`.
static void can_listener_task(void *pvParameters);
static StackType_t can_listener_task_stack[STACK_SIZE];
static StaticTask_t can_listener_task_mem;

// Purely informational current status of the CAN listener.
static can_listener_status_t can_listener_status = {0};
static SemaphoreHandle_t can_listener_status_mutex = NULL;
static StaticSemaphore_t can_listener_status_mutex_mem;

esp_err_t can_listener_get_status(can_listener_status_t *status_out) {
  if (can_listener_status_mutex == NULL) {
    ESP_LOGE(
        TAG,
        "Can't get status because socketcand server hasn't been initialized.");
    return ESP_FAIL;
  }
  assert(xSemaphoreTake(can_listener_status_mutex, portMAX_DELAY) == pdTRUE);
  *status_out = can_listener_status;
  assert(xSemaphoreGive(can_listener_status_mutex) == pdTRUE);
  return ESP_OK;
}

esp_err_t can_listener_start() {
  // Create the queue that tracks unused `can_receiver_t`.
  unused_can_receivers_queue = xQueueCreateStatic(
      CAN_LISTENERS_MAX, sizeof(can_receiver_t *),
      unused_can_receiver_queue_storage, &unused_can_receiver_queue_buf);

  if (unused_can_receivers_queue == NULL) {
    ESP_LOGE(TAG, "Error initializing queue in can_listener.");
    return ESP_FAIL;
  }

  // Initialize every `can_receiver_t` in `can_receivers`.
  // Add them to the `unused_can_receivers_queue`.
  for (size_t i = 0; i < CAN_LISTENERS_MAX; i++) {
    // Initialize the `can_receiver_t`.
    atomic_store(&can_receivers[i].in_use, false);
    can_receivers[i].rx_queue =
        xQueueCreateStatic(CAN_RX_QUEUE_LEN, sizeof(twai_message_t),
                           can_receivers[i].q_storage, &can_receivers[i].q_buf);

    if (can_receivers[i].rx_queue == NULL) {
      ESP_LOGE(TAG, "Error initializing queue in can_listener.");
      return ESP_FAIL;
    }

    // Add this `can_receiver_t` to the `unused_can_receivers_queue`.
    can_receiver_t *can_receiver_ptr = &can_receivers[i];
    BaseType_t res =
        xQueueSend(unused_can_receivers_queue, &can_receiver_ptr, 0);
    if (res != pdTRUE) {
      ESP_LOGE(TAG, "Error initializing queue in can_listener.");
      return ESP_FAIL;
    }
  }

  // Initialize the mutex for accessing the `can_listener_status` struct.
  can_listener_status_mutex =
      xSemaphoreCreateMutexStatic(&can_listener_status_mutex_mem);
  if (can_listener_status_mutex == NULL) {
    ESP_LOGE(TAG,
             "Unreachable. server_status_mutex couldn't be created in "
             "can_listener.");
    return ESP_FAIL;
  }

  // Spawn the task that will insert incoming CAN messages
  // to active queues in `can_receivers`.
  xTaskCreateStatic(can_listener_task, "can_listener",
                    sizeof(can_listener_task_stack), NULL, 14,
                    can_listener_task_stack, &can_listener_task_mem);

  return ESP_OK;
}

esp_err_t can_listener_get(QueueHandle_t *can_rx_out) {
  // Get an unused `can_receiver_t`.
  can_receiver_t *can_receiver;
  BaseType_t res = xQueueReceive(unused_can_receivers_queue, &can_receiver, 0);

  // If there are no free `can_receiver_t`, return an error.
  if (res != pdTRUE) {
    return ESP_ERR_NO_MEM;
  }

  xQueueReset(can_receiver->rx_queue);
  *can_rx_out = can_receiver->rx_queue;
  atomic_store(&can_receiver->in_use, true);
  return ESP_OK;
}

esp_err_t can_listener_free(const QueueHandle_t can_rx) {
  // Find `can_rx` in `can_receivers`.
  can_receiver_t *can_receiver = NULL;
  for (size_t i = 0; i < CAN_LISTENERS_MAX; i++) {
    if (can_rx == can_receivers[i].rx_queue) {
      can_receiver = &can_receivers[i];
      break;
    }
  }

  // If the given `can_rx` isn't in our list of all `can_receivers`,
  // return an error.
  if (can_receiver == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // Mark the `can_receiver` as unused.
  atomic_store(&can_receiver->in_use, false);

  // Add the unused `can_receiver` to the `unused_can_receivers_queue`.
  BaseType_t res = xQueueSend(unused_can_receivers_queue, &can_receiver, 0);
  if (res != pdTRUE) {
    ESP_LOGE(TAG, "Invalid state. Couldn't free CAN listener.");
    return ESP_ERR_INVALID_STATE;
  }

  return ESP_OK;
}

void can_listener_enqueue_msg(const twai_message_t *message,
                              const QueueHandle_t skip_queue) {
  // send the message to all `can_receivers` that are `in_use`.
  for (int i = 0; i < CAN_LISTENERS_MAX; i++) {
    if (atomic_load(&can_receivers[i].in_use) &&
        can_receivers[i].rx_queue != skip_queue) {
      // append the message to the queue
      if (xQueueSend(can_receivers[i].rx_queue, message, 0) != pdTRUE) {
        ESP_LOGE(TAG, "CAN bus task receive queue %d full. Dropping message.",
                 i);
        // Increment the status dropped frame counter
        assert(xSemaphoreTake(can_listener_status_mutex, portMAX_DELAY) ==
               pdTRUE);
        can_listener_status.can_bus_incoming_frames_dropped += 1;
        assert(xSemaphoreGive(can_listener_status_mutex) == pdTRUE);
      }
    }
  }
}

static void can_listener_task(void *pvParameters) {
  while (true) {
    // receive a message from the CAN bus
    twai_message_t received_msg = {0};
    esp_err_t res = twai_receive(&received_msg, portMAX_DELAY);
    if (res != ESP_OK) {
      ESP_LOGE(TAG, "Error receiving message from CAN bus: %s",
               esp_err_to_name(res));
      continue;
    }

    // send the message to the queues
    can_listener_enqueue_msg(&received_msg, NULL);

    // Increment the status can bus counter
    assert(xSemaphoreTake(can_listener_status_mutex, portMAX_DELAY) == pdTRUE);
    can_listener_status.can_bus_frames_received += 1;
    assert(xSemaphoreGive(can_listener_status_mutex) == pdTRUE);
  }
}
