#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "switch_input.h"
#include "platformdefs.h"

#define SWITCH_NPAD_COUNT 8

static PadState pads[SWITCH_NPAD_COUNT];
static bool initialized = false;

static void mapLibnxToGml(GamepadSlot* slot, PadState* pad, u64 cur) {
    // GameMaker numbers face buttons by position, not by the letter printed on them:
    // gp_face1 = bottom, gp_face2 = right, gp_face3 = left, gp_face4 = top.
    // On Nintendo controllers that is B, A, Y, X. Games that show Nintendo button names
    // (AM2R's "B", "A", "Y", "X" table for gp_face1..4) rely on this.
    if (cur & HidNpadButton_B) slot->buttonDown[0] = true;
    if (cur & HidNpadButton_A) slot->buttonDown[1] = true;
    if (cur & HidNpadButton_Y) slot->buttonDown[2] = true;
    if (cur & HidNpadButton_X) slot->buttonDown[3] = true;
    if (cur & HidNpadButton_L) slot->buttonDown[4] = true;
    if (cur & HidNpadButton_R) slot->buttonDown[5] = true;
    slot->buttonValue[6] = (cur & HidNpadButton_ZL) ? 1.0f : 0.0f;
    slot->buttonValue[7] = (cur & HidNpadButton_ZR) ? 1.0f : 0.0f;
    if (cur & HidNpadButton_ZL) slot->buttonDown[6] = true;
    if (cur & HidNpadButton_ZR) slot->buttonDown[7] = true;
    if (cur & HidNpadButton_Minus) slot->buttonDown[8] = true;
    if (cur & HidNpadButton_Plus) slot->buttonDown[9] = true;
    if (cur & HidNpadButton_StickL) slot->buttonDown[10] = true;
    if (cur & HidNpadButton_StickR) slot->buttonDown[11] = true;
    if (cur & HidNpadButton_AnyUp) slot->buttonDown[12] = true;
    if (cur & HidNpadButton_AnyDown) slot->buttonDown[13] = true;
    if (cur & HidNpadButton_AnyLeft) slot->buttonDown[14] = true;
    if (cur & HidNpadButton_AnyRight) slot->buttonDown[15] = true;

    HidAnalogStickState l = padGetStickPos(pad, 0);
    HidAnalogStickState r = padGetStickPos(pad, 1);
    slot->axisValue[0] = l.x / 32767.0f;
    slot->axisValue[1] = -l.y / 32767.0f;
    slot->axisValue[2] = r.x / 32767.0f;
    slot->axisValue[3] = -r.y / 32767.0f;
}

static void fillGamepadSlot(GamepadSlot* slot, PadState* pad, u64 cur, bool connected, int jid, const char* guid) {
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDownPrev));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));

    slot->connected = connected;
    slot->jid = jid;
    strncpy(slot->description, "Nintendo Switch Controller", sizeof(slot->description) - 1);
    strncpy(slot->guid, guid, sizeof(slot->guid) - 1);

    if (connected) mapLibnxToGml(slot, pad, cur);

    for (int btn = 0; GP_BUTTON_COUNT > btn; btn++) {
        bool wasDown = slot->buttonDownPrev[btn];
        if (slot->buttonDown[btn] && !wasDown) slot->buttonPressed[btn] = true;
        if (!slot->buttonDown[btn] && wasDown) slot->buttonReleased[btn] = true;
    }
}


// ===[ Rumble (gamepad_set_vibration) ]===
// HD rumble through libnx. Each pad has its own vibration devices depending on how it is held:
// handheld (two attached Joy-Cons), a Joy-Con pair, a Pro Controller, or a single Joy-Con.

typedef struct {
    HidNpadIdType id;
    u32 style;
    int count;
    HidVibrationDeviceHandle handles[2];
    float lastLeft;
    float lastRight;
} VibrationTarget;

static VibrationTarget vibrationTargets[SWITCH_NPAD_COUNT];

// Low/high frequencies close to the Switch's default rumble feel.
#define VIBRATION_FREQ_LOW  160.0f
#define VIBRATION_FREQ_HIGH 320.0f

