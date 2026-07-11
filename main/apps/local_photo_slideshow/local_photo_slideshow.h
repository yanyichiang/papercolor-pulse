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
 * @brief Photo slideshow mode controller.
 */
class PhotoSlideshow {
public:
    /**
     * @brief Initializes the slideshow from a directory.
     *
     * @param dir_path Directory containing slideshow images.
     * @param interval_min Auto-play interval in minutes. A value of 0 disables auto-play.
     *
     * @return True on success, otherwise false.
     */
    bool init(const char* dir_path, uint8_t interval_min = 0);

    /**
     * @brief Releases slideshow resources.
     */
    void deinit();

    /**
     * @brief Starts slideshow playback.
     */
    void start();

    /**
     * @brief Stops slideshow playback.
     */
    void stop();

    /**
     * @brief Runs the periodic slideshow update.
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
     * @brief Toggles the display rotation used for slideshow rendering.
     */
    void toggleRotation();

    /**
     * @brief Displays a specific image by file path.
     *
     * @param path Absolute or mounted file path to the image.
     */
    bool displayPhotoByPath(const char* path);

    /**
     * @brief Updates the auto-play interval.
     *
     * @param min Auto-play interval in minutes. A value of 0 disables auto-play.
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
     * @brief Returns the number of discovered images.
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
     * @brief Indicates whether slideshow playback is active.
     *
     * @return True if running, otherwise false.
     */
    bool isRunning() const;

    /**
     * @brief Indicates whether the slideshow is waiting for settle time.
     *
     * @return True if waiting, otherwise false.
     */
    bool isWaitingSettle() const;

private:
    static constexpr uint16_t MAX_PHOTOS              = 500;
    static constexpr uint32_t DEFAULT_SETTLE_MS       = 1000;
    static constexpr size_t DIR_PATH_MAX              = 128;
    static constexpr uint8_t RX8130_RAM_INDEX_CURRENT = 0;

    int _scr_w = 0;
    int _scr_h = 0;

    char _dir_path[DIR_PATH_MAX] = {};  // Stores the directory path for reuse during rescans
    std::vector<std::string> _photo_list;

    uint16_t _current_index = 0;  // Currently displayed on screen
    uint16_t _pending_index = 0;  // Selected by buttons but not yet refreshed
    uint8_t _interval_min   = 0;
    uint32_t _settle_ms     = DEFAULT_SETTLE_MS;

    volatile bool _running = false;
    bool _needs_refresh    = false;

    uint32_t _last_refresh_ms = 0;
    uint32_t _last_button_ms  = 0;
    bool _last_btn_c          = false;
    bool _last_btn_b          = false;
    bool _last_btn_a          = false;
    bool _last_sd_inserted    = false;
    bool _sd_fallback_locked  = false;

    /* ---- Internal helpers ---- */
    void syncSettings();
    bool scanPhotos();      // Rescan using _dir_path
    bool rescanAndClamp();  // Rescan and wrap indices into range
    void clampIndices();    // Wrap indices by _photo_list.size()
    bool isImageFile(const char* name);
    void displayPhoto(uint16_t index);
    bool renderPhotoPath(const char* path);
    void handleButtons();
    void requestRefresh(uint16_t new_index);
};
