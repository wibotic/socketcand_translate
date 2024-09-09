#include "socketcand_server.h"

#include "can_listener.h"
#include "driver/twai.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "frame_io.h"
#include "lwip/sockets.h"
#include "socketcand_translate.h"

// Name that will be used for logging
static const char *TAG = "socketcand_server";

// Maximum number of TCP socketcand client connections
// that can be served simultaneously before new connections
// are dropped.
#define MAX_CLIENTS 4

// Stack size allocated for every FreeRTOS task.
#define STACK_SIZE 4096

// Set the `twai_message_t` `data_length_code` to this value
// to indicate this isn't a CAN bus frame.
// If a task pops this off the queue, that means it should
// exit and disconnect from its TCP client.
#define CAN_INTERRUPT_FRAME 0xff

// Data that each client handler gets a pointer to.
typedef struct {
  // Queue of `twai_message_t` incoming from the CAN bus.
  // Initialized using `can_listener_get()`.
  QueueHandle_t can_rx_queue;

  // Messenger used for communicating with TCP client.
  // Its `socket_fd` is set to -1 when it isn't connected to any clients.
  frame_io_messenger tcp_messenger;

  // Mutex that the 2 tasks serving the client take
  // during the critical section of closing the connection
  // and deleting themselves.
  SemaphoreHandle_t handler_task_delete_mutex;

  // FreeRTOS memory for the `handler_task_delete_mutex`.
  StaticSemaphore_t handler_task_delete_mutex_mem;

  // FreeRTOS stack for the first task serving this client.
  StackType_t free_rtos_stack_1[STACK_SIZE];

  // FreeRTOS memory for the first task serving this client.
  StaticTask_t free_rtos_mem_1;

  // FreeRTOS stack for the second task serving this client.
  StackType_t free_rtos_stack_2[STACK_SIZE];

  // FreeRTOS memory for the second task serving this client.
  StaticTask_t free_rtos_mem_2;

} client_handler_data_t;

// An array of `client_handler_data_t`. Each pair of tasks handling
// a client gets assigned a pointer to one of the elements.
static client_handler_data_t client_handler_datas[MAX_CLIENTS];

// This queue stores pointers to all unused `client_handler_data_t`
// in the array `client_handler_datas`.
//
// An item is popped when spawning a new task to handle a client,
// and pushed again once the client disconnects.
static QueueHandle_t unused_client_handler_data_queue = NULL;
static StaticQueue_t unused_client_handler_data_queue_buffer;
static uint8_t
    unused_client_handler_data_queue_storage[MAX_CLIENTS *
                                             sizeof(client_handler_data_t *)];

// Returns a pointer to an initialized `client_handler_data_t` from
// `unused_client_handler_data_queue`.
// Returns NULL if all `MAX_CLIENTS` of them are already in use.
static client_handler_data_t *get_client_handler_data(int client_sock);

// Resets the `client_handler_data_t`
// and sends it to the `unused_client_handler_data_queue`.
static void free_client_handler_data(
    client_handler_data_t *client_handler_data);

// Task that continuously listens for incoming TCP connections.
// pvParameters should be a listener socket FD.
static void run_server_task(void *pvParameters);
static StackType_t run_server_task_stack[STACK_SIZE];
static StaticTask_t run_server_task_mem;

// Task that serves a client.
// pvParameters should be a pointer to a `client_handler_data_t`.
static void serve_client_task(void *pvParameters);

// Task that forwards messages from TCP to CAN bus.
// pvParameters should be a pointer to a `client_handler_data_t`.
static void socketcand_to_bus_task(void *pvParameters);

// Task that forwards messages from CAN bus to TCP.
// pvParameters should be a pointer to a `client_handler_data_t`.
static void bus_to_socketcand_task(void *pvParameters);

// Both tasks serving a client must call this function
// before returning.
// This function deletes the task and does some things
// to set everything up for the next user of this `client_handler_data`.
// See comments inside the function for more details.
static void delete_serve_client_task(
    client_handler_data_t *client_handler_data);

