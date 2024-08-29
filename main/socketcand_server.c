#include "socketcand_server.h"

#include "driver/twai.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "frame_io.h"
#include "lwip/sockets.h"
#include "socketcand_translate.h"

// Name that will be used for logging
static const char *TAG = "socketcand_server";

// The length of the `can_rx_queue` in each `client_handler_data_t`.
#define CAN_RX_QUEUE_LENGTH 32

// Maximum number of TCP socketcand client connections
// that can be served simultaneously before new connections
// are dropped.
#define MAX_CLIENTS 5

// Stack size allocated for every FreeRTOS task.
#define STACK_SIZE 4096

// Set the `socketcand_translate_frame_t` `user_data` value
// to this, to indicate this is a normal CAN bus frame.
#define SOCKETCAND_NORMAL_FRAME 0

// Set the `socketcand_translate_frame_t` `user_data` value
// to this, to indicate this isn't a CAN bus frame.
// If a task pops this off the queue, that means it should
// exit and disconnect from its TCP client.
#define SOCKETCAND_INTERRUPT_FRAME 1

// Data that tasks serving a client get a pointer to.
typedef struct {
  // The `can_listener_task` pushes `socketcand_translate_frame_t`s
  // from the CAN bus onto all the active `can_rx_queue`s.
  // Each `serve_client_task` then pops from the queue at its assigned index.
  QueueHandle_t can_rx_queue;

  // The data structure that `can_rx_queue` uses.
  StaticQueue_t can_rx_queue_buffer;

  // The storage that `can_rx_queue` uses.
  uint8_t can_rx_queue_storage[CAN_RX_QUEUE_LENGTH *
                               sizeof(socketcand_translate_frame_t)];

  // Messenger used for communicating with TCP client.
  frame_io_messenger tcp_messenger;

  // Mutex that tasks serving the client take
  // during the critical section of closing the connection and deleting
  // themselves.
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

// An array of `client_handler_data_t`. Each group of tasks handling
// a client gets assigned a pointer to one of the elements.
static client_handler_data_t client_handler_datas[MAX_CLIENTS];

// This queue stores pointers to all unused `client_handler_data_t`
// in the array `client_handler_datas`.
//
// An item is popped when spawning a new task to handle a client,
// and pushed again once the client disconnects.
static QueueHandle_t unused_client_handler_data_queue = NULL;
StaticQueue_t unused_client_handler_data_queue_buffer;
uint8_t
    unused_client_handler_data_queue_storage[MAX_CLIENTS *
                                             sizeof(client_handler_data_t *)];

// Task that continuously listens for incoming TCP connections.
// pvParameters should be a listener socket FD.
static void run_server_task(void *pvParameters);
static StackType_t run_server_task_stack[STACK_SIZE];
static StaticTask_t run_server_task_mem;

// Task that continuously pushes messages from the CAN bus
// onto all the active `can_rx_queue`s.
static void can_listener_task(void *pvParameters);
static StackType_t can_listener_task_stack[STACK_SIZE];
static StaticTask_t can_listener_task_mem;

// Pushes the `message` to all of the active `can_rx_queue`s.
// This will cause it to be sent to all TCP socketcand clients EXCEPT
// for `skip_client`, which will be omitted.
// Set `skip_client` to `NULL` to not skip any clients.
static void enqueue_can_message(socketcand_translate_frame_t *message,
                                const client_handler_data_t *skip_client);

// Task that serves a client.
// pvParameters should be the index of the client.
static void serve_client_task(void *pvParameters);

// Task that forwards messages from TCP to CAN bus.
// pvParameters should be the index of the client.
static void socketcand_to_bus_task(void *pvParameters);

// Task that forwards messages from CAN bus to TCP.
// pvParameters should be the index of the client.
static void bus_to_socketcand_task(void *pvParameters);

// Deletes one of the two tasks that are using `client_handler_data`.
// See comments inside the function for more details.
static void delete_serve_client_task(
    client_handler_data_t *client_handler_data);

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
  if (xSemaphoreTake(server_status_mutex, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Unreachable: couldn't acquire server_status_mutex.");
    return ESP_FAIL;
  }

  memcpy(status_out, &server_status, sizeof(server_status));
  xSemaphoreGive(server_status_mutex);
  return ESP_OK;
}

esp_err_t socketcand_server_start(uint16_t port) {
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

  // Bind the TCP socket
  struct sockaddr_in server_addr = {0};
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
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

  // Log that we've started listening.
  ESP_LOGD(TAG, "Started socketcand TCP server listening on %s:%d",
           inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));

  // Initialize the mutex for accessing the `server_status` struct.
  server_status_mutex = xSemaphoreCreateMutexStatic(&server_status_mutex_mem);
  if (server_status_mutex == NULL) {
    ESP_LOGE(TAG, "Unreachable. server_status_mutex couldn't be created.");
    return ESP_FAIL;
  }

  // Initialize `client_handler_datas`.
  for (int i = 0; i < MAX_CLIENTS; i++) {
    client_handler_datas[i].can_rx_queue = xQueueCreateStatic(
        CAN_RX_QUEUE_LENGTH, sizeof(socketcand_translate_frame_t),
        client_handler_datas[i].can_rx_queue_storage,
        &client_handler_datas[i].can_rx_queue_buffer);

    if (client_handler_datas[i].can_rx_queue == NULL) {
      ESP_LOGE(TAG, "Unreachable. A can_rx_queue couldn't be created.");
      return ESP_FAIL;
    }

    client_handler_datas[i].tcp_messenger.socket_fd = -1;

    client_handler_datas[i].handler_task_delete_mutex =
        xSemaphoreCreateMutexStatic(
            &client_handler_datas[i].handler_task_delete_mutex_mem);
    if (client_handler_datas[i].handler_task_delete_mutex == NULL) {
      ESP_LOGE(TAG,
               "Unreachable. A handler_task_delete_mutex couldn't be created.");
      return ESP_FAIL;
    }
  }

  // initialize the queue that holds all the unused client_handler_data
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

  for (int i = 0; i < MAX_CLIENTS; i++) {
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

  // Task that continuously pushes messages from the CAN bus
  // onto all the active can_rx_queues.
  xTaskCreateStatic(can_listener_task, "can_listener_task",
                    sizeof(can_listener_task_stack), NULL, 14,
                    can_listener_task_stack, &can_listener_task_mem);

  // Task that continuously listens for incoming TCP connections.
  // pvParameters is set to the listener socket FD.
  xTaskCreateStatic(run_server_task, "socketcand_server",
                    sizeof(run_server_task_stack), (void *)listen_sock, 6,
                    run_server_task_stack, &run_server_task_mem);

  return ESP_OK;
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
    ESP_LOGI(TAG, "Accepted socketcand client TCP connectrion from: %s",
             inet_ntoa(source_addr.sin_addr));

    // Assign a pointer to an unused `client_handler_data_t` to the task
    // about to be spawned.
    client_handler_data_t *client_handler_data = NULL;

    BaseType_t res = xQueueReceive(unused_client_handler_data_queue,
                                   &client_handler_data, 0);

    // if all the handler data is already in use
    if (res != pdTRUE) {
      ESP_LOGE(TAG,
               "Dropping socketcand TCP connection because reached limit of %d "
               "simultaneous clients.",
               MAX_CLIENTS);
      shutdown(client_sock, 0);
      close(client_sock);
      continue;
    }

    // set up the frame messenger and queue for this index
    client_handler_data->tcp_messenger.l = 0;
    client_handler_data->tcp_messenger.r = 0;
    client_handler_data->tcp_messenger.socket_fd = client_sock;

    xQueueReset(client_handler_data->can_rx_queue);

    // spawn a thread to serve the client with this index
    xTaskCreateStatic(serve_client_task, "serving_socketcand_client",
                      sizeof(client_handler_data->free_rtos_stack_1),
                      (void *)client_handler_data, 10,
                      client_handler_data->free_rtos_stack_1,
                      &client_handler_data->free_rtos_mem_1);
  }
  vTaskDelete(NULL);
}

