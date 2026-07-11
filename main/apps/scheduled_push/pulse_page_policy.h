#pragma once

#include <cstddef>

namespace papercolor {

constexpr size_t next_pulse_index(size_t current, size_t count)
{
    return count == 0 ? 0 : (current + 1) % count;
}

constexpr size_t previous_pulse_index(size_t current, size_t count)
{
    return count == 0 ? 0 : (current + count - 1) % count;
}

}  // namespace papercolor
