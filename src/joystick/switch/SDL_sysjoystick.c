/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#if SDL_JOYSTICK_SWITCH

#include "../SDL_sysjoystick.h"

#include <switch.h>

#define JOYSTICK_COUNT 8

typedef struct SWITCHJoystickState
{
    PadState pad;
    HidAnalogStickState sticks_old[2];
    // Rumble motors (two, one per side, except on a lone Joy-Con) and the
    // motion sensor, for whichever controller style is attached right now.
    HidVibrationDeviceHandle vibration[2];
    int nvibration;
    HidSixAxisSensorHandle sixaxis;
    bool sixaxis_ok;
    bool sensors_on;
    u64 sixaxis_sample;
    HidNpadButton *pad_mapping;
    u32 pad_type;
    u32 pad_type_prev;
    HidNpadStyleTag pad_style;
    HidNpadStyleTag pad_style_prev;
} SWITCHJoystickState;

static SWITCHJoystickState state[JOYSTICK_COUNT];

// Player 1 is also the handheld controller: padInitializeDefault reads both,
// but rumble and motion need the id of the one actually in use.
static HidNpadIdType SWITCH_NpadId(int index)
{
    if (index == 0 && padIsHandheld(&state[0].pad)) {
        return HidNpadIdType_Handheld;
    }
    return (HidNpadIdType)(HidNpadIdType_No1 + index);
}

// The style set can have several bits; vibration and six-axis handles are
// asked for with exactly one.
static HidNpadStyleTag SWITCH_NpadStyle(HidNpadIdType id)
{
    static const HidNpadStyleTag order[] = {
        HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadJoyDual,
        HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight
    };
    const u32 set = hidGetNpadStyleSet(id);
    for (size_t i = 0; i < SDL_arraysize(order); i++) {
        if (set & order[i]) {
            return order[i];
        }
    }
    return (HidNpadStyleTag)0;
}

// (Re)acquire the rumble and motion handles for pad `index`, after it is set up
// and whenever what is attached to it changes.
static void SWITCH_SetupDevices(int index)
{
    SWITCHJoystickState *st = &state[index];
    const HidNpadIdType id = SWITCH_NpadId(index);
    const HidNpadStyleTag style = SWITCH_NpadStyle(id);

    if (st->sixaxis_ok && st->sensors_on) {
        hidStopSixAxisSensor(st->sixaxis);
    }
    st->nvibration = 0;
    st->sixaxis_ok = false;
    if (!style) {
        return;
    }

    const bool single = style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight;
    if (R_SUCCEEDED(hidInitializeVibrationDevices(st->vibration, single ? 1 : 2, id, style))) {
        st->nvibration = single ? 1 : 2;
    }

    // A pair of Joy-Cons has a sensor in each; the right one is the one that
    // gets pointed at things.
    HidSixAxisSensorHandle handles[2];
    const int nsix = style == HidNpadStyleTag_NpadJoyDual ? 2 : 1;
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(handles, nsix, id, style))) {
        st->sixaxis = handles[nsix - 1];
        st->sixaxis_ok = true;
        if (st->sensors_on) {
            hidStartSixAxisSensor(st->sixaxis);
        }
    }
}

static const HidNpadButton pad_mapping_default[] = {
    HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
    HidNpadButton_StickL, HidNpadButton_StickR,
    HidNpadButton_L, HidNpadButton_R,
    HidNpadButton_ZL, HidNpadButton_ZR,
    HidNpadButton_Plus, HidNpadButton_Minus,
    HidNpadButton_Left, HidNpadButton_Up, HidNpadButton_Right, HidNpadButton_Down,
    HidNpadButton_StickLLeft, HidNpadButton_StickLUp, HidNpadButton_StickLRight, HidNpadButton_StickLDown,
    HidNpadButton_StickRLeft, HidNpadButton_StickRUp, HidNpadButton_StickRRight, HidNpadButton_StickRDown,
    HidNpadButton_LeftSL, HidNpadButton_LeftSR, HidNpadButton_RightSL, HidNpadButton_RightSR
};

