#pragma once

namespace papercolor {

enum class ButtonId {
    SideLowerA,
    SideUpperB,
    TopC,
};

enum class ButtonAction {
    NextPhoto,
    PreviousPhoto,
    SyncGateway,
};

constexpr ButtonAction short_button_action(ButtonId button)
{
    switch (button) {
        case ButtonId::SideLowerA:
            return ButtonAction::NextPhoto;
        case ButtonId::SideUpperB:
            return ButtonAction::PreviousPhoto;
        case ButtonId::TopC:
            return ButtonAction::SyncGateway;
    }
    return ButtonAction::SyncGateway;
}

}  // namespace papercolor
