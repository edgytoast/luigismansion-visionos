// Window mode's Sense controllers as one gamepad: see sense_pad.h. Objective-C++ with ARC.

#include "sense_pad.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#if __has_include(<SDL3/SDL_joystick.h>)
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#define SENSE_PAD_SDL 1
#endif

namespace sense_pad {
namespace {

struct Half {
    bool present = false;
    float stickX = 0, stickY = 0, trigger = 0, grip = 0;
    bool face1 = false, face2 = false, stickClick = false, menu = false;
};

float ButtonValue(GCPhysicalInputProfile* profile, NSArray<NSString*>* names) {
    for (NSString* name in names) {
        if (GCControllerButtonInput* button = profile.buttons[name]) {
            return button.value;
        }
    }
    return 0.0f;
}

GCControllerDirectionPad* Stick(GCPhysicalInputProfile* profile, NSArray<NSString*>* names) {
    for (NSString* name in names) {
        if (GCControllerDirectionPad* stick = profile.dpads[name]) {
            return stick;
        }
    }
    return nil;
}

// Each half names its face buttons "Button A"/"Button B" (X/Y on the left), as the OpenXR
// provider reads them.
API_AVAILABLE(visionos(26.0))
void Read(GCController* controller, bool left, Half& half) {
    GCPhysicalInputProfile* profile = controller.physicalInputProfile;
    if (profile == nil) {
        return;
    }
    half.present = true;
    GCControllerDirectionPad* stick =
        Stick(profile, @[left ? GCInputLeftThumbstick : GCInputRightThumbstick, GCInputThumbstick]);
    half.stickX = stick.xAxis.value;
    half.stickY = stick.yAxis.value;
    half.face1 = ButtonValue(profile, left ? @[GCInputButtonX, GCInputButtonA] : @[GCInputButtonA]) > 0.5f;
    half.face2 = ButtonValue(profile, left ? @[GCInputButtonY, GCInputButtonB] : @[GCInputButtonB]) > 0.5f;
    half.trigger = ButtonValue(profile, @[left ? GCInputLeftTrigger : GCInputRightTrigger, GCInputTrigger]);
    half.grip = ButtonValue(profile, @[@"Grip", left ? GCInputLeftShoulder : GCInputRightShoulder,
                                       left ? GCInputLeftBumper : GCInputRightBumper]);
    half.stickClick = ButtonValue(profile, @[left ? GCInputLeftThumbstickButton : GCInputRightThumbstickButton,
                                             GCInputThumbstickButton]) > 0.5f;
    half.menu = ButtonValue(profile, left ? @[GCInputButtonShare, GCInputButtonMenu, GCInputButtonOptions]
                                          : @[GCInputButtonOptions, GCInputButtonMenu]) > 0.5f;
}

// --- Adapt: the layout for the game. A standard pad's; the Sense halves have no d-pad (TPVR, a
// GameCube game with no L1 or Back, put it on L1 up, Create left, L3 down, R3 right).
uint32_t Layout(const Half& left, const Half& right) {
    uint32_t buttons = 0;
    const auto set = [&](bool on, Button button) {
        if (on) {
            buttons |= button;
        }
    };
    set(right.face1, South);
    set(right.face2, East);
    set(left.face1, West);
    set(left.face2, North);
    set(left.grip > 0.5f, LeftShoulder);
    set(right.grip > 0.5f, RightShoulder);
    set(right.menu, Start);
    set(left.menu, Back);
    set(left.stickClick, LeftStick);
    set(right.stickClick, RightStick);
    return buttons;
}

API_AVAILABLE(visionos(26.0))
State ReadHalves() {
    State state;
    @autoreleasepool {
        Half halves[2];
        GCController* unnamed[2] = {nil, nil};
        int unnamedCount = 0;
        for (GCController* controller in GCController.controllers) {
            if (![controller.productCategory isEqualToString:GCProductCategorySpatialController]) {
                continue;
            }
            NSString* name = controller.vendorName;
            if ([name hasSuffix:@"(L)"]) {
                Read(controller, true, halves[0]);
            } else if ([name hasSuffix:@"(R)"]) {
                Read(controller, false, halves[1]);
            } else if (unnamedCount < 2) {
                unnamed[unnamedCount++] = controller;
            }
        }
        // Halves that don't say which they are fill the missing ones, left first.
        for (int i = 0; i < unnamedCount; ++i) {
            const int hand = !halves[0].present ? 0 : 1;
            if (!halves[hand].present) {
                Read(unnamed[i], hand == 0, halves[hand]);
            }
        }
        const Half& left = halves[0];
        const Half& right = halves[1];
        state.connected = left.present || right.present;
        state.left_x = left.stickX;
        state.left_y = left.stickY;
        state.right_x = right.stickX;
        state.right_y = right.stickY;
        state.left_trigger = left.trigger;
        state.right_trigger = right.trigger;
        state.buttons = Layout(left, right);
    }
    return state;
}

}  // namespace

State read() {
    // Spatial controllers (and their element names) arrived with visionOS 26.
    if (@available(visionOS 26.0, *)) {
        return ReadHalves();
    }
    return State{};
}

#if SENSE_PAD_SDL
namespace {
SDL_JoystickID g_pad = 0;

// SDL names each Sense half after its GameController vendorName, "… Sense Controller (L)" or
// "(R)". Never test for a bare "Sense": "DualSense Wireless Controller" matches it, and the
// DualSense is then ignored and loses player 1 every frame (TPVR, 2026-10-09).
bool IsSenseHalf(const char* name) {
    return name != nullptr && std::strstr(name, "Sense Controller") != nullptr &&
           std::strstr(name, "DualSense") == nullptr;
}

bool Attach() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.name = "PS VR2 Sense Controllers";
    // Every standard button and axis: with gaps, SDL compacts the indices and writes land on the
    // wrong ones (TPVR).
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    g_pad = SDL_AttachVirtualJoystick(&desc);
    if (g_pad == 0) {
        NSLog(@"[LMVR] sense: attaching the gamepad failed: %s", SDL_GetError());
        return false;
    }
    NSLog(@"[LMVR] sense: Sense controllers joined as one gamepad");
    return true;
}

// Player 1, unless a real gamepad holds it; SDL's own view of a Sense half none.
void ClaimPort() {
    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (gamepads == nullptr) {
        return;
    }
    bool taken = false;
    for (int i = 0; i < count; ++i) {
        const SDL_JoystickID id = gamepads[i];
        if (id == g_pad) {
            continue;
        }
        SDL_Gamepad* gamepad = SDL_GetGamepadFromID(id);
        if (gamepad == nullptr) {
            continue;
        }
        const char* name = SDL_GetGamepadName(gamepad);
        if (IsSenseHalf(name)) {
            if (SDL_GetGamepadPlayerIndex(gamepad) != -1) {
                SDL_SetGamepadPlayerIndex(gamepad, -1);
            }
            continue;
        }
        taken = taken || SDL_GetGamepadPlayerIndex(gamepad) == 0;
    }
    SDL_free(gamepads);
    SDL_Gamepad* pad = SDL_GetGamepadFromID(g_pad);
    if (pad != nullptr && !taken && SDL_GetGamepadPlayerIndex(pad) != 0) {
        SDL_SetGamepadPlayerIndex(pad, 0);
    }
}
}  // namespace