// left single joycon mapping (start = left stick, select = minus)
static const HidNpadButton pad_mapping_left_joy[] = {
    HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right, HidNpadButton_Up,
    BIT(31), BIT(31),
    BIT(31), BIT(31),
    HidNpadButton_LeftSL, HidNpadButton_LeftSR,
    HidNpadButton_StickL, HidNpadButton_Minus,
    HidNpadButton_StickLUp, HidNpadButton_StickLRight, HidNpadButton_StickLDown, HidNpadButton_StickLLeft,
    BIT(31), BIT(31), BIT(31), BIT(31),
    BIT(31), BIT(31), BIT(31), BIT(31),
    BIT(31), BIT(31), BIT(31), BIT(31)
};

// right single joycon mapping (start = right stick, select = plus)
static const HidNpadButton pad_mapping_right_joy[] = {
    HidNpadButton_X, HidNpadButton_A, HidNpadButton_Y, HidNpadButton_B,
    BIT(31), BIT(31),
    BIT(31), BIT(31),
    HidNpadButton_RightSL, HidNpadButton_RightSR,
    HidNpadButton_StickR, HidNpadButton_Plus,
    HidNpadButton_StickRDown, HidNpadButton_StickRLeft, HidNpadButton_StickRUp, HidNpadButton_StickRRight,
    BIT(31), BIT(31), BIT(31), BIT(31),
    BIT(31), BIT(31), BIT(31), BIT(31),
    BIT(31), BIT(31), BIT(31), BIT(31)
};

/* Function to perform the mapping from device index to the instance id for this index */
static SDL_JoystickID SWITCH_JoystickGetDeviceInstanceID(int device_index)
{
    return (SDL_JoystickID)(device_index + 1);
}

static void SWITCH_UpdateControllerSupport(Uint64 timestamp, bool handheld)
{
    if (!handheld) {
        HidLaControllerSupportResultInfo info;
        HidLaControllerSupportArg args;
        hidLaCreateControllerSupportArg(&args);
        args.hdr.player_count_max = JOYSTICK_COUNT;
        hidLaShowControllerSupportForSystem(&info, &args, false);
    }

    // update pads states
    for (int i = 0; i < JOYSTICK_COUNT; i++) {
        SDL_Joystick *joy = SDL_GetJoystickFromID(SWITCH_JoystickGetDeviceInstanceID(i));
        if (joy) {
            padUpdate(&state[i].pad);
            state[i].pad_type = state[i].pad_type_prev = hidGetNpadDeviceType((HidNpadIdType)i);
            state[i].pad_style = state[i].pad_style_prev = hidGetNpadStyleSet((HidNpadIdType)i);
            // update pad mapping
            if (!(state[i].pad_style & HidNpadStyleTag_NpadJoyDual) &&
                (state[i].pad_type & HidDeviceTypeBits_JoyLeft)) {
                state[i].pad_mapping = (HidNpadButton *)&pad_mapping_left_joy;
            } else if (!(state[i].pad_style & HidNpadStyleTag_NpadJoyDual) &&
                       (state[i].pad_type & HidDeviceTypeBits_JoyRight)) {
                state[i].pad_mapping = (HidNpadButton *)&pad_mapping_right_joy;
            } else {
                state[i].pad_mapping = (HidNpadButton *)&pad_mapping_default;
            }
            SWITCH_SetupDevices(i);
            // reset sdl joysticks states
            SDL_SendJoystickAxis(timestamp, joy, 0, 0);
            SDL_SendJoystickAxis(timestamp, joy, 1, 0);
            SDL_SendJoystickAxis(timestamp, joy, 2, 0);
            SDL_SendJoystickAxis(timestamp, joy, 3, 0);
            state[i].pad.buttons_cur = 0;
            state[i].pad.buttons_old = 0;
            for (int j = 0; j < joy->nbuttons; j++) {
                SDL_SendJoystickButton(timestamp, joy, j, false);
            }
        }
    }
}