// Purely informational status of this server
static socketcand_server_status_t server_status = {0};
static SemaphoreHandle_t server_status_mutex = NULL;
static StaticSemaphore_t server_status_mutex_mem;

esp_err_t socketcand_server_status(socketcand_server_status_t *status_out) {
  if (server_status_mutex == NULL) {
    ESP_LOGE(
        TAG,
        "Can't get status because socketcand server hasn't been initialized.");
    return ESP_FAIL;
  }

  assert(xSemaphoreTake(server_status_mutex, portMAX_DELAY) == pdTRUE);
  *status_out = server_status;
  assert(xSemaphoreGive(server_status_mutex) == pdTRUE);

  return ESP_OK;
}

esp_err_t socketcand_server_start(void) {
  // create a TCP socket
  int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_sock < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    return ESP_FAIL;
  }

  // Disable Nagle's algorithm to reduce latency.
  int tcp_nodelay = 1;
  int err = setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, &tcp_nodelay,
                       sizeof(tcp_nodelay));
  if (err != 0) {
    ESP_LOGE(TAG, "Unable to set TCP_NODELAY on socket: errno %d", errno);
    close(listen_sock);
    return ESP_FAIL;
  }

  // Bind the listener TCP socket
  struct sockaddr_in server_addr = {0};
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(29536);
  err = bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    close(listen_sock);
    return ESP_FAIL;
  }

  // Listen on the TCP socket
  err = listen(listen_sock, 1);
  if (err != 0) {
    ESP_LOGE(TAG, "Couldn't listen through socket. errno %d", errno);
    close(listen_sock);
    return ESP_FAIL;
  }

  // Initialize the mutex for accessing the `server_status` struct.
  server_status_mutex = xSemaphoreCreateMutexStatic(&server_status_mutex_mem);
  if (server_status_mutex == NULL) {
    ESP_LOGE(TAG, "Unreachable. server_status_mutex couldn't be created.");
    return ESP_FAIL;
  }

  // Create the queue that holds pointers to all the unused
  // `client_handler_data_t`.
  unused_client_handler_data_queue =
      xQueueCreateStatic(MAX_CLIENTS, sizeof(client_handler_data_t *),
                         unused_client_handler_data_queue_storage,
                         &unused_client_handler_data_queue_buffer);
  if (unused_client_handler_data_queue == NULL) {
    ESP_LOGE(
        TAG,
        "Unreachable. unused_client_handler_data_queue couldn't be created.");
    return ESP_FAIL;
  }

  // Initialize `client_handler_datas`
  // and `unused_client_handler_data_queue`.
  for (int i = 0; i < MAX_CLIENTS; i++) {
    // Initialize the `client_handler_data_t`.
    client_handler_datas[i].can_rx_queue = NULL;

    client_handler_datas[i].tcp_messenger.socket_fd = -1;

    client_handler_datas[i].handler_task_delete_mutex =
        xSemaphoreCreateMutexStatic(
            &client_handler_datas[i].handler_task_delete_mutex_mem);
    if (client_handler_datas[i].handler_task_delete_mutex == NULL) {
      ESP_LOGE(TAG,
               "Unreachable. A handler_task_delete_mutex couldn't be created.");
      return ESP_FAIL;
    }

    // Push a pointer to this `client_handler_data_t` to the
    // `unused_client_handler_data_queue`.
    const client_handler_data_t *pointer_to_client_handler_data =
        &client_handler_datas[i];
    if (xQueueSend(unused_client_handler_data_queue,
                   &pointer_to_client_handler_data, 0) != pdTRUE) {
      ESP_LOGE(TAG,
               "Unreachable. unused_client_handler_data_queue should have "
               "MAX_CLIENT slots.");
      return ESP_FAIL;
    }
  }

  // Log that we've started listening.
  ESP_LOGD(TAG, "Started socketcand TCP server listening on %s:%d",
           inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));

  // Task that continuously listens for incoming TCP connections.
  // pvParameters is set to the listener socket FD.
  xTaskCreateStatic(run_server_task, "socketcand_server",
                    sizeof(run_server_task_stack), (void *)listen_sock, 6,
                    run_server_task_stack, &run_server_task_mem);

  return ESP_OK;
}

