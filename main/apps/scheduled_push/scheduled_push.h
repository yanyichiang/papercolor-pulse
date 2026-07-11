#pragma once

#include <cstdint>

class PhotoSlideshow;

namespace scheduled_push {

bool enabled();
void reset_state();
bool display_due_cached(PhotoSlideshow& slideshow);
bool sync_now(PhotoSlideshow& slideshow);
bool show_previous_pulse(PhotoSlideshow& slideshow);
bool show_next_pulse(PhotoSlideshow& slideshow);
bool show_current_pulse(PhotoSlideshow& slideshow);
bool periodic_sync_due(uint32_t now_ms);
void defer_periodic_sync(uint32_t now_ms);
int64_t next_wake_epoch();
bool schedule_next_wake_and_power_off();

}  // namespace scheduled_push
