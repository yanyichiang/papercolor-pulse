#pragma once

#include <cstdint>

namespace voice_capture {

enum class Event {
    None,
    Started,
    Saved,
    Failed,
};

Event update(bool toggle_requested);
bool active();

}  // namespace voice_capture