static client_handler_data_t *get_client_handler_data(int client_sock) {
  client_handler_data_t *client_handler_data_ptr;
  BaseType_t res = xQueueReceive(unused_client_handler_data_queue,
                                 &client_handler_data_ptr, 0);
  if (res != pdTRUE) {
    return NULL;
  }

  // Fill out the `can_rx_queue` of this `client_handler_data`.
  esp_err_t err = can_listener_get(&client_handler_data_ptr->can_rx_queue);
  if (err != ESP_OK) {
    assert(xQueueSend(unused_client_handler_data_queue,
                      &client_handler_data_ptr, 0) == pdTRUE);
    return NULL;
  }

  // Initialize the `tcp_messenger`.
  client_handler_data_ptr->tcp_messenger.l = 0;
  client_handler_data_ptr->tcp_messenger.r = 0;
  client_handler_data_ptr->tcp_messenger.socket_fd = client_sock;

  return client_handler_data_ptr;
}

static void free_client_handler_data(
    client_handler_data_t *client_handler_data) {
  // Close client connection if one is still open
  if (client_handler_data->tcp_messenger.socket_fd != -1) {
    shutdown(client_handler_data->tcp_messenger.socket_fd, 0);
    close(client_handler_data->tcp_messenger.socket_fd);
  }

  // Reset the `tcp_messenger`.
  client_handler_data->tcp_messenger.l = 0;
  client_handler_data->tcp_messenger.r = 0;
  client_handler_data->tcp_messenger.socket_fd = -1;

  esp_err_t err = can_listener_free(client_handler_data->can_rx_queue);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Unreachable. Couldn't free CAN RX queue.");
    abort();
  }
  client_handler_data->can_rx_queue = NULL;

  assert(xQueueSend(unused_client_handler_data_queue, &client_handler_data,
                    0) == pdTRUE);
}

static void run_server_task(void *pvParameters) {
  int listen_sock = (int)pvParameters;

  // Run the server
  while (true) {
    // Accept an incoming connection.
    struct sockaddr_in source_addr;
    socklen_t addr_len = sizeof(source_addr);
    int client_sock =
        accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
    if (client_sock < 0) {
      ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
      continue;
    }

    // Log the origin of the incoming connection
    ESP_LOGI(TAG, "Accepted socketcand client TCP connection from: %s",
             inet_ntoa(source_addr.sin_addr));

    // Get a pointer to an unused `client_handler_data_t`.
    client_handler_data_t *client_handler_data =
        get_client_handler_data(client_sock);

    if (client_handler_data == NULL) {
      ESP_LOGE(TAG,
               "Dropping incoming socketcand TCP connection because reached "
               "limit of %d "
               "simultaneous clients.",
               MAX_CLIENTS);
      shutdown(client_sock, 0);
      close(client_sock);
      continue;
    }

    // spawn a thread to serve the client with this index
    xTaskCreateStatic(serve_client_task, "serving_socketcand_client",
                      sizeof(client_handler_data->free_rtos_stack_1),
                      (void *)client_handler_data, 10,
                      client_handler_data->free_rtos_stack_1,
                      &client_handler_data->free_rtos_mem_1);
  }
}

