#include "cyphal_node.h"

#include <string.h>

#include "can_listener.h"
#include "canard.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "o1heap.h"
#include "uavcan/node/Health_1_0.h"
#include "uavcan/node/Heartbeat_1_0.h"
#include "uavcan/node/Mode_1_0.h"

// Size of the OpenCyphal O1 heap
#define HEAP_MEM_SIZE 32000

// Maximum number of CAN frames waiting in the Canard transmit queue.
#define TX_QUEUE_CAPACITY 32

// How often the heartbeat is published, and how often Canard is polled.
#define HEARTBEAT_PERIOD_US 1000000
#define POLL_PERIOD_MS 10

// Name that will be used for logging
static const char* TAG = "cyphal_node";

// The OpenCyphal O1 heap
static O1HeapInstance* o1_heap_instance = NULL;
static uint8_t o1_heap_mem[HEAP_MEM_SIZE]
    __attribute__((aligned(O1HEAP_ALIGNMENT)));

// Guards every access to the O1 heap.
static SemaphoreHandle_t o1_heap_mutex = NULL;
static StaticSemaphore_t o1_heap_mutex_mem;

// Queue with stream of incoming CAN frames.
static QueueHandle_t can_rx_queue;

// Canard instance for sending and receiving OpenCyphal messages.
static canard_t canard_instance;

// Guards every call into Canard. Canard is not thread-safe, and both
// OpenCyphal tasks use the same instance.
static SemaphoreHandle_t canard_mutex = NULL;
static StaticSemaphore_t canard_mutex_mem;

// The transfer ID of heartbeat messages.
// This is incremented after each transfer and wraps at
// CANARD_TRANSFER_ID_MODULO.
static uint8_t heartbeat_transfer_id = 0;

// Subscribtion to heartbeat messages
static canard_subscription_t heartbeat_subscription;

// Task that listens to heartbeats from other OpenCyphal nodes.
// `pvParameters` should be NULL.
static void cyphal_listener_task(void* pvParameters);
static StackType_t cyphal_listener_task_stack[4096];
static StaticTask_t cyphal_listener_task_mem;

// Task that sends heartbeats to other OpenCyphal nodes
// and runs the Canard transmit pipeline.
// `pvParameters` should be NULL.
static void cyphal_heartbeat_task(void* pvParameters);
static StackType_t cyphal_heartbeat_task_stack[4096];
static StaticTask_t cyphal_heartbeat_task_mem;

// Purely informational current status of the OpenCyphal node.
static cyphal_node_status_t cyphal_node_status = {0};
static SemaphoreHandle_t cyphal_node_status_mutex = NULL;
static StaticSemaphore_t cyphal_node_status_mutex_mem;

esp_err_t cyphal_node_get_status(cyphal_node_status_t* status_out) {
  if (cyphal_node_status_mutex == NULL) {
    return ESP_FAIL;
  }
  assert(xSemaphoreTake(cyphal_node_status_mutex, portMAX_DELAY) == pdTRUE);
  *status_out = cyphal_node_status;
  assert(xSemaphoreGive(cyphal_node_status_mutex) == pdTRUE);
  return ESP_OK;
}

// Locks the O1 heap. o1heap is not thread-safe, and both OpenCyphal tasks
// allocate and free, so every access to it must hold this lock.
// The calls are kept outside of `assert` so the lock still works
// if assertions are compiled out.
static void o1_heap_lock(void) {
  BaseType_t taken = xSemaphoreTake(o1_heap_mutex, portMAX_DELAY);
  assert(taken == pdTRUE);
  (void)taken;
}

// Unlocks the O1 heap.
static void o1_heap_unlock(void) {
  BaseType_t given = xSemaphoreGive(o1_heap_mutex);
  assert(given == pdTRUE);
  (void)given;
}

// Locks the Canard instance.
static void canard_lock(void) {
  BaseType_t taken = xSemaphoreTake(canard_mutex, portMAX_DELAY);
  assert(taken == pdTRUE);
  (void)taken;
}

