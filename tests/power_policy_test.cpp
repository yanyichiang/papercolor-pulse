#include "main/hal/power_policy.h"

#include <cassert>

int main()
{
    assert(!papercolor::should_shutdown_for_low_battery(true, 3000, true, 5000));
    assert(papercolor::should_shutdown_for_low_battery(true, 3000, false, 0));
    assert(!papercolor::should_shutdown_for_low_battery(true, 3800, false, 0));
    assert(!papercolor::should_shutdown_for_low_battery(false, 0, false, 0));
    return 0;
}