/* Function to scan the system for joysticks.
 * It should return 0, or -1 on an unrecoverable fatal error.
 */
static bool SWITCH_JoystickInit(void)
{
    padConfigureInput(JOYSTICK_COUNT, HidNpadStyleSet_NpadStandard);

    // initialize first pad to defaults
    padInitializeDefault(&state[0].pad);
    padUpdate(&state[0].pad);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);

    state[0].pad_type = state[0].pad_type_prev = hidGetNpadDeviceType((HidNpadIdType)0);
    state[0].pad_style = state[0].pad_style_prev = hidGetNpadStyleSet((HidNpadIdType)0);
    if (!(state[0].pad_style & HidNpadStyleTag_NpadJoyDual) &&
        (state[0].pad_type & HidDeviceTypeBits_JoyLeft)) {
        state[0].pad_mapping = (HidNpadButton *)&pad_mapping_left_joy;
    } else if (!(state[0].pad_style & HidNpadStyleTag_NpadJoyDual) &&
               (state[0].pad_type & HidDeviceTypeBits_JoyRight)) {
        state[0].pad_mapping = (HidNpadButton *)&pad_mapping_right_joy;
    } else {
        state[0].pad_mapping = (HidNpadButton *)&pad_mapping_default;
    }

    // initialize pad and vibrations for pad 1 to 7
    for (int i = 1; i < JOYSTICK_COUNT; i++) {
        padInitialize(&state[i].pad, HidNpadIdType_No1 + i);
        padUpdate(&state[i].pad);
        state[i].pad_type = state[i].pad_type_prev = hidGetNpadDeviceType((HidNpadIdType)i);
        state[i].pad_style = state[i].pad_style_prev = hidGetNpadStyleSet((HidNpadIdType)i);
        if (!(state[i].pad_style & HidNpadStyleTag_NpadJoyDual) &&
            (state[i].pad_type & HidDeviceTypeBits_JoyLeft)) {
            state[i].pad_mapping = (HidNpadButton *)&pad_mapping_left_joy;
        } else if (!(state[i].pad_style & HidNpadStyleTag_NpadJoyDual) &&
                   (state[i].pad_type & HidDeviceTypeBits_JoyRight)) {
            state[i].pad_mapping = (HidNpadButton *)&pad_mapping_right_joy;
        } else {
            state[i].pad_mapping = (HidNpadButton *)&pad_mapping_default;
        }
    }

    // All eight, player 1 included: it used to be skipped here, so rumble on
    // the main controller (and in handheld mode) never had a device to go to.
    for (int i = 0; i < JOYSTICK_COUNT; ++i) {
        SWITCH_SetupDevices(i);
        SDL_PrivateJoystickAdded(SWITCH_JoystickGetDeviceInstanceID(i));
    }

    return true;
}

static int SWITCH_JoystickGetCount(void)
{
    return JOYSTICK_COUNT;
}

static void SWITCH_JoystickDetect(void)
{
}

static bool SWITCH_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    // We don't override any other drivers
    return false;
}

/* Function to get the device-dependent name of a joystick */
static const char *SWITCH_JoystickGetDeviceName(int device_index)
{
    return "Switch Controller";
}

static const char *SWITCH_JoystickGetDevicePath(int index)
{
    return NULL;
}

static int SWITCH_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    return -1;
}

static int SWITCH_JoystickGetDevicePlayerIndex(int device_index)
{
    return -1;
}

static void SWITCH_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
}

static SDL_GUID SWITCH_JoystickGetDeviceGUID(int device_index)
{
    /* the GUID is just the name for now */
    const char *name = SWITCH_JoystickGetDeviceName(device_index);
    return SDL_CreateJoystickGUIDForName(name);
}

/* Function to open a joystick for use.
   The joystick to open is specified by the device index.
   This should fill the nbuttons and naxes fields of the joystick structure.
   It returns 0, or -1 if there is an error.
 */