// Unlocks the Canard instance.
static void canard_unlock(void) {
  BaseType_t given = xSemaphoreGive(canard_mutex);
  assert(given == pdTRUE);
  (void)given;
}

// Allocates memory on the o1 heap
static void* allocate_mem(canard_mem_t memory, size_t amount) {
  (void)memory;
  o1_heap_lock();
  void* pointer = o1heapAllocate(o1_heap_instance, amount);
  o1_heap_unlock();
  return pointer;
}

// Frees memory on the o1 heap
static void free_mem(canard_mem_t memory, size_t size, void* pointer) {
  (void)memory;
  (void)size;
  o1_heap_lock();
  o1heapFree(o1_heap_instance, pointer);
  o1_heap_unlock();
}

static const canard_mem_vtable_t canard_mem_vtable = {
    .free = free_mem,
    .alloc = allocate_mem,
};

// Returns the current monotonic time for Canard.
static canard_us_t canard_now(const canard_t* self) {
  (void)self;
  return esp_timer_get_time();
}

// Called by Canard from `canard_poll` for each CAN frame to transmit.
// Returns false if the CAN driver has no room, so Canard tries again later.
static bool canard_tx(canard_t* self, void* user_context, canard_us_t deadline,
                      uint_least8_t iface_index, bool fd,
                      uint32_t extended_can_id, canard_bytes_t can_data) {
  (void)self;
  (void)user_context;
  (void)deadline;
  (void)iface_index;

  // Only classic CAN is used (`canard_instance.tx.fd` is false), so a frame
  // that doesn't fit can't happen. Drop it rather than retry forever.
  if (fd || can_data.size > sizeof(((twai_message_t*)0)->data)) {
    ESP_LOGE(TAG, "Dropping OpenCyphal frame that doesn't fit classic CAN.");
    return true;
  }

  twai_message_t tx_frame = {0};
  tx_frame.identifier = extended_can_id;
  tx_frame.data_length_code = can_data.size;
  tx_frame.extd = true;
  if (can_data.size > 0) {
    memcpy(tx_frame.data, can_data.data, can_data.size);
  }

  // Don't block: Canard calls this again on the next poll if the driver is
  // busy, and the frame expires once its deadline passes.
  esp_err_t err = twai_transmit(&tx_frame, 0);
  if (err != ESP_OK) {
    return false;
  }

  can_listener_enqueue_msg(&tx_frame, can_rx_queue);

  // Heartbeats are the only thing this node transmits, and each one fits in a
  // single frame.
  assert(xSemaphoreTake(cyphal_node_status_mutex, portMAX_DELAY) == pdTRUE);
  cyphal_node_status.heartbeats_sent += 1;
  assert(xSemaphoreGive(cyphal_node_status_mutex) == pdTRUE);
  return true;
}

static const canard_vtable_t canard_vtable = {
    .now = canard_now,
    .tx = canard_tx,
    .filter = NULL,
};

// Called by Canard when a heartbeat is received from another node.
static void on_heartbeat(canard_subscription_t* self, canard_us_t timestamp,
                         canard_prio_t priority, uint_least8_t source_node_id,
                         uint_least8_t transfer_id, canard_payload_t payload) {
  (void)self;
  (void)timestamp;
  (void)priority;
  (void)transfer_id;

  ESP_LOGD(TAG, "Received an OpenCyphal heartbeat from node ID: %d",
           source_node_id);

  assert(xSemaphoreTake(cyphal_node_status_mutex, portMAX_DELAY) == pdTRUE);
  cyphal_node_status.heartbeats_received += 1;
  assert(xSemaphoreGive(cyphal_node_status_mutex) == pdTRUE);

  // Multi-frame payloads are handed over in allocated memory that we must
  // free. Single-frame payloads point into the CAN frame, so `origin` is empty.
  if (payload.origin.data != NULL) {
    free_mem((canard_mem_t){0}, payload.origin.size, payload.origin.data);
  }
}

