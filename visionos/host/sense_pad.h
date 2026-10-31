#pragma once

// Window mode's PS VR2 Sense controllers as one gamepad. Outside a Full Space visionOS gives an
// app no controller tracking, but GameController still reports each half's buttons and sticks
// (visionOS 26), so the two halves can play as one pad. From the TPVR port
// (src/dusk/visionos/visionos_sense_pad.mm), which feeds them to SDL as a virtual gamepad.
// The immersive modes read the controllers through the OpenXR provider instead.

#include <cstdint>

namespace sense_pad {

// A standard gamepad's buttons, in SDL's order and names.
enum Button : uint32_t {
    South = 1u << 0,  // Cross
    East = 1u << 1,   // Circle
    West = 1u << 2,   // Square
    North = 1u << 3,  // Triangle
    Back = 1u << 4,   // Create
    Guide = 1u << 5,
    Start = 1u << 6,  // Options
    LeftStick = 1u << 7,
    RightStick = 1u << 8,
    LeftShoulder = 1u << 9,   // L1
    RightShoulder = 1u << 10, // R1
    DpadUp = 1u << 11,
    DpadDown = 1u << 12,
    DpadLeft = 1u << 13,
    DpadRight = 1u << 14,
};

struct State {
    bool connected = false;  // either half
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;  // -1..1, +y up
    float left_trigger = 0, right_trigger = 0;               // 0..1 (L2, R2)
    uint32_t buttons = 0;                                    // Button bits
};

// Both halves, read now, as one gamepad. The Sense controllers have no d-pad: Layout() in
// sense_pad.mm maps spare buttons to it where the game needs one (TPVR: L1 up, Create left, L3
// down, R3 right). Call once a frame on the game's thread. Never connected before visionOS 26.
State read();

// SDL3 games: the state as an SDL virtual gamepad, player 1 unless a real gamepad holds it, and
// SDL's own view of the Sense halves (if any) kept out of play. Call once a frame, then drop the
// events ignores() names. Returns the virtual joystick's id, or 0.
uint32_t update_sdl_gamepad();
// Whether an SDL event comes from a Sense half's own device (not the joined pad): drop it.
bool ignores(const void* sdl_event);

}  // namespace sense_pad
