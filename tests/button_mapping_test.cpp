#include "main/apps/app_manager/button_mapping.h"

#include <cassert>

int main()
{
    using papercolor::ButtonAction;
    using papercolor::ButtonId;

    assert(papercolor::short_button_action(ButtonId::SideLowerA) == ButtonAction::NextPhoto);
    assert(papercolor::short_button_action(ButtonId::SideUpperB) == ButtonAction::PreviousPhoto);
    assert(papercolor::short_button_action(ButtonId::TopC) == ButtonAction::SyncGateway);
    return 0;
}
