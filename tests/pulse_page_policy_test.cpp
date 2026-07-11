#include "main/apps/scheduled_push/pulse_page_policy.h"

#include <cassert>

int main()
{
    using papercolor::next_pulse_index;
    using papercolor::previous_pulse_index;

    assert(next_pulse_index(0, 3) == 1);
    assert(next_pulse_index(2, 3) == 0);
    assert(previous_pulse_index(0, 3) == 2);
    assert(previous_pulse_index(2, 3) == 1);
    assert(next_pulse_index(0, 0) == 0);
    assert(previous_pulse_index(0, 0) == 0);
    return 0;
}
