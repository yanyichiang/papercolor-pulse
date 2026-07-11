#include "voice_capture.h"

#include <M5Unified.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "hal/storage/hal_storage.h"

namespace voice_capture {
namespace {

constexpr const char* TAG        = "VoiceCapture";
constexpr const char* ROOT_DIR   = "/data/.papercolor";
constexpr const char* OUTBOX_DIR = "/data/.papercolor/voice-outbox";
constexpr uint32_t SAMPLE_RATE   = 16000;
constexpr size_t BLOCK_SAMPLES   = 1024;
constexpr size_t MAX_SAMPLES     = SAMPLE_RATE * 60;
constexpr int64_t STOP_GUARD_US  = 1200 * 1000;
enum class State { Idle, Recording, Stopping };

State state                  = State::Idle;
bool block_pending           = false;
size_t samples_written       = 0;
int64_t recording_started_us = 0;
FILE* output                 = nullptr;
char temporary_path[176]     = {};
char final_path[160]         = {};
int16_t block[BLOCK_SAMPLES] = {};

void write_u16(FILE* stream, uint16_t value)
{
    uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    fwrite(bytes, 1, sizeof(bytes), stream);
}

void write_u32(FILE* stream, uint32_t value)
{
    uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    fwrite(bytes, 1, sizeof(bytes), stream);
}

bool write_wav_header(FILE* stream, uint32_t sample_count)
{
    if (fseek(stream, 0, SEEK_SET) != 0) return false;
    uint32_t data_bytes = sample_count * sizeof(int16_t);
    fwrite("RIFF", 1, 4, stream);
    write_u32(stream, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, stream);
    write_u32(stream, 16);
    write_u16(stream, 1);
    write_u16(stream, 1);
    write_u32(stream, SAMPLE_RATE);
    write_u32(stream, SAMPLE_RATE * sizeof(int16_t));
    write_u16(stream, sizeof(int16_t));
    write_u16(stream, 16);
    fwrite("data", 1, 4, stream);
    write_u32(stream, data_bytes);
    return ferror(stream) == 0;
}

void restore_audio()
{
    M5.Mic.end();
    M5.Speaker.begin();
}

void reset_state()
{
    output               = nullptr;
    block_pending        = false;
    samples_written      = 0;
    recording_started_us = 0;
    state                = State::Idle;
    temporary_path[0]    = '\0';
    final_path[0]        = '\0';
}

Event fail_capture(const char* message)
{
    ESP_LOGE(TAG, "%s", message);
    restore_audio();
    if (output) fclose(output);
    if (temporary_path[0]) unlink(temporary_path);
    hal_storage_unlock();
    reset_state();
    return Event::Failed;
}

bool queue_block()
{
    block_pending = M5.Mic.record(block, BLOCK_SAMPLES, SAMPLE_RATE, false);
    return block_pending;
}

Event begin_capture()
{
    if (!hal.isSDCardInserted()) return Event::Failed;
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    if ((mkdir(ROOT_DIR, 0750) != 0 && errno != EEXIST) || (mkdir(OUTBOX_DIR, 0750) != 0 && errno != EEXIST)) {
        hal_storage_unlock();
        return Event::Failed;
    }
    int64_t epoch = hal.rtcEpochUtc();
    snprintf(final_path, sizeof(final_path), "%s/pc-%lld-%08lx.wav", OUTBOX_DIR, static_cast<long long>(epoch),
             static_cast<unsigned long>(esp_random()));
    snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", final_path);
    output = fopen(temporary_path, "wb+");
    if (!output) {
        hal_storage_unlock();
        return Event::Failed;
    }
    if (!write_wav_header(output, 0)) return fail_capture("failed to write WAV header");

    M5.Speaker.end();
    if (!M5.Mic.begin()) return fail_capture("microphone start failed");
    int16_t warmup[BLOCK_SAMPLES] = {};
    if (!M5.Mic.record(warmup, BLOCK_SAMPLES, SAMPLE_RATE, false)) {
        return fail_capture("microphone warmup queue failed");
    }
    uint32_t started = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    while (M5.Mic.isRecording() && static_cast<uint32_t>(esp_timer_get_time() / 1000ULL) - started < 500) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (M5.Mic.isRecording() || !queue_block()) return fail_capture("microphone capture queue failed");
    state                = State::Recording;
    recording_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "recording started: %s", final_path);
    return Event::Started;
}

Event finalize_capture()
{
    if (!write_wav_header(output, samples_written) || fflush(output) != 0 || fsync(fileno(output)) != 0) {
        return fail_capture("failed to finalize WAV file");
    }
    if (fclose(output) != 0) {
        output = nullptr;
        return fail_capture("failed to close WAV file");
    }
    output = nullptr;
    restore_audio();
    if (samples_written == 0 || rename(temporary_path, final_path) != 0) {
        unlink(temporary_path);
        hal_storage_unlock();
        reset_state();
        return Event::Failed;
    }
    hal_storage_unlock();
    ESP_LOGI(TAG, "recording saved: %s (%u samples)", final_path, static_cast<unsigned>(samples_written));
    reset_state();
    return Event::Saved;
}

}  // namespace

Event update(bool toggle_requested)
{
    if (state == State::Idle) {
        return toggle_requested ? begin_capture() : Event::None;
    }
    if (block_pending && !M5.Mic.isRecording()) {
        block_pending = false;
        if (fwrite(block, sizeof(int16_t), BLOCK_SAMPLES, output) != BLOCK_SAMPLES) {
            return fail_capture("failed to stream audio to microSD");
        }
        samples_written += BLOCK_SAMPLES;
    }
    bool stop_requested = toggle_requested && esp_timer_get_time() - recording_started_us >= STOP_GUARD_US;
    if (toggle_requested && !stop_requested) {
        ESP_LOGW(TAG, "ignored stop request during recording debounce guard");
    }
    if (state == State::Recording && (stop_requested || samples_written >= MAX_SAMPLES)) {
        state = State::Stopping;
    }
    if (state == State::Recording && !block_pending && !queue_block()) {
        return fail_capture("failed to queue audio block");
    }
    if (state == State::Stopping && !block_pending) return finalize_capture();
    return Event::None;
}

bool active()
{
    return state == State::Recording || state == State::Stopping;
}

}  // namespace voice_capture