uint32_t update_sdl_gamepad() {
    const State state = read();
    if (!state.connected) {
        if (g_pad != 0) {
            SDL_DetachVirtualJoystick(g_pad);
            g_pad = 0;
            NSLog(@"[LMVR] sense: Sense controllers gone");
        }
        return 0;
    }
    if (g_pad == 0 && !Attach()) {
        return 0;
    }
    // Opened once SDL reports it added (at most a frame later).
    SDL_Joystick* joystick = SDL_GetJoystickFromID(g_pad);
    if (joystick == nullptr) {
        return g_pad;
    }
    const auto axis = [](float v) { return static_cast<Sint16>(std::clamp(v, -1.f, 1.f) * 32767.f); };
    // A virtual gamepad's trigger reads the whole axis range: released is the minimum (written as 0,
    // it read half pressed, and the game's trigger holds never let go).
    const auto trigger = [](float v) {
        return static_cast<Sint16>(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f) - 32768);
    };
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, axis(state.left_x));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, axis(-state.left_y));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis(state.right_x));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis(-state.right_y));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(state.left_trigger));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger(state.right_trigger));
    for (int button = 0; button <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT; ++button) {
        SDL_SetJoystickVirtualButton(joystick, button, (state.buttons & (1u << button)) != 0);
    }
    ClaimPort();
    // The writes are only staged until SDL next updates its joysticks: now, not a frame later.
    SDL_UpdateJoysticks();
    return g_pad;
}

bool ignores(const void* sdl_event) {
    const SDL_Event& event = *static_cast<const SDL_Event*>(sdl_event);
    SDL_JoystickID which = 0;
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        which = event.gaxis.which;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        which = event.gbutton.which;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        which = event.gdevice.which;
        break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        which = event.jaxis.which;
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        which = event.jbutton.which;
        break;
    default:
        return false;
    }
    if (which == 0 || which == g_pad) {
        return false;
    }
    const char* name = SDL_GetGamepadNameForID(which);
    if (name == nullptr) {
        name = SDL_GetJoystickNameForID(which);
    }
    return IsSenseHalf(name);
}
#else
uint32_t update_sdl_gamepad() {
    return 0;
}

bool ignores(const void*) {
    return false;
}
#endif

}  // namespace sense_pad