static void serve_client_task(void *pvParameters) {
  client_handler_data_t *client_handler_data =
      (client_handler_data_t *)pvParameters;

  // Establish a socketcand rawmode connection
  char frame_str[SOCKETCAND_RAW_MAX_LEN] = "";
  while (true) {
    // write a handshake frame
    int32_t phase = socketcand_translate_open_raw(frame_str, sizeof(frame_str));
    esp_err_t err = frame_io_write_str(
        client_handler_data->tcp_messenger.socket_fd, frame_str);
    if (err != ESP_OK) {
      ESP_LOGE(TAG,
               "Disconnecting because couldn't send socketcand to client: %s",
               esp_err_to_name(err));
      free_client_handler_data(client_handler_data);
      vTaskDelete(NULL);
      return;
    }

    if (phase == -1) {
      ESP_LOGE(
          TAG,
          "Unreachable. Buffer too small when negotiating socketcand rawmode.");
      free_client_handler_data(client_handler_data);
      vTaskDelete(NULL);
      return;
    } else if (phase == 0) {
      ESP_LOGE(TAG,
               "Client sent unknown socketcand message '%s' while negotiating "
               "rawmode. "
               "Closing connection.",
               frame_str);
      // Increment the server status error counter
      assert(xSemaphoreTake(server_status_mutex, portMAX_DELAY) == pdTRUE);
      server_status.invalid_socketcand_frames_received += 1;
      assert(xSemaphoreGive(server_status_mutex) == pdTRUE);

      free_client_handler_data(client_handler_data);
      vTaskDelete(NULL);
      return;

    } else if (phase == 3) {
      // Socketcand rawmode successfully established.
      // Exit this negotiation loop.
      break;
    }

    // read the next rawmode negotiation frame from the client
    err = frame_io_read_next_frame(&client_handler_data->tcp_messenger,
                                   frame_str, sizeof(frame_str));
    if (err != ESP_OK) {
      ESP_LOGI(TAG,
               "Error reading socketcand rawmode negotiation < > frame from "
               "client. Closing connection.");
      free_client_handler_data(client_handler_data);
      vTaskDelete(NULL);
      return;
    }
  }

  // run translation in both directions simultaneously
  xTaskCreateStatic(bus_to_socketcand_task, "bus_to_socketcand",
                    sizeof(client_handler_data->free_rtos_stack_2),
                    pvParameters, 10, client_handler_data->free_rtos_stack_2,
                    &client_handler_data->free_rtos_mem_2);
  socketcand_to_bus_task(pvParameters);
}

static void socketcand_to_bus_task(void *pvParameters) {
  client_handler_data_t *client_handler_data =
      (client_handler_data_t *)pvParameters;

  // A single C string storing a complete frame.
  char frame_str[SOCKETCAND_RAW_MAX_LEN];

  while (true) {
    // Try to read the next data < > frame from the network.
    esp_err_t err = frame_io_read_next_frame(
        &client_handler_data->tcp_messenger, frame_str, sizeof(frame_str));
    if (err != ESP_OK) {
      ESP_LOGD(
          TAG,
          "Couldn't read the next < > frame from socketcand. Disconnecting.");
      delete_serve_client_task(client_handler_data);
      return;
    }

    // Parse the message
    twai_message_t received_msg = {0};
    err = socketcand_translate_string_to_frame(frame_str, &received_msg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG,
               "Couldn't parse socketcand frame from client. Disconnecting.");
      // Increment the server status error counter
      assert(xSemaphoreTake(server_status_mutex, portMAX_DELAY) == pdTRUE);
      server_status.invalid_socketcand_frames_received += 1;
      assert(xSemaphoreGive(server_status_mutex) == pdTRUE);

      delete_serve_client_task(client_handler_data);
      return;
    }

    // Increment the server status can bus counter
    assert(xSemaphoreTake(server_status_mutex, portMAX_DELAY) == pdTRUE);
    server_status.socketcand_frames_received += 1;
    assert(xSemaphoreGive(server_status_mutex) == pdTRUE);

    // Send the message to other TCP socketcand clients.
    can_listener_enqueue_msg(&received_msg, client_handler_data->can_rx_queue);

    // Enqueue the frame for CAN transmission, with a timeout of 2 seconds
    err = twai_transmit(&received_msg, pdMS_TO_TICKS(2000));
    if (err == ESP_OK) {
      // Increment the server status can bus counter
      assert(xSemaphoreTake(server_status_mutex, portMAX_DELAY) == pdTRUE);
      server_status.can_bus_frames_sent += 1;
      assert(xSemaphoreGive(server_status_mutex) == pdTRUE);
    } else {
      ESP_LOGE(TAG, "Couldn't transmit frame to CAN. %s", esp_err_to_name(err));

      assert(xSemaphoreTake(server_status_mutex, portMAX_DELAY) == pdTRUE);
      server_status.can_bus_frames_send_timeouts += 1;
      assert(xSemaphoreGive(server_status_mutex) == pdTRUE);
    }
  }
  delete_serve_client_task(client_handler_data);
  return;
}

