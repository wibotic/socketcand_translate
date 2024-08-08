#include "socketcand_server.h"

#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "frame_io.h"
#include "lwip/sockets.h"
#include "socketcand_translate.h"

// Maximum number of TCP socketcand connections
// that can be served simultaneously before new connections
// are dropped.
#define MAX_CLIENTS 5

// Stack size allocated for every FreeRTOS task.
#define STACK_SIZE 4096

// `can_listener_task` pushes frames from the CAN bus
// onto all the active queues.
// Each `serve_client_task` then pops from the queue at its assigned index.
static QueueHandle_t can_bus_queues[MAX_CLIENTS] = {0};

// An array of socketcand messengers.
// Each `server_client_task` uses the one at its assigned index.
// Unused messengers have `socket_fd` set to -1.
static frame_io_messenger task_frame_messengers[MAX_CLIENTS];

// Name that will be used for logging
static const char *TAG = "socketcand_server";

// Task that continuously runs the server
// pvParameters should be a listener socket FD.
static void run_server_task(void *pvParameters);

// Task that continuously pushes messages from the CAN bus
// onto all the active `can_bus_queues`.
static void can_listener_task(void *pvParameters);

// Pushes the message to all of the active `can_bus_queues`.
// This will cause it to be sent to all TCP socketcand clients EXCEPT
// for `skip_client_i`, which will be omitted.
static void enqueue_can_message(socketcand_translate_frame *message,
                                int skip_client_i);

// Task that serves a client.
// pvParameters should be the index of the client.
static void serve_client_task(void *pvParameters);

// Task that forwards messages from TCP to CAN bus.
// pvParameters should be the index of the client.
static void socketcand_to_bus_task(void *pvParameters);

// Task that forwards messages from CAN bus to TCP.
// pvParameters should be the index of the client.
static void bus_to_socketcand_task(void *pvParameters);

// Deletes the the serve client task that called this function
// and resets its `frame_io_messenger` socket fd to -1.
static void delete_serve_client_task(int client_i);

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

  // Log the socket we're listening on
  struct sockaddr_in local_addr;
  socklen_t local_addr_size = sizeof(local_addr);
  err = getsockname(listen_sock, (struct sockaddr *)&local_addr,
                    &local_addr_size);
  if (err != 0) {
    ESP_LOGE(TAG, "Unable to get local socket address: errno %d", errno);
    close(listen_sock);
    return ESP_FAIL;
  }
  char addr_str[128];
  inet_ntoa_r(local_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
  ESP_LOGI(TAG, "Started TCP server listening on %s:%d", addr_str,
           ntohs(local_addr.sin_port));

  // Initialize `can_bus_queues` and `task_frame_messengers`.
  for (int i = 0; i < MAX_CLIENTS; i++) {
    can_bus_queues[i] = xQueueCreate(32, sizeof(socketcand_translate_frame));
    task_frame_messengers[i].socket_fd = -1;
  }

  // Task that continuously pushes messages from the CAN bus
  // onto all the active `can_bus_queues`.
  xTaskCreate(can_listener_task, "can_listener_task", STACK_SIZE,
              (void *)listen_sock, 2, NULL);

  // Task that continuously runs the server
  // pvParameters is set to the listener socket FD.
  xTaskCreate(run_server_task, "socketcand_server", STACK_SIZE,
              (void *)listen_sock, 1, NULL);

  return ESP_OK;
}

static void run_server_task(void *pvParameters) {
  int listen_sock = (int)pvParameters;

  // Run the server
  while (true) {
    // Accept an incoming connection.
    struct sockaddr_in6 source_addr;
    socklen_t addr_len = sizeof(source_addr);
    int client_sock =
        accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
    if (client_sock < 0) {
      ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
      continue;
    }

    // Log the origin of the incoming connection
    char addr_str[128];
    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str,
                sizeof(addr_str));
    ESP_LOGI(TAG, "Accepted socketcand client TCP connectrion from: %s",
             addr_str);

    // Find a free index to assign to this connection
    int client_i = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (task_frame_messengers[i].socket_fd == -1) {
        client_i = i;
        break;
      }
    }

    // drop the connection if no free index is available
    if (client_i == -1) {
      ESP_LOGE(TAG,
               "Dropping socketcand TCP connection because reached limit of %d "
               "simultaneous clients.",
               MAX_CLIENTS);
      shutdown(client_sock, 0);
      close(client_sock);
      continue;
    }

    // set up the frame messenger and queue for this index
    task_frame_messengers[client_i].l = 0;
    task_frame_messengers[client_i].r = 0;
    task_frame_messengers[client_i].socket_fd = client_sock;
    xQueueReset(can_bus_queues[client_i]);

    // spawn a thread to serve the client with this index
    xTaskCreate(serve_client_task, "serving_socketcand_client", STACK_SIZE,
                (void *)client_i, 15, NULL);
  }
  vTaskDelete(NULL);
}

