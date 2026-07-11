/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <M5Unified.h>
#include <random>

namespace audio {

static std::vector<int> c_major_scale = {60, 62, 64, 65, 67, 69, 71};  // C大调音阶（C D E F G A B）

void play_tone(int frequency, double duration_sec)
{
    if (M5.Speaker.getVolume() <= 0) {
        return;
    }

    const int sample_rate = M5.Speaker.config().sample_rate;
    const int samples     = static_cast<int>(sample_rate * duration_sec);
    std::vector<int16_t> buffer(samples * 2);  // 双声道

    const int fade_len    = 200;  // 淡出长度（采样点）
    const float amplitude = 32767.0f / 5;

    for (int i = 0; i < samples; ++i) {
        float amp = amplitude;

        // 应用结尾淡出（fade-out）
        if (i >= samples - fade_len) {
            float fade_factor = static_cast<float>(samples - i) / fade_len;
            amp *= fade_factor;
        }

        int16_t value     = static_cast<int16_t>(amp * sin(2.0 * M_PI * frequency * i / sample_rate));
        buffer[i * 2]     = value;  // 左声道
        buffer[i * 2 + 1] = value;  // 右声道
    }

    M5.Speaker.playRaw(buffer.data(), buffer.size());
}

void play_melody(const std::vector<int>& midi_list, double duration_sec = 0.1)
{
    if (M5.Speaker.getVolume() <= 0) {
        return;
    }

    const int sample_rate      = M5.Speaker.config().sample_rate;
    const int samples_per_note = static_cast<int>(sample_rate * duration_sec);
    const int fade_len         = 200;  // 每个音符结尾的淡出长度
    const float amplitude      = 32767.0f / 5;

    std::vector<int16_t> buffer;                              // 大 buffer 存放整首旋律
    buffer.reserve(midi_list.size() * samples_per_note * 2);  // 双声道预留空间

    for (int midi_note : midi_list) {
        for (int i = 0; i < samples_per_note; ++i) {
            float amp = amplitude;

            // 应用淡出（仅每个音符的结尾）
            if (i >= samples_per_note - fade_len) {
                float fade_factor = static_cast<float>(samples_per_note - i) / fade_len;
                amp *= fade_factor;
            }

            int16_t sample = 0;
            if (midi_note >= 0) {
                double freq = 440.0 * pow(2.0, (midi_note - 69) / 12.0);
                sample      = static_cast<int16_t>(amp * sin(2.0 * M_PI * freq * i / sample_rate));
            }

            buffer.push_back(sample);  // 左声道
            buffer.push_back(sample);  // 右声道
        }
    }

    M5.Speaker.playRaw(buffer.data(), buffer.size());
}

void play_tone_from_midi(int midi, double duration_sec)
{
    if (M5.Speaker.getVolume() <= 0) {
        return;
    }

    double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
    play_tone(static_cast<int>(freq), duration_sec);
}

void play_random_tone(int semitone_shift = 0, double duration_sec = 0.15)
{
    if (M5.Speaker.getVolume() <= 0) {
        return;
    }

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, static_cast<int>(c_major_scale.size()) - 1);

    int index = dist(gen);
    int midi  = c_major_scale[index] + semitone_shift;
    play_tone_from_midi(midi, duration_sec);
}

}  // namespace audio