static bool SWITCH_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    joystick->nbuttons = sizeof(pad_mapping_default) / sizeof(*pad_mapping_default);
    joystick->naxes = 4;
    joystick->nhats = 0;
    joystick->instance_id = SWITCH_JoystickGetDeviceInstanceID(device_index);

    SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);

    // Every first-party controller has a six-axis sensor. The rate is how
    // often hid samples it; SDL only uses it to describe the sensor.
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 200.0f);
    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 200.0f);

    return true;
}

static bool SWITCH_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    SWITCHJoystickState *st = &state[joystick->instance_id - 1];

    if (st->nvibration == 0) {
        SWITCH_SetupDevices(joystick->instance_id - 1);
        if (st->nvibration == 0) {
            return SDL_Unsupported();
        }
    }

    // SDL's two motors are strengths (0-65535); hid wants an amplitude (0-1)
    // per band. 160 Hz and 320 Hz are the bands' default frequencies, and
    // both sides of the controller get the same.
    const HidVibrationValue v = {
        .amp_low = low_frequency_rumble / 65535.0f,
        .freq_low = 160.0f,
        .amp_high = high_frequency_rumble / 65535.0f,
        .freq_high = 320.0f,
    };
    const HidVibrationValue values[2] = { v, v };
    const Result rc = hidSendVibrationValues(st->vibration, values, st->nvibration);
    if (R_FAILED(rc)) {
        return SDL_SetError("hidSendVibrationValues failed: 0x%x", rc);
    }
    return true;
}

static bool SWITCH_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left, Uint16 right)
{
    return SDL_Unsupported();
}

static bool SWITCH_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return 0;
}

static bool SWITCH_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool SWITCH_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    SWITCHJoystickState *st = &state[joystick->instance_id - 1];

    if (st->sixaxis_ok && enabled != st->sensors_on) {
        const Result rc = enabled ? hidStartSixAxisSensor(st->sixaxis) : hidStopSixAxisSensor(st->sixaxis);
        if (R_FAILED(rc)) {
            return SDL_SetError("six-axis sensor: 0x%x", rc);
        }
    }
    st->sensors_on = enabled;
    return true;
}

// hid reports angular velocity in turns per second and acceleration in g,
// with x to the right, y away from the player and z up. SDL wants radians per
// second and m/s^2, with x right, y up and z towards the player.
static void SWITCH_SendSensors(Uint64 timestamp, SDL_Joystick *joystick, SWITCHJoystickState *st)
{
    HidSixAxisSensorState s;

    if (!st->sensors_on || !st->sixaxis_ok ||
        hidGetSixAxisSensorStates(st->sixaxis, &s, 1) == 0 ||
        s.sampling_number == st->sixaxis_sample) {
        return;
    }
    st->sixaxis_sample = s.sampling_number;

    const float gyro[3] = {
        s.angular_velocity.x * 2.0f * SDL_PI_F,
        s.angular_velocity.z * 2.0f * SDL_PI_F,
        -s.angular_velocity.y * 2.0f * SDL_PI_F,
    };
    const float accel[3] = {
        s.acceleration.x * SDL_STANDARD_GRAVITY,
        s.acceleration.z * SDL_STANDARD_GRAVITY,
        -s.acceleration.y * SDL_STANDARD_GRAVITY,
    };
    SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, timestamp, gyro, 3);
    SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, timestamp, accel, 3);
}

/* Function to update the state of a joystick - called as a device poll.
 * This function shouldn't update the joystick structure directly,
 * but instead should call SDL_PrivateJoystick*() to deliver events
 * and update joystick device state.
 */