static void serve_client_task(void *pvParameters) {
  int client_i = (int)pvParameters;

  // Establish a socketcand rawmode connection
  char frame_str[SOCKETCAND_BUF_LEN] = "";
  while (true) {
    // write a handshake frame
    int phase = socketcand_translate_open_raw(frame_str);
    frame_io_write_str(task_frame_messengers[client_i].socket_fd, frame_str);

    if (phase == -1) {
      ESP_LOGE(TAG,
               "Unexpected socketcand message while negotiating rawmode. "
               "Closing connection.");
      delete_serve_client_task(client_i);
      return;
    } else if (phase == 3) {
      break;  // socketcand rawmode successfully established
    }

    // read a frame from the client
    if (frame_io_read_next_frame(&task_frame_messengers[client_i], frame_str) ==
        -1) {
      ESP_LOGI(
          TAG,
          "Error reading socketcand frame from client. Closing connection.");
      delete_serve_client_task(client_i);
      return;
    }
  }

  // run translation in both directions simultaneously
  xTaskCreate(bus_to_socketcand_task, "bus_to_socketcand", STACK_SIZE,
              pvParameters, 15, NULL);
  xTaskCreate(socketcand_to_bus_task, "socketcand_to_bus", STACK_SIZE,
              pvParameters, 15, NULL);

  // delete own task
  vTaskDelete(NULL);
}

static void socketcand_to_bus_task(void *pvParameters) {
  int client_i = (int)pvParameters;

  // A single C string storing a complete frame.
  char frame_str[SOCKETCAND_BUF_LEN];

  while (true) {
    // Try to read the next < > frame from the network.
    int bytes_read =
        frame_io_read_next_frame(&task_frame_messengers[client_i], frame_str);
    if (bytes_read == -1) {
      ESP_LOGD(TAG, "Couldn't read the next < > frame from socketcand.");
      delete_serve_client_task(client_i);
      return;
    }

    // Parse the message
    socketcand_translate_frame received_msg;
    if (socketcand_translate_string_to_frame(frame_str, &received_msg) == -1) {
      ESP_LOGE(TAG, "Couldn't parse socketcand frame from client.");
      delete_serve_client_task(client_i);
      return;
    }

    // Send the message to other TCP socketcand clients.
    enqueue_can_message(&received_msg, client_i);

    // Send the frame over CAN
    twai_message_t twai_message = {0};
    memcpy(twai_message.data, received_msg.data, received_msg.len);
    twai_message.data_length_code = received_msg.len;
    twai_message.extd = received_msg.ext;
    twai_message.identifier = received_msg.id;

    esp_err_t can_res = twai_transmit(&twai_message, 0);

    if (can_res != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't send frame over CAN. %s",
               esp_err_to_name(can_res));
    }
  }
  delete_serve_client_task(client_i);
  return;
}

static void bus_to_socketcand_task(void *pvParameters) {
  int client_i = (int)pvParameters;

  char buf[SOCKETCAND_BUF_LEN];

  while (true) {
    // read a frame from the CAN bus queue
    socketcand_translate_frame frame;
    assert(xQueueReceive(can_bus_queues[client_i], &frame, portMAX_DELAY) ==
           pdTRUE);

    int64_t micros = esp_timer_get_time();
    int64_t secs = micros / 1000000;
    int64_t usecs = micros % 1000000;

    // write the message to TCP
    if (socketcand_translate_frame_to_string(buf, sizeof(buf), &frame, secs,
                                             usecs) == -1) {
      ESP_LOGE(
          TAG,
          "Couldn't translate CAN frame to socketcand < > string. Errno %d",
          errno);
      delete_serve_client_task(client_i);
      return;
    }
    if (frame_io_write_str(task_frame_messengers[client_i].socket_fd, buf) ==
        -1) {
      ESP_LOGD(TAG,
               "Error sending socketcand frame to client over TCP. Errno: %d",
               errno);
      delete_serve_client_task(client_i);
      return;
    }
  }

  delete_serve_client_task(client_i);
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

    // put the message in a socketcand struct
    socketcand_translate_frame frame = {0};
    memcpy(frame.data, received_msg.data, received_msg.data_length_code);
    frame.ext = received_msg.extd;
    frame.id = received_msg.identifier;
    frame.len = received_msg.data_length_code;

    enqueue_can_message(&frame, -1);
  }
}

static void enqueue_can_message(socketcand_translate_frame *message,
                                int skip_client_i) {
  // send the message to all the tasks currently serving clients
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (i != skip_client_i && task_frame_messengers[i].socket_fd != -1) {
      // append the message to the queue
      if (xQueueSend(can_bus_queues[i], message, 0) != pdTRUE) {
        ESP_LOGE(TAG, "CAN bus task receive queue %d full. Dropping message.",
                 i);
      }
    }
  }
}

static void delete_serve_client_task(int client_i) {
  if (task_frame_messengers[client_i].socket_fd != -1) {
    shutdown(task_frame_messengers[client_i].socket_fd, 0);
    close(task_frame_messengers[client_i].socket_fd);
    task_frame_messengers[client_i].socket_fd = -1;
  }
  vTaskDelete(NULL);
  return;
}