static const canard_subscription_vtable_t heartbeat_subscription_vtable = {
    .on_message = on_heartbeat,
};

esp_err_t cyphal_node_start(uint8_t node_id) {
  cyphal_node_status_mutex =
      xSemaphoreCreateMutexStatic(&cyphal_node_status_mutex_mem);
  if (cyphal_node_status_mutex == NULL) {
    ESP_LOGE(TAG, "Unreachable. cyphal_node_status_mutex couldn't be created.");
    return ESP_FAIL;
  }

  // Create the mutex that guards the O1 heap. It must exist before
  // anything allocates, including `canard_subscribe_13b` below.
  o1_heap_mutex = xSemaphoreCreateMutexStatic(&o1_heap_mutex_mem);
  if (o1_heap_mutex == NULL) {
    ESP_LOGE(TAG, "Unreachable. o1_heap_mutex couldn't be created.");
    return ESP_FAIL;
  }

  canard_mutex = xSemaphoreCreateMutexStatic(&canard_mutex_mem);
  if (canard_mutex == NULL) {
    ESP_LOGE(TAG, "Unreachable. canard_mutex couldn't be created.");
    return ESP_FAIL;
  }

  // Initialize the O1 heap
  o1_heap_instance = o1heapInit((void*)o1_heap_mem, sizeof(o1_heap_mem));
  if (o1_heap_instance == NULL) {
    ESP_LOGE(TAG, "Couldn't initialize OpenCyphal O1 heap.");
    return ESP_FAIL;
  }

  // Get a CAN receive queue
  esp_err_t err = can_listener_get(&can_rx_queue);
  ESP_RETURN_ON_ERROR(err, TAG,
                      "OpenCyphal node couldn't get CAN receive queue.");

  // Every kind of Canard allocation comes from the same O1 heap.
  const canard_mem_t canard_memory = {
      .vtable = &canard_mem_vtable,
      .context = NULL,
  };
  const canard_mem_set_t canard_memory_set = {
      .tx_transfer = canard_memory,
      .tx_frame = canard_memory,
      .rx_session = canard_memory,
      .rx_payload = canard_memory,
      .rx_filters = canard_memory,
  };

  // The random seed only matters for automatic node ID allocation, which we
  // don't use, so any value is fine. The MAC address keeps it distinct anyway.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_ETH);
  uint64_t prng_seed = 0;
  memcpy(&prng_seed, mac, sizeof(mac));

  // Initialize the OpenCyphal Canard instance with one CAN interface and no
  // hardware filters.
  if (!canard_new(&canard_instance, &canard_vtable, canard_memory_set, 1,
                  TX_QUEUE_CAPACITY, prng_seed, 0)) {
    ESP_LOGE(TAG, "Couldn't initialize OpenCyphal Canard instance.");
    can_listener_free(can_rx_queue);
    return ESP_FAIL;
  }
  // The TWAI driver is configured for classic CAN, not CAN FD.
  canard_instance.tx.fd = false;

  if (!canard_set_node_id(&canard_instance, node_id)) {
    ESP_LOGE(TAG, "OpenCyphal node ID %d is not valid.", node_id);
    canard_destroy(&canard_instance);
    can_listener_free(can_rx_queue);
    return ESP_ERR_INVALID_ARG;
  }

  // Subscribe to heartbeat messages
  canard_subscription_t* subscription = canard_subscribe_13b(
      &canard_instance, &heartbeat_subscription,
      uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_,
      uavcan_node_Heartbeat_1_0_EXTENT_BYTES_,
      CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_us, &heartbeat_subscription_vtable);
  if (subscription != &heartbeat_subscription) {
    ESP_LOGE(TAG, "OpenCyphal node couldn't subscribe to heartbeat.");
    canard_destroy(&canard_instance);
    can_listener_free(can_rx_queue);
    return ESP_FAIL;
  }

  // Spawn the OpenCyphal listener task
  xTaskCreateStatic(cyphal_listener_task, "cyphal_listener_task",
                    sizeof(cyphal_listener_task_stack), NULL, 3,
                    cyphal_listener_task_stack, &cyphal_listener_task_mem);

  // Spawn the OpenCyphal node task
  xTaskCreateStatic(cyphal_heartbeat_task, "cyphal_heartbeat_task",
                    sizeof(cyphal_heartbeat_task_stack), NULL, 3,
                    cyphal_heartbeat_task_stack, &cyphal_heartbeat_task_mem);

  return ESP_OK;
}

