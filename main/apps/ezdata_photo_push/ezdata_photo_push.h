/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/**
 * @brief Ezdata photo push mode controller.
 */
class EzdataPhotoPush {
public:
    /**
     * @brief Initializes the Ezdata photo push mode.
     *
     * @param interval_min Auto-refresh interval in minutes. A value of 0 disables auto-refresh.
     *
     * @return True on success, otherwise false.
     */
    bool init(uint8_t interval_min = 0);

    /**
     * @brief Releases Ezdata mode resources.
     */
    void deinit();

    /**
     * @brief Starts Ezdata mode processing.
     */
    void start();

    /**
     * @brief Stops Ezdata mode processing.
     */
    void stop();

    /**
     * @brief Runs the periodic Ezdata update.
     */
    void update();

    /**
     * @brief Advances to the next image.
     */
    void next();

    /**
     * @brief Moves to the previous image.
     */
    void prev();

    /**
     * @brief Selects an image by index.
     *
     * @param index Zero-based image index.
     */
    void goTo(uint16_t index);

    /**
     * @brief Toggles the display rotation used for image rendering.
     */
    void toggleRotation();

    /**
     * @brief Updates the auto-refresh interval.
     *
     * @param min Auto-refresh interval in minutes. A value of 0 disables auto-refresh.
     */
    void setInterval(uint8_t min);

    /**
     * @brief Updates the settle delay before applying a pending refresh.
     *
     * @param ms Settle time in milliseconds.
     */
    void setSettleTime(uint32_t ms);

    /**
     * @brief Forces a single refresh pass.
     *
     * @return True if a refresh was performed, otherwise false.
     */
    bool runOneShotRefresh();

    /**
     * @brief Returns the number of tracked images.
     *
     * @return Total image count.
     */
    uint16_t getTotal() const;

    /**
     * @brief Returns the index of the image currently shown on screen.
     *
     * @return Current image index.
     */
    uint16_t getCurrentIndex() const;

    /**
     * @brief Returns the index selected for the next refresh.
     *
     * @return Pending image index.
     */
    uint16_t getPendingIndex() const;

    /**
     * @brief Indicates whether Ezdata mode processing is active.
     *
     * @return True if running, otherwise false.
     */
    bool isRunning() const;

    /**
     * @brief Indicates whether the mode is waiting for settle time.
     *
     * @return True if waiting, otherwise false.
     */
    bool isWaitingSettle() const;

private:
    static constexpr uint16_t MAX_PHOTOS                         = 200;
    static constexpr uint32_t DEFAULT_SETTLE_MS                  = 1000;
    static constexpr size_t MAX_IMAGE_SIZE                       = 2 * 1024 * 1024;
    static constexpr uint8_t RX8130_RAM_INDEX_EZDATA_CURRENT     = 2;
    static constexpr uint8_t RX8130_RAM_INDEX_EZDATA_FINGERPRINT = 3;

    int _scr_w = 0;
    int _scr_h = 0;

    uint16_t _current_index = 0;
    uint16_t _pending_index = 0;
    uint8_t _interval_min   = 0;
    uint32_t _settle_ms     = DEFAULT_SETTLE_MS;

    volatile bool _running               = false;
    bool _needs_refresh                  = false;
    bool _force_refresh_for_image_update = false;
    bool _binding_qrcode_scheduled       = false;
    bool _binding_qrcode_shown_once      = false;
    bool _startup_state_initialized      = false;

    uint32_t _last_refresh_ms = 0;
    uint32_t _last_button_ms  = 0;
    bool _last_btn_c          = false;
    bool _last_btn_b          = false;
    bool _last_btn_a          = false;

    void syncSettings();
    int findPhotoIndexByUrl(const char* url) const;
    void syncTrackedIndicesWithList();
    void clampIndices();
    void displayPhoto(uint16_t index);
    void handleButtons();
    void requestRefresh(uint16_t new_index);
};
