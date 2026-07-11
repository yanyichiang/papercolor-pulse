/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Describes a Wi-Fi network discovered during scanning.
 */
typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} wifi_network_t;

/**
 * @brief Captures the current device state exposed to the web UI.
 */
typedef struct {
    bool wifi_connected;
    char ip_address[16];
    char current_mode[16];
    char requested_mode[16];
    char current_image[64];
    char connected_ssid[33];
    char conn_err[32];
} device_state_t;

/**
 * @brief Stores the slideshow mode configuration reported by the web UI.
 */
typedef struct {
    char orientation[16];
    bool auto_slideshow;
    int interval_minutes;
    bool low_power_mode;
} mode1_config_t;

/**
 * @brief Starts the HTTP server and related background services.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if initialization fails
 */
esp_err_t app_server_init(void);

/**
 * @brief Stops the HTTP server and releases its resources.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if shutdown fails
 */
esp_err_t app_server_stop(void);

/**
 * @brief Synchronizes the server-side mode state with the active mode.
 *
 * @param mode_id Mode identifier to publish to the web layer.
 */
void app_server_sync_mode(const char* mode_id);

/**
 * @brief Returns the latest device state snapshot.
 *
 * @return Current device state.
 */
device_state_t app_server_get_state(void);

/**
 * @brief Returns the current slideshow mode configuration.
 *
 * @return Current mode configuration.
 */
mode1_config_t app_server_get_mode1_config(void);

#ifdef __cplusplus
}
#endif
