#include "cyphal_node.h"

#include "can_listener.h"
#include "canard.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "o1heap.h"
#include "uavcan/node/Health_1_0.h"
#include "uavcan/node/Heartbeat_1_0.h"
#include "uavcan/node/Mode_1_0.h"

// Size of the OpenCyphal O1 heap
#define HEAP_MEM_SIZE 32000

// Name that will be used for logging
static const char* TAG = "cyphal_node";

// The OpenCyphal O1 heap
static O1HeapInstance* o1_heap_instance = NULL;
static uint8_t o1_heap_mem[HEAP_MEM_SIZE]
    __attribute__((aligned(O1HEAP_ALIGNMENT)));

// Queue with stream of incoming CAN frames.
static QueueHandle_t can_rx_queue;

// Canard instance for sending and receiving OpenCyphal messages.
static CanardInstance canard_instance;

// Queue for sending OpenCyphal messages.
static CanardTxQueue canard_tx_queue;

// The transfer ID of heartbeat messages.
// This is incremented after each transfer,
// with unsigned integer wrapping.
static uint8_t heartbeat_transfer_id = 0;

// Subscribtion to heartbeat messages
static CanardRxSubscription heartbeat_subscription;

// Task that listens to heartbeats from other OpenCyphal nodes.
// `pvParameters` should be NULL.
static void cyphal_listener_task(void* pvParameters);
static StackType_t cyphal_listener_task_stack[4096];
static StaticTask_t cyphal_listener_task_mem;

// Task that sends heartbeats to other OpenCyphal nodes.
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

// Allocates memory on the o1 heap of this `CanardInstance`.
static void* allocate_mem(CanardInstance* ins, size_t amount) {
  return o1heapAllocate(o1_heap_instance, amount);
}

// Frees memory on the o1 heap of this `CanardInstance`.
static void free_mem(CanardInstance* ins, void* pointer) {
  o1heapFree(o1_heap_instance, pointer);
}

esp_err_t cyphal_node_start(uint8_t node_id) {
  cyphal_node_status_mutex =
      xSemaphoreCreateMutexStatic(&cyphal_node_status_mutex_mem);
  if (cyphal_node_status_mutex == NULL) {
    ESP_LOGE(TAG, "Unreachable. cyphal_node_status_mutex couldn't be created.");
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

  // Initialize the OpenCyphal Canard instance
  canard_instance = canardInit(&allocate_mem, &free_mem);
  canard_instance.node_id = node_id;

  // Initialize the transmit queue
  canard_tx_queue = canardTxInit(100, CANARD_MTU_CAN_CLASSIC);

  // Subscribe to heartbeat messages
  int8_t res = canardRxSubscribe(&canard_instance, CanardTransferKindMessage,
                                 uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_,
                                 uavcan_node_Heartbeat_1_0_EXTENT_BYTES_,
                                 CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
                                 &heartbeat_subscription);
  if (res != 1) {
    ESP_LOGE(TAG,
             "OpenCyphal node couldn't subscribe to heartbeat. Error code: %d",
             res);
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

    CanardMicrosecond micros = esp_timer_get_time();

    CanardFrame canard_frame;
    canard_frame.extended_can_id = can_frame.identifier;
    canard_frame.payload = (void*)can_frame.data;
    canard_frame.payload_size = (size_t)can_frame.data_length_code;

    // Have OpenCyphal process the received frame
    CanardRxTransfer received_cyphal_msg;
    int8_t res = canardRxAccept(&canard_instance, micros, &canard_frame, 0,
                                &received_cyphal_msg, NULL);

    if (res < 0) {
      // Error occured
      ESP_LOGE(TAG, "OpenCyphal error reading CAN frame. Error code: %d", res);
      continue;

    } else if (res == 1) {
      // Complete OpenCyphal message received
      ESP_LOGD(TAG, "Received an OpenCyphal heartbeat from node ID: %d",
               received_cyphal_msg.metadata.remote_node_id);

      assert(xSemaphoreTake(cyphal_node_status_mutex, portMAX_DELAY) == pdTRUE);
      cyphal_node_status.heartbeats_received += 1;
      assert(xSemaphoreGive(cyphal_node_status_mutex) == pdTRUE);

      free_mem(&canard_instance, received_cyphal_msg.payload);
    }
  }
}

static void cyphal_heartbeat_task(void* pvParameters) {
  while (true) {
    // Send a heartbeat every second
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Create a heartbeat message
    CanardTransferMetadata transfer_metadata = {
        .priority = CanardPriorityNominal,
        .transfer_kind = CanardTransferKindMessage,
        .port_id = uavcan_node_Heartbeat_1_0_FIXED_PORT_ID_,
        .remote_node_id = CANARD_NODE_ID_UNSET,
        .transfer_id = heartbeat_transfer_id,
    };
    heartbeat_transfer_id += 1;

    uavcan_node_Heartbeat_1_0 heartbeat = {
        .uptime = esp_timer_get_time() / 1000000,
        .health.value = uavcan_node_Health_1_0_NOMINAL,
        .mode.value = uavcan_node_Mode_1_0_OPERATIONAL,
        .vendor_specific_status_code = 0,
    };

    uint8_t heartbeat_buf[uavcan_node_Heartbeat_1_0_EXTENT_BYTES_];
    size_t heartbeat_buf_size = sizeof(heartbeat_buf);

    int8_t res = uavcan_node_Heartbeat_1_0_serialize_(&heartbeat, heartbeat_buf,
                                                      &heartbeat_buf_size);
    if (res != 0) {
      ESP_LOGE(TAG, "Error serializing heartbeat to send: %d", res);
      continue;
    }

    // Enqueue the heartbeat message.
    int32_t result =
        canardTxPush(&canard_tx_queue, &canard_instance, 0, &transfer_metadata,
                     heartbeat_buf_size, (void*)heartbeat_buf);
    if (result < 1) {
      ESP_LOGE(TAG,
               "Canard error queueing heartbeat frame for transmission: %ld",
               result);
      continue;
    }

    // Transmit all the CAN frames in the queue.
    const CanardTxQueueItem* tx_item = NULL;
    while ((tx_item = canardTxPeek(&canard_tx_queue)) != NULL) {
      twai_message_t tx_frame = {0};
      tx_frame.identifier = tx_item->frame.extended_can_id;
      tx_frame.data_length_code = tx_item->frame.payload_size;
      tx_frame.extd = true;
      memcpy(tx_frame.data, tx_item->frame.payload,
             tx_item->frame.payload_size);

      esp_err_t err = twai_transmit(&tx_frame, pdMS_TO_TICKS(portMAX_DELAY));
      while (err != ESP_OK) {
        ESP_LOGE(TAG, "Couldn't transmit OpenCyphal frame: %s",
                 esp_err_to_name(err));
        err = twai_transmit(&tx_frame, pdMS_TO_TICKS(portMAX_DELAY));
      }
      
      can_listener_enqueue_msg(&tx_frame, can_rx_queue);

      free_mem(&canard_instance, canardTxPop(&canard_tx_queue, tx_item));
    }

    // Finished sending heartbeat, so let's increment the counter.
    assert(xSemaphoreTake(cyphal_node_status_mutex, portMAX_DELAY) == pdTRUE);
    cyphal_node_status.heartbeats_sent += 1;
    assert(xSemaphoreGive(cyphal_node_status_mutex) == pdTRUE);
  }
}