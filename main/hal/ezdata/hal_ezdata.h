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
 * @brief Initializes the Ezdata service.
 */
void ezdata_init();

/**
 * @brief Deinitializes the Ezdata service.
 */
void ezdata_deinit();

/**
 * @brief Returns whether Ezdata is currently connected.
 */
bool ezdata_is_connected();

/**
 * @brief Fetches a photo by index.
 *
 * @param index Zero-based photo index.
 * @param out_data Output buffer pointer.
 * @param out_len Output data length.
 * @param max_size Maximum allowed payload size.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code on failure
 */
esp_err_t ezdata_fetch_photo(int index, uint8_t** out_data, size_t* out_len, size_t max_size);

/**
 * @brief Fetches a photo by URL.
 *
 * @param url Remote photo URL.
 * @param out_data Output buffer pointer.
 * @param out_len Output data length.
 * @param max_size Maximum allowed payload size.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code on failure
 */
esp_err_t ezdata_fetch_photo_by_url(const char* url, uint8_t** out_data, size_t* out_len, size_t max_size);

/**
 * @brief Refreshes the remote image link list.
 */
bool getImageFileLinkList();

/**
 * @brief Returns whether a new photo is available.
 */
bool ezdata_has_new_photo();

/**
 * @brief Returns the URL of the newest photo.
 */
const char* ezdata_get_new_photo_url();

/**
 * @brief Clears the new-photo flag.
 */
void ezdata_clear_new_photo_flag();

/**
 * @brief Returns whether an image record is available.
 */
bool ezdata_has_image_record();

/**
 * @brief Returns the persisted image record URL.
 */
const char* ezdata_get_image_record_url();

/**
 * @brief Returns the persisted image record fingerprint.
 */
uint8_t ezdata_get_image_record_fingerprint();

/**
 * @brief Marks the current image record as persisted.
 */
void ezdata_mark_image_record_persisted();

/**
 * @brief Takes and clears the image-update flag.
 */
bool ezdata_take_image_update_flag();

/**
 * @brief Returns the number of available photos.
 */
size_t ezdata_get_photo_count();

/**
 * @brief Returns the photo URL at the given index.
 *
 * @param index Zero-based photo index.
 */
const char* ezdata_get_photo_url(size_t index);

#ifdef __cplusplus
}
#endif