static void SWITCH_JoystickUpdate(SDL_Joystick *joystick)
{
    Uint64 timestamp = SDL_GetTicksNS();

    u64 diff;
    int index = joystick->instance_id - 1;

    if (index >= JOYSTICK_COUNT || SDL_TextInputActive(SDL_GetKeyboardFocus())) {
        return;
    }

    padUpdate(&state[index].pad);
    if (!padIsConnected(&state[index].pad)) {
        return;
    }

    // update pad type and style, open controller support applet if needed
    state[index].pad_type = hidGetNpadDeviceType((HidNpadIdType)index);
    state[index].pad_style = hidGetNpadStyleSet((HidNpadIdType)index);
    if (state[index].pad_type != state[index].pad_type_prev || state[index].pad_style != state[index].pad_style_prev) {
        SWITCH_UpdateControllerSupport(timestamp, padIsHandheld(&state[index].pad) ? true : false);
        return;
    }

    // only handle axes in non-single joycon mode
    if (state[index].pad_style & HidNpadStyleTag_NpadJoyDual || (state[index].pad_type != HidDeviceTypeBits_JoyLeft && state[index].pad_type != HidDeviceTypeBits_JoyRight)) {
        // axis left
        if (state[index].sticks_old[0].x != state[index].pad.sticks[0].x) {
            SDL_SendJoystickAxis(timestamp, joystick, 0, (Sint16)state[index].pad.sticks[0].x);
            state[index].sticks_old[0].x = state[index].pad.sticks[0].x;
        }
        if (state[index].sticks_old[0].y != state[index].pad.sticks[0].y) {
            SDL_SendJoystickAxis(timestamp, joystick, 1, (Sint16)-state[index].pad.sticks[0].y);
            state[index].sticks_old[0].y = -state[index].pad.sticks[0].y;
        }
        state[index].sticks_old[0] = padGetStickPos(&state[index].pad, 0);
        // axis right
        if (state[index].sticks_old[1].x != state[index].pad.sticks[1].x) {
            SDL_SendJoystickAxis(timestamp, joystick, 2, (Sint16)state[index].pad.sticks[1].x);
            state[index].sticks_old[1].x = state[index].pad.sticks[1].x;
        }
        if (state[index].sticks_old[1].y != state[index].pad.sticks[1].y) {
            SDL_SendJoystickAxis(timestamp, joystick, 3, (Sint16)-state[index].pad.sticks[1].y);
            state[index].sticks_old[1].y = -state[index].pad.sticks[1].y;
        }
        state[index].sticks_old[1] = padGetStickPos(&state[index].pad, 1);
    }

    SWITCH_SendSensors(timestamp, joystick, &state[index]);

    // handle buttons
    diff = state[index].pad.buttons_old ^ state[index].pad.buttons_cur;
    if (diff) {
        for (int i = 0; i < joystick->nbuttons; i++) {
            if (diff & state[index].pad_mapping[i]) {
                SDL_SendJoystickButton(
                    timestamp,
                    joystick, i,
                    (state[index].pad.buttons_cur & state[index].pad_mapping[i]) != 0);
            }
        }
    }
}

/* Function to close a joystick after use */
static void SWITCH_JoystickClose(SDL_Joystick *joystick)
{
    SWITCHJoystickState *st = &state[joystick->instance_id - 1];

    if (st->sixaxis_ok && st->sensors_on) {
        hidStopSixAxisSensor(st->sixaxis);
    }
    st->sensors_on = false;
}

/* Function to perform any system-specific joystick related cleanup */
static void SWITCH_JoystickQuit(void)
{
    for (int i = 0; i < JOYSTICK_COUNT; ++i) {
        SDL_PrivateJoystickRemoved(SWITCH_JoystickGetDeviceInstanceID(i));
    }
}