static void bus_to_socketcand_task(void *pvParameters) {
  client_handler_data_t *client_handler_data =
      (client_handler_data_t *)pvParameters;

  char buf[SOCKETCAND_RAW_MAX_LEN];

  while (true) {
    // Receive an incoming frame from the CAN bus queue
    twai_message_t twai_msg;
    BaseType_t res = xQueueReceive(client_handler_data->can_rx_queue, &twai_msg,
                                   portMAX_DELAY);
    if (res != pdTRUE) {
      ESP_LOGE(TAG, "Unreachable. Couldn't receive CAN bus frame from queue.");
      delete_serve_client_task(client_handler_data);
      return;
    }

    // If received a special frame that means we should
    // disconnect from the client.
    if (twai_msg.data_length_code == CAN_INTERRUPT_FRAME) {
      delete_serve_client_task(client_handler_data);
      return;
    }

    // `socketcand_translate_frame_to_string()` requires the current time.
    int64_t micros = esp_timer_get_time();
    int64_t secs = micros / 1000000;
    int64_t usecs = micros % 1000000;

    // write the message to TCP
    esp_err_t err = socketcand_translate_frame_to_string(
        buf, sizeof(buf), &twai_msg, secs, usecs);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't translate CAN frame to socketcand < > string.");
      delete_serve_client_task(client_handler_data);
      return;
    }

    err = frame_io_write_str(client_handler_data->tcp_messenger.socket_fd, buf);
    if (err != ESP_OK) {
      ESP_LOGD(TAG, "Error sending socketcand frame to client over TCP.");
      delete_serve_client_task(client_handler_data);
      return;
    }

    // Increment the server status socketcand sent counter
    assert(xSemaphoreTake(server_status_mutex, portMAX_DELAY) == pdTRUE);
    server_status.socketcand_frames_sent += 1;
    assert(xSemaphoreGive(server_status_mutex) == pdTRUE);
  }

  delete_serve_client_task(client_handler_data);
  return;
}

static void delete_serve_client_task(
    client_handler_data_t *client_handler_data) {
  // Enter the critical section.
  assert(xSemaphoreTake(client_handler_data->handler_task_delete_mutex,
                        portMAX_DELAY) == pdTRUE);

  // If socket_fd hasn't already been set to -1, that means
  // I'm the first task to notice the client disconnected.
  if (client_handler_data->tcp_messenger.socket_fd != -1) {
    // Gracefully shutdown the socket that the client is connected to.
    shutdown(client_handler_data->tcp_messenger.socket_fd, 0);
    close(client_handler_data->tcp_messenger.socket_fd);
    client_handler_data->tcp_messenger.socket_fd = -1;

    // Send a `termination_msg` to `can_rx_queue` so if the other task
    // is blocking on receiving `can_rx_queue`, it knows to stop.
    twai_message_t termination_msg = {0};
    termination_msg.data_length_code = CAN_INTERRUPT_FRAME;
    xQueueSend(client_handler_data->can_rx_queue, &termination_msg, 0);

  } else {
    // Else, the other task has already disconnected from the client.
    // Free  this client handler data.
    free_client_handler_data(client_handler_data);

    ESP_LOGI(TAG, "Socketcand client disconnected.");
  }

  assert(xSemaphoreGive(client_handler_data->handler_task_delete_mutex) ==
         pdTRUE);

  vTaskDelete(NULL);
  return;
}
