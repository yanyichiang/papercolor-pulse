#pragma once

namespace papercolor {

constexpr bool requires_wifi_setup(bool connected)
{
    return !connected;
}

}  // namespace papercolor