static bool SWITCH_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    // clang-format off
    *out = (SDL_GamepadMapping){
#if 0
        /*
         * This is as they are physically.
         */
        .a             = {.kind = EMappingKind_Button, .target =   0},
        .b             = {.kind = EMappingKind_Button, .target =   1},
        .x             = {.kind = EMappingKind_Button, .target =   2},
        .y             = {.kind = EMappingKind_Button, .target =   3},
#else
        /*
         * This is as they are when swapped, i.e. what everything else expects.
         * This also matches the behaviour of the old SDL2 port.
         */
        .a             = {.kind = EMappingKind_Button, .target =   1},
        .b             = {.kind = EMappingKind_Button, .target =   0},
        .x             = {.kind = EMappingKind_Button, .target =   3},
        .y             = {.kind = EMappingKind_Button, .target =   2},
#endif
        .back          = {.kind = EMappingKind_Button, .target =  11},
        .guide         = {.kind = EMappingKind_None,   .target = 255},
        .start         = {.kind = EMappingKind_Button, .target =  10},
        .leftstick     = {.kind = EMappingKind_Button, .target =   4},
        .rightstick    = {.kind = EMappingKind_Button, .target =   5},
        .leftshoulder  = {.kind = EMappingKind_Button, .target =   6},
        .rightshoulder = {.kind = EMappingKind_Button, .target =   7},
        .dpup          = {.kind = EMappingKind_Button, .target =  13},
        .dpdown        = {.kind = EMappingKind_Button, .target =  15},
        .dpleft        = {.kind = EMappingKind_Button, .target =  12},
        .dpright       = {.kind = EMappingKind_Button, .target =  14},
        .misc1         = {.kind = EMappingKind_None,   .target = 255},
        .misc2         = {.kind = EMappingKind_None,   .target = 255},
        .misc3         = {.kind = EMappingKind_None,   .target = 255},
        .misc4         = {.kind = EMappingKind_None,   .target = 255},
        .misc5         = {.kind = EMappingKind_None,   .target = 255},
        .misc6         = {.kind = EMappingKind_None,   .target = 255},
        .right_paddle1 = {.kind = EMappingKind_None,   .target = 255},
        .left_paddle1  = {.kind = EMappingKind_None,   .target = 255},
        .right_paddle2 = {.kind = EMappingKind_None,   .target = 255},
        .left_paddle2  = {.kind = EMappingKind_None,   .target = 255},
        .leftx         = {.kind = EMappingKind_Axis,   .target =   0},
        .lefty         = {.kind = EMappingKind_Axis,   .target =   1},
        .rightx        = {.kind = EMappingKind_Axis,   .target =   2},
        .righty        = {.kind = EMappingKind_Axis,   .target =   3},
        .lefttrigger   = {.kind = EMappingKind_Button, .target =   8},
        .righttrigger  = {.kind = EMappingKind_Button, .target =   9},
        .touchpad      = {.kind = EMappingKind_None,   .target = 255},
    };
    // clang-format on
    return true;
}

SDL_JoystickDriver SDL_SWITCH_JoystickDriver = {
    .Init = SWITCH_JoystickInit,
    .GetCount = SWITCH_JoystickGetCount,
    .Detect = SWITCH_JoystickDetect,
    .IsDevicePresent = SWITCH_JoystickIsDevicePresent,
    .GetDeviceName = SWITCH_JoystickGetDeviceName,
    .GetDevicePath = SWITCH_JoystickGetDevicePath,
    .GetDeviceSteamVirtualGamepadSlot = SWITCH_JoystickGetDeviceSteamVirtualGamepadSlot,
    .GetDevicePlayerIndex = SWITCH_JoystickGetDevicePlayerIndex,
    .SetDevicePlayerIndex = SWITCH_JoystickSetDevicePlayerIndex,
    .GetDeviceGUID = SWITCH_JoystickGetDeviceGUID,
    .GetDeviceInstanceID = SWITCH_JoystickGetDeviceInstanceID,

    .Open = SWITCH_JoystickOpen,

    .Rumble = SWITCH_JoystickRumble,
    .RumbleTriggers = SWITCH_JoystickRumbleTriggers,

    .SetLED = SWITCH_JoystickSetLED,
    .SendEffect = SWITCH_JoystickSendEffect,
    .SetSensorsEnabled = SWITCH_JoystickSetSensorsEnabled,

    .Update = SWITCH_JoystickUpdate,
    .Close = SWITCH_JoystickClose,
    .Quit = SWITCH_JoystickQuit,

    .GetGamepadMapping = SWITCH_JoystickGetGamepadMapping,
};

#endif /* SDL_JOYSTICK_SWITCH */

/* vi: set ts=4 sw=4 expandtab: */
