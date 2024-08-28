#pragma once

#include "esp_err.h"
#include "esp_netif.h"

// Sets `json_out` to a pointer to a C-string containing
// the current network and CAN status in JSON format.
//
// Set `eth_netif` and `wifi_netif` to the network netif handles.
// Set them to NULL to mark them as "disabled" in the JSON.
//
// Since the string is in a shared buffer,
// this function will block if another task is also
// currently reading the status.
// Once the caller is done reading the string,
// they must call `driver_setup_release_json_status()` to
// allow other callers to access the status.
esp_err_t status_report_get(const char** json_out, esp_netif_t* eth_netif,
                            esp_netif_t* wifi_netif);

// Must be called once finished reading the buffer
// returned by `driver_setup_get_status_json()`.
esp_err_t status_report_release(void);