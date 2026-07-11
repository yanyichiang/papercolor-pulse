/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

/** @brief Enables or disables automatic shutdown of the Wi-Fi AP.
 *
 * The AP is brought up at boot so the user can do initial WiFi onboarding
 * via the captive portal. Once STA settles on the user's home network and
 * no phone is currently connected to the AP, the AP is turned off so the
 * device stops broadcasting an open SSID.
 */
#ifndef WIFI_AP_AUTO_OFF_ENABLE
#define WIFI_AP_AUTO_OFF_ENABLE (1)
#endif

/**
 * @brief Timeout in minutes before the Wi-Fi AP is turned off automatically.
 *
 * The timeout only applies when no client is connected to the AP and the
 * STA interface is connected. Kept short so the device drops the AP almost
 * immediately once it has settled on the user's home Wi-Fi.
 */
#define WIFI_AP_AUTO_OFF_TIMEOUT_MIN (1)

/** @brief Current application software version. */
#define APP_SW_VERSION "1.1.0"

/**
 * @brief Displays the boot guide image.
 */
void display_boot_guide_image(void);

/**
 * @brief Restores factory settings and reinitializes runtime state.
 */
void app_manager_factory_reset_machine(void);

/**
 * @brief Starts the application manager.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if startup fails
 */
esp_err_t app_manager_start(void);

/**
 * @brief Applies the requested mode through the mode switch workflow.
 *
 * @param mode_id Mode identifier to apply.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if the mode cannot be applied
 */
esp_err_t app_manager_apply_mode(const char* mode_id);

/**
 * @brief Updates the current mode record without running the full switch flow.
 *
 * @param mode_id Mode identifier to store.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if the mode cannot be stored
 */
esp_err_t app_manager_set_current_mode(const char* mode_id);

/**
 * @brief Pauses automatic Wi-Fi reconnect attempts.
 */
void app_manager_pause_wifi_reconnect(void);

/**
 * @brief Resumes automatic Wi-Fi reconnect attempts.
 */
void app_manager_resume_wifi_reconnect(void);

/**
 * @brief Disconnects the STA interface while keeping the AP enabled.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code if disconnect fails
 */
esp_err_t app_manager_disconnect_sta_keep_ap(void);

/**
 * @brief Marks the app manager as having recent user or system activity.
 */
void app_manager_mark_activity(void);

/**
 * @brief Updates the refresh-in-progress state.
 *
 * @param in_progress True while a refresh is running, otherwise false.
 */
void app_manager_set_refresh_in_progress(bool in_progress);

/** @brief Prevents low-power shutdown while a request or storage transaction is active. */
void app_manager_operation_begin(void);

/** @brief Releases one active request or storage transaction guard. */
void app_manager_operation_end(void);
