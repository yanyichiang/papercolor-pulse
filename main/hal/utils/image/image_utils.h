/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>

bool get_image_size_from_memory(const uint8_t* data, size_t len, int* width, int* height, const char* ext);

bool get_image_size_from_file(const char* path, int* width, int* height);
