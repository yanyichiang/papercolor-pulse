/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <vector>

namespace audio {

void play_tone(int frequency, double duration_sec = 0.02);

void play_melody(const std::vector<int>& midi_list, double duration_sec = 0.02);

void play_tone_from_midi(int midi, double duration_sec = 0.02);

void play_random_tone(int semitone_shift = 48, double duration_sec = 0.02);

}  // namespace audio