static bool resolveVibrationTarget(int device, HidNpadIdType* outId, u32* outStyle, int* outCount) {
    // Pad 0 also covers handheld mode (padInitializeDefault reads No1 and Handheld).
    if (device == 0 && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld)) {
        *outId = HidNpadIdType_Handheld;
        *outStyle = HidNpadStyleTag_NpadHandheld;
        *outCount = 2;
        return true;
    }

    HidNpadIdType id = (HidNpadIdType) (HidNpadIdType_No1 + device);
    u32 styles = hidGetNpadStyleSet(id);
    *outId = id;
    if (styles & HidNpadStyleTag_NpadFullKey) {
        *outStyle = HidNpadStyleTag_NpadFullKey;
        *outCount = 2;
    } else if (styles & HidNpadStyleTag_NpadJoyDual) {
        *outStyle = HidNpadStyleTag_NpadJoyDual;
        *outCount = 2;
    } else if (styles & HidNpadStyleTag_NpadJoyLeft) {
        *outStyle = HidNpadStyleTag_NpadJoyLeft;
        *outCount = 1;
    } else if (styles & HidNpadStyleTag_NpadJoyRight) {
        *outStyle = HidNpadStyleTag_NpadJoyRight;
        *outCount = 1;
    } else {
        return false;
    }
    return true;
}

static float clampUnit(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static void setVibrationValue(HidVibrationValue* value, float amplitude) {
    value->amp_low = amplitude;
    value->freq_low = VIBRATION_FREQ_LOW;
    value->amp_high = amplitude;
    value->freq_high = VIBRATION_FREQ_HIGH;
}

void SwitchInput_setVibration(int device, float left, float right) {
    if (device < 0 || device >= SWITCH_NPAD_COUNT) return;

    left = clampUnit(left);
    right = clampUnit(right);

    HidNpadIdType id;
    u32 style;
    int count;
    if (!resolveVibrationTarget(device, &id, &style, &count)) return;

    VibrationTarget* target = &vibrationTargets[device];
    bool reinit = target->count == 0 || target->id != id || target->style != style;
    if (reinit) {
        if (R_FAILED(hidInitializeVibrationDevices(target->handles, count, id, (HidNpadStyleTag) style))) {
            target->count = 0;
            return;
        }
        target->id = id;
        target->style = style;
        target->count = count;
        target->lastLeft = -1.0f;
        target->lastRight = -1.0f;
    }

    // Games often set the same strength every frame; only talk to the controller when it changes.
    if (!reinit && target->lastLeft == left && target->lastRight == right) return;
    target->lastLeft = left;
    target->lastRight = right;

    HidVibrationValue values[2];
    if (count == 2) {
        // Left motor -> left side (left Joy-Con / left grip), right motor -> right side.
        setVibrationValue(&values[0], left);
        setVibrationValue(&values[1], right);
    } else {
        // A single Joy-Con held sideways gets the stronger of the two.
        setVibrationValue(&values[0], left > right ? left : right);
    }
    hidSendVibrationValues(target->handles, values, count);
}

bool SwitchInput_handleEvents(struct Runner* runner) {
    if (!initialized) {
        padConfigureInput(SWITCH_NPAD_COUNT, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pads[0]);
        for (int i = 1; SWITCH_NPAD_COUNT > i; i++) {
            padInitialize(&pads[i], HidNpadIdType_No1 + i);
        }
        runner->gamepads->setVibration = SwitchInput_setVibration;
        initialized = true;
    }

    if (!appletMainLoop()) return true;

    int connectedCount = 0;
    for (int i = 0; SWITCH_NPAD_COUNT > i; i++) {
        padUpdate(&pads[i]);
        u64 cur = pads[i].buttons_cur;
        bool connected = padIsConnected(&pads[i]);

        char guid[32];
        snprintf(guid, sizeof(guid), "switch-nx-%d", i + 1);
        fillGamepadSlot(&runner->gamepads->slots[i], &pads[i], cur, connected, i, guid);

        if (connected) connectedCount++;
    }

    runner->gamepads->connectedCount = connectedCount;

    return false;
}