static void serve_client_task(void *pvParameters) {
  client_handler_data_t *client_handler_data =
      (client_handler_data_t *)pvParameters;

  // Establish a socketcand rawmode connection
  char frame_str[SOCKETCAND_RAW_MAX_LEN] = "";
  while (true) {
    // write a handshake frame
    int32_t phase = socketcand_translate_open_raw(frame_str, sizeof(frame_str));
    frame_io_write_str(client_handler_data->tcp_messenger.socket_fd, frame_str);

    if (phase == -1) {
      ESP_LOGE(TAG,
               "Buffer too small when negotiating socketcand rawmode. Closing "
               "connection.");
      delete_serve_client_task(client_handler_data);
      return;
    } else if (phase == 0) {
      ESP_LOGE(TAG,
               "Client sent unknown socketcand message '%s 'while negotiating "
               "rawmode. "
               "Closing connection.",
               frame_str);
      // Increment the server status error counter
      xSemaphoreTake(server_status_mutex, portMAX_DELAY);
      server_status.invalid_socketcand_frames_received += 1;
      xSemaphoreGive(server_status_mutex);

      delete_serve_client_task(client_handler_data);
      return;
    } else if (phase == 3) {
      break;  // socketcand rawmode successfully established
    }

    // read the next < > frame from the client
    esp_err_t err = frame_io_read_next_frame(
        &client_handler_data->tcp_messenger, frame_str, sizeof(frame_str));
    if (err != ESP_OK) {
      ESP_LOGI(
          TAG,
          "Error reading socketcand frame from client. Closing connection.");
      delete_serve_client_task(client_handler_data);
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
    // Try to read the next < > frame from the network.
    esp_err_t err = frame_io_read_next_frame(
        &client_handler_data->tcp_messenger, frame_str, sizeof(frame_str));
    if (err != ESP_OK) {
      ESP_LOGD(TAG, "Couldn't read the next < > frame from socketcand.");
      delete_serve_client_task(client_handler_data);
      return;
    }

    // Parse the message
    socketcand_translate_frame_t received_msg;
    if (socketcand_translate_string_to_frame(frame_str, &received_msg) == -1) {
      ESP_LOGE(TAG, "Couldn't parse socketcand frame from client.");
      // Increment the server status error counter
      xSemaphoreTake(server_status_mutex, portMAX_DELAY);
      server_status.invalid_socketcand_frames_received += 1;
      xSemaphoreGive(server_status_mutex);

      delete_serve_client_task(client_handler_data);
      return;
    }

    // Increment the server status can bus counter
    xSemaphoreTake(server_status_mutex, portMAX_DELAY);
    server_status.socketcand_frames_received += 1;
    xSemaphoreGive(server_status_mutex);

    // Send the message to other TCP socketcand clients.
    enqueue_can_message(&received_msg, client_handler_data);

    // Send the frame over CAN
    twai_message_t twai_message = {0};
    memcpy(twai_message.data, received_msg.data, received_msg.len);
    twai_message.data_length_code = received_msg.len;
    twai_message.extd = received_msg.ext;
    twai_message.identifier = received_msg.id;

    esp_err_t can_res = twai_transmit(&twai_message, 0);

    if (can_res == ESP_OK) {
      // Increment the server status can bus counter
      xSemaphoreTake(server_status_mutex, portMAX_DELAY);
      server_status.can_bus_frames_sent += 1;
      xSemaphoreGive(server_status_mutex);
    } else {
      ESP_LOGE(TAG, "Couldn't transmit frame to CAN. %s",
               esp_err_to_name(can_res));

      xSemaphoreTake(server_status_mutex, portMAX_DELAY);
      server_status.can_bus_frames_send_fails += 1;
      xSemaphoreGive(server_status_mutex);
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
    // read a frame from the CAN bus queue
    socketcand_translate_frame_t frame;
    BaseType_t res =
        xQueueReceive(client_handler_data->can_rx_queue, &frame, portMAX_DELAY);
    if (res != pdTRUE) {
      // Should be unreachable
      ESP_LOGE(TAG, "Couldn't receive CAN bus frame from queue. Panicking.");
      abort();
    }

    // If received a special frame that means we should
    // disconnect from the client.
    if (frame.user_data == SOCKETCAND_INTERRUPT_FRAME) {
      delete_serve_client_task(client_handler_data);
      return;
    }

    int64_t micros = esp_timer_get_time();
    int64_t secs = micros / 1000000;
    int64_t usecs = micros % 1000000;

    // write the message to TCP
    if (socketcand_translate_frame_to_string(buf, sizeof(buf), &frame, secs,
                                             usecs) != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't translate CAN frame to socketcand < > string.");
      delete_serve_client_task(client_handler_data);
      return;
    }
    if (frame_io_write_str(client_handler_data->tcp_messenger.socket_fd, buf) !=
        ESP_OK) {
      ESP_LOGD(TAG, "Error sending socketcand frame to client over TCP.");
      delete_serve_client_task(client_handler_data);
      return;
    }

    // Increment the server status socketcand sent counter
    xSemaphoreTake(server_status_mutex, portMAX_DELAY);
    server_status.socketcand_frames_sent += 1;
    xSemaphoreGive(server_status_mutex);
  }

  delete_serve_client_task(client_handler_data);
  return;
}

static void can_listener_task(void *pvParameters) {
  while (true) {
    // receive a message from the CAN bus
    twai_message_t received_msg;
    esp_err_t res;
    while ((res = twai_receive(&received_msg, portMAX_DELAY)) != ESP_OK) {
      ESP_LOGE(TAG, "Error receiving message from CAN bus: %s",
               esp_err_to_name(res));
    }

    // Increment the server status can bus counter
    xSemaphoreTake(server_status_mutex, portMAX_DELAY);
    server_status.can_bus_frames_received += 1;
    xSemaphoreGive(server_status_mutex);

    // put the message in a socketcand struct
    socketcand_translate_frame_t frame = {0};
    memcpy(frame.data, received_msg.data, received_msg.data_length_code);
    frame.ext = received_msg.extd;
    frame.id = received_msg.identifier;
    frame.len = received_msg.data_length_code;
    frame.user_data = SOCKETCAND_NORMAL_FRAME;

    enqueue_can_message(&frame, NULL);
  }
}

static void enqueue_can_message(socketcand_translate_frame_t *message,
                                const client_handler_data_t *skip_client_i) {
  // send the message to all the tasks currently serving clients
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (&client_handler_datas[i] != skip_client_i &&
        client_handler_datas[i].tcp_messenger.socket_fd != -1) {
      // append the message to the queue
      if (xQueueSend(client_handler_datas[i].can_rx_queue, message, 0) !=
          pdTRUE) {
        ESP_LOGE(TAG, "CAN bus task receive queue %d full. Dropping message.",
                 i);
      }
    }
  }
}

static void delete_serve_client_task(
    client_handler_data_t *client_handler_data) {
  xSemaphoreTake(client_handler_data->handler_task_delete_mutex, portMAX_DELAY);

  // If socket_fd hasn't already been set to -1, that means
  // I'm the first task to notice the client disconnected.
  if (client_handler_data->tcp_messenger.socket_fd != -1) {
    // Gracefully shutdown the socket that the client is connected to.
    shutdown(client_handler_data->tcp_messenger.socket_fd, 0);
    close(client_handler_data->tcp_messenger.socket_fd);
    client_handler_data->tcp_messenger.socket_fd = -1;

    // Send an interrupt frame to `can_rx_queue` so if the other task
    // is blocking on receiving `can_rx_queue`, it knows to stop.
    socketcand_translate_frame_t termination_frame;
    termination_frame.user_data = SOCKETCAND_INTERRUPT_FRAME;
    xQueueSend(client_handler_data->can_rx_queue, &termination_frame, 0);

  } else {
    // Else, the other task has already disconnected the client.
    // Now I'll just return the `client_handler_data` back to the
    // `unused_client_handler_data_queue`.
    BaseType_t res =
        xQueueSend(unused_client_handler_data_queue, &client_handler_data, 0);
    if (res != pdTRUE) {
      ESP_LOGE(
          TAG,
          "Unreachable. Couldn't return client_handler_data to unused queue.");
      abort();
    }
    ESP_LOGI(TAG, "Socketcand client disconnected.");
  }

  xSemaphoreGive(client_handler_data->handler_task_delete_mutex);

  vTaskDelete(NULL);
  return;
}
