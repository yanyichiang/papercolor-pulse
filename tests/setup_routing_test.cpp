#include "main/apps/app_server/setup_routing.h"

#include <cassert>

int main()
{
    assert(papercolor::requires_wifi_setup(false));
    assert(!papercolor::requires_wifi_setup(true));
    return 0;
}