static void cyphal_listener_task(void* pvParameters) {
  while (true) {
    // Receive the next frame from the CAN bus.
    twai_message_t can_frame = {0};
    xQueueReceive(can_rx_queue, &can_frame, portMAX_DELAY);

    // OpenCyphal only uses extended data frames, and Canard only accepts
    // frames with a 29-bit ID.
    if (!can_frame.extd || can_frame.rtr || can_frame.data_length_code > 8) {
      continue;
    }

    const canard_bytes_t can_data = {
        .size = (size_t)can_frame.data_length_code,
        .data = can_frame.data,
    };

    // Have OpenCyphal process the received frame. Canard calls `on_heartbeat`
    // before this returns if the frame completes a heartbeat.
    canard_lock();
    bool ok = canard_ingest_frame(&canard_instance, esp_timer_get_time(), 0,
                                  can_frame.identifier, can_data);
    canard_unlock();

    if (!ok) {
      ESP_LOGE(TAG, "OpenCyphal error reading CAN frame.");
    }
  }
}

static void cyphal_heartbeat_task(void* pvParameters) {
  int64_t next_heartbeat_us = esp_timer_get_time() + HEARTBEAT_PERIOD_US;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));

    const int64_t now_us = esp_timer_get_time();

    if (now_us >= next_heartbeat_us) {
      next_heartbeat_us += HEARTBEAT_PERIOD_US;

      // Create a heartbeat message
      uavcan_node_Heartbeat_1_0 heartbeat = {
          .uptime = now_us / 1000000,
          .health.value = uavcan_node_Health_1_0_NOMINAL,
          .mode.value = uavcan_node_Mode_1_0_OPERATIONAL,
          .vendor_specific_status_code = 0,
      };

      uint8_t heartbeat_buf[uavcan_node_Heartbeat_1_0_EXTENT_BYTES_];
      size_t heartbeat_buf_size = sizeof(heartbeat_buf);

      int8_t res = uavcan_node_Heartbeat_1_0_serialize_(
          &heartbeat, heartbeat_buf, &heartbeat_buf_size);
      if (res != 0) {
        ESP_LOGE(TAG, "Error serializing heartbeat to send: %d", res);
      } else {
        const canard_bytes_chain_t heartbeat_payload = {
            .bytes = {.size = heartbeat_buf_size, .data = heartbeat_buf},
            .next = NULL,
        };

        // Enqueue the heartbeat message. It is dropped if it can't be sent
        // within one period, so a dead bus doesn't pile up stale heartbeats.
        canard_lock();
        bool queued = canard_publish_13b(
            &canard_instance, now_us + HEARTBEAT_PERIOD_US,
            CANARD_IFACE_BITMAP_ALL, canard_prio_nominal,
            uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_, heartbeat_transfer_id,
            heartbeat_payload, NULL);
        canard_unlock();

        if (!queued) {
          ESP_LOGE(TAG, "Canard error queueing heartbeat for transmission.");
        }
        heartbeat_transfer_id =
            (heartbeat_transfer_id + 1) % CANARD_TRANSFER_ID_MODULO;
      }
    }

    // Transmit the frames in the queue. `canard_tx` counts the heartbeats
    // that were actually sent.
    canard_lock();
    canard_poll(&canard_instance, CANARD_IFACE_BITMAP_ALL);
    canard_unlock();
  }
}
